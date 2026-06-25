#include <gtest/gtest.h>

#include "sap_http/net/http.h"

#include <sap_core/async/sleep_for.h>
#include <sap_network/tcp_socket.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using namespace sap::http;
using sap::async::Task;
using sap::async::sleep_for;
namespace netw = sap::network;

namespace {

    constexpr u16 PORT_GET        = 12100;
    constexpr u16 PORT_POST       = 12101;
    constexpr u16 PORT_PARAM      = 12102;
    constexpr u16 PORT_SLEEP      = 12103;
    constexpr u16 PORT_MIDDLEWARE = 12104;
    constexpr u16 PORT_THROW      = 12105;
    constexpr u16 PORT_NOTFOUND   = 12106;
    constexpr u16 PORT_KEEPALIVE  = 12107;
    constexpr u16 PORT_ASYNC_MW   = 12108;

    stl::thread start_async_server(HttpServerAsync& server) {
        auto res = server.start();
        EXPECT_TRUE(res.has_value()) << "Server failed to start: " << res.error();
        stl::thread t([&server]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return t;
    }

    stl::result<HttpServerAsync> make_server(u16 port) {
        return HttpServerAsync::create({.host = "127.0.0.1", .port = port});
    }

    stl::unique_ptr<netw::TCPSocket> raw_connect(u16 port) {
        netw::SocketConfig sc;
        sc.host            = "127.0.0.1";
        sc.port            = port;
        sc.connect_timeout = std::chrono::milliseconds{2000};
        sc.recv_timeout    = std::chrono::milliseconds{5000};
        sc.send_timeout    = std::chrono::milliseconds{2000};
        auto sock          = stl::make_unique<netw::TCPSocket>(std::move(sc));
        if (!sock->connect())
            return nullptr;
        return sock;
    }

    bool send_all(netw::TCPSocket& sock, std::string_view data) {
        stl::size_t sent = 0;
        while (sent < data.size()) {
            auto n = sock.send(stl::span<const stl::byte>(reinterpret_cast<const stl::byte*>(data.data() + sent), data.size() - sent));
            if (!n || n.value() == 0)
                return false;
            sent += n.value();
        }
        return true;
    }

    std::string recv_until_close(netw::TCPSocket& sock) {
        std::string out;
        stl::byte   buf[4096];
        while (true) {
            auto n = sock.recv(stl::span<stl::byte>(buf, sizeof(buf)));
            if (!n || n.value() == 0)
                break;
            out.append(reinterpret_cast<const char*>(buf), n.value());
        }
        return out;
    }

    void route_shutdown(HttpServerAsync& server) {
        server.public_route("/_shutdown", EMethod::GET, [&server](const Request&) -> Task<Response> {
            server.stop();
            co_return Response(EStatusCode::OK, "bye");
        });
    }

    void trigger_shutdown(u16 port) {
        auto sock = raw_connect(port);
        if (!sock)
            return;
        std::string req = "GET /_shutdown HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        send_all(*sock, req);
        (void)recv_until_close(*sock);
    }

} // namespace

TEST(ServerAsyncTest, GetReturnsBody) {
    auto _sr = make_server(PORT_GET); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.route("/hello", EMethod::GET, [](const Request&) -> Task<Response> { co_return Response(EStatusCode::OK, "hi there"); });
    route_shutdown(server);
    auto t = start_async_server(server);

    auto sock = raw_connect(PORT_GET);
    ASSERT_NE(sock, nullptr);
    ASSERT_TRUE(send_all(*sock, "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    auto resp = recv_until_close(*sock);
    EXPECT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(resp.find("\r\n\r\nhi there"), std::string::npos);

    trigger_shutdown(PORT_GET);
    if (t.joinable())
        t.join();
}

TEST(ServerAsyncTest, PostEchoesBody) {
    auto _sr = make_server(PORT_POST); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.route("/echo", EMethod::POST, [](Request req) -> Task<Response> { co_return Response(EStatusCode::OK, req.body); });
    route_shutdown(server);
    auto t = start_async_server(server);

    auto sock = raw_connect(PORT_POST);
    ASSERT_NE(sock, nullptr);
    std::string body = "ping pong";
    std::string req  = "POST /echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    ASSERT_TRUE(send_all(*sock, req));
    auto resp = recv_until_close(*sock);
    EXPECT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(resp.find("\r\n\r\nping pong"), std::string::npos);

    trigger_shutdown(PORT_POST);
    if (t.joinable())
        t.join();
}

TEST(ServerAsyncTest, RouteParam) {
    auto _sr = make_server(PORT_PARAM); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.route("/users/:id", EMethod::GET,
                 [](Request req) -> Task<Response> { co_return Response(EStatusCode::OK, "user=" + req.params["id"]); });
    route_shutdown(server);
    auto t = start_async_server(server);

    auto sock = raw_connect(PORT_PARAM);
    ASSERT_NE(sock, nullptr);
    ASSERT_TRUE(send_all(*sock, "GET /users/42 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    auto resp = recv_until_close(*sock);
    EXPECT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(resp.find("\r\n\r\nuser=42"), std::string::npos);

    trigger_shutdown(PORT_PARAM);
    if (t.joinable())
        t.join();
}

TEST(ServerAsyncTest, HandlerWithSleep) {
    auto _sr = make_server(PORT_SLEEP); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    auto&           ex = server.executor();
    server.route("/slow", EMethod::GET, [&ex](const Request&) -> Task<Response> {
        co_await sleep_for(ex, std::chrono::milliseconds(30));
        co_return Response(EStatusCode::OK, "done");
    });
    route_shutdown(server);
    auto t = start_async_server(server);

    auto sock = raw_connect(PORT_SLEEP);
    ASSERT_NE(sock, nullptr);
    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(send_all(*sock, "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    auto resp     = recv_until_close(*sock);
    auto elapsed  = std::chrono::steady_clock::now() - start;
    EXPECT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(resp.find("\r\n\r\ndone"), std::string::npos);
    EXPECT_GE(elapsed, std::chrono::milliseconds(25));

    trigger_shutdown(PORT_SLEEP);
    if (t.joinable())
        t.join();
}

TEST(ServerAsyncTest, MiddlewareShortCircuits) {
    auto _sr = make_server(PORT_MIDDLEWARE); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.use([](Request& req) -> std::optional<Response> {
        if (req.headers.get("X-Block") == "1")
            return Response(EStatusCode::BadRequest, "blocked");
        return std::nullopt;
    });
    server.route("/ok", EMethod::GET, [](const Request&) -> Task<Response> { co_return Response(EStatusCode::OK, "passed"); });
    route_shutdown(server);
    auto t = start_async_server(server);

    {
        auto sock = raw_connect(PORT_MIDDLEWARE);
        ASSERT_NE(sock, nullptr);
        ASSERT_TRUE(send_all(*sock, "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nX-Block: 1\r\n\r\n"));
        auto resp = recv_until_close(*sock);
        EXPECT_NE(resp.find("HTTP/1.1 400"), std::string::npos);
        EXPECT_NE(resp.find("\r\n\r\nblocked"), std::string::npos);
    }
    {
        auto sock = raw_connect(PORT_MIDDLEWARE);
        ASSERT_NE(sock, nullptr);
        ASSERT_TRUE(send_all(*sock, "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
        auto resp = recv_until_close(*sock);
        EXPECT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
        EXPECT_NE(resp.find("\r\n\r\npassed"), std::string::npos);
    }

    trigger_shutdown(PORT_MIDDLEWARE);
    if (t.joinable())
        t.join();
}

TEST(ServerAsyncTest, HandlerExceptionReturns500) {
    auto _sr = make_server(PORT_THROW); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.route("/boom", EMethod::GET, [](const Request&) -> Task<Response> {
        throw std::runtime_error("nope");
        co_return Response(EStatusCode::OK, "");
    });
    route_shutdown(server);
    auto t = start_async_server(server);

    auto sock = raw_connect(PORT_THROW);
    ASSERT_NE(sock, nullptr);
    ASSERT_TRUE(send_all(*sock, "GET /boom HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    auto resp = recv_until_close(*sock);
    EXPECT_NE(resp.find("HTTP/1.1 500"), std::string::npos);

    trigger_shutdown(PORT_THROW);
    if (t.joinable())
        t.join();
}

TEST(ServerAsyncTest, UnknownPathReturns404) {
    auto _sr = make_server(PORT_NOTFOUND); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    route_shutdown(server);
    auto t = start_async_server(server);

    auto sock = raw_connect(PORT_NOTFOUND);
    ASSERT_NE(sock, nullptr);
    ASSERT_TRUE(send_all(*sock, "GET /nope HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    auto resp = recv_until_close(*sock);
    EXPECT_NE(resp.find("HTTP/1.1 404"), std::string::npos);

    trigger_shutdown(PORT_NOTFOUND);
    if (t.joinable())
        t.join();
}

TEST(ServerAsyncTest, AsyncMiddlewareCanSuspend) {
    auto _sr = make_server(PORT_ASYNC_MW); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    auto& ex = server.executor();
    server.use([&ex](Request& req) -> Task<std::optional<Response>> {
        co_await sleep_for(ex, std::chrono::milliseconds(20));
        if (req.headers.get("X-Block") == "1")
            co_return Response(EStatusCode::Forbidden, "denied");
        co_return std::nullopt;
    });
    server.route("/ok", EMethod::GET, [](const Request&) -> Task<Response> { co_return Response(EStatusCode::OK, "passed"); });
    route_shutdown(server);
    auto t = start_async_server(server);

    {
        auto sock = raw_connect(PORT_ASYNC_MW);
        ASSERT_NE(sock, nullptr);
        ASSERT_TRUE(send_all(*sock, "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nX-Block: 1\r\n\r\n"));
        auto resp = recv_until_close(*sock);
        EXPECT_NE(resp.find("HTTP/1.1 403"), std::string::npos);
        EXPECT_NE(resp.find("\r\n\r\ndenied"), std::string::npos);
    }
    {
        auto sock = raw_connect(PORT_ASYNC_MW);
        ASSERT_NE(sock, nullptr);
        ASSERT_TRUE(send_all(*sock, "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
        auto resp = recv_until_close(*sock);
        EXPECT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
        EXPECT_NE(resp.find("\r\n\r\npassed"), std::string::npos);
    }

    trigger_shutdown(PORT_ASYNC_MW);
    if (t.joinable())
        t.join();
}

TEST(ServerAsyncTest, KeepAliveAcrossRequests) {
    auto _sr = make_server(PORT_KEEPALIVE); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.route("/ping", EMethod::GET, [](const Request&) -> Task<Response> { co_return Response(EStatusCode::OK, "pong"); });
    route_shutdown(server);
    auto t = start_async_server(server);

    auto sock = raw_connect(PORT_KEEPALIVE);
    ASSERT_NE(sock, nullptr);
    ASSERT_TRUE(send_all(*sock, "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\nGET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    auto resp = recv_until_close(*sock);
    auto first = resp.find("pong");
    ASSERT_NE(first, std::string::npos);
    auto second = resp.find("pong", first + 1);
    EXPECT_NE(second, std::string::npos);

    trigger_shutdown(PORT_KEEPALIVE);
    if (t.joinable())
        t.join();
}
