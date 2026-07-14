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
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <cstdlib>
#include <sstream>

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

// Ensure certificate exists
void generate_certificate() {
    std::ifstream f("proxy.whatsapp.net.pem");
    if (f.good()) return; // File exists
    
    std::cout << "Certificate not found, generating proxy.whatsapp.net.pem..." << std::endl;
    // Execute OpenSSL
    std::string cmd = "openssl req -new -newkey rsa:2048 -days 365 -nodes -x509 -subj \"/CN=proxy.whatsapp.net\" -keyout proxy.whatsapp.net.pem -out proxy.whatsapp.net.pem";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "Failed to generate certificate! Ensure openssl is in your PATH." << std::endl;
    }
}

// Templated bridge to support both raw TCP and SSL streams
template <typename Stream>
class Bridge : public std::enable_shared_from_this<Bridge<Stream>> {
public:
    Bridge(Stream client_stream, asio::any_io_executor executor, const std::string& target_host, const std::string& target_port, bool send_proxy_proto)
        : client_stream_(std::move(client_stream)), upstream_socket_(executor), target_host_(target_host), target_port_(target_port), send_proxy_proto_(send_proxy_proto) {}

    void start() {
        do_client_handshake();
    }

private:
    void do_client_handshake() {
        if constexpr (std::is_same_v<Stream, asio::ssl::stream<tcp::socket>>) {
            client_stream_.async_handshake(asio::ssl::stream_base::server,
                [this, self = this->shared_from_this()](const asio::error_code& ec) {
                    if (!ec) {
                        connect_upstream();
                    } else {
                        std::cerr << "[Port 443] SSL Handshake failed: " << ec.message() << " (This usually means the client sent non-TLS data to the SSL port)" << std::endl;
                    }
                });
        } else {
            connect_upstream();
        }
    }

    void connect_upstream() {
        auto self(this->shared_from_this());
        auto resolver = std::make_shared<tcp::resolver>(upstream_socket_.get_executor());
        resolver->async_resolve(g_config.upstream_host, g_config.upstream_port,
            [this, self, resolver](const asio::error_code& ec, tcp::resolver::results_type results) {
                if (!ec) {
                    asio::async_connect(upstream_socket_, results,
                        [this, self](const asio::error_code& ec, const tcp::endpoint&) {
                            if (!ec) handshake();
                            else std::cerr << "Failed to connect to upstream proxy: " << ec.message() << std::endl;
                        });
                } else {
                    std::cerr << "Failed to resolve upstream proxy: " << ec.message() << std::endl;
                }
            });
    }

    void handshake() {
        if (g_config.type == ProxyType::SOCKS5) do_socks5_handshake();
        else do_http_handshake();
    }

    void do_socks5_handshake() {
        auto self(this->shared_from_this());
        out_buffer_ = {0x05, 0x01, 0x00};
        asio::async_write(upstream_socket_, asio::buffer(out_buffer_),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    in_buffer_.resize(2);
                    asio::async_read(upstream_socket_, asio::buffer(in_buffer_),
                        [this, self](const asio::error_code& ec, std::size_t) {
                            if (!ec && in_buffer_[1] == 0x00) {
                                send_socks5_connect();
                            } else if (!ec) {
                                std::cerr << "SOCKS5 Proxy rejected 'No Authentication'. It may require a username/password." << std::endl;
                            } else {
                                std::cerr << "SOCKS5 Greeting read failed: " << ec.message() << std::endl;
                            }
                        });
                } else {
                    std::cerr << "SOCKS5 Greeting write failed: " << ec.message() << std::endl;
                }
            });
    }

    void send_socks5_connect() {
        auto self(this->shared_from_this());
        out_buffer_.clear();
        out_buffer_.push_back(0x05);
        out_buffer_.push_back(0x01);
        out_buffer_.push_back(0x00);
        out_buffer_.push_back(0x03); // Domain name
        out_buffer_.push_back(static_cast<char>(target_host_.length()));
        for (char c : target_host_) out_buffer_.push_back(c);
        
        uint16_t port = static_cast<uint16_t>(std::stoi(target_port_));
        out_buffer_.push_back((port >> 8) & 0xFF);
        out_buffer_.push_back(port & 0xFF);

        asio::async_write(upstream_socket_, asio::buffer(out_buffer_),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    in_buffer_.resize(4);
                    asio::async_read(upstream_socket_, asio::buffer(in_buffer_),
                        [this, self](const asio::error_code& ec, std::size_t) {
                            if (!ec && in_buffer_[1] == 0x00) {
                                int atyp = in_buffer_[3];
                                read_socks5_response_address(atyp);
                            } else if (!ec) {
                                std::cerr << "SOCKS5 Connection to target failed! Reply Code: " << (int)in_buffer_[1] << std::endl;
                            } else {
                                std::cerr << "SOCKS5 Connect read failed: " << ec.message() << std::endl;
                            }
                        });
                } else {
                    std::cerr << "SOCKS5 Connect write failed: " << ec.message() << std::endl;
                }
            });
    }

    void read_socks5_response_address(int atyp) {
        auto self(this->shared_from_this());
        if (atyp == 0x01) { // IPv4
            in_buffer_.resize(6);
            asio::async_read(upstream_socket_, asio::buffer(in_buffer_),
                [this, self](const asio::error_code& ec, std::size_t) {
                    if (!ec) check_proxy_proto_and_relay();
                });
        } else if (atyp == 0x04) { // IPv6
            in_buffer_.resize(18);
            asio::async_read(upstream_socket_, asio::buffer(in_buffer_),
                [this, self](const asio::error_code& ec, std::size_t) {
                    if (!ec) check_proxy_proto_and_relay();
                });
        } else if (atyp == 0x03) { // Domain
            in_buffer_.resize(1);
            asio::async_read(upstream_socket_, asio::buffer(in_buffer_),
                [this, self](const asio::error_code& ec, std::size_t) {
                    if (!ec) {
                        int len = in_buffer_[0];
                        in_buffer_.resize(len + 2);
                        asio::async_read(upstream_socket_, asio::buffer(in_buffer_),
                            [this, self](const asio::error_code& ec, std::size_t) {
                                if (!ec) check_proxy_proto_and_relay();
                            });
                    }
                });
        } else {
            std::cerr << "SOCKS5 unknown ATYP: " << atyp << std::endl;
        }
    }

    void do_http_handshake() {
        auto self(this->shared_from_this());
        std::string req = "CONNECT " + target_host_ + ":" + target_port_ + " HTTP/1.1\r\nHost: " + target_host_ + ":" + target_port_ + "\r\n\r\n";
        out_buffer_.assign(req.begin(), req.end());

        asio::async_write(upstream_socket_, asio::buffer(out_buffer_),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    asio::async_read_until(upstream_socket_, http_response_buf_, "\r\n\r\n",
                        [this, self](const asio::error_code& ec, std::size_t) {
                            if (!ec) {
                                // Assume 200 OK for simplicity
                                check_proxy_proto_and_relay();
                            } else {
                                std::cerr << "HTTP CONNECT read failed: " << ec.message() << std::endl;
                            }
                        });
                } else {
                    std::cerr << "HTTP CONNECT write failed: " << ec.message() << std::endl;
                }
            });
    }

    void check_proxy_proto_and_relay() {
        if (send_proxy_proto_) {
            auto self(this->shared_from_this());
            
            auto remote_ep = client_stream_.lowest_layer().remote_endpoint();
            auto local_ep = client_stream_.lowest_layer().local_endpoint();
            
            std::ostringstream oss;
            oss << "PROXY ";
            if (remote_ep.address().is_v4()) oss << "TCP4 ";
            else oss << "TCP6 ";
            oss << remote_ep.address().to_string() << " "
                << local_ep.address().to_string() << " "
                << remote_ep.port() << " "
                << local_ep.port() << "\r\n";
                
            std::string header = oss.str();
            out_buffer_.assign(header.begin(), header.end());
            
            asio::async_write(upstream_socket_, asio::buffer(out_buffer_),
                [this, self](const asio::error_code& ec, std::size_t) {
                    if (!ec) relay_traffic();
                    else std::cerr << "PROXY header write failed: " << ec.message() << std::endl;
                });
        } else {
            relay_traffic();
        }
    }

    void relay_traffic() {
        do_read_client();
        do_read_upstream();
    }

    void do_read_client() {
        auto self(this->shared_from_this());
        client_stream_.async_read_some(asio::buffer(client_data_, max_length),
            [this, self](const asio::error_code& ec, std::size_t length) {
                if (!ec) {
                    asio::async_write(upstream_socket_, asio::buffer(client_data_, length),
                        [this, self](const asio::error_code& ec, std::size_t) {
                            if (!ec) do_read_client();
                        });
                } else if (ec != asio::error::eof) {
                    // Ignore EOF silently as it just means the connection closed gracefully
                }
            });
    }

    void do_read_upstream() {
        auto self(this->shared_from_this());
        upstream_socket_.async_read_some(asio::buffer(upstream_data_, max_length),
            [this, self](const asio::error_code& ec, std::size_t length) {
                if (!ec) {
                    asio::async_write(client_stream_, asio::buffer(upstream_data_, length),
                        [this, self](const asio::error_code& ec, std::size_t) {
                            if (!ec) do_read_upstream();
                        });
                } else if (ec != asio::error::eof) {
                    // Ignore EOF silently
                }
            });
    }

    Stream client_stream_;
    tcp::socket upstream_socket_;
    std::string target_host_;
    std::string target_port_;
    bool send_proxy_proto_;
    
    std::vector<char> out_buffer_;
    std::vector<char> in_buffer_;
    asio::streambuf http_response_buf_;
    
    enum { max_length = 16384 };
    char client_data_[max_length];
    char upstream_data_[max_length];
};

class TcpListener {
public:
    TcpListener(asio::io_context& io_context, short local_port, const std::string& target_host, const std::string& target_port, bool send_proxy_proto)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), local_port)), target_host_(target_host), target_port_(target_port), send_proxy_proto_(send_proxy_proto) {
        do_accept();
    }
private:
    void do_accept() {
        acceptor_.async_accept(
            [this](const asio::error_code& ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Bridge<tcp::socket>>(std::move(socket), acceptor_.get_executor(), target_host_, target_port_, send_proxy_proto_)->start();
                }
                do_accept();
            });
    }
    tcp::acceptor acceptor_;
    std::string target_host_;
    std::string target_port_;
    bool send_proxy_proto_;
};

class SslListener {
public:
    SslListener(asio::io_context& io_context, asio::ssl::context& ssl_context, short local_port, const std::string& target_host, const std::string& target_port, bool send_proxy_proto)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), local_port)), ssl_context_(ssl_context), target_host_(target_host), target_port_(target_port), send_proxy_proto_(send_proxy_proto) {
        do_accept();
    }
private:
    void do_accept() {
        acceptor_.async_accept(
            [this](const asio::error_code& ec, tcp::socket socket) {
                if (!ec) {
                    auto stream = asio::ssl::stream<tcp::socket>(std::move(socket), ssl_context_);
                    std::make_shared<Bridge<asio::ssl::stream<tcp::socket>>>(std::move(stream), acceptor_.get_executor(), target_host_, target_port_, send_proxy_proto_)->start();
                }
                do_accept();
            });
    }
    tcp::acceptor acceptor_;
    asio::ssl::context& ssl_context_;
    std::string target_host_;
    std::string target_port_;
    bool send_proxy_proto_;
};

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
        std::cerr << "SSL Error: " << e.what() << " - Ensure proxy.whatsapp.net.pem exists!" << std::endl;
        return;
    }

    // Port 80 -> g.whatsapp.net:80 (HTTP + Proxy Protocol)
    TcpListener l_80(io_context, 80, "g.whatsapp.net", "80", true);
    
    // Port 443 -> SSL Termination -> g.whatsapp.net:5222 (Proxy Protocol)
    SslListener l_443(io_context, ssl_context, 443, "g.whatsapp.net", "5222", true);
    
    // Port 5222 -> g.whatsapp.net:5222 (Proxy Protocol)
    TcpListener l_5222(io_context, 5222, "g.whatsapp.net", "5222", true);
    
    // Port 587 -> whatsapp.net:443 (Direct TCP)
    TcpListener l_587(io_context, 587, "whatsapp.net", "443", false);
    
    // Port 7777 -> whatsapp.net:443 (Direct TCP)
    TcpListener l_7777(io_context, 7777, "whatsapp.net", "443", false);

    std::cout << "WhatsApp Independent Proxy Translator is now RUNNING." << std::endl;
    std::cout << "Listening for connections on ports:" << std::endl;
    std::cout << "  80   : Standard web traffic (HTTP)" << std::endl;
    std::cout << "  443  : Standard web traffic, encrypted (HTTPS)" << std::endl;
    std::cout << "  5222 : Jabber protocol traffic (WhatsApp default)" << std::endl;
    std::cout << "  587  : WhatsApp Media traffic (HTTPS)" << std::endl;
    std::cout << "  7777 : WhatsApp Media traffic (HTTPS)" << std::endl;

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
            std::cout << "Service installed.\n";
            return 0;
#endif
        }
    }

    if (g_config.upstream_host.empty()) {
        std::cerr << "Usage: " << argv[0] << " --proxy <host:port> [--type socks5|http] [--install|--service]\n";
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
