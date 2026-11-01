#include "config.hpp"
#include <string>

Config g_config;

bool parse_proxy_string(const std::string& str, std::string& host, std::string& port) {
    if (str.empty()) return false;
    auto colon = str.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == str.length() - 1) {
        return false;
    }
    host = str.substr(0, colon);
    port = str.substr(colon + 1);
    if (host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.length() - 2);
    }
    try {
        int p = std::stoi(port);
        if (p <= 0 || p > 65535) return false;
    } catch (...) {
        return false;
    }
    return !host.empty();
}
