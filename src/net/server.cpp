#include "sap_http/net/common.h"
#include "sap_http/net/http.h"

#include <algorithm>
#include <mutex>

#include <sap_network/tcp_socket.h>

namespace sap::http {

    // Lowercase helper for case-insensitive header comparisons.
    static stl::string to_lower(stl::string_view s) {
        stl::string out;
        out.reserve(s.size());
        for (char c : s)
            out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        return out;
    }

    // Reads until "\r\n\r\n" is in `carry`, recv-ing more as needed. Returns the full
    // buffer (headers plus any already-read body bytes); `carry` is cleared. Callers
    // thread `carry` across requests so keep-alive connections preserve pipelined bytes
    // that arrived after the current request's headers.
    static stl::result<stl::string> read_header(sap::network::ISocket& sock, stl::size_t max_header_size,
                                                stl::string& carry) {
        stl::byte chunk[4096];
        while (true) {
            auto pos = carry.find("\r\n\r\n");
            if (pos != stl::string::npos) {
                if (pos > max_header_size)
                    return stl::make_error<stl::string>("Headers exceeded max size");
                stl::string out = std::move(carry);
                carry.clear();
                return out;
            }
            if (carry.size() > max_header_size)
                return stl::make_error<stl::string>("Headers exceeded max size");
            auto n = sock.recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
            if (n == 0)
                return stl::make_error<stl::string>("Connection closed during header read");
            carry.append(reinterpret_cast<const char*>(chunk), n);
        }
    }

    // Reads exactly `content_length` body bytes. `raw` is the header-phase buffer
    // (headers + already-read body prefix). Any bytes past content_length are moved
    // into `carry` for the next pipelined request on the same connection.
    static stl::result<stl::string> read_body(sap::network::ISocket& sock, const stl::string& raw,
                                              stl::size_t header_end, stl::size_t content_length,
                                              stl::size_t max_body_bytes, stl::string& carry) {
        if (content_length > max_body_bytes)
            return stl::make_error<stl::string>("Body exceeds max size");
        stl::size_t body_start = header_end + 4;
        stl::string body = raw.substr(body_start);
        stl::byte chunk[4096];
        while (body.size() < content_length) {
            auto n = sock.recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
            if (n == 0)
                return stl::make_error<stl::string>("Connection closed during body read");
            body.append(reinterpret_cast<const char*>(chunk), n);
        }
        if (body.size() > content_length) {
            carry.assign(body, content_length, body.size() - content_length);
        }
        body.resize(content_length);
        return body;
    }

    static stl::result<Request> parse_request(const stl::string& raw_request) {
        std::istringstream stream(raw_request);
        stl::string line;
        if (!std::getline(stream, line)) {
            return stl::make_error<Request>("Empty request");
        }

        std::istringstream first_line(line);
        stl::string method_str, path_str, version;
        first_line >> method_str >> path_str >> version;

        auto method = string_to_method(method_str);
        auto url_result = URL::from_path(path_str);
        if (!url_result)
            return stl::make_error<Request>("{}", url_result.error());
        Request req(method, std::move(url_result.value()));
        // Note: we still parse headers for UNKNOWN methods so the keep-alive logic
        // upstream can honor the client's Connection header before responding 405.
        while (std::getline(stream, line) && line != "\r" && !line.empty()) {
            if (line.back() == '\r')
                line.pop_back();
            auto colon = line.find(':');
            if (colon != stl::string::npos) {
                auto key = line.substr(0, colon);
                auto value = line.substr(colon + 1);
                if (!value.empty() && value[0] == ' ')
                    value.erase(0, 1);
                req.headers.set(key, value);
            }
        }

        return req;
    }

    // Returns true if the request-line on the first line of `header_section` is
    // HTTP/1.0. Used to pick the default keep-alive policy (1.0 defaults to close,
    // 1.1 defaults to keep-alive).
    static bool is_http_1_0(const stl::string& header_section) {
        auto eol = header_section.find("\r\n");
        stl::string line = (eol == stl::string::npos) ? stl::string(header_section) : header_section.substr(0, eol);
        return line.find("HTTP/1.0") != stl::string::npos;
    }

    static const char* status_reason_phrase(EStatusCode code) {
        switch (code) {
        case EStatusCode::OK: return "OK";
        case EStatusCode::Created: return "Created";
        case EStatusCode::NoContent: return "No Content";
        case EStatusCode::BadRequest: return "Bad Request";
        case EStatusCode::NotFound: return "Not Found";
        case EStatusCode::MethodNotAllowed: return "Method Not Allowed";
        case EStatusCode::PayloadTooLarge: return "Payload Too Large";
        case EStatusCode::RequestHeaderFieldsTooLarge: return "Request Header Fields Too Large";
        case EStatusCode::InternalServerError: return "Internal Server Error";
        default: return "Unknown";
        }
    }

    // Serializes a response for the wire. Automatically injects Content-Length (from
    // resp.body.size()) and Connection, unless the handler set them explicitly. The
    // automatic Content-Length is required for keep-alive framing — without it the
    // client has no way to find the end of the body other than EOF, which is
    // incompatible with connection reuse.
    static stl::string build_response(const Response& resp, bool keep_alive) {
        std::ostringstream ss;
        ss << "HTTP/1.1 " << static_cast<i32>(resp.status_code) << " "
           << status_reason_phrase(resp.status_code) << "\r\n";

        bool has_content_length = false;
        bool has_connection = false;
        for (const auto& [key, value] : resp.headers.data) {
            auto lk = to_lower(key);
            if (lk == "content-length") has_content_length = true;
            else if (lk == "connection") has_connection = true;
            ss << key << ": " << value << "\r\n";
        }
        if (!has_content_length)
            ss << "Content-Length: " << resp.body.size() << "\r\n";
        if (!has_connection)
            ss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
        ss << "\r\n";
        if (!resp.body.empty()) {
            ss << resp.body;
        }
        return ss.str();
    }

    static void send_all(sap::network::ISocket& sock, stl::string_view data) {
        stl::size_t sent = 0;
        while (sent < data.size()) {
            auto n = sock.send(stl::span<const stl::byte>(
                reinterpret_cast<const stl::byte*>(data.data() + sent), data.size() - sent));
            if (n == 0)
                return;
            sent += n;
        }
    }

    Server::Server(ServerConfig cfg) :
        m_Config(std::move(cfg)), m_Routes(), m_IsRunning(false),
        m_JobSystem(sap::job_system_config{.thread_count = m_Config.is_multithreaded ? stl::thread::hardware_concurrency() : 0}) {}

    Server::~Server() { stop(); }

    void Server::handle_client(stl::unique_ptr<sap::network::ISocket> client_socket) {
        auto& sock = *client_socket;
        // `idle` is true while we're parked in read_header() waiting for the next
        // request on a keep-alive connection. Server::stop() closes only sockets
        // whose entries are currently flagged idle, letting in-handler work finish.
        std::atomic<bool> idle{false};
        {
            std::lock_guard<std::mutex> lk(m_ClientsMutex);
            m_ActiveClients.push_back({&sock, &idle});
        }
        // RAII deregister: runs even on early break / exception.
        struct Deregister {
            Server* self;
            sap::network::ISocket* s;
            ~Deregister() {
                std::lock_guard<std::mutex> lk(self->m_ClientsMutex);
                auto& v = self->m_ActiveClients;
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [s = s](const ClientEntry& e) { return e.sock == s; }),
                        v.end());
            }
        } dereg{this, &sock};

        // Carry buffer preserves bytes that arrive past the current request's framing
        // so pipelined follow-up requests on the same keep-alive connection aren't lost.
        stl::string carry;

        while (true) {
            idle.store(true, std::memory_order_release);
            auto header_result = read_header(sock, Server::max_header_size, carry);
            idle.store(false, std::memory_order_release);
            if (!header_result) {
                // Client closed or read timed out (keep-alive idle timeout). Either way
                // we're done with this connection.
                break;
            }
            auto& raw = header_result.value();
            auto header_end = raw.find("\r\n\r\n");
            stl::string header_section = raw.substr(0, header_end);
            bool http_1_0 = is_http_1_0(header_section);
            auto req_result = parse_request(header_section);

            // Track whether we have to close after this response regardless of what
            // the client asked for (e.g. chunked request body, or a malformed request).
            bool force_close = false;

            if (req_result) {
                auto te = req_result.value().headers.get("Transfer-Encoding");
                if (te.find("chunked") != stl::string::npos) {
                    // read_chunked_body doesn't currently thread carry forward, so after
                    // a chunked request we can't safely continue pipelining on the same
                    // connection. Serve this request then close.
                    stl::string leftover = raw.substr(header_end + 4);
                    auto body_result = read_chunked_body(sock, leftover, Server::max_body_size);
                    if (body_result)
                        req_result.value().body = std::move(body_result.value());
                    else
                        req_result = stl::make_error<Request>("Chunked body read failed");
                    force_close = true;
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
                            auto body_result = read_body(sock, raw, header_end, content_length,
                                                         Server::max_body_size, carry);
                            if (body_result)
                                req_result.value().body = std::move(body_result.value());
                            else
                                req_result = stl::make_error<Request>("Body read failed");
                        } else if (req_result && content_length == 0) {
                            // No body: everything after \r\n\r\n in raw is already the
                            // next pipelined request.
                            stl::string tail = raw.substr(header_end + 4);
                            if (!tail.empty())
                                carry.insert(0, tail);
                        }
                    } else {
                        // No body declared. Any bytes after \r\n\r\n belong to the next request.
                        stl::string tail = raw.substr(header_end + 4);
                        if (!tail.empty())
                            carry.insert(0, tail);
                    }
                }
            }

            Response resp = req_result ? Response(EStatusCode::NotFound, "Not Found") : Response(EStatusCode::BadRequest, "");
            if (!req_result) {
                // Bad request: abandon the connection — the stream is in an unknown state.
                force_close = true;
            }
            if (req_result) {
                auto& req = req_result.value();
                bool short_circuited = false;
                for (const auto& mw : m_Middleware) {
                    try {
                        auto mw_resp = mw(req);
                        if (mw_resp) {
                            resp = std::move(*mw_resp);
                            short_circuited = true;
                            break;
                        }
                    } catch (const std::exception& e) {
                        resp = Response(EStatusCode::InternalServerError, stl::string("Middleware error: ") + e.what());
                        short_circuited = true;
                        break;
                    }
                }
                if (short_circuited) {
                    // skip routing
                } else if (req.method == EMethod::UNKNOWN) {
                    resp.status_code = EStatusCode::MethodNotAllowed;
                    resp.body = "";
                } else {
                    stl::vector<stl::string> req_segments;
                    {
                        size_t start = 0;
                        if (!req.url.path.empty() && req.url.path[0] == '/')
                            start = 1;
                        while (start <= req.url.path.size()) {
                            size_t slash = req.url.path.find('/', start);
                            size_t end = (slash == stl::string::npos) ? req.url.path.size() : slash;
                            if (end > start)
                                req_segments.push_back(req.url.path.substr(start, end - start));
                            if (slash == stl::string::npos)
                                break;
                            start = slash + 1;
                        }
                    }
                    // Score: literals matched. Higher wins. Static exact match gets a +1 boost.
                    const Route* best_match = nullptr;
                    int best_score = -1;
                    stl::map<stl::string, stl::string> best_params;

                    for (const auto& route : m_Routes) {
                        if (route.method != req.method)
                            continue;
                        if (route.has_params) {
                            if (route.segments.size() != req_segments.size())
                                continue;
                            stl::map<stl::string, stl::string> params;
                            int literals = 0;
                            bool ok = true;
                            for (size_t i = 0; i < route.segments.size(); ++i) {
                                if (route.segments[i].is_param) {
                                    params[route.segments[i].text] = req_segments[i];
                                } else if (route.segments[i].text == req_segments[i]) {
                                    ++literals;
                                } else {
                                    ok = false;
                                    break;
                                }
                            }
                            if (ok && literals > best_score) {
                                best_score = literals;
                                best_match = &route;
                                best_params = std::move(params);
                            }
                        } else {
                            // Static route: exact or segment-bounded prefix
                            if (route.path == req.url.path) {
                                int score = static_cast<int>(route.segments.size()) + 1000;
                                if (score > best_score) {
                                    best_score = score;
                                    best_match = &route;
                                    best_params.clear();
                                }
                            } else if (req.url.path.size() > route.path.size() &&
                                       req.url.path.substr(0, route.path.size()) == route.path &&
                                       req.url.path[route.path.size()] == '/') {
                                int score = static_cast<int>(route.segments.size());
                                if (score > best_score) {
                                    best_score = score;
                                    best_match = &route;
                                    best_params.clear();
                                }
                            }
                        }
                    }
                    if (best_match) {
                        for (auto& [k, v] : best_params)
                            req.params[k] = std::move(v);
                        try {
                            resp = best_match->handler(req);
                        } catch (const std::exception& e) {
                            resp = Response(EStatusCode::InternalServerError, stl::string("Error: ") + e.what());
                        }
                    }
                }
            }

            // Decide keep-alive:
            //   HTTP/1.1: keep-alive by default, unless request header says "close"
            //   HTTP/1.0: close by default, unless request header says "keep-alive"
            //   force_close overrides everything (bad request, chunked body, etc.)
            bool keep_alive = !force_close;
            if (keep_alive) {
                stl::string conn_hdr;
                if (req_result)
                    conn_hdr = to_lower(req_result.value().headers.get("Connection"));
                if (http_1_0) {
                    keep_alive = (conn_hdr.find("keep-alive") != stl::string::npos);
                } else {
                    keep_alive = (conn_hdr.find("close") == stl::string::npos);
                }
            }

            stl::string response_str = build_response(resp, keep_alive);
            send_all(sock, response_str);

            if (!keep_alive)
                break;
        }
        sock.close();
    }

    stl::result<> Server::start() {
        sap::network::SocketConfig sc;
        sc.host = m_Config.host;
        sc.port = m_Config.port;
        sc.reuse_addr = true;
        sc.recv_timeout = std::chrono::milliseconds{m_Config.timeout_ms};
        sc.send_timeout = std::chrono::milliseconds{m_Config.timeout_ms};
        m_ServerSocket = stl::make_unique<sap::network::TCPSocket>(std::move(sc));
        if (!m_ServerSocket->valid())
            return stl::make_error<>("Failed to create socket");
        if (!m_ServerSocket->bind())
            return stl::make_error<>("Failed to bind to {}:{}", m_Config.host, std::to_string(m_Config.port));
        if (!m_ServerSocket->listen())
            return stl::make_error<>("Failed to listen on port {}", std::to_string(m_Config.port));
        m_IsRunning = true;
        return stl::result_success();
    }

    void Server::run() {
        while (m_IsRunning.load()) {
            auto client = m_ServerSocket->accept();
            if (!client) {
                if (!m_IsRunning.load())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (m_Config.is_multithreaded) {
                auto* raw = client.release();
                if (!m_JobSystem.submit([this, raw]() {
                        stl::unique_ptr<sap::network::ISocket> owned(raw);
                        handle_client(std::move(owned));
                    })) {
                    stl::unique_ptr<sap::network::ISocket> owned(raw);
                    const char* msg = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
                    send_all(*owned, msg);
                    owned->close();
                }
            } else {
                handle_client(std::move(client));
            }
        }
    }

    void Server::run_async() {
        m_RunThread = std::thread([this]() { run(); });
    }

    void Server::stop() {
        if (!m_IsRunning.exchange(false)) {
            if (m_RunThread.joinable())
                m_RunThread.join();
            return;
        }
        if (m_ServerSocket)
            m_ServerSocket->close();
        // Close any client sockets currently parked in read_header() waiting for the
        // next keep-alive request, so their handle_client thread unwinds immediately.
        // Sockets currently inside a route handler are left alone so the handler can
        // finish and write its response — that's the graceful-shutdown contract.
        {
            std::lock_guard<std::mutex> lk(m_ClientsMutex);
            for (const auto& e : m_ActiveClients) {
                if (e.idle->load(std::memory_order_acquire))
                    e.sock->close();
            }
        }
        if (m_Config.is_multithreaded) {
            m_JobSystem.wait_idle();
        }
        m_ServerSocket.reset();
        if (m_RunThread.joinable())
            m_RunThread.join();
    }

} // namespace sap::http
