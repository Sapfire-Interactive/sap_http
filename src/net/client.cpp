#include "sap_http/net/common.h"
#include "sap_http/net/http.h"

#include <sap_core/stl/unique_ptr.h>
#include <sap_core/stl/unordered_map.h>
#include <sap_network/tcp_socket.h>
#include <sap_network/tls_socket.h>

#include <chrono>
#include <mutex>

namespace sap::http {

using Clock = std::chrono::steady_clock;

static stl::string endpoint_key(const URL& u) {
    return u.host + ":" + stl::string(u.port);
}

static stl::unique_ptr<sap::network::TCPSocket>
dial_socket(const URL& u, const HttpClientConfig&) {
    sap::network::SocketConfig sc;
    sc.host = u.host;
    try {
        sc.port = static_cast<u16>(std::stoul(u.port));
    } catch (...) {
        return nullptr;
    }
    sc.connect_timeout = std::chrono::milliseconds{10000};
    sc.recv_timeout    = std::chrono::milliseconds{30000};
    sc.send_timeout    = std::chrono::milliseconds{30000};
    auto sock = stl::make_unique<sap::network::TCPSocket>(std::move(sc));
    if (!sock->valid() || !sock->connect())
        return nullptr;
    return sock;
}

static stl::unique_ptr<sap::network::TLSSocket>
dial_socket(const URL& u, const HttpsClientConfig& cfg) {
    sap::network::TlsClientConfig tls;
    try {
        tls.tcp.port = static_cast<u16>(std::stoul(u.port));
    } catch (...) {
        return nullptr;
    }
    tls.tcp.host             = u.host;
    tls.tcp.connect_timeout  = std::chrono::milliseconds{10000};
    tls.tcp.recv_timeout     = std::chrono::milliseconds{30000};
    tls.tcp.send_timeout     = std::chrono::milliseconds{30000};
    tls.verify_peer          = cfg.verify_peer;
    tls.verify_hostname      = cfg.verify_hostname;
    tls.ca_file              = cfg.ca_file;
    tls.ca_dir               = cfg.ca_dir;
    tls.client_cert_file     = cfg.client_cert_file;
    tls.client_key_file      = cfg.client_key_file;
    tls.alpn_protocols       = cfg.alpn_protocols;
    auto sock = stl::make_unique<sap::network::TLSSocket>(std::move(tls));
    if (!sock->valid() || !sock->connect())
        return nullptr;
    return sock;
}

template <sap::network::Socket S>
static stl::result<> send_all(S& sock, stl::string_view data) {
    stl::size_t sent = 0;
    while (sent < data.size()) {
        auto n = sock.send(stl::span<const stl::byte>(
            reinterpret_cast<const stl::byte*>(data.data() + sent), data.size() - sent));
        if (!n || n.value() == 0)
            return stl::make_error<>("Failed to send");
        sent += n.value();
    }
    return stl::result_success();
}

template <sap::network::Socket S>
static stl::result<> send_request_impl(S& sock, const Request& req, bool keep_alive) {
    std::ostringstream ss;
    ss << method_to_string(req.method) << " " << req.url.full_path() << " HTTP/1.1\r\n";
    ss << "Host: " << req.url.host << "\r\n";
    bool has_connection = false;
    for (const auto& [key, value] : req.headers.data) {
        ss << key << ": " << value << "\r\n";
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
    if (!has_connection)
        ss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
    ss << "\r\n";
    if (!req.body.empty())
        ss << req.body;
    return send_all(sock, ss.str());
}

template <sap::network::Socket S>
static stl::result<Response> read_response_impl(S& sock, bool& out_keep_alive) {
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
        if (!n || n.value() == 0)
            break;
        buffer.append(reinterpret_cast<const char*>(chunk), n.value());
        if (buffer.size() > Client<S>::max_response_size)
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
                    if (content_length > Client<S>::max_response_size)
                        return stl::make_error<Response>("Content-Length exceeds max_response_size");
                }
                auto te = resp.headers.get("transfer-encoding");
                if (te.find("chunked") != stl::string::npos)
                    is_chunked = true;
                if (is_chunked) {
                    auto body_result = read_chunked_body(sock, buffer, Client<S>::max_response_size);
                    if (!body_result)
                        return stl::make_error<Response>("{}", body_result.error());
                    resp.body = std::move(body_result.value());
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
        if (headers_done && !has_content_length && !is_chunked)
            continue;
    }
    if (!headers_done)
        return stl::make_error<Response>("Failed to parse response headers");
    if (!has_content_length && !is_chunked)
        resp.body = buffer;
    else if (has_content_length)
        resp.body = buffer.substr(0, content_length);
    if (has_content_length || is_chunked) {
        auto conn = resp.headers.get("connection");
        out_keep_alive = (conn.find("close") == stl::string::npos);
    }
    return resp;
}

// ---- Client<S> method implementations ----

template <sap::network::Socket S>
Client<S>::Client(Config cfg) : m_Config(std::move(cfg)) {}

template <sap::network::Socket S>
Client<S>& Client<S>::default_instance() {
    static Client<S> instance;
    return instance;
}

template <sap::network::Socket S>
void Client<S>::clear_pool() {
    std::lock_guard<stl::mutex> lk(m_Mu);
    m_Pool.clear();
}

template <sap::network::Socket S>
stl::result<Response> Client<S>::do_exchange(const Request& req) {
    auto key = endpoint_key(req.url);

    auto checkout = [this, &key]() -> stl::unique_ptr<S> {
        std::lock_guard<stl::mutex> lk(m_Mu);
        auto it = m_Pool.find(key);
        if (it == m_Pool.end() || it->second.empty())
            return nullptr;
        auto now = Clock::now();
        auto& vec = it->second;
        // Evict stale entries from the back, return the first fresh one found.
        while (!vec.empty()) {
            auto& entry = vec.back();
            if (now - entry.last_used > Client<S>::idle_timeout) {
                vec.pop_back();
                continue;
            }
            auto sock = std::move(entry.sock);
            vec.pop_back();
            return sock;
        }
        return nullptr;
    };

    auto checkin = [this, &key](stl::unique_ptr<S> sock) {
        if (Client<S>::idle_timeout.count() <= 0 || !sock || !sock->valid())
            return;
        std::lock_guard<stl::mutex> lk(m_Mu);
        m_Pool[key].push_back({std::move(sock), Clock::now()});
    };

    auto pooled = checkout();
    bool from_pool = (pooled != nullptr);
    auto sock = from_pool ? std::move(pooled) : dial_socket(req.url, m_Config);
    if (!sock)
        return stl::make_error<Response>("Failed to connect to {}:{}", req.url.host, req.url.port);

    auto send_r = send_request_impl(*sock, req, /*keep_alive=*/true);
    if (!send_r) {
        if (from_pool) {
            sock = dial_socket(req.url, m_Config);
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
            sock = dial_socket(req.url, m_Config);
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

    if (keep_alive)
        checkin(std::move(sock));
    return resp;
}

template <sap::network::Socket S>
std::future<stl::result<Response>> Client<S>::async_send_req(Request req) {
    return std::async(std::launch::async, [this, req = std::move(req)]() -> stl::result<Response> {
        return do_exchange(req);
    });
}

template <sap::network::Socket S>
stl::result<Response> Client<S>::send_req(const Request& req) {
    return do_exchange(req);
}

template <sap::network::Socket S>
std::future<stl::result<Response>> Client<S>::async_send(Request req) {
    return default_instance().async_send_req(std::move(req));
}

template <sap::network::Socket S>
stl::result<Response> Client<S>::send(const Request& req) {
    return default_instance().send_req(req);
}

template <sap::network::Socket S>
std::future<stl::result<Response>> Client<S>::get(stl::string_view url_str) {
    auto url_result = URL::parse(url_str);
    if (!url_result) {
        std::promise<stl::result<Response>> p;
        p.set_value(stl::make_error<Response>("{}", url_result.error()));
        return p.get_future();
    }
    return async_send(Request(EMethod::GET, std::move(url_result.value())));
}

template <sap::network::Socket S>
std::future<stl::result<Response>> Client<S>::post(stl::string_view url_str, stl::string body) {
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

template class Client<sap::network::TCPSocket>;
template class Client<sap::network::TLSSocket>;

} // namespace sap::http
