#include "screenshot.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <string.h>

#include "net.h"

namespace screenshot {

struct Slot {
    SemaphoreHandle_t done;
    uint8_t *out_bmp;
    size_t out_len;
    bool pending;
};

static Slot s_slot;
static SemaphoreHandle_t s_mutex;  // guards s_slot.pending

void setup() {
    s_slot = {};
    s_mutex = xSemaphoreCreateMutex();
}

#if LV_USE_SNAPSHOT
static bool build_bmp_from_snapshot(uint8_t **out, size_t *out_len) {
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_NATIVE);
    if (!buf) return false;
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    uint32_t pix_bytes = stride * h;
    uint32_t total = 14 + 40 + 12 + pix_bytes;

    uint8_t *bmp = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp) {
        lv_draw_buf_destroy(buf);
        return false;
    }
    memset(bmp, 0, 14 + 40 + 12);
    // BMP file header
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = total & 0xff;
    bmp[3] = (total >> 8) & 0xff;
    bmp[4] = (total >> 16) & 0xff;
    bmp[5] = (total >> 24) & 0xff;
    bmp[10] = (14 + 40 + 12) & 0xff;  // pixel data offset
    // DIB header (40 bytes)
    bmp[14] = 40;
    bmp[18] = w & 0xff;
    bmp[19] = (w >> 8) & 0xff;
    int32_t neg_h = -(int32_t)h;
    bmp[22] = neg_h & 0xff;
    bmp[23] = (neg_h >> 8) & 0xff;
    bmp[24] = (neg_h >> 16) & 0xff;
    bmp[25] = (neg_h >> 24) & 0xff;
    bmp[26] = 1;             // planes
    bmp[28] = 16;            // bpp
    bmp[30] = 3;             // BI_BITFIELDS
    bmp[34] = pix_bytes & 0xff;
    bmp[35] = (pix_bytes >> 8) & 0xff;
    bmp[36] = (pix_bytes >> 16) & 0xff;
    bmp[37] = (pix_bytes >> 24) & 0xff;
    // RGB565 channel masks
    bmp[54] = 0x00; bmp[55] = 0xF8;
    bmp[58] = 0xE0; bmp[59] = 0x07;
    bmp[62] = 0x1F;

    memcpy(bmp + 14 + 40 + 12, buf->data, pix_bytes);
    lv_draw_buf_destroy(buf);
    *out = bmp;
    *out_len = total;
    return true;
}
#endif

void serve_pending() {
    if (!s_mutex || !xSemaphoreTake(s_mutex, 0)) return;
    bool pending = s_slot.pending;
    xSemaphoreGive(s_mutex);
    if (!pending) return;

#if LV_USE_SNAPSHOT
    uint8_t *bmp = nullptr;
    size_t len = 0;
    bool ok = build_bmp_from_snapshot(&bmp, &len);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_slot.out_bmp = ok ? bmp : nullptr;
    s_slot.out_len = ok ? len : 0;
    s_slot.pending = false;
    xSemaphoreGive(s_mutex);
#else
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_slot.out_bmp = nullptr;
    s_slot.out_len = 0;
    s_slot.pending = false;
    xSemaphoreGive(s_mutex);
#endif
    if (s_slot.done) xSemaphoreGive(s_slot.done);
}

bool request(uint32_t timeout_ms, uint8_t **out_bmp, size_t *out_len) {
    if (!s_mutex) return false;
    if (!xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms))) return false;
    if (s_slot.pending) {
        // Someone else is mid-flight - too slow to queue, just fail.
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_slot.done = xSemaphoreCreateBinary();
    s_slot.out_bmp = nullptr;
    s_slot.out_len = 0;
    s_slot.pending = true;
    xSemaphoreGive(s_mutex);

    bool got = xSemaphoreTake(s_slot.done, pdMS_TO_TICKS(timeout_ms));
    SemaphoreHandle_t sem = s_slot.done;
    s_slot.done = nullptr;
    if (sem) vSemaphoreDelete(sem);

    if (!got) return false;
    *out_bmp = s_slot.out_bmp;
    *out_len = s_slot.out_len;
    return *out_bmp != nullptr;
}

}  // namespace screenshot
