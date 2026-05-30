#include "manager_url.h"

namespace manager_url {

namespace {

constexpr const char *PLUGIN_SUFFIX = "/plugins/espdisp-manager";

std::string strip_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

}  // namespace

bool endpoint_has_path(const std::string &endpoint) {
    auto scheme = endpoint.find("://");
    size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    return endpoint.find('/', start) != std::string::npos;
}

std::string plugin_base_from_root(const std::string &endpoint) {
    std::string base = strip_trailing_slash(endpoint);
    if (base.find(PLUGIN_SUFFIX) != std::string::npos) return base;
    return base + PLUGIN_SUFFIX;
}

std::string join_url(const std::string &base, const char *path) {
    std::string out = strip_trailing_slash(base);
    if (path) out += path;
    return out;
}

}  // namespace manager_url
