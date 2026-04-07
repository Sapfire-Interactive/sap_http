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
    auto saved = sap::http::Server::max_body_size;
    sap::http::Server::max_body_size = 64 * 1024;
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
    sap::http::Server::max_body_size = saved;

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("16384") != std::string::npos);
}

TEST(ServerRecvTest, BodyExceedsMaxBodySize) {
    sap::http::ServerConfig cfg;
    cfg.port = 11003;
    auto saved = sap::http::Server::max_body_size;
    sap::http::Server::max_body_size = 128;
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
    sap::http::Server::max_body_size = saved;

    // Server should not return 200 — the body read should fail
    EXPECT_TRUE(resp.find("200 OK") == std::string::npos);
}

TEST(ServerRecvTest, HeadersExceedMaxHeaderSize) {
    sap::http::ServerConfig cfg;
    cfg.port = 11004;
    auto saved = sap::http::Server::max_header_size;
    sap::http::Server::max_header_size = 256;
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
    sap::http::Server::max_header_size = saved;

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

TEST(ServerUrlDecodeTest, PercentEncodedSpaceInPathMatchesRoute) {
    sap::http::ServerConfig cfg;
    cfg.port = 11040;
    sap::http::Server server(std::move(cfg));
    server.route("/hello world", sap::http::EMethod::GET,
                 [](const sap::http::Request& req) {
                     return sap::http::Response(sap::http::EStatusCode::OK, req.url.path);
                 });
    auto t = start_server(server);

    std::string req = "GET /hello%20world HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11040, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 200") != std::string::npos) << resp;
    EXPECT_TRUE(resp.find("hello world") != std::string::npos);
}

TEST(ServerUrlDecodeTest, PercentEncodedSpecialCharsDecoded) {
    sap::http::ServerConfig cfg;
    cfg.port = 11041;
    sap::http::Server server(std::move(cfg));
    server.route("/users", sap::http::EMethod::GET,
                 [](const sap::http::Request& req) {
                     return sap::http::Response(sap::http::EStatusCode::OK, req.url.path);
                 });
    auto t = start_server(server);

    std::string req = "GET /users/john%40doe%3Aadmin HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11041, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 200") != std::string::npos);
    EXPECT_TRUE(resp.find("john@doe:admin") != std::string::npos);
}

TEST(ServerUrlDecodeTest, PathTraversalDotDotRejected) {
    sap::http::ServerConfig cfg;
    cfg.port = 11042;
    sap::http::Server server(std::move(cfg));
    server.route("/files", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(sap::http::EStatusCode::OK, "file contents");
                 });
    auto t = start_server(server);

    std::string req = "GET /files/../secret HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11042, req);
    server.stop();
    t.join();

    // Must send a 400 Bad Request — not 404, not hang, not 200
    EXPECT_TRUE(resp.find("HTTP/1.1 400") != std::string::npos)
        << "Expected 400 Bad Request, got: " << resp;
    EXPECT_TRUE(resp.find("file contents") == std::string::npos);
}

TEST(ServerUrlDecodeTest, EncodedPathTraversalRejected) {
    // %2e%2e decodes to ".." — must still be caught after decoding
    sap::http::ServerConfig cfg;
    cfg.port = 11043;
    sap::http::Server server(std::move(cfg));
    server.route("/files", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(sap::http::EStatusCode::OK, "file contents");
                 });
    auto t = start_server(server);

    std::string req = "GET /files/%2e%2e/secret HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11043, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 400") != std::string::npos)
        << "Expected 400, got: " << resp;
    EXPECT_TRUE(resp.find("file contents") == std::string::npos);
}

TEST(ServerUrlDecodeTest, DotsInsideSegmentNotRejected) {
    // "file..txt" has ".." inside a segment — valid filename, not a traversal
    sap::http::ServerConfig cfg;
    cfg.port = 11044;
    sap::http::Server server(std::move(cfg));
    server.route("/files", sap::http::EMethod::GET,
                 [](const sap::http::Request& req) {
                     return sap::http::Response(sap::http::EStatusCode::OK, req.url.path);
                 });
    auto t = start_server(server);

    std::string req = "GET /files/file..txt HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11044, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 200") != std::string::npos);
    EXPECT_TRUE(resp.find("file..txt") != std::string::npos);
}

TEST(ServerUrlDecodeTest, MalformedPercentEscapeRejected) {
    sap::http::ServerConfig cfg;
    cfg.port = 11045;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(sap::http::EStatusCode::OK, "OK");
                 });
    auto t = start_server(server);

    std::string req = "GET /test%ZZ HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11045, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 400") != std::string::npos)
        << "Expected 400 for malformed %%-escape, got: " << resp;
}

TEST(ServerUrlDecodeTest, TruncatedPercentEscapeRejected) {
    sap::http::ServerConfig cfg;
    cfg.port = 11046;
    sap::http::Server server(std::move(cfg));
    server.route("/test", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(sap::http::EStatusCode::OK, "OK");
                 });
    auto t = start_server(server);

    // Trailing bare '%' with no hex digits
    std::string req = "GET /test% HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11046, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 400") != std::string::npos)
        << "Expected 400 for truncated %%-escape, got: " << resp;
}

TEST(ServerUrlDecodeTest, EncodedSlashRejected) {
    // %2F (encoded /) rejected so it can't smuggle past segment-aware routing
    sap::http::ServerConfig cfg;
    cfg.port = 11047;
    sap::http::Server server(std::move(cfg));
    server.route("/api", sap::http::EMethod::GET,
                 [](const sap::http::Request&) {
                     return sap::http::Response(sap::http::EStatusCode::OK, "api");
                 });
    auto t = start_server(server);

    std::string req = "GET /api%2Fsecret HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11047, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 400") != std::string::npos)
        << "Expected 400 for %%2F in path, got: " << resp;
}

TEST(ServerUrlDecodeTest, PlusInPathStaysLiteral) {
    // '+' must NOT be decoded to space in paths (that's a query-string rule only)
    sap::http::ServerConfig cfg;
    cfg.port = 11048;
    sap::http::Server server(std::move(cfg));
    server.route("/a+b", sap::http::EMethod::GET,
                 [](const sap::http::Request& req) {
                     return sap::http::Response(sap::http::EStatusCode::OK, req.url.path);
                 });
    auto t = start_server(server);

    std::string req = "GET /a+b HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    auto resp = raw_request(11048, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 200") != std::string::npos);
    EXPECT_TRUE(resp.find("a+b") != std::string::npos);
}

// ---- Tests for issue #10: segment-aware route prefix matching ----

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

TEST(ServerRecvTest, ChunkedRequestSimple) {
    sap::http::ServerConfig cfg;
    cfg.port = 11200;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "5\r\nhello\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11200, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("\r\n\r\nhello") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestMultipleChunks) {
    sap::http::ServerConfig cfg;
    cfg.port = 11201;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "5\r\nhello\r\n"
                      "1\r\n \r\n"
                      "5\r\nworld\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11201, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\nhello world") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestHexSize) {
    sap::http::ServerConfig cfg;
    cfg.port = 11202;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, std::to_string(req.body.size())); });
    auto t = start_server(server);

    std::string data(255, 'Z');
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "ff\r\n" + data + "\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11202, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\n255") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestWithExtensions) {
    sap::http::ServerConfig cfg;
    cfg.port = 11203;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "5;name=value\r\nhello\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11203, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\nhello") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestWithTrailers) {
    sap::http::ServerConfig cfg;
    cfg.port = 11204;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "5\r\nhello\r\n"
                      "0\r\n"
                      "X-Trailer: foo\r\n"
                      "\r\n";
    auto resp = raw_request(11204, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\nhello") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestEmptyBody) {
    sap::http::ServerConfig cfg;
    cfg.port = 11205;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, std::to_string(req.body.size())); });
    auto t = start_server(server);

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11205, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\n0") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestBinaryData) {
    sap::http::ServerConfig cfg;
    cfg.port = 11206;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST, [](const sap::http::Request& req) {
        sap::http::Response resp(sap::http::EStatusCode::OK, req.body);
        resp.headers.set("X-Body-Size", std::to_string(req.body.size()));
        return resp;
    });
    auto t = start_server(server);

    std::string data;
    data.push_back('\x00');
    data.push_back('\xff');
    data.push_back('\r');
    data.push_back('\n');
    data.push_back('\x7f');

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "5\r\n" + data + "\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11206, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("x-body-size: 5") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestInvalidHexSize) {
    sap::http::ServerConfig cfg;
    cfg.port = 11207;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "zzzz\r\nhello\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11207, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("400") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestExceedsMaxBodySize) {
    sap::http::ServerConfig cfg;
    cfg.port = 11208;
    auto saved = sap::http::Server::max_body_size;
    sap::http::Server::max_body_size = 100;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.body); });
    auto t = start_server(server);

    std::string data(200, 'A');
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "c8\r\n" + data + "\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11208, req);
    server.stop();
    t.join();
    sap::http::Server::max_body_size = saved;

    EXPECT_TRUE(resp.find("400") != std::string::npos);
}

TEST(ServerRecvTest, ChunkedRequestSpanningManyChunks) {
    sap::http::ServerConfig cfg;
    cfg.port = 11209;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, std::to_string(req.body.size())); });
    auto t = start_server(server);

    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n";
    for (int i = 0; i < 100; ++i)
        req += "1\r\nA\r\n";
    req += "0\r\n\r\n";

    auto resp = raw_request(11209, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\n100") != std::string::npos);
}

TEST(ServerRecvTest, GracefulShutdownWaitsForInFlightHandler) {
    sap::http::ServerConfig cfg;
    cfg.port = 11300;
    cfg.is_multithreaded = true;
    sap::http::Server server(std::move(cfg));

    std::atomic<bool> handler_finished{false};
    server.route("/slow", sap::http::EMethod::GET, [&](const sap::http::Request&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        handler_finished = true;
        return sap::http::Response(sap::http::EStatusCode::OK, "done");
    });
    auto t = start_server(server);

    std::thread client([&]() {
        std::string req = "GET /slow HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        auto resp = raw_request(11300, req);
        EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
        EXPECT_TRUE(resp.find("done") != std::string::npos);
    });

    // Give the client time to actually be inside the handler
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop();

    // After stop() returns, the in-flight handler must have completed.
    EXPECT_TRUE(handler_finished.load());

    client.join();
    t.join();
}

TEST(ServerRecvTest, GracefulShutdownStopsAcceptingNewConnections) {
    sap::http::ServerConfig cfg;
    cfg.port = 11301;
    cfg.is_multithreaded = true;
    sap::http::Server server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "hi"); });
    auto t = start_server(server);
    server.stop();
    t.join();

    // New connection after stop should fail to connect
    int sock = raw_connect(11301);
    EXPECT_LT(sock, 0);
    if (sock >= 0)
        close(sock);
}

TEST(ServerRecvTest, RouteParamSingleCaptured) {
    sap::http::ServerConfig cfg;
    cfg.port = 11400;
    sap::http::Server server(std::move(cfg));
    server.route("/users/:id", sap::http::EMethod::GET, [](const sap::http::Request& req) {
        return sap::http::Response(sap::http::EStatusCode::OK, req.params.at("id"));
    });
    auto t = start_server(server);

    auto resp = raw_request(11400, "GET /users/123 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\n123") != std::string::npos);
}

TEST(ServerRecvTest, RouteParamMultipleCaptured) {
    sap::http::ServerConfig cfg;
    cfg.port = 11401;
    sap::http::Server server(std::move(cfg));
    server.route("/posts/:post_id/comments/:comment_id", sap::http::EMethod::GET,
                 [](const sap::http::Request& req) {
                     return sap::http::Response(sap::http::EStatusCode::OK,
                                                req.params.at("post_id") + "/" + req.params.at("comment_id"));
                 });
    auto t = start_server(server);

    auto resp = raw_request(11401, "GET /posts/42/comments/7 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\n42/7") != std::string::npos);
}

TEST(ServerRecvTest, RouteParamSegmentCountMismatch) {
    sap::http::ServerConfig cfg;
    cfg.port = 11402;
    sap::http::Server server(std::move(cfg));
    server.route("/users/:id", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "matched"); });
    auto t = start_server(server);

    // Too few segments
    auto resp1 = raw_request(11402, "GET /users HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    // Too many segments
    auto resp2 = raw_request(11402, "GET /users/123/extra HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    server.stop();
    t.join();

    EXPECT_TRUE(resp1.find("404") != std::string::npos);
    EXPECT_TRUE(resp2.find("404") != std::string::npos);
}

TEST(ServerRecvTest, RouteParamLiteralBeatsParam) {
    sap::http::ServerConfig cfg;
    cfg.port = 11403;
    sap::http::Server server(std::move(cfg));
    server.route("/users/:id", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "param"); });
    server.route("/users/me", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "literal"); });
    auto t = start_server(server);

    auto resp_me = raw_request(11403, "GET /users/me HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    auto resp_id = raw_request(11403, "GET /users/123 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    server.stop();
    t.join();

    EXPECT_TRUE(resp_me.find("\r\n\r\nliteral") != std::string::npos);
    EXPECT_TRUE(resp_id.find("\r\n\r\nparam") != std::string::npos);
}

TEST(ServerRecvTest, RouteParamLiteralBeatsParamRegardlessOfRegistrationOrder) {
    sap::http::ServerConfig cfg;
    cfg.port = 11404;
    sap::http::Server server(std::move(cfg));
    // Register literal first this time
    server.route("/users/me", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "literal"); });
    server.route("/users/:id", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "param"); });
    auto t = start_server(server);

    auto resp_me = raw_request(11404, "GET /users/me HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    server.stop();
    t.join();

    EXPECT_TRUE(resp_me.find("\r\n\r\nliteral") != std::string::npos);
}

TEST(ServerRecvTest, RouteParamWithDifferentMethods) {
    sap::http::ServerConfig cfg;
    cfg.port = 11405;
    sap::http::Server server(std::move(cfg));
    server.route("/users/:id", sap::http::EMethod::GET,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, "GET " + req.params.at("id")); });
    server.route("/users/:id", sap::http::EMethod::DELETE,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, "DEL " + req.params.at("id")); });
    auto t = start_server(server);

    auto resp_get = raw_request(11405, "GET /users/5 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    auto resp_del = raw_request(11405, "DELETE /users/5 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    server.stop();
    t.join();

    EXPECT_TRUE(resp_get.find("\r\n\r\nGET 5") != std::string::npos);
    EXPECT_TRUE(resp_del.find("\r\n\r\nDEL 5") != std::string::npos);
}

TEST(ServerRecvTest, RouteParamCapturesPercentDecodedValue) {
    sap::http::ServerConfig cfg;
    cfg.port = 11406;
    sap::http::Server server(std::move(cfg));
    server.route("/files/:name", sap::http::EMethod::GET,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, req.params.at("name")); });
    auto t = start_server(server);

    auto resp = raw_request(11406, "GET /files/hello%20world HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\nhello world") != std::string::npos);
}

TEST(ServerRecvTest, RouteParamEmptyParamsForStaticRoute) {
    sap::http::ServerConfig cfg;
    cfg.port = 11407;
    sap::http::Server server(std::move(cfg));
    server.route("/static", sap::http::EMethod::GET, [](const sap::http::Request& req) {
        return sap::http::Response(sap::http::EStatusCode::OK, std::to_string(req.params.size()));
    });
    auto t = start_server(server);

    auto resp = raw_request(11407, "GET /static HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("\r\n\r\n0") != std::string::npos);
}

TEST(ServerRecvTest, RunAsyncDoesNotBlock) {
    sap::http::ServerConfig cfg;
    cfg.port = 11310;
    sap::http::Server server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "hi"); });

    auto start_result = server.start();
    ASSERT_TRUE(start_result.has_value()) << start_result.error();
    server.run_async();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto resp = raw_request(11310, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    EXPECT_TRUE(resp.find("200 OK") != std::string::npos);
    EXPECT_TRUE(resp.find("hi") != std::string::npos);

    server.stop();
}

TEST(ServerRecvTest, RunAsyncStopJoinsInternalThread) {
    sap::http::ServerConfig cfg;
    cfg.port = 11311;
    sap::http::Server server(std::move(cfg));
    auto start_result = server.start();
    ASSERT_TRUE(start_result.has_value());
    server.run_async();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop(); // must return without leaking the thread
    SUCCEED();
}

TEST(ServerRecvTest, StopIsIdempotent) {
    sap::http::ServerConfig cfg;
    cfg.port = 11302;
    sap::http::Server server(std::move(cfg));
    auto t = start_server(server);
    server.stop();
    server.stop(); // must not crash or hang
    t.join();
    SUCCEED();
}

TEST(ServerRecvTest, ChunkedRequestLargeChunkSpanningRecvCalls) {
    sap::http::ServerConfig cfg;
    cfg.port = 11210;
    auto saved = sap::http::Server::max_body_size;
    sap::http::Server::max_body_size = 64 * 1024;
    sap::http::Server server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, std::to_string(req.body.size())); });
    auto t = start_server(server);

    std::string data(16384, 'B');
    std::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "4000\r\n" + data + "\r\n"
                      "0\r\n\r\n";
    auto resp = raw_request(11210, req);
    server.stop();
    t.join();
    sap::http::Server::max_body_size = saved;

    EXPECT_TRUE(resp.find("\r\n\r\n16384") != std::string::npos);
}
