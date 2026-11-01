#include "utils.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

std::filesystem::path get_executable_path() {
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len > 0) {
        return std::filesystem::path(path);
    }
#else
    std::error_code ec;
    auto p = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec && !p.empty()) {
        return p;
    }
#endif
    return std::filesystem::current_path() / "translator";
}

std::filesystem::path get_executable_dir() {
    return get_executable_path().parent_path();
}
