# #106 spike (env esp32-4848s040-idf5-bb), on top of sdkconfig.defaults: code and
# rodata execute from PSRAM, so a flash write/erase no longer disables the cache the
# RGB bounce-buffer refill reads the PSRAM framebuffer through.
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
# Bounce buffers take ~19 KB of internal SRAM; WiFi then ran out of it (7 KB free,
# no association). With XIP from PSRAM, PSRAM stays usable during flash writes, so
# WiFi/LWIP buffers and mbedTLS may live there.
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
