#include "server.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "cert.hpp"

#include <asio/signal_set.hpp>
#include <vector>
#include <thread>
#include <format>

static asio::io_context* g_io_context = nullptr;

static asio::awaitable<void> tcp_listener(short local_port, std::string target_host, std::string target_port, bool send_proxy_proto) {
    auto executor = co_await asio::this_coro::executor;
    try {
        tcp::acceptor acceptor(executor);
        asio::error_code ec;
        acceptor.open(tcp::v4(), ec);
        if (ec) {
            log_message(std::format("Failed to open IPv4 socket for port {}: {}", local_port, ec.message()), true);
            co_return;
        }
        acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        acceptor.bind(tcp::endpoint(tcp::v4(), local_port), ec);
        if (ec) {
            log_message(std::format("Failed to bind port {}: {} (check if port is already in use or requires root/admin)", local_port, ec.message()), true);
            co_return;
        }
        acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            log_message(std::format("Failed to listen on port {}: {}", local_port, ec.message()), true);
            co_return;
        }

        for (;;) {
            tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
            std::make_shared<Bridge<tcp::socket>>(std::move(socket), executor, target_host, target_port, send_proxy_proto)->start();
        }
    } catch (const std::exception& e) {
        log_message(std::format("Listener exception on port {}: {}", local_port, e.what()), true);
    }
}

static asio::awaitable<void> ssl_listener(asio::ssl::context& ssl_context, short local_port, std::string target_host, std::string target_port, bool send_proxy_proto) {
    auto executor = co_await asio::this_coro::executor;
    try {
        tcp::acceptor acceptor(executor);
        asio::error_code ec;
        acceptor.open(tcp::v4(), ec);
        if (ec) {
            log_message(std::format("Failed to open IPv4 SSL socket for port {}: {}", local_port, ec.message()), true);
            co_return;
        }
        acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        acceptor.bind(tcp::endpoint(tcp::v4(), local_port), ec);
        if (ec) {
            log_message(std::format("Failed to bind SSL port {}: {} (check if port is already in use or requires root/admin)", local_port, ec.message()), true);
            co_return;
        }
        acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            log_message(std::format("Failed to listen on SSL port {}: {}", local_port, ec.message()), true);
            co_return;
        }

        for (;;) {
            tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
            auto stream = asio::ssl::stream<tcp::socket>(std::move(socket), ssl_context);
            std::make_shared<Bridge<asio::ssl::stream<tcp::socket>>>(std::move(stream), executor, target_host, target_port, send_proxy_proto)->start();
        }
    } catch (const std::exception& e) {
        log_message(std::format("SSL Listener exception on port {}: {}", local_port, e.what()), true);
    }
}

void run_server() {
    generate_certificate(g_config.cert_path);

    asio::io_context io_context;
    g_io_context = &io_context;

    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&io_context](const asio::error_code&, int sig) {
        log_message(std::format("Received shutdown signal ({}), stopping translator...", sig));
        io_context.stop();
    });

    asio::ssl::context ssl_context(asio::ssl::context::tlsv12_server);
    ssl_context.set_options(
        asio::ssl::context::default_workarounds |
        asio::ssl::context::no_sslv2 |
        asio::ssl::context::single_dh_use);
    
    try {
        ssl_context.use_certificate_chain_file(g_config.cert_path.string());
        ssl_context.use_private_key_file(g_config.cert_path.string(), asio::ssl::context::pem);
    } catch (const std::exception& e) {
        log_message(std::format("SSL Error: {} - Ensure certificate exists at '{}'", e.what(), g_config.cert_path.string()), true);
        g_io_context = nullptr;
        return;
    }

    // Launch listeners concurrently using co_spawn
    asio::co_spawn(io_context, tcp_listener(80, "g.whatsapp.net", "80", true), asio::detached);
    asio::co_spawn(io_context, ssl_listener(ssl_context, 443, "g.whatsapp.net", "5222", true), asio::detached);
    asio::co_spawn(io_context, tcp_listener(5222, "g.whatsapp.net", "5222", true), asio::detached);
    asio::co_spawn(io_context, tcp_listener(587, "whatsapp.net", "443", false), asio::detached);
    asio::co_spawn(io_context, tcp_listener(7777, "whatsapp.net", "443", false), asio::detached);

    log_message("WhatsApp Independent Proxy Translator is now RUNNING.");
    log_message(std::format("Upstream proxy: {}:{} ({})", g_config.upstream_host, g_config.upstream_port,
                            (g_config.type == ProxyType::HTTP ? "HTTP" : "SOCKS5")));
    log_message("Listening for connections on ports:");
    log_message("  80   : Standard web traffic (HTTP)");
    log_message("  443  : Standard web traffic, encrypted (HTTPS)");
    log_message("  5222 : Jabber protocol traffic (WhatsApp default)");
    log_message("  587  : WhatsApp Media traffic (HTTPS)");
    log_message("  7777 : WhatsApp Media traffic (HTTPS)");

    std::vector<std::thread> threads;
    unsigned int hardware_concurrency = std::thread::hardware_concurrency();
    if (hardware_concurrency == 0) hardware_concurrency = 4;
    
    for (unsigned int i = 0; i < hardware_concurrency; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }
    for (auto& t : threads) t.join();

    g_io_context = nullptr;
    log_message("WhatsApp Independent Proxy Translator has stopped.");
}

void stop_server() {
    if (g_io_context) {
        g_io_context->stop();
    }
}
