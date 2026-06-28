#include "server_common.h"

#include "sap_http/net/http.h"
#include "sap_network/socket_async_concept.h"
#include "sap_network/socket_config.h"
#include "sap_network/tcp_socket_async.h"
#include "sap_network/tls_socket_async.h"

#include <sap_core/async/executor.h>
#include <sap_core/async/spawn.h>
#include <sap_core/async/stop_token.h>
#include <sap_core/async/task.h>
#include <sap_core/io/reactor.h>

#include <chrono>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace sap::http {

    using detail::build_response;
    using detail::is_http_1_0;
    using detail::match_route;
    using detail::parse_request;
    using detail::to_lower;

    namespace {

        constexpr std::string_view CRLFCRLF{"\r\n\r\n", 4};

        std::string_view byte_view(const stl::vector<stl::byte>& v) {
            return std::string_view(reinterpret_cast<const char*>(v.data()), v.size());
        }

        stl::string string_from_bytes(const stl::vector<stl::byte>& v, stl::size_t off, stl::size_t len) {
            return stl::string(reinterpret_cast<const char*>(v.data()) + off, len);
        }

        template <sap::network::SocketAsync S>
        sap::async::Task<stl::result<stl::vector<stl::byte>>> async_read_header(S& sock, stl::size_t max_header_size,
                                                                                stl::vector<stl::byte>& carry) {
            stl::byte chunk[4096];
            while (true) {
                auto pos = byte_view(carry).find(CRLFCRLF);
                if (pos != std::string_view::npos) {
                    if (pos > max_header_size)
                        co_return stl::make_error<stl::vector<stl::byte>>("Headers exceeded max size");
                    auto out = stl::move(carry);
                    carry.clear();
                    co_return out;
                }
                if (carry.size() > max_header_size)
                    co_return stl::make_error<stl::vector<stl::byte>>("Headers exceeded max size");
                auto n = co_await sock.read(stl::span<stl::byte>(chunk, sizeof(chunk)));
                if (!n)
                    co_return stl::make_error<stl::vector<stl::byte>>("{}", n.error());
                if (n.value() == 0)
                    co_return stl::make_error<stl::vector<stl::byte>>("Connection closed during header read");
                carry.insert(carry.end(), chunk, chunk + n.value());
            }
        }

        template <sap::network::SocketAsync S>
        sap::async::Task<stl::result<stl::vector<stl::byte>>>
        async_read_body(S& sock, const stl::vector<stl::byte>& raw, stl::size_t header_end, stl::size_t content_length,
                        stl::size_t max_body_bytes, stl::vector<stl::byte>& carry) {
            if (content_length > max_body_bytes)
                co_return stl::make_error<stl::vector<stl::byte>>("Body exceeds max size");
            stl::size_t            body_start = header_end + 4;
            stl::vector<stl::byte> body(raw.begin() + body_start, raw.end());
            stl::byte              chunk[4096];
            while (body.size() < content_length) {
                auto n = co_await sock.read(stl::span<stl::byte>(chunk, sizeof(chunk)));
                if (!n)
                    co_return stl::make_error<stl::vector<stl::byte>>("{}", n.error());
                if (n.value() == 0)
                    co_return stl::make_error<stl::vector<stl::byte>>("Connection closed during body read");
                body.insert(body.end(), chunk, chunk + n.value());
            }
            if (body.size() > content_length)
                carry.assign(body.begin() + content_length, body.end());
            body.resize(content_length);
            co_return body;
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

        sap::async::Task<Response> dispatch(Request req, const stl::vector<RouteAsync>& routes,
                                            const stl::vector<MiddlewareAsync>& middleware) {
            auto matched = match_route(routes, req.method, req.url.path);
            if (matched.route) {
                for (auto& [k, v] : matched.params)
                    req.params[k] = stl::move(v);
            }

            bool skip_mw = matched.route && matched.route->skip_middleware;
            if (!skip_mw) {
                for (const auto& mw : middleware) {
                    try {
                        auto mw_resp = co_await mw(req);
                        if (mw_resp)
                            co_return stl::move(*mw_resp);
                    } catch (const std::exception& e) {
                        co_return Response(EStatusCode::InternalServerError, std::string("Middleware error: ") + e.what());
                    }
                }
            }

            if (!matched.route)
                co_return Response(EStatusCode::NotFound, "Not Found");

            try {
                co_return co_await matched.route->handler(stl::move(req));
            } catch (const std::exception& e) {
                co_return Response(EStatusCode::InternalServerError, std::string("Error: ") + e.what());
            }
        }

        template <sap::network::SocketAsync S>
        sap::async::Task<void> handle_connection(S sock, stl::size_t max_header_size, stl::size_t max_body_size,
                                                 const stl::vector<RouteAsync>* routes, const stl::vector<MiddlewareAsync>* middleware) {
            stl::vector<stl::byte> carry;
            while (true) {
                auto header_result = co_await async_read_header(sock, max_header_size, carry);
                if (!header_result)
                    break;

                auto&       raw            = header_result.value();
                auto        header_end     = byte_view(raw).find(CRLFCRLF);
                stl::string header_section = string_from_bytes(raw, 0, header_end);
                bool        http_1_0       = is_http_1_0(header_section);
                auto        req_result     = parse_request(header_section);
                bool        force_close    = false;

                if (req_result) {
                    auto te = req_result.value().headers.get("Transfer-Encoding");
                    if (te.find("chunked") != stl::string::npos) {
                        force_close = true;
                        req_result  = stl::make_error<Request>("Chunked bodies are not yet supported on the async server");
                    } else {
                        auto cl = req_result.value().headers.get("Content-Length");
                        if (!cl.empty()) {
                            stl::size_t content_length = 0;
                            try {
                                content_length = std::stoull(cl);
                            } catch (...) {
                                req_result = stl::make_error<Request>("Invalid Content-Length");
                            }
                            if (req_result && content_length > 0) {
                                auto body_result = co_await async_read_body(sock, raw, header_end, content_length, max_body_size, carry);
                                if (body_result) {
                                    const auto& b           = body_result.value();
                                    req_result.value().body = stl::string(reinterpret_cast<const char*>(b.data()), b.size());
                                } else {
                                    req_result = stl::make_error<Request>("Body read failed");
                                }
                            } else if (req_result && content_length == 0) {
                                if (raw.size() > header_end + 4)
                                    carry.insert(carry.begin(), raw.begin() + header_end + 4, raw.end());
                            }
                        } else {
                            if (raw.size() > header_end + 4)
                                carry.insert(carry.begin(), raw.begin() + header_end + 4, raw.end());
                        }
                    }
                }

                Response resp = req_result ? Response(EStatusCode::NotFound, "Not Found") : Response(EStatusCode::BadRequest, "");
                if (!req_result)
                    force_close = true;

                // Snapshot the Connection header before dispatch moves the Request.
                stl::string conn_hdr;
                if (req_result)
                    conn_hdr = to_lower(req_result.value().headers.get("Connection"));

                if (req_result) {
                    auto& req = req_result.value();
                    if (req.method == EMethod::UNKNOWN) {
                        resp.status_code = EStatusCode::MethodNotAllowed;
                        resp.body        = "";
                    } else {
                        resp = co_await dispatch(stl::move(req), *routes, *middleware);
                    }
                }

                bool keep_alive = !force_close;
                if (keep_alive) {
                    if (http_1_0)
                        keep_alive = (conn_hdr.find("keep-alive") != stl::string::npos);
                    else
                        keep_alive = (conn_hdr.find("close") == stl::string::npos);
                }

                stl::string response_str = build_response(resp, keep_alive);
                auto send_result = co_await async_send_all(sock, stl::span<const stl::byte>(reinterpret_cast<const stl::byte*>(response_str.data()),
                                                                                            response_str.size()));
                if (!send_result || !keep_alive)
                    break;
            }
            sock.close();
        }

        template <sap::network::SocketAsync S>
        sap::async::Task<void> accept_loop(sap::async::Executor& ex, S& listener, const bool& running, sap::async::StopToken stop_tok,
                                           stl::size_t max_header, stl::size_t max_body, const stl::vector<RouteAsync>* routes,
                                           const stl::vector<MiddlewareAsync>* middleware) {
            while (running) {
                auto child = co_await listener.accept(stop_tok);
                if (!child) {
                    if (!running)
                        break;
                    continue;
                }
                (void)sap::async::spawn(ex, handle_connection(stl::move(child.value()), max_header, max_body, routes, middleware));
            }
        }

    } // namespace

    template <sap::network::SocketAsync S>
    stl::result<ServerAsync<S>> ServerAsync<S>::create(Config cfg) {
        auto rea = sap::io::Reactor::create();
        if (!rea)
            return stl::make_error<ServerAsync<S>>("Reactor::create failed: {}", rea.error());
        ServerAsync<S> s(stl::move(rea.value()), stl::move(cfg));
        return stl::result<ServerAsync<S>>(stl::success, stl::move(s));
    }

    template <sap::network::SocketAsync S>
    ServerAsync<S>::ServerAsync(sap::io::Reactor reactor, Config cfg) : m_Config(stl::move(cfg)), m_Executor(stl::move(reactor)) {}

    template <sap::network::SocketAsync S>
    ServerAsync<S>::~ServerAsync() {
        stop();
    }

    template <sap::network::SocketAsync S>
    stl::result<> ServerAsync<S>::start() {
        if constexpr (std::is_same_v<S, sap::network::TCPSocketAsync>) {
            sap::network::SocketConfig sc;
            sc.host       = m_Config.host;
            sc.port       = m_Config.port;
            sc.reuse_addr = true;
            m_Listener.emplace(m_Executor, stl::move(sc));
        } else if constexpr (std::is_same_v<S, sap::network::TLSSocketAsync>) {
            sap::network::TlsServerConfig tls = m_Config.tls_cfg;
            tls.tcp.host                      = m_Config.host;
            tls.tcp.port                      = m_Config.port;
            tls.tcp.reuse_addr                = true;
            if (tls.alpn_protocols.empty())
                tls.alpn_protocols.emplace_back("http/1.1");
            m_Listener.emplace(m_Executor, stl::move(tls));
        }

        if (!m_Listener->valid())
            return stl::make_error<>("Failed to create socket");
        if (!m_Listener->bind())
            return stl::make_error<>("Failed to bind to {}:{}", m_Config.host, m_Config.port);
        if (!m_Listener->listen())
            return stl::make_error<>("Failed to listen on port {}", m_Config.port);

        m_Running = true;
        return stl::result_success();
    }

    template <sap::network::SocketAsync S>
    void ServerAsync<S>::run() {
        if (!m_Running || !m_Listener)
            return;
        auto loop = accept_loop(m_Executor, *m_Listener, m_Running, m_StopSource.token(), max_header_size, max_body_size, &m_Routes, &m_Middleware);
        m_Executor.spawn_detach(stl::move(loop));
        m_Executor.run();
    }

    template <sap::network::SocketAsync S>
    void ServerAsync<S>::stop() {
        if (!m_Running)
            return;
        m_Running = false;
        m_StopSource.request_stop();
        if (m_Listener)
            m_Listener->close();
    }

    template <sap::network::SocketAsync S>
    sap::async::Executor& ServerAsync<S>::executor() {
        return m_Executor;
    }

    template class SAP_HTTP_API ServerAsync<sap::network::TCPSocketAsync>;
    template class SAP_HTTP_API ServerAsync<sap::network::TLSSocketAsync>;

} // namespace sap::http
