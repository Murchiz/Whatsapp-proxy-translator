#pragma once
#include <string>

#if __has_include(<print>)
#include <print>
#else
#include <iostream>
#include <format>
namespace std {
    template <typename... Args>
    void print(std::format_string<Args...> fmt, Args&&... args) {
        std::cout << std::format(fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void println(std::format_string<Args...> fmt, Args&&... args) {
        std::cout << std::format(fmt, std::forward<Args>(args)...) << '\n';
    }
    template <typename... Args>
    void println(std::FILE* stream, std::format_string<Args...> fmt, Args&&... args) {
        std::string s = std::format(fmt, std::forward<Args>(args)...) + '\n';
        std::fputs(s.c_str(), stream);
    }
}
#endif

void log_message(const std::string& msg, bool is_error = false);
