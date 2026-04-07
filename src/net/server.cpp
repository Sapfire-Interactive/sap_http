#include <cstring>
#include "sap_http/net/common.h"
#include "sap_http/net/http.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sap::http {

    static stl::result<stl::string> read_header(i32 sock, stl::size_t max_header_size) {
        stl::string buf;
        char chunk[4096];
        while (buf.size() < max_header_size) {
            ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return stl::make_error<stl::string>("Connection closed during header read");
            buf.append(chunk, n);
            auto pos = buf.find("\r\n\r\n");
            if (pos != std::string::npos) {
                if (pos > max_header_size)
                    return stl::make_error<stl::string>("Headers exceeded max size");
                return buf;
            }
        }
        return stl::make_error<stl::string>("Headers exceeded max size");
    }

    static stl::result<stl::string> read_body(i32 sock, stl::string& buf, stl::size_t header_end, stl::size_t content_length,
                                              stl::size_t max_body_bytes) {
        if (content_length > max_body_bytes)
            return stl::make_error<stl::string>("Body exceeds max size");
        // We may have already read past the headers into the body
        size_t body_start = header_end + 4; // past \r\n\r\n
        stl::string body = buf.substr(body_start);
        char chunk[4096];
        while (body.size() < content_length) {
            ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return stl::make_error<stl::string>("Connection closed during body read");
            body.append(chunk, n);
        }
        body.resize(content_length); // trim any excess
        return body;
    }

    // Parse incoming request and create a Request object
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

        // Parse headers
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

    Server::Server(ServerConfig cfg) :
        m_Config(std::move(cfg)), m_Routes(), m_IsRunning(false),
        m_JobSystem(sap::job_system_config{.thread_count = m_Config.is_multithreaded ? stl::thread::hardware_concurrency() : 0}) {}

    Server::~Server() { stop(); }

    void Server::handle_client(i32 client_socket) {
        struct timeval timeout;
        timeout.tv_sec = m_Config.timeout_ms / 1000;
        timeout.tv_usec = (m_Config.timeout_ms % 1000) * 1000;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeval));
        setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeval));
        auto header_result = read_header(client_socket, Server::max_header_size);
        if (header_result) {
            auto& raw = header_result.value();
            auto header_end = raw.find("\r\n\r\n");
            stl::string header_section = raw.substr(0, header_end);
            auto req_result = parse_request(header_section);

            // Read body if Content-Length is present
            if (req_result) {
                auto te = req_result.value().headers.get("Transfer-Encoding");
                if (te.find("chunked") != stl::string::npos) {
                    stl::string leftover = raw.substr(header_end + 4);
                    auto body_result = read_chunked_body(client_socket, leftover, Server::max_body_size);
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
                            auto body_result = read_body(client_socket, raw, header_end, content_length, Server::max_body_size);
                            if (body_result)
                                req_result.value().body = std::move(body_result.value());
                            else
                                req_result = stl::make_error<Request>("Body read failed");
                        }
                    }
                }
            }
            // Default: 400 Bad Request if the request couldn't be parsed,
            // 404 Not Found if it parsed but no route matched.
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
                    // Split request path into segments once
                    std::vector<stl::string> req_segments;
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
                    std::map<stl::string, stl::string> best_params;

                    for (const auto& route : m_Routes) {
                        if (route.method != req.method)
                            continue;
                        if (route.has_params) {
                            // Param routes require equal segment count
                            if (route.segments.size() != req_segments.size())
                                continue;
                            std::map<stl::string, stl::string> params;
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
                                int score = static_cast<int>(route.segments.size()) + 1000; // exact wins
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
            send(client_socket, response_str.c_str(), response_str.size(), 0);
        }
#ifdef _WIN32
        closesocket(client_socket);
#else
        close(client_socket);
#endif
    }

    stl::result<> Server::start() {
#ifdef _WIN32
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            return stl::make_error<>("Failed to initialize Winsock");
        }
#endif
        m_ServerSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (m_ServerSocket < 0) {
#ifdef _WIN32
            i32 err = WSAGetLastError();
            return stl::make_error<>("Failed to create socket: {}", std::to_string(err));
#else
            return stl::make_error<>("Failed to create socket: {}", stl::string(strerror(errno)));
#endif
        }
        i32 opt = 1;
        setsockopt(m_ServerSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, m_Config.host.c_str(), &addr.sin_addr) != 1) {
            return stl::make_error<>("Invalid host address: {}", m_Config.host);
        }
        addr.sin_port = htons(m_Config.port);
        if (bind(m_ServerSocket, (sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
            i32 err = WSAGetLastError();
            closesocket(m_ServerSocket);
            return stl::make_error<>("Failed to bind to port {}: {}", std::to_string(m_Config.port), std::to_string(err));
#else
            close(m_ServerSocket);
            return stl::make_error<>("Failed to bind to port {}: {}", std::to_string(m_Config.port), stl::string(strerror(errno)));
#endif
        }
        if (listen(m_ServerSocket, 10) < 0) {
#ifdef _WIN32
            i32 err = WSAGetLastError();
            closesocket(m_ServerSocket);
            return stl::make_error<>("Failed to listen: {}", std::to_string(err));
#else
            close(m_ServerSocket);
            return stl::make_error<>("Failed to listen: {}", stl::string(strerror(errno)));
#endif
        }
        m_IsRunning = true;
        return stl::result_success();
    }

    void Server::run() {
        while (m_IsRunning.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            i32 client_socket = accept(m_ServerSocket, (sockaddr*)&client_addr, &client_len);
            if (client_socket < 0) {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (!m_IsRunning.load())
                    break;
                // transient errors: sleep and continue
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
#else
                if (errno == EINTR)
                    continue;
                if (!m_IsRunning.load())
                    break;
                if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
#endif
            }
            if (m_Config.is_multithreaded) {
                if (!m_JobSystem.submit([this, client_socket]() { handle_client(client_socket); })) {
                    // Queue full — reject connection
                    const char* msg = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
                    ::send(client_socket, msg, strlen(msg), 0);
                    close(client_socket);
                }
            } else {
                handle_client(client_socket);
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
        if (m_ServerSocket >= 0) {
#ifdef _WIN32
            ::shutdown(m_ServerSocket, SD_BOTH);
#else
            ::shutdown(m_ServerSocket, SHUT_RDWR);
#endif
        }
        if (m_Config.is_multithreaded) {
            m_JobSystem.wait_idle();
        }
        if (m_ServerSocket >= 0) {
#ifdef _WIN32
            closesocket(m_ServerSocket);
            WSACleanup();
#else
            close(m_ServerSocket);
#endif
            m_ServerSocket = -1;
        }
        if (m_RunThread.joinable())
            m_RunThread.join();
    }

} // namespace sap::http