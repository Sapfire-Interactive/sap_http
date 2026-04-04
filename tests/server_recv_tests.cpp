#include "sap_http/net/http.h"
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>

// Helper: start a server on the given port, run it in a background thread,
// return the thread. Caller is responsible for stop() + join().
static std::thread start_server(sap::http::Server& server) {
    auto res = server.start();
    EXPECT_TRUE(res.has_value()) << "Server failed to start: " << res.error();
    std::thread t([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return t;
}

// Helper: open a raw TCP socket to 127.0.0.1:port, send raw bytes, read response.
static std::string raw_request(u16 port, const std::string& data) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(sock, data.c_str() + sent, data.size() - sent, 0);
        if (n <= 0) break;
        sent += n;
    }

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, n);
    }
    close(sock);
    return response;
}

TEST(ServerRecvTest, PostBodyReadViaContentLength) {
    sap::http::ServerConfig cfg;
    cfg.port = 11001;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) {
                     return sap::http::Response(200, req.body);
                 });
    auto t = start_server(server);

    std::string body = R"({"key": "value"})";
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;

    auto resp = raw_request(11001, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find(body) != std::string::npos);
}

TEST(ServerRecvTest, LargeBodySpanningMultipleChunks) {
    sap::http::ServerConfig cfg;
    cfg.port = 11002;
    cfg.max_body_size = 64 * 1024; // 64KB
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) {
                     // Echo back the body size so we can verify it arrived intact
                     return sap::http::Response(200, std::to_string(req.body.size()));
                 });
    auto t = start_server(server);

    // 16KB body — larger than the 4KB recv chunk in read_body,
    // so it must span multiple recv calls
    std::string body(16384, 'A');
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;

    auto resp = raw_request(11002, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("16384") != std::string::npos);
}

TEST(ServerRecvTest, BodyExceedsMaxBodySize) {
    sap::http::ServerConfig cfg;
    cfg.port = 11003;
    cfg.max_body_size = 128; // tiny limit
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) {
                     return sap::http::Response(200, req.body);
                 });
    auto t = start_server(server);

    // Claim a body larger than max_body_size
    std::string body(256, 'X');
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;

    auto resp = raw_request(11003, req);
    server.stop();
    t.join();

    // Server should not return 200 — the body read should fail
    EXPECT_TRUE(resp.find("200 OK") == std::string::npos);
}

TEST(ServerRecvTest, HeadersExceedMaxHeaderSize) {
    sap::http::ServerConfig cfg;
    cfg.port = 11004;
    cfg.max_header_size = 256; // tiny limit
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(200, "OK");
                 });
    auto t = start_server(server);

    // Craft headers that exceed 256 bytes total
    std::string big_header(300, 'H');
    std::string req = "GET /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "X-Junk: " + big_header + "\r\n"
                      "\r\n";

    auto resp = raw_request(11004, req);
    server.stop();
    t.join();

    // Server should reject — no 200 response
    EXPECT_TRUE(resp.find("200 OK") == std::string::npos);
}

TEST(ServerRecvTest, InvalidContentLengthDoesNotCrash) {
    sap::http::ServerConfig cfg;
    cfg.port = 11005;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) {
                     return sap::http::Response(200, req.body);
                 });
    auto t = start_server(server);

    // Send a request with garbage Content-Length
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: not-a-number\r\n"
                      "\r\n"
                      "some body";

    auto resp = raw_request(11005, req);
    server.stop();
    t.join();

    // Should not crash — we just expect some response (not 200)
    // An empty response is also acceptable (connection closed)
    EXPECT_TRUE(resp.find("200 OK") == std::string::npos);
}

TEST(ServerRecvTest, GetWithNoBodyWorks) {
    sap::http::ServerConfig cfg;
    cfg.port = 11006;
    sap::http::Server server(std::move(cfg));
    server.route("/hello", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(200, "world");
                 });
    auto t = start_server(server);

    std::string req = "GET /hello HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";

    auto resp = raw_request(11006, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("world") != std::string::npos);
}
