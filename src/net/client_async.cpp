#include "sap_http/net/http.h"

#include "sap_network/socket_async_concept.h"
#include "sap_network/socket_config.h"
#include "sap_network/tcp_socket_async.h"
#include "sap_network/tls_socket_async.h"

#include <sap_core/async/executor.h>
#include <sap_core/async/task.h>
#include <sap_core/stl/result.h>

#include <chrono>
#include <cstddef>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace sap::http {

    namespace {

        template <sap::network::SocketAsync S>
        stl::result<S> dial(sap::async::Executor& ex, const URL& url, const HttpClientConfig&) requires std::is_same_v<S, sap::network::TCPSocketAsync> {
            sap::network::SocketConfig sc;
            sc.host = url.host;
            try {
                sc.port = static_cast<u16>(std::stoul(url.port));
            } catch (...) {
                return stl::make_error<S>("Invalid port: {}", url.port);
            }
            return stl::result<S>(stl::success, S(ex, stl::move(sc)));
        }

        template <sap::network::SocketAsync S>
        stl::result<S> dial(sap::async::Executor& ex, const URL& url, const HttpsClientConfig& cfg) requires std::is_same_v<S, sap::network::TLSSocketAsync> {
            sap::network::TlsClientConfig tls;
            tls.tcp.host = url.host;
            try {
                tls.tcp.port = static_cast<u16>(std::stoul(url.port));
            } catch (...) {
                return stl::make_error<S>("Invalid port: {}", url.port);
            }
            tls.verify_peer      = cfg.verify_peer;
            tls.verify_hostname  = cfg.verify_hostname;
            tls.ca_file          = cfg.ca_file;
            tls.ca_dir           = cfg.ca_dir;
            tls.client_cert_file = cfg.client_cert_file;
            tls.client_key_file  = cfg.client_key_file;
            tls.alpn_protocols   = cfg.alpn_protocols;
            return stl::result<S>(stl::success, S(ex, stl::move(tls)));
        }

        stl::string build_request_wire(const Request& req) {
            std::ostringstream ss;
            ss << method_to_string(req.method) << " " << req.url.full_path() << " HTTP/1.1\r\n";
            ss << "Host: " << req.url.host << "\r\n";
            bool has_connection = false;
            for (const auto& [k, v] : req.headers.data) {
                ss << k << ": " << v << "\r\n";
                if (k.size() == 10) {
                    bool match    = true;
                    const char* w = "connection";
                    for (size_t i = 0; i < 10; ++i) {
                        char c = k[i];
                        if (c >= 'A' && c <= 'Z')
                            c = c - 'A' + 'a';
                        if (c != w[i]) {
                            match = false;
                            break;
                        }
                    }
                    if (match)
                        has_connection = true;
                }
            }
            if (!has_connection)
                ss << "Connection: close\r\n";
            if (!req.body.empty() && req.headers.get("Content-Length").empty() && req.headers.get("content-length").empty())
                ss << "Content-Length: " << req.body.size() << "\r\n";
            ss << "\r\n";
            if (!req.body.empty())
                ss << req.body;
            return ss.str();
        }

        template <sap::network::SocketAsync S>
        sap::async::Task<stl::result<>> async_send_all(S& sock, stl::span<const stl::byte> data) {
            stl::size_t sent = 0;
            while (sent < data.size()) {
                auto n = co_await sock.write(data.subspan(sent));
                if (!n)
                    co_return stl::make_error<>("{}", n.error());
                if (n.value() == 0)
                    co_return stl::make_error<>("Connection closed during send");
                sent += n.value();
            }
            co_return stl::result<>{};
        }

        template <sap::network::SocketAsync S>
        sap::async::Task<stl::result<Response>> async_read_response(S& sock, stl::size_t max_size) {
            Response    resp;
            stl::string buffer;
            stl::byte   chunk[4096];
            bool        headers_done       = false;
            stl::size_t content_length     = 0;
            bool        has_content_length = false;
            bool        is_chunked         = false;

            while (true) {
                auto n = co_await sock.read(stl::span<stl::byte>(chunk, sizeof(chunk)));
                if (!n)
                    co_return stl::make_error<Response>("{}", n.error());
                if (n.value() == 0)
                    break;
                buffer.append(reinterpret_cast<const char*>(chunk), n.value());
                if (buffer.size() > max_size)
                    co_return stl::make_error<Response>("Response exceeds max_response_size");

                if (!headers_done) {
                    auto header_end = buffer.find("\r\n\r\n");
                    if (header_end != stl::string::npos) {
                        stl::string        header_section = buffer.substr(0, header_end);
                        std::istringstream iss(header_section);
                        stl::string        line;
                        if (std::getline(iss, line)) {
                            std::istringstream status_stream(line);
                            stl::string        http_version;
                            i32                code_num = 0;
                            status_stream >> http_version >> code_num;
                            resp.status_code = static_cast<EStatusCode>(code_num);
                            std::getline(status_stream, resp.status_text);
                            if (!resp.status_text.empty() && resp.status_text[0] == ' ')
                                resp.status_text.erase(0, 1);
                        }
                        while (std::getline(iss, line) && !line.empty() && line != "\r") {
                            if (line.back() == '\r')
                                line.pop_back();
                            auto colon = line.find(':');
                            if (colon != stl::string::npos) {
                                auto key   = line.substr(0, colon);
                                auto value = line.substr(colon + 1);
                                if (!value.empty() && value[0] == ' ')
                                    value.erase(0, 1);
                                resp.headers.set(key, value);
                            }
                        }
                        buffer.erase(0, header_end + 4);
                        headers_done = true;
                        auto cl      = resp.headers.get("content-length");
                        if (!cl.empty()) {
                            try {
                                content_length     = std::stoull(cl);
                                has_content_length = true;
                            } catch (...) {
                                co_return stl::make_error<Response>("Invalid Content-Length");
                            }
                            if (content_length > max_size)
                                co_return stl::make_error<Response>("Content-Length exceeds max_response_size");
                        }
                        auto te = resp.headers.get("transfer-encoding");
                        if (te.find("chunked") != stl::string::npos)
                            is_chunked = true;
                        if (is_chunked)
                            co_return stl::make_error<Response>("Chunked transfer-encoding is not yet supported on the async client");
                    }
                }

                if (headers_done && has_content_length && buffer.size() >= content_length) {
                    resp.body = buffer.substr(0, content_length);
                    co_return resp;
                }
            }

            if (!headers_done)
                co_return stl::make_error<Response>("Failed to parse response headers");
            if (!has_content_length)
                resp.body = buffer;
            else
                resp.body = buffer.substr(0, content_length);
            co_return resp;
        }

    } // namespace

    template <sap::network::SocketAsync S>
    ClientAsync<S>::ClientAsync(sap::async::Executor& ex, Config cfg) : m_Executor(ex), m_Config(stl::move(cfg)) {}

    template <sap::network::SocketAsync S>
    sap::async::Task<stl::result<Response>> ClientAsync<S>::send(Request req) {
        auto sock_r = dial<S>(m_Executor, req.url, m_Config);
        if (!sock_r)
            co_return stl::make_error<Response>("{}", sock_r.error());
        S sock = stl::move(sock_r.value());
        if (!sock.valid())
            co_return stl::make_error<Response>("Failed to create socket");

        auto conn_r = co_await sock.connect();
        if (!conn_r)
            co_return stl::make_error<Response>("Failed to connect to {}:{}: {}", req.url.host, req.url.port, conn_r.error());

        auto wire = build_request_wire(req);
        auto send_r =
            co_await async_send_all(sock, stl::span<const stl::byte>(reinterpret_cast<const stl::byte*>(wire.data()), wire.size()));
        if (!send_r)
            co_return stl::make_error<Response>("{}", send_r.error());

        auto resp = co_await async_read_response(sock, ClientAsync<S>::max_response_size);
        sock.close();
        co_return resp;
    }

    template <sap::network::SocketAsync S>
    sap::async::Task<stl::result<Response>> ClientAsync<S>::get(stl::string_view url) {
        auto u = URL::parse(url);
        if (!u)
            co_return stl::make_error<Response>("{}", u.error());
        co_return co_await send(Request(EMethod::GET, stl::move(u.value())));
    }

    template <sap::network::SocketAsync S>
    sap::async::Task<stl::result<Response>> ClientAsync<S>::post(stl::string_view url, stl::string body) {
        auto u = URL::parse(url);
        if (!u)
            co_return stl::make_error<Response>("{}", u.error());
        Request req(EMethod::POST, stl::move(u.value()));
        req.set_body(stl::move(body));
        co_return co_await send(stl::move(req));
    }

    template class SAP_HTTP_API ClientAsync<sap::network::TCPSocketAsync>;
    template class SAP_HTTP_API ClientAsync<sap::network::TLSSocketAsync>;

} // namespace sap::http
