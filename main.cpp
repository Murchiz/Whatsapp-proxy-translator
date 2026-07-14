#include <asio.hpp>
// Fix for ASIO's OpenSSL 4.0.1 / LibreSSL 3.5.0+ opaque struct compilation bugs
#include <openssl/opensslv.h>
#ifdef LIBRESSL_VERSION_NUMBER
#undef LIBRESSL_VERSION_NUMBER
#endif
#if !defined(OPENSSL_VERSION_NUMBER) && defined(OPENSSL_VERSION_MAJOR) && OPENSSL_VERSION_MAJOR >= 3
#define OPENSSL_VERSION_NUMBER 0x3000000fL
#endif

#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/ssl/error.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#include <print>
#include <format>
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <cstdlib>
#include <array>

#ifdef _WIN32
#include <windows.h>
#endif

using asio::ip::tcp;

enum class ProxyType {
    SOCKS5,
    HTTP
};

struct Config {
    std::string upstream_host;
    std::string upstream_port;
    ProxyType type = ProxyType::SOCKS5;
    bool run_as_service = false;
};

Config g_config;

void generate_certificate() {
    std::ifstream f("proxy.whatsapp.net.pem");
    if (f.good()) return; 
    
    std::println("Certificate not found, generating proxy.whatsapp.net.pem...");
    std::string cmd = "openssl req -new -newkey rsa:2048 -days 365 -nodes -x509 -subj \"/CN=proxy.whatsapp.net\" -keyout proxy.whatsapp.net.pem -out proxy.whatsapp.net.pem";
    if (std::system(cmd.c_str()) != 0) {
        std::println(stderr, "Failed to generate certificate! Ensure openssl is in your PATH.");
    }
}

template <typename Stream>
class Bridge : public std::enable_shared_from_this<Bridge<Stream>> {
public:
    Bridge(Stream client_stream, asio::any_io_executor executor, std::string target_host, std::string target_port, bool send_proxy_proto)
        : client_stream_(std::move(client_stream)), upstream_socket_(executor), target_host_(std::move(target_host)), target_port_(std::move(target_port)), send_proxy_proto_(send_proxy_proto) {}

    void start() {
        asio::co_spawn(upstream_socket_.get_executor(),
            [self = this->shared_from_this()]() -> asio::awaitable<void> {
                co_await self->bridge_connection();
            },
            asio::detached);
    }

private:
    asio::awaitable<void> bridge_connection() {
        try {
            if constexpr (std::is_same_v<Stream, asio::ssl::stream<tcp::socket>>) {
                co_await client_stream_.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            }

            tcp::resolver resolver(upstream_socket_.get_executor());
            auto results = co_await resolver.async_resolve(g_config.upstream_host, g_config.upstream_port, asio::use_awaitable);
            
            co_await asio::async_connect(upstream_socket_, results, asio::use_awaitable);

            if (g_config.type == ProxyType::SOCKS5) {
                co_await do_socks5_handshake();
            } else {
                co_await do_http_handshake();
            }

            if (send_proxy_proto_) {
                co_await send_proxy_protocol_header();
            }

            // Start bidirectional relay utilizing C++20/23 coroutines
            asio::co_spawn(upstream_socket_.get_executor(), relay_client_to_upstream(), asio::detached);
            asio::co_spawn(upstream_socket_.get_executor(), relay_upstream_to_client(), asio::detached);

        } catch (const std::exception& e) {
            std::println(stderr, "[Bridge Disconnected] {}", e.what());
        }
    }

    asio::awaitable<void> do_socks5_handshake() {
        std::vector<uint8_t> out_buffer = {0x05, 0x01, 0x00};
        co_await asio::async_write(upstream_socket_, asio::buffer(out_buffer), asio::use_awaitable);

        std::vector<uint8_t> in_buffer(2);
        co_await asio::async_read(upstream_socket_, asio::buffer(in_buffer), asio::use_awaitable);

        if (in_buffer[1] != 0x00) {
            throw std::runtime_error(std::format("SOCKS5 Auth Rejected. Code: {}", in_buffer[1]));
        }

        out_buffer.clear();
        out_buffer.push_back(0x05);
        out_buffer.push_back(0x01);
        out_buffer.push_back(0x00);
        out_buffer.push_back(0x03); // Domain name
        out_buffer.push_back(static_cast<uint8_t>(target_host_.length()));
        for (char c : target_host_) out_buffer.push_back(c);
        
        uint16_t port = static_cast<uint16_t>(std::stoi(target_port_));
        out_buffer.push_back((port >> 8) & 0xFF);
        out_buffer.push_back(port & 0xFF);

        co_await asio::async_write(upstream_socket_, asio::buffer(out_buffer), asio::use_awaitable);

        in_buffer.resize(4);
        co_await asio::async_read(upstream_socket_, asio::buffer(in_buffer), asio::use_awaitable);

        if (in_buffer[1] != 0x00) {
            throw std::runtime_error(std::format("SOCKS5 Connect Rejected. Reply Code: {}", in_buffer[1]));
        }

        int atyp = in_buffer[3];
        if (atyp == 0x01) { // IPv4
            in_buffer.resize(6);
            co_await asio::async_read(upstream_socket_, asio::buffer(in_buffer), asio::use_awaitable);
        } else if (atyp == 0x04) { // IPv6
            in_buffer.resize(18);
            co_await asio::async_read(upstream_socket_, asio::buffer(in_buffer), asio::use_awaitable);
        } else if (atyp == 0x03) { // Domain
            in_buffer.resize(1);
            co_await asio::async_read(upstream_socket_, asio::buffer(in_buffer), asio::use_awaitable);
            int len = in_buffer[0];
            in_buffer.resize(len + 2);
            co_await asio::async_read(upstream_socket_, asio::buffer(in_buffer), asio::use_awaitable);
        } else {
            throw std::runtime_error(std::format("SOCKS5 unknown ATYP: {}", atyp));
        }
    }

    asio::awaitable<void> do_http_handshake() {
        std::string req = std::format("CONNECT {0}:{1} HTTP/1.1\r\nHost: {0}:{1}\r\n\r\n", target_host_, target_port_);
        co_await asio::async_write(upstream_socket_, asio::buffer(req), asio::use_awaitable);

        asio::streambuf http_response_buf;
        co_await asio::async_read_until(upstream_socket_, http_response_buf, "\r\n\r\n", asio::use_awaitable);
    }

    asio::awaitable<void> send_proxy_protocol_header() {
        auto remote_ep = client_stream_.lowest_layer().remote_endpoint();
        auto local_ep = client_stream_.lowest_layer().local_endpoint();
        
        std::string header = std::format("PROXY TCP{} {} {} {} {}\r\n",
            remote_ep.address().is_v4() ? "4" : "6",
            remote_ep.address().to_string(),
            local_ep.address().to_string(),
            remote_ep.port(),
            local_ep.port());

        co_await asio::async_write(upstream_socket_, asio::buffer(header), asio::use_awaitable);
    }

    asio::awaitable<void> relay_client_to_upstream() {
        auto self = this->shared_from_this();
        try {
            for (;;) {
                std::size_t n = co_await client_stream_.async_read_some(asio::buffer(client_data_), asio::use_awaitable);
                co_await asio::async_write(upstream_socket_, asio::buffer(client_data_, n), asio::use_awaitable);
            }
        } catch (...) {
            asio::error_code ignore_ec;
            upstream_socket_.close(ignore_ec);
        }
    }

    asio::awaitable<void> relay_upstream_to_client() {
        auto self = this->shared_from_this();
        try {
            for (;;) {
                std::size_t n = co_await upstream_socket_.async_read_some(asio::buffer(upstream_data_), asio::use_awaitable);
                co_await asio::async_write(client_stream_, asio::buffer(upstream_data_, n), asio::use_awaitable);
            }
        } catch (...) {
            asio::error_code ignore_ec;
            client_stream_.lowest_layer().close(ignore_ec);
        }
    }

    Stream client_stream_;
    tcp::socket upstream_socket_;
    std::string target_host_;
    std::string target_port_;
    bool send_proxy_proto_;
    
    std::array<char, 16384> client_data_;
    std::array<char, 16384> upstream_data_;
};

asio::awaitable<void> tcp_listener(short local_port, std::string target_host, std::string target_port, bool send_proxy_proto) {
    auto executor = co_await asio::this_coro::executor;
    tcp::acceptor acceptor(executor, tcp::endpoint(tcp::v4(), local_port));
    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::make_shared<Bridge<tcp::socket>>(std::move(socket), executor, target_host, target_port, send_proxy_proto)->start();
    }
}

asio::awaitable<void> ssl_listener(asio::ssl::context& ssl_context, short local_port, std::string target_host, std::string target_port, bool send_proxy_proto) {
    auto executor = co_await asio::this_coro::executor;
    tcp::acceptor acceptor(executor, tcp::endpoint(tcp::v4(), local_port));
    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
        auto stream = asio::ssl::stream<tcp::socket>(std::move(socket), ssl_context);
        std::make_shared<Bridge<asio::ssl::stream<tcp::socket>>>(std::move(stream), executor, target_host, target_port, send_proxy_proto)->start();
    }
}

void start_service() {
    generate_certificate();

    asio::io_context io_context;

    asio::ssl::context ssl_context(asio::ssl::context::tlsv12_server);
    ssl_context.set_options(
        asio::ssl::context::default_workarounds |
        asio::ssl::context::no_sslv2 |
        asio::ssl::context::single_dh_use);
    
    try {
        ssl_context.use_certificate_chain_file("proxy.whatsapp.net.pem");
        ssl_context.use_private_key_file("proxy.whatsapp.net.pem", asio::ssl::context::pem);
    } catch (std::exception& e) {
        std::println(stderr, "SSL Error: {} - Ensure proxy.whatsapp.net.pem exists!", e.what());
        return;
    }

    // Launch listeners concurrently using co_spawn
    asio::co_spawn(io_context, tcp_listener(80, "g.whatsapp.net", "80", true), asio::detached);
    asio::co_spawn(io_context, ssl_listener(ssl_context, 443, "g.whatsapp.net", "5222", true), asio::detached);
    asio::co_spawn(io_context, tcp_listener(5222, "g.whatsapp.net", "5222", true), asio::detached);
    asio::co_spawn(io_context, tcp_listener(587, "whatsapp.net", "443", false), asio::detached);
    asio::co_spawn(io_context, tcp_listener(7777, "whatsapp.net", "443", false), asio::detached);

    std::println("WhatsApp Independent Proxy Translator is now RUNNING.");
    std::println("Listening for connections on ports:");
    std::println("  80   : Standard web traffic (HTTP)");
    std::println("  443  : Standard web traffic, encrypted (HTTPS)");
    std::println("  5222 : Jabber protocol traffic (WhatsApp default)");
    std::println("  587  : WhatsApp Media traffic (HTTPS)");
    std::println("  7777 : WhatsApp Media traffic (HTTPS)");

    std::vector<std::thread> threads;
    unsigned int hardware_concurrency = std::thread::hardware_concurrency();
    if (hardware_concurrency == 0) hardware_concurrency = 4;
    
    for (unsigned int i = 0; i < hardware_concurrency; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }
    for (auto& t : threads) t.join();
}

#ifdef _WIN32
SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;

void WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
void WINAPI ServiceCtrlHandler(DWORD);

void InstallService() {
    SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!schSCManager) return;
    
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string cmd = std::string(path) + " --service";

    SC_HANDLE schService = CreateServiceA(
        schSCManager, "WhatsAppTranslator", "WhatsApp Proxy Translator",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
        cmd.c_str(), NULL, NULL, NULL, NULL, NULL);

    if (schService) CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
}

void WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    g_StatusHandle = RegisterServiceCtrlHandlerA("WhatsAppTranslator", ServiceCtrlHandler);
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    
    start_service();
    
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

void WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    if (CtrlCode == SERVICE_CONTROL_STOP) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        exit(0);
    }
}
#endif

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--proxy" && i + 1 < argc) {
            std::string p = argv[++i];
            auto colon = p.find(':');
            if (colon != std::string::npos) {
                g_config.upstream_host = p.substr(0, colon);
                g_config.upstream_port = p.substr(colon + 1);
            }
        } else if (arg == "--type" && i + 1 < argc) {
            std::string t = argv[++i];
            if (t == "http") g_config.type = ProxyType::HTTP;
            else g_config.type = ProxyType::SOCKS5;
        } else if (arg == "--service") {
            g_config.run_as_service = true;
        } else if (arg == "--install") {
#ifdef _WIN32
            InstallService();
            std::println("Service installed.");
            return 0;
#endif
        }
    }

    if (g_config.upstream_host.empty()) {
        std::println(stderr, "Usage: {} --proxy <host:port> [--type socks5|http] [--install|--service]", argv[0]);
        return 1;
    }

#ifdef _WIN32
    if (g_config.run_as_service) {
        SERVICE_TABLE_ENTRYA ServiceTable[] = {
            {(LPSTR)"WhatsAppTranslator", (LPSERVICE_MAIN_FUNCTIONA)ServiceMain},
            {NULL, NULL}
        };
        StartServiceCtrlDispatcherA(ServiceTable);
        return 0;
    }
#endif

    start_service();
    return 0;
}
