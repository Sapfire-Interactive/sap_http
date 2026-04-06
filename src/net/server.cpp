#include <cstring>
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
        // Create Request with URL containing just path and query
        Request req(method, URL::from_path(path_str));
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

    static stl::string build_response(const Response& resp) {
        std::ostringstream ss;
        ss << "HTTP/1.1 " << resp.status_code << " ";
        switch (resp.status_code) {
        case 200:
            ss << "OK";
            break;
        case 201:
            ss << "Created";
            break;
        case 204:
            ss << "No Content";
            break;
        case 400:
            ss << "Bad Request";
            break;
        case 404:
            ss << "Not Found";
            break;
        case 405:
            ss << "Method Not Allowed";
            break;
        case 500:
            ss << "Internal Server Error";
            break;
        default:
            ss << "Unknown";
            break;
        }
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
        auto header_result = read_header(client_socket, m_Config.max_header_size);
        if (header_result) {
            auto& raw = header_result.value();
            auto header_end = raw.find("\r\n\r\n");
            stl::string header_section = raw.substr(0, header_end);
            auto req_result = parse_request(header_section);

            // Read body if Content-Length is present
            if (req_result) {
                auto cl = req_result.value().headers.get("Content-Length");
                if (!cl.empty()) {
                    stl::size_t content_length = 0;
                    try {
                        content_length = std::stoull(cl);
                    } catch (...) {
                        req_result = stl::make_error<Request>("Invalid Content-Length");
                    }
                    if (req_result && content_length > 0) {
                        auto body_result = read_body(client_socket, raw, header_end, content_length, m_Config.max_body_size);
                        if (body_result)
                            req_result.value().body = std::move(body_result.value());
                        else
                            req_result = stl::make_error<Request>("Body read failed");
                    }
                }
            }
            Response resp(404, "Not Found");
            if (req_result) {
                auto& req = req_result.value();
                if (req.method == EMethod::UNKNOWN) {
                    resp.status_code = 405;
                    resp.body = "";
                } else {
                    // Find matching route using URL path with prefix matching
                    // Routes are sorted by specificity (longer paths first)
                    const Route* best_match = nullptr;
                    size_t best_match_len = 0;
                    for (const auto& route : m_Routes) {
                        if (route.method == req.method) {
                            // Check for exact match first
                            if (route.path == req.url.path) {
                                best_match = &route;
                                break;
                            }
                            // Check for prefix match (route path must be a prefix of request path,
                            // AND must end on a segment boundary so /api doesn't match /api-v2)
                            if (req.url.path.size() > route.path.size() && req.url.path.substr(0, route.path.size()) == route.path &&
                                req.url.path[route.path.size()] == '/' && route.path.size() > best_match_len) {
                                best_match = &route;
                                best_match_len = route.path.size();
                            }
                        }
                    }
                    if (best_match) {
                        try {
                            resp = best_match->handler(req);
                        } catch (const std::exception& e) {
                            resp = Response(500, stl::string("Error: ") + e.what());
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
        m_Config.server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (m_Config.server_socket < 0) {
#ifdef _WIN32
            i32 err = WSAGetLastError();
            return stl::make_error<>("Failed to create socket: {}", std::to_string(err));
#else
            return stl::make_error<>("Failed to create socket: {}", stl::string(strerror(errno)));
#endif
        }
        i32 opt = 1;
        setsockopt(m_Config.server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, m_Config.host.c_str(), &addr.sin_addr) != 1) {
            return stl::make_error<>("Invalid host address: {}", m_Config.host);
        }
        addr.sin_port = htons(m_Config.port);
        if (bind(m_Config.server_socket, (sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
            i32 err = WSAGetLastError();
            closesocket(m_Config.server_socket);
            return stl::make_error<>("Failed to bind to port {}: {}", std::to_string(m_Config.port), std::to_string(err));
#else
            close(m_Config.server_socket);
            return stl::make_error<>("Failed to bind to port {}: {}", std::to_string(m_Config.port), stl::string(strerror(errno)));
#endif
        }
        if (listen(m_Config.server_socket, 10) < 0) {
#ifdef _WIN32
            i32 err = WSAGetLastError();
            closesocket(m_Config.server_socket);
            return stl::make_error<>("Failed to listen: {}", std::to_string(err));
#else
            close(m_Config.server_socket);
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
            i32 client_socket = accept(m_Config.server_socket, (sockaddr*)&client_addr, &client_len);
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

    void Server::stop() {
        m_IsRunning.store(false);
        if (m_Config.server_socket >= 0) {
#ifdef _WIN32
            ::shutdown(m_Config.server_socket, SD_BOTH);
            closesocket(m_Config.server_socket);
            WSACleanup();
#else
            ::shutdown(m_Config.server_socket, SHUT_RDWR);
            close(m_Config.server_socket);
#endif
            m_Config.server_socket = -1;
        }
    }

} // namespace sap::http