#include "service_manager.hpp"

#ifdef _WIN32
#include "config.hpp"
#include "logger.hpp"
#include "cert.hpp"
#include "utils.hpp"
#include "server.hpp"

#include <windows.h>
#include <winsvc.h>
#include <vector>
#include <format>
#include <filesystem>

static SERVICE_STATUS g_ServiceStatus = {0};
static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;

static void WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    if (CtrlCode == SERVICE_CONTROL_STOP || CtrlCode == SERVICE_CONTROL_SHUTDOWN) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        stop_server();
    }
}

static void WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    g_StatusHandle = RegisterServiceCtrlHandlerA("WhatsAppTranslator", ServiceCtrlHandler);
    if (!g_StatusHandle) return;

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    std::filesystem::current_path(get_executable_dir());

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    
    run_server();
    
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

namespace ServiceManager {

int run_as_service() {
    SERVICE_TABLE_ENTRYA ServiceTable[] = {
        {(LPSTR)"WhatsAppTranslator", (LPSERVICE_MAIN_FUNCTIONA)ServiceMain},
        {NULL, NULL}
    };
    return StartServiceCtrlDispatcherA(ServiceTable) ? 0 : 1;
}

bool install(const std::string& proxy, ProxyType type) {
    SC_HANDLE schSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            std::println(stderr, "Error: Access denied. Please run this command as Administrator.");
        } else {
            std::println(stderr, "Error: OpenSCManager failed with error code {}.", err);
        }
        return false;
    }
    
    std::filesystem::path exe_path = get_executable_path();
    std::filesystem::path exe_dir = exe_path.parent_path();
    generate_certificate(exe_dir / "proxy.whatsapp.net.pem");

    std::string type_str = (type == ProxyType::HTTP) ? "http" : "socks5";
    std::string cmd = std::format("\"{}\" --service --proxy {} --type {}", exe_path.string(), proxy, type_str);

    SC_HANDLE schService = OpenServiceA(schSCManager, "WhatsAppTranslator", SERVICE_ALL_ACCESS);
    if (schService) {
        std::println("Service 'WhatsAppTranslator' already exists. Updating configuration...");
        if (!ChangeServiceConfigA(
                schService, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                cmd.c_str(), NULL, NULL, NULL, NULL, NULL, NULL)) {
            std::println(stderr, "Error: Failed to update service config (Error {}).", GetLastError());
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
            return false;
        }
    } else {
        schService = CreateServiceA(
            schSCManager, "WhatsAppTranslator", "WhatsApp Proxy Translator",
            SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            cmd.c_str(), NULL, NULL, NULL, NULL, NULL);

        if (!schService) {
            std::println(stderr, "Error: Failed to create service (Error {}).", GetLastError());
            CloseServiceHandle(schSCManager);
            return false;
        }
    }

    SERVICE_DESCRIPTIONA desc;
    char desc_str[] = "WhatsApp Proxy SOCKS5/HTTP Standalone Translator Bridge";
    desc.lpDescription = desc_str;
    ChangeServiceConfig2A(schService, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Start or restart service
    SERVICE_STATUS_PROCESS ssp;
    DWORD bytesNeeded;
    if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            std::println("Restarting service to apply configuration...");
            SERVICE_STATUS status;
            ControlService(schService, SERVICE_CONTROL_STOP, &status);
            for (int i = 0; i < 50; ++i) {
                Sleep(200);
                QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded);
                if (ssp.dwCurrentState == SERVICE_STOPPED) break;
            }
            StartServiceA(schService, 0, NULL);
        } else if (ssp.dwCurrentState == SERVICE_STOPPED) {
            StartServiceA(schService, 0, NULL);
        }
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    std::println("WhatsApp Proxy Translator Windows Service installed successfully.");
    std::println("  Service Name: WhatsAppTranslator");
    std::println("  Binary Path:  {}", cmd);
    std::println("  Upstream:     {} ({})", proxy, type_str);
    return true;
}

bool change_proxy(const std::string& new_proxy, std::optional<ProxyType> new_type) {
    SC_HANDLE schSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            std::println(stderr, "Error: Access denied. Please run this command as Administrator.");
        } else {
            std::println(stderr, "Error: OpenSCManager failed with error {}.", GetLastError());
        }
        return false;
    }

    SC_HANDLE schService = OpenServiceA(schSCManager, "WhatsAppTranslator", SERVICE_ALL_ACCESS);
    if (!schService) {
        std::println(stderr, "Error: Service 'WhatsAppTranslator' is not installed. Use --install --proxy <host:port> first.");
        CloseServiceHandle(schSCManager);
        return false;
    }

    DWORD bytesNeeded = 0;
    QueryServiceConfigA(schService, NULL, 0, &bytesNeeded);
    ProxyType final_type = ProxyType::SOCKS5;
    std::string exe_path;

    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytesNeeded > 0) {
        std::vector<BYTE> buffer(bytesNeeded);
        LPQUERY_SERVICE_CONFIGA pConfig = (LPQUERY_SERVICE_CONFIGA)buffer.data();
        if (QueryServiceConfigA(schService, pConfig, bytesNeeded, &bytesNeeded)) {
            std::string existing_cmd = pConfig->lpBinaryPathName ? pConfig->lpBinaryPathName : "";
            if (existing_cmd.find("--type http") != std::string::npos) {
                final_type = ProxyType::HTTP;
            }
            if (!existing_cmd.empty()) {
                if (existing_cmd[0] == '"') {
                    size_t close_quote = existing_cmd.find('"', 1);
                    if (close_quote != std::string::npos) {
                        exe_path = existing_cmd.substr(1, close_quote - 1);
                    }
                }
                if (exe_path.empty()) {
                    size_t first_space = existing_cmd.find(' ');
                    exe_path = (first_space != std::string::npos) ? existing_cmd.substr(0, first_space) : existing_cmd;
                }
            }
        }
    }

    if (exe_path.empty()) {
        exe_path = get_executable_path().string();
    }
    if (new_type.has_value()) {
        final_type = new_type.value();
    }

    std::string type_str = (final_type == ProxyType::HTTP) ? "http" : "socks5";
    std::string new_cmd = std::format("\"{}\" --service --proxy {} --type {}", exe_path, new_proxy, type_str);

    if (!ChangeServiceConfigA(
            schService, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
            new_cmd.c_str(), NULL, NULL, NULL, NULL, NULL, NULL)) {
        std::println(stderr, "Error updating service configuration: {}", GetLastError());
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp;
    if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            std::println("Restarting service to apply new proxy settings...");
            SERVICE_STATUS status;
            ControlService(schService, SERVICE_CONTROL_STOP, &status);
            for (int i = 0; i < 50; ++i) {
                Sleep(200);
                QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded);
                if (ssp.dwCurrentState == SERVICE_STOPPED) break;
            }
            if (StartServiceA(schService, 0, NULL)) {
                std::println("Service proxy successfully updated to {} ({}) and restarted.", new_proxy, type_str);
            } else {
                std::println(stderr, "Service proxy updated, but failed to restart automatically (Error: {}).", GetLastError());
            }
        } else {
            std::println("Service proxy successfully updated to {} ({}). Service is currently stopped.", new_proxy, type_str);
        }
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return true;
}

bool uninstall() {
    SC_HANDLE schSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            std::println(stderr, "Error: Access denied. Please run this command as Administrator.");
        } else {
            std::println(stderr, "Error: OpenSCManager failed with error {}.", GetLastError());
        }
        return false;
    }

    SC_HANDLE schService = OpenServiceA(schSCManager, "WhatsAppTranslator", SERVICE_ALL_ACCESS);
    if (!schService) {
        std::println(stderr, "Service 'WhatsAppTranslator' is not installed.");
        CloseServiceHandle(schSCManager);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp;
    DWORD bytesNeeded;
    if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            std::println("Stopping service before removal...");
            SERVICE_STATUS status;
            ControlService(schService, SERVICE_CONTROL_STOP, &status);
            for (int i = 0; i < 50; ++i) {
                Sleep(200);
                QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded);
                if (ssp.dwCurrentState == SERVICE_STOPPED) break;
            }
        }
    }

    if (!DeleteService(schService)) {
        std::println(stderr, "Failed to delete service (Error: {}).", GetLastError());
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return false;
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    std::println("Service 'WhatsAppTranslator' uninstalled successfully.");
    return true;
}

void print_status() {
    SC_HANDLE schSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!schSCManager) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            std::println(stderr, "Error accessing Service Manager: Access denied.");
        } else {
            std::println(stderr, "Error accessing Service Manager: {}", err);
        }
        return;
    }

    SC_HANDLE schService = OpenServiceA(schSCManager, "WhatsAppTranslator", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    if (!schService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::println("Service 'WhatsAppTranslator' is NOT installed.");
        } else {
            std::println(stderr, "Service 'WhatsAppTranslator' could not be opened (Error: {}).", err);
        }
        CloseServiceHandle(schSCManager);
        return;
    }

    std::println("Service: WhatsAppTranslator (installed)");

    DWORD bytesNeeded = 0;
    QueryServiceConfigA(schService, NULL, 0, &bytesNeeded);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytesNeeded > 0) {
        std::vector<BYTE> buffer(bytesNeeded);
        LPQUERY_SERVICE_CONFIGA pConfig = (LPQUERY_SERVICE_CONFIGA)buffer.data();
        if (QueryServiceConfigA(schService, pConfig, bytesNeeded, &bytesNeeded)) {
            std::println("Display Name: {}", pConfig->lpDisplayName ? pConfig->lpDisplayName : "");
            std::println("Binary Path:  {}", pConfig->lpBinaryPathName ? pConfig->lpBinaryPathName : "");
            std::string start_type = "Unknown";
            if (pConfig->dwStartType == SERVICE_AUTO_START) start_type = "Automatic";
            else if (pConfig->dwStartType == SERVICE_DEMAND_START) start_type = "Manual";
            else if (pConfig->dwStartType == SERVICE_DISABLED) start_type = "Disabled";
            std::println("Startup Type: {}", start_type);
        }
    }

    SERVICE_STATUS_PROCESS ssp;
    if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
        std::string state_str = "UNKNOWN";
        switch (ssp.dwCurrentState) {
            case SERVICE_STOPPED: state_str = "STOPPED"; break;
            case SERVICE_START_PENDING: state_str = "START_PENDING"; break;
            case SERVICE_STOP_PENDING: state_str = "STOP_PENDING"; break;
            case SERVICE_RUNNING: state_str = "RUNNING"; break;
            case SERVICE_CONTINUE_PENDING: state_str = "CONTINUE_PENDING"; break;
            case SERVICE_PAUSE_PENDING: state_str = "PAUSE_PENDING"; break;
            case SERVICE_PAUSED: state_str = "PAUSED"; break;
        }
        std::println("State:        {}", state_str);
        if (ssp.dwProcessId != 0) {
            std::println("PID:          {}", ssp.dwProcessId);
        }
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
}

bool control(const std::string& action) {
    SC_HANDLE schSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            std::println(stderr, "Error: Access denied. Please run this command as Administrator.");
        } else {
            std::println(stderr, "Error: OpenSCManager failed (Error {}).", GetLastError());
        }
        return false;
    }

    SC_HANDLE schService = OpenServiceA(schSCManager, "WhatsAppTranslator", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!schService) {
        std::println(stderr, "Error: Service 'WhatsAppTranslator' is not installed.");
        CloseServiceHandle(schSCManager);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp;
    DWORD bytesNeeded;
    bool success = true;

    if (action == "start") {
        if (!StartServiceA(schService, 0, NULL)) {
            std::println(stderr, "StartService failed: {}", GetLastError());
            success = false;
        } else {
            std::print("Starting service");
            for (int i = 0; i < 50; ++i) {
                Sleep(200);
                std::print(".");
                if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
                    if (ssp.dwCurrentState == SERVICE_RUNNING) {
                        std::println(" started successfully.");
                        break;
                    }
                }
            }
        }
    } else if (action == "stop") {
        SERVICE_STATUS status;
        if (!ControlService(schService, SERVICE_CONTROL_STOP, &status)) {
            std::println(stderr, "StopService failed: {}", GetLastError());
            success = false;
        } else {
            std::print("Stopping service");
            for (int i = 0; i < 50; ++i) {
                Sleep(200);
                std::print(".");
                if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
                    if (ssp.dwCurrentState == SERVICE_STOPPED) {
                        std::println(" stopped successfully.");
                        break;
                    }
                }
            }
        }
    } else if (action == "restart") {
        SERVICE_STATUS status;
        ControlService(schService, SERVICE_CONTROL_STOP, &status);
        for (int i = 0; i < 50; ++i) {
            Sleep(200);
            if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
                if (ssp.dwCurrentState == SERVICE_STOPPED) break;
            }
        }
        if (!StartServiceA(schService, 0, NULL)) {
            std::println(stderr, "StartService failed during restart: {}", GetLastError());
            success = false;
        } else {
            std::println("Service restarted successfully.");
        }
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return success;
}

} // namespace ServiceManager
#endif
