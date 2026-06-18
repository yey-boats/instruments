#pragma once
// Pure (no-Arduino) selector for the effective OTA password: a runtime value
// stored in NVS wins over the compile-time default; empty/null runtime falls
// back to the compiled default. Host-tested under [env:native].
inline const char *ota_pass_effective(const char *nvs, const char *compiled) {
    if (nvs && nvs[0]) return nvs;
    return compiled ? compiled : "";
}
