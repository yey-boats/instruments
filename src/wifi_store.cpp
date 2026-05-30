#include "wifi_store.h"

#include <ArduinoJson.h>
#include <string.h>

#include "net.h"
#include "storage.h"

namespace wifi_store {

static Network s_nets[MAX_NETWORKS];
static size_t s_count = 0;

static void save_to_nvs() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < s_count; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["s"] = s_nets[i].ssid;
        o["p"] = s_nets[i].pass;
    }
    String out;
    serializeJson(doc, out);
    storage::Namespace p("wifi", false);
    p.put_string("nets", out.c_str());
}

static void load_from_nvs() {
    s_count = 0;
    storage::Namespace p("wifi", true);
    std::string raw = p.get_string("nets", "");
    if (raw.empty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, raw.c_str(), raw.size())) return;
    for (JsonObject o : doc.as<JsonArray>()) {
        if (s_count >= MAX_NETWORKS) break;
        const char *s = o["s"];
        const char *pw = o["p"] | "";
        if (!s || !*s) continue;
        Network &n = s_nets[s_count++];
        memset(&n, 0, sizeof(n));
        strncpy(n.ssid, s, SSID_MAX);
        strncpy(n.pass, pw, PASS_MAX);
        n.used = true;
    }
}

void migrate_legacy_if_any() {
    // Legacy: net::wifiStart used Preferences("net") with keys "ssid"/"pass"
    storage::Namespace p("net", true);
    String ssid = String(p.get_string("ssid", "").c_str());
    String pass = String(p.get_string("pass", "").c_str());
    if (ssid.length() == 0) return;
    // Only migrate if we don't already have this one in the list.
    for (size_t i = 0; i < s_count; ++i) {
        if (ssid.equals(s_nets[i].ssid)) return;
    }
    put(ssid.c_str(), pass.c_str());
    // Don't clear the legacy keys yet - net.cpp may still read them on
    // boot to find the most-recent for status reporting. They become
    // dead weight after the first multi-network boot.
}

void load() {
    load_from_nvs();
}

size_t count() {
    return s_count;
}

const Network &at(size_t index) {
    return s_nets[index];
}

size_t put(const char *ssid, const char *pass) {
    if (!ssid || !*ssid) return s_count;
    // If already present, remove first so we can move it to the front.
    remove(ssid);
    // Make room at the front: shift everything down by one.
    if (s_count >= MAX_NETWORKS) s_count = MAX_NETWORKS - 1;
    for (size_t i = s_count; i > 0; --i)
        s_nets[i] = s_nets[i - 1];
    Network &n = s_nets[0];
    memset(&n, 0, sizeof(n));
    strncpy(n.ssid, ssid, SSID_MAX);
    strncpy(n.pass, pass ? pass : "", PASS_MAX);
    n.used = true;
    s_count++;
    save_to_nvs();
    return s_count;
}

bool remove(const char *ssid) {
    if (!ssid) return false;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) {
            for (size_t j = i; j + 1 < s_count; ++j)
                s_nets[j] = s_nets[j + 1];
            s_count--;
            save_to_nvs();
            return true;
        }
    }
    return false;
}

void clear_all() {
    s_count = 0;
    save_to_nvs();
}

String to_json(bool with_passwords) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < s_count; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = s_nets[i].ssid;
        if (with_passwords) o["password"] = s_nets[i].pass;
        o["has_password"] = s_nets[i].pass[0] != 0;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

}  // namespace wifi_store
