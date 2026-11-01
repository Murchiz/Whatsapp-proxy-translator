#include "service_manager.hpp"

#ifndef _WIN32
#include "config.hpp"
#include "logger.hpp"
#include "cert.hpp"
#include "utils.hpp"
#include "server.hpp"

#include <unistd.h>
#include <sys/types.h>
#include <fstream>
#include <format>
#include <filesystem>
#include <cstdlib>

namespace ServiceManager {

int run_as_service() {
    run_server();
    return 0;
}

bool install(const std::string& proxy, ProxyType type) {
    if (geteuid() != 0) {
        std::println(stderr, "Error: Root privileges required to install systemd service. Please run with sudo.");
        return false;
    }

    std::filesystem::path exe_path = get_executable_path();
    std::filesystem::path exe_dir = exe_path.parent_path();
    generate_certificate(exe_dir / "proxy.whatsapp.net.pem");

    std::string type_str = (type == ProxyType::HTTP) ? "http" : "socks5";
    const std::filesystem::path service_file = "/etc/systemd/system/whatsapp-translator.service";

    std::ofstream out(service_file);
    if (!out.is_open()) {
        std::println(stderr, "Error: Failed to write to '{}'.", service_file.string());
        return false;
    }

    out << "[Unit]\n"
        << "Description=WhatsApp Proxy SOCKS5/HTTP Standalone Translator\n"
        << "After=network.target network-online.target\n"
        << "Wants=network-online.target\n\n"
        << "[Service]\n"
        << "Type=simple\n"
        << "WorkingDirectory=" << exe_dir.string() << "\n"
        << "ExecStart=" << exe_path.string() << " --service --proxy " << proxy << " --type " << type_str << "\n"
        << "Restart=always\n"
        << "RestartSec=3\n"
        << "LimitNOFILE=65535\n"
        << "AmbientCapabilities=CAP_NET_BIND_SERVICE\n\n"
        << "[Install]\n"
        << "WantedBy=multi-user.target\n";
    out.close();

    std::println("Created systemd unit file at '{}'", service_file.string());
    std::system("systemctl daemon-reload");
    std::system("systemctl enable whatsapp-translator.service");
    std::println("Starting whatsapp-translator.service...");
    int r = std::system("systemctl restart whatsapp-translator.service");

    if (r == 0 && std::system("systemctl is-active --quiet whatsapp-translator.service") == 0) {
        std::println("WhatsApp Proxy Translator systemd service installed and started successfully!");
        std::println("  Service Name: whatsapp-translator.service");
        std::println("  Binary Path:  {}", exe_path.string());
        std::println("  Upstream:     {} ({})", proxy, type_str);
        std::println("  Status:       active (running)");
    } else {
        std::println(stderr, "Service installed, but check status: 'journalctl -u whatsapp-translator.service -e'");
    }

    return true;
}

bool change_proxy(const std::string& new_proxy, std::optional<ProxyType> new_type) {
    if (geteuid() != 0) {
        std::println(stderr, "Error: Root privileges required. Please run with sudo.");
        return false;
    }

    const std::filesystem::path service_file = "/etc/systemd/system/whatsapp-translator.service";
    if (!std::filesystem::exists(service_file)) {
        std::println(stderr, "Error: Service 'whatsapp-translator' is not installed. Use --install --proxy <host:port> first.");
        return false;
    }

    std::ifstream in(service_file);
    if (!in.is_open()) {
        std::println(stderr, "Error: Could not open service file '{}'.", service_file.string());
        return false;
    }

    std::string line;
    std::string exe_path;
    std::string working_dir;
    ProxyType current_type = ProxyType::SOCKS5;

    while (std::getline(in, line)) {
        if (line.starts_with("WorkingDirectory=")) {
            working_dir = line.substr(17);
        } else if (line.starts_with("ExecStart=")) {
            std::string exec = line.substr(10);
            if (exec.find("--type http") != std::string::npos) {
                current_type = ProxyType::HTTP;
            }
            size_t s = exec.find(" --service");
            if (s != std::string::npos) {
                exe_path = exec.substr(0, s);
            } else {
                size_t sp = exec.find(' ');
                exe_path = (sp != std::string::npos) ? exec.substr(0, sp) : exec;
            }
        }
    }
    in.close();

    if (exe_path.empty()) {
        exe_path = get_executable_path().string();
    }
    if (working_dir.empty()) {
        working_dir = get_executable_dir().string();
    }

    ProxyType final_type = new_type.value_or(current_type);
    std::string type_str = (final_type == ProxyType::HTTP) ? "http" : "socks5";

    std::ofstream out(service_file);
    if (!out.is_open()) {
        std::println(stderr, "Error: Failed to write to '{}'.", service_file.string());
        return false;
    }

    out << "[Unit]\n"
        << "Description=WhatsApp Proxy SOCKS5/HTTP Standalone Translator\n"
        << "After=network.target network-online.target\n"
        << "Wants=network-online.target\n\n"
        << "[Service]\n"
        << "Type=simple\n"
        << "WorkingDirectory=" << working_dir << "\n"
        << "ExecStart=" << exe_path << " --service --proxy " << new_proxy << " --type " << type_str << "\n"
        << "Restart=always\n"
        << "RestartSec=3\n"
        << "LimitNOFILE=65535\n"
        << "AmbientCapabilities=CAP_NET_BIND_SERVICE\n\n"
        << "[Install]\n"
        << "WantedBy=multi-user.target\n";
    out.close();

    std::println("Updated systemd unit file with new proxy: {} ({})", new_proxy, type_str);
    std::system("systemctl daemon-reload");

    int active = std::system("systemctl is-active --quiet whatsapp-translator.service");
    if (active == 0) {
        std::println("Restarting whatsapp-translator.service...");
        int r = std::system("systemctl restart whatsapp-translator.service");
        if (r == 0) {
            std::println("Service restarted successfully with new proxy.");
        } else {
            std::println(stderr, "Warning: Failed to restart service. Check 'journalctl -u whatsapp-translator.service -e'.");
        }
    } else {
        std::println("Service proxy updated. Note: Service is currently not active. Start it with 'systemctl start whatsapp-translator' or '--start'.");
    }

    return true;
}

bool uninstall() {
    if (geteuid() != 0) {
        std::println(stderr, "Error: Root privileges required. Please run with sudo.");
        return false;
    }

    const std::filesystem::path service_file = "/etc/systemd/system/whatsapp-translator.service";
    if (!std::filesystem::exists(service_file)) {
        std::println("Service 'whatsapp-translator' is not installed.");
        return true;
    }

    std::println("Stopping and disabling whatsapp-translator.service...");
    std::system("systemctl stop whatsapp-translator.service 2>/dev/null");
    std::system("systemctl disable whatsapp-translator.service 2>/dev/null");

    std::error_code ec;
    std::filesystem::remove(service_file, ec);
    std::system("systemctl daemon-reload");

    std::println("WhatsApp Proxy Translator systemd service uninstalled successfully.");
    return true;
}

void print_status() {
    const std::filesystem::path service_file = "/etc/systemd/system/whatsapp-translator.service";
    if (!std::filesystem::exists(service_file)) {
        std::println("Service 'whatsapp-translator' is NOT installed.");
        return;
    }

    std::println("Service: whatsapp-translator.service (installed)");
    std::println("Unit file: {}", service_file.string());

    std::ifstream in(service_file);
    std::string line;
    std::string exec_line;
    std::string work_dir;
    while (std::getline(in, line)) {
        if (line.starts_with("ExecStart=")) exec_line = line.substr(10);
        else if (line.starts_with("WorkingDirectory=")) work_dir = line.substr(17);
    }
    in.close();

    if (!work_dir.empty()) std::println("Working Directory: {}", work_dir);
    if (!exec_line.empty()) std::println("ExecStart: {}", exec_line);

    std::print("Service State: ");
    std::fflush(stdout);
    int active = std::system("systemctl is-active whatsapp-translator.service");
    (void)active;
}

bool control(const std::string& action) {
    if (geteuid() != 0) {
        std::println(stderr, "Error: Root privileges required. Please run with sudo.");
        return false;
    }
    std::string cmd = std::format("systemctl {} whatsapp-translator.service", action);
    int r = std::system(cmd.c_str());
    if (r == 0) {
        std::println("Service {} action succeeded.", action);
        return true;
    } else {
        std::println(stderr, "Service {} action failed with code {}.", action, r);
        return false;
    }
}

} // namespace ServiceManager
#endif
