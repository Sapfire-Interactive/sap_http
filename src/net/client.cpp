#include "sap_http/net/common.h"
#include "sap_http/net/http.h"

#include <sap_core/stl/unique_ptr.h>
#include <sap_network/tcp_socket.h>

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace sap::http {

    using Clock = std::chrono::steady_clock;

    struct PooledConn {
        stl::unique_ptr<sap::network::TCPSocket> sock;
        Clock::time_point last_used;
    };

    struct Client::Impl {
        std::mutex mu;
        // Multimap-style: multiple idle conns can exist per endpoint.
        std::unordered_multimap<std::string, PooledConn> pool;
    };

    Client::Client() : m_Impl(stl::make_unique<Impl>()) {}
    Client::~Client() = default;

    Client& Client::default_instance() {
        static Client instance;
        return instance;
    }

    void Client::clear_pool() {
        std::lock_guard<std::mutex> lk(m_Impl->mu);
        m_Impl->pool.clear();
    }

    static std::string endpoint_key(const URL& u) {
        return u.host + ":" + std::string(u.port);
    }

    static stl::unique_ptr<sap::network::TCPSocket> dial(const URL& u) {
        sap::network::SocketConfig sc;
        sc.host = u.host;
        try {
            sc.port = static_cast<u16>(std::stoul(u.port));
        } catch (...) {
            return nullptr;
        }
        sc.connect_timeout = std::chrono::milliseconds{10000};
        sc.recv_timeout = std::chrono::milliseconds{30000};
        sc.send_timeout = std::chrono::milliseconds{30000};
        auto sock = stl::make_unique<sap::network::TCPSocket>(std::move(sc));
        if (!sock->valid())
            return nullptr;
        if (!sock->connect())
            return nullptr;
        return sock;
    }

    // Check out an idle connection for the endpoint if one exists and is still fresh.
    // Stale entries (past idle_timeout) are evicted lazily here. Returns nullptr if
    // no usable pooled conn is available — caller should dial a fresh one.
    static stl::unique_ptr<sap::network::TCPSocket>
    checkout(Client::Impl& impl, const std::string& key) {
        std::lock_guard<std::mutex> lk(impl.mu);
        auto now = Clock::now();
        auto range = impl.pool.equal_range(key);
        for (auto it = range.first; it != range.second;) {
            if (now - it->second.last_used > Client::idle_timeout) {
                it = impl.pool.erase(it);
                continue;
            }
            auto sock = std::move(it->second.sock);
            impl.pool.erase(it);
            return sock;
        }
        return nullptr;
    }

    static void
    checkin(Client::Impl& impl, const std::string& key, stl::unique_ptr<sap::network::TCPSocket> sock) {
        if (Client::idle_timeout.count() <= 0 || !sock || !sock->valid())
            return;
        std::lock_guard<std::mutex> lk(impl.mu);
        impl.pool.insert({key, PooledConn{std::move(sock), Clock::now()}});
    }

    static stl::result<> send_all(sap::network::ISocket& sock, stl::string_view data) {
        stl::size_t sent = 0;
        while (sent < data.size()) {
            auto n = sock.send(stl::span<const stl::byte>(
                reinterpret_cast<const stl::byte*>(data.data() + sent), data.size() - sent));
            if (n == 0)
                return stl::make_error<>("Failed to send");
            sent += n;
        }
        return stl::result_success();
    }

    static stl::result<> send_request_impl(sap::network::ISocket& sock, const Request& req, bool keep_alive) {
        std::ostringstream ss;
        ss << method_to_string(req.method) << " " << req.url.full_path() << " HTTP/1.1\r\n";
        ss << "Host: " << req.url.host << "\r\n";
        bool has_connection = false;
        for (const auto& [key, value] : req.headers.data) {
            ss << key << ": " << value << "\r\n";
            // Cheap case-insensitive check for "Connection"
            if (key.size() == 10) {
                bool match = true;
                const char* want = "connection";
                for (size_t i = 0; i < 10; ++i) {
                    char c = key[i];
                    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                    if (c != want[i]) { match = false; break; }
                }
                if (match) has_connection = true;
            }
        }
        if (!has_connection) {
            ss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
        }
        ss << "\r\n";
        if (!req.body.empty()) {
            ss << req.body;
        }
        return send_all(sock, ss.str());
    }

    // Returns the response and whether the connection is reusable for keep-alive.
    static stl::result<Response> read_response_impl(sap::network::ISocket& sock, bool& out_keep_alive) {
        out_keep_alive = false;
        Response resp;
        stl::string buffer;
        stl::byte chunk[4096];
        bool headers_done = false;
        stl::size_t content_length = 0;
        bool has_content_length = false;
        bool is_chunked = false;
        while (true) {
            auto n = sock.recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
            if (n == 0)
                break;
            buffer.append(reinterpret_cast<const char*>(chunk), n);
            if (buffer.size() > Client::max_response_size)
                return stl::make_error<Response>("Response exceeds max_response_size");
            if (!headers_done) {
                auto header_end = buffer.find("\r\n\r\n");
                if (header_end != stl::string::npos) {
                    stl::string header_section = buffer.substr(0, header_end);
                    std::istringstream iss(header_section);
                    stl::string line;
                    if (std::getline(iss, line)) {
                        std::istringstream status_stream(line);
                        stl::string http_version;
                        i32 code_num = 0;
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
                            auto key = line.substr(0, colon);
                            auto value = line.substr(colon + 1);
                            if (!value.empty() && value[0] == ' ')
                                value.erase(0, 1);
                            resp.headers.set(key, value);
                        }
                    }
                    buffer.erase(0, header_end + 4);
                    headers_done = true;
                    auto cl = resp.headers.get("content-length");
                    if (!cl.empty()) {
                        try {
                            content_length = std::stoull(cl);
                            has_content_length = true;
                        } catch (...) {
                            return stl::make_error<Response>("Invalid Content-Length");
                        }
                        if (content_length > Client::max_response_size)
                            return stl::make_error<Response>("Content-Length exceeds max_response_size");
                    }
                    auto te = resp.headers.get("transfer-encoding");
                    if (te.find("chunked") != stl::string::npos)
                        is_chunked = true;
                    if (is_chunked) {
                        auto body_result = read_chunked_body(sock, buffer, Client::max_response_size);
                        if (!body_result)
                            return stl::make_error<Response>("{}", body_result.error());
                        resp.body = std::move(body_result.value());
                        // Decide keep-alive from Connection header.
                        auto conn = resp.headers.get("connection");
                        out_keep_alive = (conn.find("close") == stl::string::npos);
                        return resp;
                    }
                }
            }
            if (headers_done && has_content_length && buffer.size() >= content_length) {
                resp.body = buffer.substr(0, content_length);
                break;
            }
            if (headers_done && !has_content_length && !is_chunked) {
                // No framing — must read to EOF. Connection is NOT reusable.
                continue;
            }
        }
        if (!headers_done)
            return stl::make_error<Response>("Failed to parse response headers");
        if (!has_content_length && !is_chunked)
            resp.body = buffer;  // read to EOF, connection already closed
        else if (has_content_length)
            resp.body = buffer.substr(0, content_length);

        // Keep-alive requires deterministic framing (Content-Length or chunked) so we
        // know where the response ended. Without it we just read to EOF.
        if (has_content_length || is_chunked) {
            auto conn = resp.headers.get("connection");
            out_keep_alive = (conn.find("close") == stl::string::npos);
        }
        return resp;
    }

    static stl::result<Response> do_exchange(Client::Impl& impl, const Request& req) {
        auto key = endpoint_key(req.url);

        // Try a pooled connection first. If the send/recv fails immediately, retry once
        // with a fresh connection — handles the race where the server closed an idle conn.
        auto pooled = checkout(impl, key);
        bool from_pool = (pooled != nullptr);
        auto sock = from_pool ? std::move(pooled) : dial(req.url);
        if (!sock)
            return stl::make_error<Response>("Failed to connect to {}:{}", req.url.host, req.url.port);

        auto send_r = send_request_impl(*sock, req, /*keep_alive=*/true);
        if (!send_r) {
            if (from_pool) {
                // Stale connection — retry once on a fresh socket.
                sock = dial(req.url);
                if (!sock)
                    return stl::make_error<Response>("Failed to reconnect to {}:{}", req.url.host, req.url.port);
                auto r2 = send_request_impl(*sock, req, /*keep_alive=*/true);
                if (!r2)
                    return stl::make_error<Response>("{}", r2.error());
            } else {
                return stl::make_error<Response>("{}", send_r.error());
            }
        }

        bool keep_alive = false;
        auto resp = read_response_impl(*sock, keep_alive);
        if (!resp) {
            if (from_pool) {
                // Stale mid-exchange — retry once with a fresh connection.
                sock = dial(req.url);
                if (!sock)
                    return stl::make_error<Response>("Failed to reconnect to {}:{}", req.url.host, req.url.port);
                auto r2 = send_request_impl(*sock, req, /*keep_alive=*/true);
                if (!r2)
                    return stl::make_error<Response>("{}", r2.error());
                keep_alive = false;
                resp = read_response_impl(*sock, keep_alive);
                if (!resp)
                    return resp;
            } else {
                return resp;
            }
        }

        if (keep_alive) {
            checkin(impl, key, std::move(sock));
        }
        return resp;
    }

    std::future<stl::result<Response>> Client::async_send_req(Request req) {
        Impl* impl = m_Impl.get();
        return std::async(std::launch::async, [impl, req = std::move(req)]() -> stl::result<Response> {
            return do_exchange(*impl, req);
        });
    }

    stl::result<Response> Client::send_req(const Request& req) {
        return do_exchange(*m_Impl, req);
    }

    std::future<stl::result<Response>> Client::async_send(Request req) {
        return default_instance().async_send_req(std::move(req));
    }

    stl::result<Response> Client::send(const Request& req) {
        return default_instance().send_req(req);
    }

    std::future<stl::result<Response>> Client::get(stl::string_view url_str) {
        auto url_result = URL::parse(url_str);
        if (!url_result) {
            std::promise<stl::result<Response>> p;
            p.set_value(stl::make_error<Response>("{}", url_result.error()));
            return p.get_future();
        }
        return async_send(Request(EMethod::GET, std::move(url_result.value())));
    }

    std::future<stl::result<Response>> Client::post(stl::string_view url_str, stl::string body) {
        auto url_result = URL::parse(url_str);
        if (!url_result) {
            std::promise<stl::result<Response>> p;
            p.set_value(stl::make_error<Response>("{}", url_result.error()));
            return p.get_future();
        }
        Request req(EMethod::POST, std::move(url_result.value()));
        req.set_body(std::move(body));
        return async_send(std::move(req));
    }

} // namespace sap::http
