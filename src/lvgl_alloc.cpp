// LVGL allocator hooks routed to ESP32-S3 PSRAM.
//
// LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM in include/lv_conf.h. LVGL calls
// the lv_*_core() functions here for every alloc; we forward to ESP-IDF's
// heap_caps allocators with MALLOC_CAP_SPIRAM so widget metadata, canvas
// buffers, qrcode bitmaps, snapshot pixmaps all land in the 8 MB PSRAM
// pool instead of the (much smaller) internal SRAM.
//
// IMPORTANT: the LVGL display flush BUFFERS (lv_display_set_buffers in
// main.cpp) stay in MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL. PSRAM is not
// DMA-safe on this chip for the RGB panel push path.

#include <lvgl.h>
#include <esp_heap_caps.h>
#include <string.h>

extern "C" {

void lv_mem_init(void) {
}
void lv_mem_deinit(void) {
}

void *lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) {
    if (p) heap_caps_free(p);
}

void *lv_realloc_core(void *p, size_t new_size) {
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon) {
    if (!mon) return;
    memset(mon, 0, sizeof(*mon));
    mon->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon->max_used = mon->total_size - mon->free_size;
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}

}  // extern "C"
