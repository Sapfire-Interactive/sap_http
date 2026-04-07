#include "sap_http/net/common.h"
#include "sap_http/net/http.h"

#include <sap_network/tcp_socket.h>

namespace sap::http {

    static stl::result<stl::string> read_header(sap::network::ISocket& sock, stl::size_t max_header_size) {
        stl::string buf;
        stl::byte chunk[4096];
        while (buf.size() < max_header_size) {
            auto n = sock.recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
            if (n == 0)
                return stl::make_error<stl::string>("Connection closed during header read");
            buf.append(reinterpret_cast<const char*>(chunk), n);
            auto pos = buf.find("\r\n\r\n");
            if (pos != stl::string::npos) {
                if (pos > max_header_size)
                    return stl::make_error<stl::string>("Headers exceeded max size");
                return buf;
            }
        }
        return stl::make_error<stl::string>("Headers exceeded max size");
    }

    static stl::result<stl::string> read_body(sap::network::ISocket& sock, stl::string& buf, stl::size_t header_end,
                                              stl::size_t content_length, stl::size_t max_body_bytes) {
        if (content_length > max_body_bytes)
            return stl::make_error<stl::string>("Body exceeds max size");
        stl::size_t body_start = header_end + 4;
        stl::string body = buf.substr(body_start);
        stl::byte chunk[4096];
        while (body.size() < content_length) {
            auto n = sock.recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
            if (n == 0)
                return stl::make_error<stl::string>("Connection closed during body read");
            body.append(reinterpret_cast<const char*>(chunk), n);
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
        if (method == EMethod::UNKNOWN)
            return req;

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

    static stl::string build_response(const Response& resp) {
        std::ostringstream ss;
        ss << "HTTP/1.1 " << static_cast<i32>(resp.status_code) << " "
           << status_reason_phrase(resp.status_code);
        ss << "\r\n";
        for (const auto& [key, value] : resp.headers.data) {
            ss << key << ": " << value << "\r\n";
        }
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
        auto header_result = read_header(sock, Server::max_header_size);
        if (header_result) {
            auto& raw = header_result.value();
            auto header_end = raw.find("\r\n\r\n");
            stl::string header_section = raw.substr(0, header_end);
            auto req_result = parse_request(header_section);

            if (req_result) {
                auto te = req_result.value().headers.get("Transfer-Encoding");
                if (te.find("chunked") != stl::string::npos) {
                    stl::string leftover = raw.substr(header_end + 4);
                    auto body_result = read_chunked_body(sock, leftover, Server::max_body_size);
                    if (body_result)
                        req_result.value().body = std::move(body_result.value());
                    else
                        req_result = stl::make_error<Request>("Chunked body read failed");
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
                            auto body_result = read_body(sock, raw, header_end, content_length, Server::max_body_size);
                            if (body_result)
                                req_result.value().body = std::move(body_result.value());
                            else
                                req_result = stl::make_error<Request>("Body read failed");
                        }
                    }
                }
            }
            Response resp = req_result ? Response(EStatusCode::NotFound, "Not Found") : Response(EStatusCode::BadRequest, "");
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
            stl::string response_str = build_response(resp);
            send_all(sock, response_str);
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
        if (m_Config.is_multithreaded) {
            m_JobSystem.wait_idle();
        }
        m_ServerSocket.reset();
        if (m_RunThread.joinable())
            m_RunThread.join();
    }

} // namespace sap::http
