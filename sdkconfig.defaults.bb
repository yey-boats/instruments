# #106 spike (env esp32-4848s040-idf5-bb), on top of sdkconfig.defaults: code and
# rodata execute from PSRAM, so a flash write/erase no longer disables the cache the
# RGB bounce-buffer refill reads the PSRAM framebuffer through.
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
