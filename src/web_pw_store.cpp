// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "web_pw_store.h"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/version.h>
#include <string.h>

#include "net.h"
#include "storage.h"
#include "web_pw.h"

namespace web_pw_store {

namespace {

constexpr const char *NS = "web";

// -1 unknown, 0 no password, 1 password provisioned.
volatile int s_set = -1;

// Serialises NVS read-modify-write and the cache between the web task
// (verify) and the manager task (set_password). Created in migrate(), which
// runs from setup() before either task exists.
SemaphoreHandle_t s_mtx = nullptr;
web_pw::VerifyCache s_cache;
uint8_t s_tag_key[32];  // per-boot random HMAC key for cache tags
bool s_logged_cost = false;

struct Lock {
    Lock() {
        if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    ~Lock() {
        if (s_mtx) xSemaphoreGive(s_mtx);
    }
};

struct NvsKv : web_pw::Kv {
    std::string get(const char *key) override {
        storage::Namespace p(NS, true);
        return p.get_string(key, "");
    }
    bool put(const char *key, const char *value) override {
        storage::Namespace p(NS, false);
        return p.put_string(key, value);
    }
    void remove(const char *key) override {
        storage::Namespace p(NS, false);
        p.remove(key);
    }
};

bool pbkdf2_sha256(const char *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[web_pw::DK_LEN]) {
    int64_t t0 = esp_timer_get_time();
#if MBEDTLS_VERSION_NUMBER >= 0x03030000
    bool ok = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, pw_len,
                                            salt, salt_len, iterations, web_pw::DK_LEN, out) == 0;
#else
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    bool ok = mbedtls_md_setup(&ctx, info, 1 /* HMAC */) == 0 &&
              mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *)pw, pw_len, salt, salt_len,
                                        iterations, web_pw::DK_LEN, out) == 0;
    mbedtls_md_free(&ctx);
#endif
    if (!s_logged_cost) {
        // Field measurement of the per-auth cost (see web_pw.h).
        s_logged_cost = true;
        net::logf("[web] pbkdf2-sha256 x%u took %u ms", (unsigned)iterations,
                  (unsigned)((esp_timer_get_time() - t0) / 1000));
    }
    return ok;
}

bool cache_tag(const char *record, const char *pw, size_t pw_len, uint8_t out[web_pw::TAG_LEN]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const unsigned char sep = 0;
    bool ok = mbedtls_md_setup(&ctx, info, 1) == 0 &&
              mbedtls_md_hmac_starts(&ctx, s_tag_key, sizeof(s_tag_key)) == 0 &&
              mbedtls_md_hmac_update(&ctx, (const unsigned char *)record, strlen(record)) == 0 &&
              mbedtls_md_hmac_update(&ctx, &sep, 1) == 0 &&
              mbedtls_md_hmac_update(&ctx, (const unsigned char *)pw, pw_len) == 0 &&
              mbedtls_md_hmac_finish(&ctx, out) == 0;
    mbedtls_md_free(&ctx);
    return ok;
}

// The bootloader may still roll this image back.
bool image_pending() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

void reconcile(bool pending) {
    NvsKv kv;
    uint8_t salt[web_pw::SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    bool had_plain = !kv.get(web_pw::KEY_PLAIN).empty();
    if (!web_pw::reconcile(kv, pending, salt, pbkdf2_sha256)) {
        net::logf("[web] web password migration FAILED; keeping legacy key");
    } else if (had_plain) {
        net::logf(pending ? "[web] web password hashed; plaintext kept until the image is confirmed"
                          : "[web] web password hashed; plaintext erased");
    }
    s_cache.clear();
    s_set = web_pw::is_set(kv) ? 1 : 0;
}

}  // namespace

void migrate() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    esp_fill_random(s_tag_key, sizeof(s_tag_key));
    Lock l;
    reconcile(image_pending());
}

void image_confirmed() {
    Lock l;
    reconcile(false);
}

bool set_password(const char *pw) {
    Lock l;
    NvsKv kv;
    uint8_t salt[web_pw::SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    bool ok = web_pw::set_password(kv, pw, image_pending(), salt, pbkdf2_sha256);
    s_cache.clear();
    s_set = web_pw::is_set(kv) ? 1 : 0;
    return ok;
}

// Fail safe: if a migration could not write the record (NVS full/error), the
// legacy plaintext stays and still counts as "set" and still verifies, so a
// failed migration never opens the API.
bool is_set() {
    if (s_set < 0) {
        NvsKv kv;
        s_set = web_pw::is_set(kv) ? 1 : 0;
    }
    return s_set == 1;
}

bool verify(const char *pw, size_t len) {
    Lock l;
    NvsKv kv;
    return web_pw::verify_cached(kv, pw, len, pbkdf2_sha256, cache_tag, s_cache,
                                 (uint32_t)millis());
}

}  // namespace web_pw_store
