#include <gtest/gtest.h>
#include "sap_http/net/http.h"

#include <sap_network/tcp_socket.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

// Minimal TCP server (built on sap::network::TCPSocket) that listens on `port`,
// accepts one connection, drains the request headers, then sends the canned
// `response` bytes and closes. Runs in a background thread.
struct FakeServer {
    stl::unique_ptr<sap::network::TCPSocket> listener;
    stl::thread thread;
    stl::atomic<bool> done{false};

    FakeServer(u16 port, stl::string response) {
        sap::network::SocketConfig sc;
        sc.host = "127.0.0.1";
        sc.port = port;
        sc.reuse_addr = true;
        sc.listen_backlog = 1;
        sc.recv_timeout = std::chrono::milliseconds{2000};
        sc.send_timeout = std::chrono::milliseconds{2000};
        listener = std::make_unique<sap::network::TCPSocket>(std::move(sc));
        if (!listener->valid() || !listener->bind() || !listener->listen())
            return;

        thread = stl::thread([this, response = std::move(response)]() {
            auto client = listener->accept();
            if (!client) return;
            // Drain the request headers (best-effort).
            stl::byte buf[4096];
            stl::string acc;
            while (true) {
                auto n = client->recv(stl::span<stl::byte>(buf, sizeof(buf)));
                if (n == 0) break;
                acc.append(reinterpret_cast<const char*>(buf), n);
                if (acc.find("\r\n\r\n") != stl::string::npos) break;
            }
            client->send(stl::span<const stl::byte>(
                reinterpret_cast<const stl::byte*>(response.data()), response.size()));
            // Close immediately so client's read loop exits
            client->close();
            done = true;
        });
        // Give the listener a moment to be ready
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~FakeServer() {
        if (thread.joinable()) thread.join();
        if (listener) listener->close();
    }
};

TEST(ClientRecvTest, MalformedContentLengthDoesNotCrash) {
    stl::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: not-a-number\r\n"
        "\r\n"
        "body";
    FakeServer fake(12001, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12001/");
    bool threw = false;
    try {
        auto result = fut.get();
        // Should be an error result (not a successful response)
        EXPECT_FALSE(result.has_value());
    } catch (...) {
        threw = true;
    }
    EXPECT_FALSE(threw) << "Client must not throw on malformed Content-Length";
}

TEST(ClientRecvTest, OverflowContentLengthDoesNotCrash) {
    stl::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 99999999999999999999999\r\n"
        "\r\n";
    FakeServer fake(12002, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12002/");
    auto result = fut.get();

    SUCCEED();
}

TEST(ClientRecvTest, NegativeContentLengthDoesNotCrash) {
    stl::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: -1\r\n"
        "\r\n";
    FakeServer fake(12003, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12003/");
    auto result = fut.get();

    SUCCEED();
}

TEST(ClientRecvTest, ContentLengthExceedingCapIsRejected) {
    // Server claims a huge body via Content-Length — client should reject
    // before allocating, returning an error result.
    auto saved = sap::http::Client::max_response_size;
    sap::http::Client::max_response_size = 1024; // 1KB cap

    stl::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 999999999\r\n"
        "\r\n";
    FakeServer fake(12005, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12005/");
    auto result = fut.get();

    sap::http::Client::max_response_size = saved;

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().find("max_response_size") != stl::string::npos)
        << "Got: " << result.error();
}

TEST(ClientRecvTest, AccumulatedBytesExceedingCapIsRejected) {
    // No Content-Length, just streamed bytes — client must still cap based
    // on accumulated buffer size to prevent OOM from chunked / EOF-framed responses.
    auto saved = sap::http::Client::max_response_size;
    sap::http::Client::max_response_size = 512;

    // 4KB body, no Content-Length header — client reads until EOF but should
    // bail once it exceeds the cap
    stl::string big_body(4096, 'X');
    stl::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n" + big_body;
    FakeServer fake(12006, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12006/");
    auto result = fut.get();

    sap::http::Client::max_response_size = saved;

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().find("max_response_size") != stl::string::npos)
        << "Got: " << result.error();
}

TEST(ClientRecvTest, ResponseWithinCapSucceeds) {
    // Sanity: a normal small response under the cap should still work
    auto saved = sap::http::Client::max_response_size;
    sap::http::Client::max_response_size = 10 * 1024;

    stl::string body = "hello world";
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;
    FakeServer fake(12007, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12007/");
    auto result = fut.get();

    sap::http::Client::max_response_size = saved;

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().status_code, sap::http::EStatusCode::OK);
    EXPECT_EQ(result.value().body, body);
}

TEST(ClientRecvTest, EmptyContentLengthDoesNotCrash) {
    stl::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: \r\n"
        "\r\n";
    FakeServer fake(12004, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12004/");
    auto result = fut.get();

    SUCCEED();
}

TEST(ClientRecvTest, ChunkedSimpleSingleChunk) {
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "0\r\n\r\n";
    FakeServer fake(12100, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12100/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, "hello");
}

TEST(ClientRecvTest, ChunkedMultipleChunks) {
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "1\r\n \r\n"
        "5\r\nworld\r\n"
        "0\r\n\r\n";
    FakeServer fake(12101, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12101/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, "hello world");
}

TEST(ClientRecvTest, ChunkedHexSize) {
    // 1a hex = 26 bytes
    stl::string data(26, 'X');
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "1a\r\n" + data + "\r\n"
        "0\r\n\r\n";
    FakeServer fake(12102, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12102/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body.size(), 26u);
    EXPECT_EQ(result.value().body, data);
}

TEST(ClientRecvTest, ChunkedUppercaseHexSize) {
    stl::string data(255, 'Y');
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "FF\r\n" + data + "\r\n"
        "0\r\n\r\n";
    FakeServer fake(12103, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12103/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body.size(), 255u);
}

TEST(ClientRecvTest, ChunkedWithExtensions) {
    // Chunk extensions after `;` must be ignored
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;name=value\r\nhello\r\n"
        "0\r\n\r\n";
    FakeServer fake(12104, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12104/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, "hello");
}

TEST(ClientRecvTest, ChunkedWithTrailers) {
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "0\r\n"
        "X-Trailer: foo\r\n"
        "X-Other: bar\r\n"
        "\r\n";
    FakeServer fake(12105, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12105/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, "hello");
}

TEST(ClientRecvTest, ChunkedEmptyBody) {
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";
    FakeServer fake(12106, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12106/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, "");
}

TEST(ClientRecvTest, ChunkedBinaryData) {
    stl::string data;
    data.push_back('\x00');
    data.push_back('\xff');
    data.push_back('\r');
    data.push_back('\n');
    data.push_back('\x7f');
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n" + data + "\r\n"
        "0\r\n\r\n";
    FakeServer fake(12107, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12107/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body.size(), 5u);
    EXPECT_EQ(result.value().body, data);
}

TEST(ClientRecvTest, ChunkedInvalidHexSize) {
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "zzzz\r\nbody\r\n"
        "0\r\n\r\n";
    FakeServer fake(12108, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12108/");
    auto result = fut.get();
    EXPECT_FALSE(result.has_value());
}

TEST(ClientRecvTest, ChunkedMissingCRLFAfterChunk) {
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhelloXX" // XX instead of CRLF
        "0\r\n\r\n";
    FakeServer fake(12109, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12109/");
    auto result = fut.get();
    EXPECT_FALSE(result.has_value());
}

TEST(ClientRecvTest, ChunkedTruncatedBeforeTerminator) {
    // Server closes connection mid-chunk
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhel";
    FakeServer fake(12110, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12110/");
    auto result = fut.get();
    EXPECT_FALSE(result.has_value());
}

TEST(ClientRecvTest, ChunkedTruncatedBeforeFinalZero) {
    // Body chunk completes but connection drops before terminator
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n";
    FakeServer fake(12111, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12111/");
    auto result = fut.get();
    EXPECT_FALSE(result.has_value());
}

TEST(ClientRecvTest, ChunkedExceedsMaxResponseSize) {
    auto saved = sap::http::Client::max_response_size;
    sap::http::Client::max_response_size = 100;

    stl::string data(200, 'A');
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "c8\r\n" + data + "\r\n" // c8 = 200
        "0\r\n\r\n";
    FakeServer fake(12112, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12112/");
    auto result = fut.get();
    sap::http::Client::max_response_size = saved;
    EXPECT_FALSE(result.has_value());
}

TEST(ClientRecvTest, ChunkedManySmallChunks) {
    stl::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    stl::string expected;
    for (int i = 0; i < 50; ++i) {
        response += "1\r\nA\r\n";
        expected += "A";
    }
    response += "0\r\n\r\n";
    FakeServer fake(12113, std::move(response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12113/");
    auto result = fut.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, expected);
}
