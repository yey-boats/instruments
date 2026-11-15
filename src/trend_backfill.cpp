// Trend history backfill worker (SignalK History API). See include/trend_backfill.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "trend_backfill.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "net.h"
#include "psram_json.h"
#include "signalk.h"

namespace trend::backfill {

namespace {

constexpr int MAX_ROWS = 512;          // server rows kept per response
constexpr int MAX_BODY = 64 * 1024;    // larger responses are skipped
constexpr uint32_t TIMEOUT_MS = 5000;  // connect + read timeout
constexpr UBaseType_t QUEUE_LEN = MAX_SERIES_CAP;
constexpr uint32_t TASK_STACK = 6144;    // HTTPClient + streaming JSON parse
constexpr uint32_t IDLE_EXIT_MS = 3000;  // worker exits when idle (frees its stack)

struct Job {
    uint32_t gen;
    uint8_t series;
    Agg agg;
    uint32_t window_s;
    float step_s;
    char path[KEY_MAX];
};

QueueHandle_t s_q = nullptr;
SemaphoreHandle_t s_mtx = nullptr;
TaskHandle_t s_task = nullptr;
volatile uint32_t s_gen = 0;

// Single result mailbox (PSRAM). The worker writes it only while !s_ready;
// the UI task reads it only while s_ready, then done() clears the flag.
float *s_age = nullptr;
float *s_val = nullptr;
volatile bool s_ready = false;
Result s_result = {};

bool ensure_init() {
    if (s_mtx && s_q && s_age && s_val) return true;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_q) s_q = xQueueCreate(QUEUE_LEN, sizeof(Job));
    if (!s_age) {
        s_age = (float *)heap_caps_malloc(sizeof(float) * MAX_ROWS, MALLOC_CAP_SPIRAM);
        if (!s_age) s_age = (float *)malloc(sizeof(float) * MAX_ROWS);
    }
    if (!s_val) {
        s_val = (float *)heap_caps_malloc(sizeof(float) * MAX_ROWS, MALLOC_CAP_SPIRAM);
        if (!s_val) s_val = (float *)malloc(sizeof(float) * MAX_ROWS);
    }
    return s_mtx && s_q && s_age && s_val;
}

// Outcome of one HTTP attempt.
enum class Fetch { Rows, NoData, Retry };

Fetch fetch_once(const Job &j, int &rows, double &t_resp) {
    rows = 0;
    String host = sk::targetHost();
    if (host.length() == 0 || WiFi.status() != WL_CONNECTED) return Fetch::NoData;
    char query[KEY_MAX + 128];
    if (!build_history_query(query, sizeof(query), j.path, j.agg, j.window_s, j.step_s))
        return Fetch::NoData;
    String url = String("http://") + host + ":" + String(sk::targetPort()) + query;
    HTTPClient http;
    http.setConnectTimeout(TIMEOUT_MS);
    http.setTimeout(TIMEOUT_MS);
    if (!http.begin(url)) return Fetch::Retry;
    String token = sk::authToken();
    if (token.length()) http.addHeader("Authorization", String("Bearer ") + token);
    int code = http.GET();
    t_resp = now_s();
    if (code != 200) {
        http.end();
        net::logf("[trend] backfill %s:%s -> %d", j.path, agg_sk_method(j.agg), code);
        // 404/501 = no history API / no provider; 4xx = not for us. Retry only
        // a transport error (negative code) or a server error.
        if (code < 0 || (code >= 500 && code != 501)) return Fetch::Retry;
        return Fetch::NoData;
    }
    int size = http.getSize();
    if (size <= 0 || size > MAX_BODY) {
        http.end();
        net::logf("[trend] backfill %s: body size %d not handled", j.path, size);
        return Fetch::NoData;
    }
    JsonDocument doc(&yeyboats::psram_json);
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        net::logf("[trend] backfill %s: bad json (%s)", j.path, err.c_str());
        return Fetch::Retry;
    }
    int n = parse_history_rows(doc.as<JsonVariantConst>(), s_age, s_val, MAX_ROWS);
    net::logf("[trend] backfill %s:%s window=%lus step=%.3fs -> %d row(s)", j.path,
              agg_sk_method(j.agg), (unsigned long)j.window_s, (double)j.step_s, n);
    if (n <= 0) return Fetch::NoData;
    rows = n;
    return Fetch::Rows;
}

void worker(void *) {
    for (;;) {
        Job j;
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(IDLE_EXIT_MS)) != pdTRUE) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            if (uxQueueMessagesWaiting(s_q) == 0) {
                s_task = nullptr;
                xSemaphoreGive(s_mtx);
                vTaskDelete(nullptr);
                return;
            }
            xSemaphoreGive(s_mtx);
            continue;
        }
        if (j.gen != s_gen) continue;  // screen left: cancelled
        // The mailbox holds one result: wait for the UI to take the last one.
        while (s_ready && j.gen == s_gen)
            vTaskDelay(pdMS_TO_TICKS(50));
        if (j.gen != s_gen) continue;
        int rows = 0;
        double t_resp = 0;
        Fetch f = fetch_once(j, rows, t_resp);
        if (f == Fetch::Retry && j.gen == s_gen) f = fetch_once(j, rows, t_resp);
        if (f != Fetch::Rows) continue;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (j.gen == s_gen) {
            s_result.gen = j.gen;
            s_result.series = j.series;
            s_result.n = rows;
            s_result.t_resp_s = t_resp;
            s_result.age = s_age;
            s_result.val = s_val;
            s_ready = true;
        }
        xSemaphoreGive(s_mtx);
    }
}

}  // namespace

double now_s() {
    return (double)esp_timer_get_time() / 1e6;
}

uint32_t begin() {
    if (!ensure_init()) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t g = s_gen + 1;
    if (g == 0) g = 1;
    s_gen = g;
    xQueueReset(s_q);
    s_ready = false;  // drop an unconsumed result of the old generation
    xSemaphoreGive(s_mtx);
    return g;
}

bool enqueue(uint32_t gen, uint8_t series, const char *path, Agg agg, uint32_t window_s,
             double step_s) {
    if (!ensure_init() || gen == 0 || !path_ok(path)) return false;
    Job j;
    memset(&j, 0, sizeof(j));
    j.gen = gen;
    j.series = series;
    j.agg = agg;
    j.window_s = window_s;
    j.step_s = (float)step_s;
    strncpy(j.path, path, sizeof(j.path) - 1);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ok = gen == s_gen && xQueueSend(s_q, &j, 0) == pdTRUE;
    if (ok && !s_task) {
        // Core 0 with the other network workers; low priority (the UI and the
        // SignalK WS task come first).
        if (xTaskCreatePinnedToCore(worker, "trend-bf", TASK_STACK, nullptr, 1, &s_task, 0) !=
            pdPASS) {
            s_task = nullptr;
            xQueueReset(s_q);
            ok = false;
            net::logf("[trend] backfill worker create failed");
        }
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

bool poll(uint32_t gen, Result &out) {
    if (!s_ready || !s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ok = s_ready && s_result.gen == gen;
    if (ok) {
        out = s_result;
    } else if (s_ready) {
        s_ready = false;  // stale generation
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

void done() {
    s_ready = false;
}

}  // namespace trend::backfill
