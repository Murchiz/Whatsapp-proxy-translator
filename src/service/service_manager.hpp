#pragma once

#include "config.hpp"
#include <string>
#include <optional>

namespace ServiceManager {
    bool install(const std::string& proxy, ProxyType type);
    bool change_proxy(const std::string& new_proxy, std::optional<ProxyType> new_type);
    bool uninstall();
    void print_status();
    bool control(const std::string& action);
    int run_as_service();
}
