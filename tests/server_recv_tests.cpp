#include "sap_http/net/http.h"
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
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

// Helper: open a raw TCP socket to 127.0.0.1:port and return the fd.
// Caller is responsible for closing.
static int raw_connect(u16 port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
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

TEST(ServerTimeoutTest, SlowlorisConnectionTimesOut) {
    sap::http::ServerConfig cfg;
    cfg.port = 11007;
    cfg.timeout_ms = 500; // 500ms timeout
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(200, "OK");
                 });
    auto t = start_server(server);

    // Connect but send nothing — classic slowloris
    auto start = std::chrono::steady_clock::now();
    int sock = raw_connect(11007);
    ASSERT_GE(sock, 0) << "Failed to connect";

    // Wait for the server to close the connection
    char buf[128];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    auto elapsed = std::chrono::steady_clock::now() - start;
    close(sock);

    server.stop();
    t.join();

    // recv should have returned <= 0 (connection closed by server)
    EXPECT_LE(n, 0);
    // Should have taken roughly the timeout duration, not forever.
    // Allow generous upper bound (3s) but it must not hang.
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_GE(ms, 400);  // at least ~timeout
    EXPECT_LE(ms, 3000); // but not stuck forever
}

TEST(ServerTimeoutTest, PartialHeaderTimesOut) {
    sap::http::ServerConfig cfg;
    cfg.port = 11008;
    cfg.timeout_ms = 500;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(200, "OK");
                 });
    auto t = start_server(server);

    // Send partial headers (no \r\n\r\n terminator) then stall
    int sock = raw_connect(11008);
    ASSERT_GE(sock, 0);
    std::string partial = "GET /test HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    ::send(sock, partial.c_str(), partial.size(), 0);

    auto start = std::chrono::steady_clock::now();
    char buf[128];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    auto elapsed = std::chrono::steady_clock::now() - start;
    close(sock);

    server.stop();
    t.join();

    EXPECT_LE(n, 0);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_GE(ms, 400);
    EXPECT_LE(ms, 3000);
}

TEST(ServerTimeoutTest, NormalRequestStillWorksWithTimeout) {
    sap::http::ServerConfig cfg;
    cfg.port = 11009;
    cfg.timeout_ms = 2000; // generous timeout
    sap::http::Server server(std::move(cfg));
    server.route("/hello", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(200, "world");
                 });
    auto t = start_server(server);

    std::string req = "GET /hello HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11009, req);
    server.stop();
    t.join();

    // Normal requests should still succeed even with timeouts enabled
    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("world") != std::string::npos);
}
