#pragma once
#include <string>
#include <filesystem>
#include <optional>

enum class ProxyType {
    SOCKS5,
    HTTP
};

struct Config {
    std::string upstream_host;
    std::string upstream_port;
    ProxyType type = ProxyType::SOCKS5;
    bool run_as_service = false;
    std::filesystem::path cert_path;
    std::filesystem::path log_path;
};

extern Config g_config;

bool parse_proxy_string(const std::string& str, std::string& host, std::string& port);
