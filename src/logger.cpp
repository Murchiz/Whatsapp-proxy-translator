#include "logger.hpp"
#include "config.hpp"
#include <fstream>
#include <chrono>
#include <format>

void log_message(const std::string& msg, bool is_error) {
    if (is_error) {
        std::println(stderr, "{}", msg);
    } else {
        std::println("{}", msg);
    }

    if (g_config.run_as_service && !g_config.log_path.empty()) {
        std::ofstream log_file(g_config.log_path, std::ios::app);
        if (log_file.good()) {
            auto now = std::chrono::system_clock::now();
            log_file << std::format("[{:%Y-%m-%d %H:%M:%S}] {}\n",
                                   std::chrono::floor<std::chrono::seconds>(now), msg);
        }
    }
}
