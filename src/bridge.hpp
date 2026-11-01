#pragma once

#include <openssl/opensslv.h>
#ifdef LIBRESSL_VERSION_NUMBER
#undef LIBRESSL_VERSION_NUMBER
#endif
#if !defined(OPENSSL_VERSION_NUMBER) && defined(OPENSSL_VERSION_MAJOR) && OPENSSL_VERSION_MAJOR >= 3
#define OPENSSL_VERSION_NUMBER 0x3000000fL
#endif

#include <asio.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/ssl/error.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>

#include "config.hpp"
#include "logger.hpp"

#include <format>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <iostream>

using asio::ip::tcp;

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

            // Start bidirectional relay utilizing C++ coroutines
            asio::co_spawn(upstream_socket_.get_executor(), relay_client_to_upstream(), asio::detached);
            asio::co_spawn(upstream_socket_.get_executor(), relay_upstream_to_client(), asio::detached);

        } catch (const std::exception& e) {
            log_message(std::format("[Bridge Disconnected] {}", e.what()), false);
            asio::error_code ignore_ec;
            upstream_socket_.close(ignore_ec);
            client_stream_.lowest_layer().close(ignore_ec);
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
        
        std::istream response_stream(&http_response_buf);
        std::string http_version;
        unsigned int status_code = 0;
        response_stream >> http_version >> status_code;
        if (status_code != 200) {
            throw std::runtime_error(std::format("HTTP Proxy CONNECT failed with status: {}", status_code));
        }
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
        } catch (...) {}
        asio::error_code ignore_ec;
        upstream_socket_.close(ignore_ec);
        client_stream_.lowest_layer().close(ignore_ec);
    }

    asio::awaitable<void> relay_upstream_to_client() {
        auto self = this->shared_from_this();
        try {
            for (;;) {
                std::size_t n = co_await upstream_socket_.async_read_some(asio::buffer(upstream_data_), asio::use_awaitable);
                co_await asio::async_write(client_stream_, asio::buffer(upstream_data_, n), asio::use_awaitable);
            }
        } catch (...) {}
        asio::error_code ignore_ec;
        upstream_socket_.close(ignore_ec);
        client_stream_.lowest_layer().close(ignore_ec);
    }

    Stream client_stream_;
    tcp::socket upstream_socket_;
    std::string target_host_;
    std::string target_port_;
    bool send_proxy_proto_;
    
    std::array<char, 16384> client_data_;
    std::array<char, 16384> upstream_data_;
};
