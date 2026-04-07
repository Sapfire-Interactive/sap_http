#include "sap_http/net/common.h"
#include "sap_http/net/http.h"

#include <sap_core/stl/unique_ptr.h>
#include <sap_network/tcp_socket.h>

namespace sap::http {

    static stl::unique_ptr<sap::network::TCPSocket> connect_to(const URL& u) {
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

    static stl::result<> send_request_impl(sap::network::ISocket& sock, const Request& req) {
        std::ostringstream ss;
        ss << method_to_string(req.method) << " " << req.url.full_path() << " HTTP/1.1\r\n";
        ss << "Host: " << req.url.host << "\r\n";
        for (const auto& [key, value] : req.headers.data) {
            ss << key << ": " << value << "\r\n";
        }
        ss << "\r\n";
        if (!req.body.empty()) {
            ss << req.body;
        }
        return send_all(sock, ss.str());
    }

    static stl::result<Response> read_response_impl(sap::network::ISocket& sock) {
        Response resp;
        stl::string buffer;
        stl::byte chunk[4096];
        bool headers_done = false;
        stl::size_t content_length = 0;
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
                        return resp;
                    }
                }
            }
            if (headers_done && content_length == 0)
                break;
            if (headers_done && content_length > 0 && buffer.size() >= content_length) {
                resp.body = buffer.substr(0, content_length);
                break;
            }
        }
        if (!headers_done)
            return stl::make_error<Response>("Failed to parse response headers");
        if (content_length == 0 && !buffer.empty())
            resp.body = buffer;
        else if (content_length > 0)
            resp.body = buffer.substr(0, content_length);
        return resp;
    }

    std::future<stl::result<Response>> Client::async_send(Request req) {
        return std::async(std::launch::async, [req = std::move(req)]() -> stl::result<Response> {
            auto sock = connect_to(req.url);
            if (!sock)
                return stl::make_error<Response>("Failed to connect to {}:{}", req.url.host, req.url.port);
            auto send_result = send_request_impl(*sock, req);
            if (!send_result)
                return stl::make_error<Response>("{}", send_result.error());
            return read_response_impl(*sock);
        });
    }

    stl::result<Response> Client::send(const Request& req) {
        return async_send(Request(req)).get();
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
