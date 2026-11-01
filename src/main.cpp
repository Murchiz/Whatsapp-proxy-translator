#include "config.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include "server.hpp"
#include "service/service_manager.hpp"

#include <asio/version.hpp>
#include <string>
#include <optional>

#ifndef TRANSLATOR_VERSION
#define TRANSLATOR_VERSION "v1.0.0"
#endif

void print_version() {
    int asio_major = ASIO_VERSION / 100000;
    int asio_minor = (ASIO_VERSION / 100) % 1000;
    int asio_patch = ASIO_VERSION % 100;
    std::println("WhatsApp Proxy Translator {} (Asio {}.{}.{})",
                 TRANSLATOR_VERSION, asio_major, asio_minor, asio_patch);
}

void print_usage(const char* prog) {
    std::println(
R"(WhatsApp Proxy SOCKS5/HTTP Standalone Translator

Usage:
  {0} [options]

Proxy Options:
  --proxy <host:port>     Upstream SOCKS5 or HTTP proxy address (e.g. 127.0.0.1:1080)
  --type <socks5|http>    Proxy type: socks5 (default) or http
  --cert <path>           Path to SSL certificate (default: proxy.whatsapp.net.pem)

Service Management (Windows Service / Linux systemd):
  --install               Install translator as a background system service
                          Requires --proxy <host:port> [--type socks5|http]
                          Example: {0} --install --proxy 127.0.0.1:1080 --type socks5

  --change-proxy <addr>   Change the upstream proxy for the installed service
                          Aliases: --set-proxy, --update-proxy
                          Optionally specify --type <socks5|http>
                          Automatically reloads/restarts the running service.
                          Example: {0} --change-proxy 192.168.1.50:9050

  --uninstall             Uninstall and remove the background service (alias: --remove)
  --status                Check service installation, state, and configured proxy
  --start                 Start the installed background service
  --stop                  Stop the installed background service
  --restart               Restart the installed background service

Internal:
  --service               Run in background service mode (invoked by service managers)

Help & Information:
  --help, -h              Show this help message
  --version, -v           Show version information

Examples:
  # Console mode:
  {0} --proxy 127.0.0.1:1080 --type socks5

  # Install background service:
  {0} --install --proxy 127.0.0.1:1080 --type socks5

  # Change service proxy later:
  {0} --change-proxy 127.0.0.1:9050

  # Check service status:
  {0} --status)", prog);
}

int main(int argc, char* argv[]) {
    auto exe_dir = get_executable_dir();
    g_config.cert_path = exe_dir / "proxy.whatsapp.net.pem";
    g_config.log_path = exe_dir / "service.log";

    std::string proxy_arg;
    std::optional<ProxyType> type_arg;
    std::string change_proxy_arg;
    bool install_action = false;
    bool change_proxy_action = false;
    bool uninstall_action = false;
    bool status_action = false;
    std::string control_action;
    bool help_action = false;
    bool version_action = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--proxy" && i + 1 < argc) {
            proxy_arg = argv[++i];
        } else if (arg == "--type" && i + 1 < argc) {
            std::string t = argv[++i];
            if (t == "http") type_arg = ProxyType::HTTP;
            else type_arg = ProxyType::SOCKS5;
        } else if (arg == "--cert" && i + 1 < argc) {
            g_config.cert_path = argv[++i];
        } else if (arg == "--service") {
            g_config.run_as_service = true;
        } else if (arg == "--install") {
            install_action = true;
        } else if (arg == "--change-proxy" || arg == "--set-proxy" || arg == "--update-proxy") {
            change_proxy_action = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                change_proxy_arg = argv[++i];
            }
        } else if (arg == "--uninstall" || arg == "--remove") {
            uninstall_action = true;
        } else if (arg == "--status") {
            status_action = true;
        } else if (arg == "--start" || arg == "--stop" || arg == "--restart") {
            control_action = arg.substr(2);
        } else if (arg == "--version" || arg == "-v") {
            version_action = true;
        } else if (arg == "--help" || arg == "-h") {
            help_action = true;
        } else {
            std::println(stderr, "Unknown option: {}", arg);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (version_action) {
        print_version();
        return 0;
    }

    if (help_action) {
        print_usage(argv[0]);
        return 0;
    }

    if (status_action) {
        ServiceManager::print_status();
        return 0;
    }

    if (uninstall_action) {
        return ServiceManager::uninstall() ? 0 : 1;
    }

    if (!control_action.empty()) {
        return ServiceManager::control(control_action) ? 0 : 1;
    }

    if (change_proxy_action) {
        std::string target_proxy = !change_proxy_arg.empty() ? change_proxy_arg : proxy_arg;
        if (target_proxy.empty()) {
            std::println(stderr, "Error: --change-proxy requires a proxy address.\nExample: {} --change-proxy 127.0.0.1:1080 [--type socks5|http]", argv[0]);
            return 1;
        }
        std::string h, p;
        if (!parse_proxy_string(target_proxy, h, p)) {
            std::println(stderr, "Error: Invalid proxy address '{}'. Expected <host:port>.", target_proxy);
            return 1;
        }
        return ServiceManager::change_proxy(target_proxy, type_arg) ? 0 : 1;
    }

    if (install_action) {
        if (proxy_arg.empty()) {
            std::println(stderr, "Error: --proxy <host:port> is required to install the service.\nExample: {} --install --proxy 127.0.0.1:1080 [--type socks5|http]", argv[0]);
            return 1;
        }
        std::string h, p;
        if (!parse_proxy_string(proxy_arg, h, p)) {
            std::println(stderr, "Error: Invalid proxy address '{}'. Expected <host:port>.", proxy_arg);
            return 1;
        }
        ProxyType pt = type_arg.value_or(ProxyType::SOCKS5);
        return ServiceManager::install(proxy_arg, pt) ? 0 : 1;
    }

    if (proxy_arg.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    if (!parse_proxy_string(proxy_arg, g_config.upstream_host, g_config.upstream_port)) {
        std::println(stderr, "Error: Invalid proxy address '{}'. Expected <host:port>.", proxy_arg);
        return 1;
    }
    g_config.type = type_arg.value_or(ProxyType::SOCKS5);

    if (g_config.run_as_service) {
        return ServiceManager::run_as_service();
    }

    run_server();
    return 0;
}
