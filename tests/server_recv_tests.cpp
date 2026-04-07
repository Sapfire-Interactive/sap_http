#include <gtest/gtest.h>
#include "sap_http/net/http.h"

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
        if (n <= 0)
            break;
        sent += n;
    }

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        response.append(buf, n);
    }
    close(sock);
    return response;
}

TEST(ServerRecvTest, PostBodyReadViaContentLength) {
    sap::http::ServerConfig cfg;
    cfg.port = 11001;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST, [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    std::string body = R"({"key": "value"})";
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n"
        "\r\n" +
        body;

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
    server.route("/echo", sap::http::EMethod::POST, [](const sap::http::Request& req) {
        // Echo back the body size so we can verify it arrived intact
        return sap::http::Response(sap::http::EStatusCode::OK, std::to_string(req.body.size()));
    });
    auto t = start_server(server);

    // 16KB body — larger than the 4KB recv chunk in read_body,
    // so it must span multiple recv calls
    std::string body(16384, 'A');
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n"
        "\r\n" +
        body;

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
    server.route("/echo", sap::http::EMethod::POST, [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    // Claim a body larger than max_body_size
    std::string body(256, 'X');
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n"
        "\r\n" +
        body;

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
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
    auto t = start_server(server);

    // Craft headers that exceed 256 bytes total
    std::string big_header(300, 'H');
    std::string req = "GET /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "X-Junk: " +
        big_header +
        "\r\n"
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
    server.route("/echo", sap::http::EMethod::POST, [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
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
    server.route("/hello", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "world"); });
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
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
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
    EXPECT_GE(ms, 400); // at least ~timeout
    EXPECT_LE(ms, 3000); // but not stuck forever
}

TEST(ServerTimeoutTest, PartialHeaderTimesOut) {
    sap::http::ServerConfig cfg;
    cfg.port = 11008;
    cfg.timeout_ms = 500;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
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

TEST(ServerRecvTest, BinaryBodyWithNullBytes) {
    sap::http::ServerConfig cfg;
    cfg.port = 11020;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST, [](const sap::http::Request& req) {
        // Echo back the body length so we can verify exact size
        sap::http::Response resp(sap::http::EStatusCode::OK, req.body);
        resp.headers.set("X-Body-Size", std::to_string(req.body.size()));
        return resp;
    });
    auto t = start_server(server);

    // Body with null bytes, newlines, and high bytes — would all be mangled
    // by the old getline-based parser
    std::string body;
    body.push_back('\x00');
    body.push_back('\xFF');
    body.push_back('\n');
    body.push_back('\r');
    body.push_back('\x00');
    body.push_back('\x80');
    body.append("normal text");
    body.push_back('\x00');

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n"
        "\r\n" +
        body;

    auto resp = raw_request(11020, req);
    server.stop();
    t.join();

    ASSERT_TRUE(resp.find("200 OK") != std::string::npos);
    // Confirm the server saw the exact byte count we sent.
    // Headers::set lowercases keys, so search for the lowercase form.
    std::string expected = "x-body-size: " + std::to_string(body.size());
    EXPECT_TRUE(resp.find(expected) != std::string::npos) << "Expected " << expected << " in response";

    // Also verify the echoed body matches byte-for-byte.
    // Use find (not rfind) — the FIRST \r\n\r\n is the header/body boundary;
    // any subsequent occurrences are part of the body.
    auto body_start = resp.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    std::string echoed = resp.substr(body_start + 4);
    EXPECT_EQ(echoed, body);
}

TEST(ServerRecvTest, BodyWithEmbeddedCRLFCRLF) {
    // A naive parser might think \r\n\r\n inside the body marks header end.
    // Content-Length-aware reading should handle it correctly.
    sap::http::ServerConfig cfg;
    cfg.port = 11021;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST, [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    std::string body = "before\r\n\r\nafter";
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n"
        "\r\n" +
        body;

    auto resp = raw_request(11021, req);
    server.stop();
    t.join();

    ASSERT_TRUE(resp.find("200 OK") != std::string::npos);
    // First \r\n\r\n is the header/body boundary. Body's embedded \r\n\r\n
    // would be a later occurrence — must use find, not rfind.
    auto body_start = resp.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    std::string echoed = resp.substr(body_start + 4);
    EXPECT_EQ(echoed, body);
}

TEST(ServerRouteTest, PrefixDoesNotMatchAcrossSegmentBoundary) {
    // /api should NOT match /api-v2 — they're different segments
    sap::http::ServerConfig cfg;
    cfg.port = 11030;
    sap::http::Server server(std::move(cfg));
    server.route("/api", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "api root"); });
    auto t = start_server(server);

    std::string req = "GET /api-v2 HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11030, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("404") != std::string::npos) << "Expected 404 for /api-v2, got: " << resp;
    EXPECT_TRUE(resp.find("api root") == std::string::npos);
}

TEST(ServerRouteTest, PrefixMatchAcrossSegmentBoundaryStillWorks) {
    // /api SHOULD match /api/users — that's a real sub-path
    sap::http::ServerConfig cfg;
    cfg.port = 11031;
    sap::http::Server server(std::move(cfg));
    server.route("/api", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "api handler"); });
    auto t = start_server(server);

    std::string req = "GET /api/users HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11031, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("api handler") != std::string::npos);
}

TEST(ServerRouteTest, ExactMatchStillWorks) {
    sap::http::ServerConfig cfg;
    cfg.port = 11032;
    sap::http::Server server(std::move(cfg));
    server.route("/api", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "exact"); });
    auto t = start_server(server);

    std::string req = "GET /api HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11032, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("exact") != std::string::npos);
}

TEST(ServerRouteTest, NoFalseMatchOnSuffix) {
    // /api should NOT match /apifoo
    sap::http::ServerConfig cfg;
    cfg.port = 11033;
    sap::http::Server server(std::move(cfg));
    server.route("/api", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "api"); });
    auto t = start_server(server);

    std::string req = "GET /apifoo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11033, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("404") != std::string::npos);
}

TEST(ServerRouteTest, LongestPrefixStillWins) {
    // When multiple routes could match, the longest valid prefix wins
    sap::http::ServerConfig cfg;
    cfg.port = 11034;
    sap::http::Server server(std::move(cfg));
    server.route("/api", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "short"); });
    server.route("/api/users", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "long"); });
    auto t = start_server(server);

    std::string req = "GET /api/users/42 HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11034, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("long") != std::string::npos);
    EXPECT_TRUE(resp.find("short") == std::string::npos);
}

TEST(ServerMethodTest, UnknownMethodReturns405) {
    sap::http::ServerConfig cfg;
    cfg.port = 11010;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
    auto t = start_server(server);

    std::string req = "FROBNICATE /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11010, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("405") != std::string::npos) << "Expected 405 Method Not Allowed, got: " << resp;
    EXPECT_TRUE(resp.find("200 OK") == std::string::npos);
}

TEST(ServerMethodTest, ConnectMethodReturns405) {
    sap::http::ServerConfig cfg;
    cfg.port = 11011;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
    auto t = start_server(server);

    // CONNECT is a real HTTP method but not supported by this server
    std::string req = "CONNECT /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11011, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("405") != std::string::npos) << "Expected 405, got: " << resp;
}

TEST(ServerMethodTest, GarbageMethodReturns405) {
    sap::http::ServerConfig cfg;
    cfg.port = 11012;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
    auto t = start_server(server);

    // Random garbage in the method position
    std::string req = "@#$%! /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11012, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("405") != std::string::npos) << "Expected 405, got: " << resp;
}

TEST(ServerMethodTest, UnknownMethodBodyDoesNotSayNotFound) {
    sap::http::ServerConfig cfg;
    cfg.port = 11015;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
    auto t = start_server(server);

    std::string req = "FROBNICATE /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11015, req);
    server.stop();
    t.join();

    // Status should be 405
    ASSERT_TRUE(resp.find("405") != std::string::npos);
    // Body should NOT be "Not Found" — that's confusing for a 405
    auto body_start = resp.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    std::string body = resp.substr(body_start + 4);
    EXPECT_TRUE(body.empty() || body.find("Not Found") == std::string::npos)
        << "405 response should not have 'Not Found' body, got: " << body;
}

TEST(ServerMethodTest, UnknownMethodSkipsRouteHandlers) {
    // Verify that handlers are NOT invoked when an unknown method comes in,
    // even if a route exists at the same path with a known method.
    sap::http::ServerConfig cfg;
    cfg.port = 11016;
    sap::http::Server server(std::move(cfg));

    std::atomic<int> handler_calls{0};
    server.route("/test", sap::http::EMethod::GET, [&handler_calls](const sap::http::Request&) {
        handler_calls.fetch_add(1);
        return sap::http::Response(sap::http::EStatusCode::OK, "OK");
    });
    auto t = start_server(server);

    std::string req = "FROBNICATE /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11016, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("405") != std::string::npos);
    EXPECT_EQ(handler_calls.load(), 0) << "Handler should not be invoked for unknown methods";
}

TEST(ServerMethodTest, KnownMethodsStillWork) {
    sap::http::ServerConfig cfg;
    cfg.port = 11013;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
    auto t = start_server(server);

    std::string req = "GET /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11013, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("405") == std::string::npos);
}

TEST(ServerMethodTest, UnknownMethodWithBodyDoesNotHang) {
    sap::http::ServerConfig cfg;
    cfg.port = 11014;
    cfg.timeout_ms = 2000;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::POST, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
    auto t = start_server(server);

    // Unknown method with a Content-Length and body — server should
    // reject with 405 quickly without trying to consume the body
    std::string body = "some body data";
    std::string req = "WEIRDO /test HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n"
        "\r\n" +
        body;

    auto start = std::chrono::steady_clock::now();
    auto resp = raw_request(11014, req);
    auto elapsed = std::chrono::steady_clock::now() - start;
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("405") != std::string::npos);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LE(ms, 1500) << "Server took too long — likely waiting on body";
}

TEST(ServerTimeoutTest, NormalRequestStillWorksWithTimeout) {
    sap::http::ServerConfig cfg;
    cfg.port = 11009;
    cfg.timeout_ms = 2000; // generous timeout
    sap::http::Server server(std::move(cfg));
    server.route("/hello", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "world"); });
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
