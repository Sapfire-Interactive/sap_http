#include <gtest/gtest.h>
#include "sap_http/net/http.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

// Minimal raw TCP server that listens on `port`, accepts one connection,
// reads (and discards) the request, then sends the canned `response` bytes
// back. Runs in a background thread; returns immediately after binding.
struct FakeServer {
    int listen_sock = -1;
    std::thread thread;
    std::atomic<bool> done{false};

    FakeServer(u16 port, std::string response) {
        listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bind(listen_sock, (sockaddr*)&addr, sizeof(addr));
        listen(listen_sock, 1);

        thread = std::thread([this, response = std::move(response)]() {
            int client = accept(listen_sock, nullptr, nullptr);
            if (client < 0) return;
            // Drain the request (best-effort, non-blocking-ish)
            char buf[4096];
            // Read until we see end of headers, then bail
            std::string acc;
            while (true) {
                ssize_t n = recv(client, buf, sizeof(buf), 0);
                if (n <= 0) break;
                acc.append(buf, n);
                if (acc.find("\r\n\r\n") != std::string::npos) break;
            }
            ::send(client, response.c_str(), response.size(), 0);
            // Close immediately so client's read loop exits
            close(client);
            done = true;
        });
        // Give the listener a moment to be ready
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~FakeServer() {
        if (thread.joinable()) thread.join();
        if (listen_sock >= 0) close(listen_sock);
    }
};

TEST(ClientRecvTest, MalformedContentLengthDoesNotCrash) {
    std::string bad_response =
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
    std::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 99999999999999999999999\r\n"
        "\r\n";
    FakeServer fake(12002, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12002/");
    auto result = fut.get();

    SUCCEED();
}

TEST(ClientRecvTest, NegativeContentLengthDoesNotCrash) {
    std::string bad_response =
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

    std::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 999999999\r\n"
        "\r\n";
    FakeServer fake(12005, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12005/");
    auto result = fut.get();

    sap::http::Client::max_response_size = saved;

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().find("max_response_size") != std::string::npos)
        << "Got: " << result.error();
}

TEST(ClientRecvTest, AccumulatedBytesExceedingCapIsRejected) {
    // No Content-Length, just streamed bytes — client must still cap based
    // on accumulated buffer size to prevent OOM from chunked / EOF-framed responses.
    auto saved = sap::http::Client::max_response_size;
    sap::http::Client::max_response_size = 512;

    // 4KB body, no Content-Length header — client reads until EOF but should
    // bail once it exceeds the cap
    std::string big_body(4096, 'X');
    std::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n" + big_body;
    FakeServer fake(12006, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12006/");
    auto result = fut.get();

    sap::http::Client::max_response_size = saved;

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().find("max_response_size") != std::string::npos)
        << "Got: " << result.error();
}

TEST(ClientRecvTest, ResponseWithinCapSucceeds) {
    // Sanity: a normal small response under the cap should still work
    auto saved = sap::http::Client::max_response_size;
    sap::http::Client::max_response_size = 10 * 1024;

    std::string body = "hello world";
    std::string response =
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
    std::string bad_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: \r\n"
        "\r\n";
    FakeServer fake(12004, std::move(bad_response));

    auto fut = sap::http::Client::get("http://127.0.0.1:12004/");
    auto result = fut.get();

    SUCCEED();
}
