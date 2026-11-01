#include "cert.hpp"
#include "logger.hpp"
#include <format>
#include <cstdlib>

void generate_certificate(const std::filesystem::path& cert_path) {
    if (std::filesystem::exists(cert_path)) return; 
    
    log_message(std::format("Certificate not found at '{}', generating...", cert_path.string()));
#ifdef _WIN32
    std::string cmd = std::format("openssl req -new -newkey rsa:2048 -days 365 -nodes -x509 -subj \"/CN=proxy.whatsapp.net\" -keyout \"{}\" -out \"{}\"",
                                  cert_path.string(), cert_path.string());
#else
    std::string cmd = std::format("openssl req -new -newkey rsa:2048 -days 365 -nodes -x509 -subj \"/CN=proxy.whatsapp.net\" -keyout \"{}\" -out \"{}\" >/dev/null 2>&1",
                                  cert_path.string(), cert_path.string());
#endif
    if (std::system(cmd.c_str()) != 0) {
        log_message("Failed to generate certificate! Ensure openssl is installed and in PATH.", true);
    } else {
        log_message(std::format("Certificate successfully generated at '{}'.", cert_path.string()));
    }
}
