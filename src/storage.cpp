#include "storage.h"

#include <nvs_flash.h>

namespace storage {

namespace {

// NVS read-side error suppression: treat NOT_FOUND as the "default
// value" path; anything else is a real error we'd want surfaced via
// ok() once we wire that in. The Arduino Preferences wrapper does the
// same silently.
bool err_is_missing(esp_err_t e) {
    return e == ESP_ERR_NVS_NOT_FOUND;
}

}  // namespace

Namespace::Namespace(const char *name, bool readonly) : readonly_(readonly) {
    esp_err_t e = nvs_open(name,
                           readonly ? NVS_READONLY : NVS_READWRITE,
                           &handle_);
    ok_ = (e == ESP_OK);
}

Namespace::~Namespace() {
    if (!ok_) return;
    if (!readonly_ && dirty_) {
        nvs_commit(handle_);
    }
    nvs_close(handle_);
}

// ---- get helpers --------------------------------------------------------

std::string Namespace::get_string(const char *key, const char *default_) {
    if (!ok_) return default_ ? default_ : "";
    size_t need = 0;
    esp_err_t e = nvs_get_str(handle_, key, nullptr, &need);
    if (e != ESP_OK || need == 0) return default_ ? default_ : "";
    std::string out;
    out.resize(need);  // includes trailing NUL slot per NVS contract
    e = nvs_get_str(handle_, key, &out[0], &need);
    if (e != ESP_OK) return default_ ? default_ : "";
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

uint8_t Namespace::get_u8(const char *key, uint8_t default_) {
    if (!ok_) return default_;
    uint8_t v = default_;
    esp_err_t e = nvs_get_u8(handle_, key, &v);
    if (e != ESP_OK && !err_is_missing(e)) return default_;
    if (err_is_missing(e)) return default_;
    return v;
}

int8_t Namespace::get_i8(const char *key, int8_t default_) {
    if (!ok_) return default_;
    int8_t v = default_;
    esp_err_t e = nvs_get_i8(handle_, key, &v);
    if (e != ESP_OK) return default_;
    return v;
}

uint16_t Namespace::get_u16(const char *key, uint16_t default_) {
    if (!ok_) return default_;
    uint16_t v = default_;
    esp_err_t e = nvs_get_u16(handle_, key, &v);
    if (e != ESP_OK) return default_;
    return v;
}

uint32_t Namespace::get_u32(const char *key, uint32_t default_) {
    if (!ok_) return default_;
    uint32_t v = default_;
    esp_err_t e = nvs_get_u32(handle_, key, &v);
    if (e != ESP_OK) return default_;
    return v;
}

// ---- put helpers --------------------------------------------------------

bool Namespace::put_string(const char *key, const char *value) {
    if (!ok_ || readonly_) return false;
    esp_err_t e = nvs_set_str(handle_, key, value ? value : "");
    if (e == ESP_OK) dirty_ = true;
    return e == ESP_OK;
}

bool Namespace::put_u8(const char *key, uint8_t value) {
    if (!ok_ || readonly_) return false;
    esp_err_t e = nvs_set_u8(handle_, key, value);
    if (e == ESP_OK) dirty_ = true;
    return e == ESP_OK;
}

bool Namespace::put_i8(const char *key, int8_t value) {
    if (!ok_ || readonly_) return false;
    esp_err_t e = nvs_set_i8(handle_, key, value);
    if (e == ESP_OK) dirty_ = true;
    return e == ESP_OK;
}

bool Namespace::put_u16(const char *key, uint16_t value) {
    if (!ok_ || readonly_) return false;
    esp_err_t e = nvs_set_u16(handle_, key, value);
    if (e == ESP_OK) dirty_ = true;
    return e == ESP_OK;
}

bool Namespace::put_u32(const char *key, uint32_t value) {
    if (!ok_ || readonly_) return false;
    esp_err_t e = nvs_set_u32(handle_, key, value);
    if (e == ESP_OK) dirty_ = true;
    return e == ESP_OK;
}

bool Namespace::remove(const char *key) {
    if (!ok_ || readonly_) return false;
    esp_err_t e = nvs_erase_key(handle_, key);
    if (e == ESP_OK) dirty_ = true;
    // NOT_FOUND on remove is a success-equivalent.
    return e == ESP_OK || err_is_missing(e);
}

}  // namespace storage
