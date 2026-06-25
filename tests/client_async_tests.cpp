#include <gtest/gtest.h>

#include "sap_http/net/http.h"

#include <sap_core/async/executor.h>
#include <sap_core/async/spawn.h>
#include <sap_core/async/task.h>
#include <sap_network/tcp_socket.h>

#include <chrono>
#include <string>
#include <thread>

using namespace sap::http;
using sap::async::Task;
namespace netw = sap::network;

namespace {

    constexpr u16 PORT_CGET   = 12200;
    constexpr u16 PORT_CPOST  = 12201;
    constexpr u16 PORT_CERR   = 12202;
    constexpr u16 PORT_CCHUNK = 12203;

    stl::thread start_async_server(HttpServerAsync& server) {
        auto res = server.start();
        EXPECT_TRUE(res.has_value()) << "Server failed to start: " << res.error();
        stl::thread t([&server]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return t;
    }

    void route_shutdown(HttpServerAsync& server) {
        server.public_route("/_shutdown", EMethod::GET, [&server](const Request&) -> Task<Response> {
            server.stop();
            co_return Response(EStatusCode::OK, "bye");
        });
    }

    void trigger_shutdown(u16 port) {
        netw::SocketConfig sc;
        sc.host            = "127.0.0.1";
        sc.port            = port;
        sc.connect_timeout = std::chrono::milliseconds{2000};
        sc.recv_timeout    = std::chrono::milliseconds{2000};
        sc.send_timeout    = std::chrono::milliseconds{2000};
        netw::TCPSocket sock(stl::move(sc));
        if (!sock.connect())
            return;
        std::string req = "GET /_shutdown HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        (void)sock.send(stl::span<const stl::byte>(reinterpret_cast<const stl::byte*>(req.data()), req.size()));
        stl::byte buf[256];
        while (true) {
            auto n = sock.recv(stl::span<stl::byte>(buf, sizeof(buf)));
            if (!n || n.value() == 0)
                break;
        }
    }

    std::string url_for(u16 port, std::string_view path) {
        return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
    }

} // namespace

TEST(ClientAsyncTest, GetReturnsBody) {
    auto _sr = HttpServerAsync::create({.host = "127.0.0.1", .port = PORT_CGET}); ASSERT_TRUE(_sr.has_value());
    auto& server = _sr.value();
    server.route("/hello", EMethod::GET, [](const Request&) -> Task<Response> { co_return Response(EStatusCode::OK, "hi there"); });
    route_shutdown(server);
    auto t = start_async_server(server);

    auto ex_r = sap::async::Executor::create();
    ASSERT_TRUE(ex_r.has_value());
    auto& ex = ex_r.value();
    HttpClientAsync client(ex);

    auto handle = sap::async::spawn(ex, client.get(url_for(PORT_CGET, "/hello")));
    auto result = sap::async::sync_wait(stl::move(handle));
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(static_cast<int>(result.value().status_code), 200);
    EXPECT_EQ(result.value().body, "hi there");

    trigger_shutdown(PORT_CGET);
    if (t.joinable())
        t.join();
}

TEST(ClientAsyncTest, PostEchoesBody) {
    auto _sr = HttpServerAsync::create({.host = "127.0.0.1", .port = PORT_CPOST}); ASSERT_TRUE(_sr.has_value());
    auto& server = _sr.value();
    server.route("/echo", EMethod::POST, [](Request req) -> Task<Response> { co_return Response(EStatusCode::OK, req.body); });
    route_shutdown(server);
    auto t = start_async_server(server);

    auto ex_r = sap::async::Executor::create();
    ASSERT_TRUE(ex_r.has_value());
    auto& ex = ex_r.value();
    HttpClientAsync client(ex);

    auto handle = sap::async::spawn(ex, client.post(url_for(PORT_CPOST, "/echo"), "ping pong"));
    auto result = sap::async::sync_wait(stl::move(handle));
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(static_cast<int>(result.value().status_code), 200);
    EXPECT_EQ(result.value().body, "ping pong");

    trigger_shutdown(PORT_CPOST);
    if (t.joinable())
        t.join();
}

TEST(ClientAsyncTest, MalformedUrlReturnsError) {
    auto ex_r = sap::async::Executor::create();
    ASSERT_TRUE(ex_r.has_value());
    auto& ex = ex_r.value();
    HttpClientAsync client(ex);

    auto handle = sap::async::spawn(ex, client.get("not a url"));
    auto result = sap::async::sync_wait(stl::move(handle));
    EXPECT_FALSE(result.has_value());
}

TEST(ClientAsyncTest, ChunkedResponseRejected) {
    netw::SocketConfig sc;
    sc.host       = "127.0.0.1";
    sc.port       = PORT_CCHUNK;
    sc.reuse_addr = true;
    netw::TCPSocket listener(stl::move(sc));
    ASSERT_TRUE(listener.bind());
    ASSERT_TRUE(listener.listen());

    stl::thread server_t([&listener]() {
        auto conn = listener.accept();
        if (!conn)
            return;
        auto& sock = conn.value();
        stl::byte buf[4096];
        (void)sock.recv(stl::span<stl::byte>(buf, sizeof(buf)));
        std::string resp =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
        (void)sock.send(stl::span<const stl::byte>(reinterpret_cast<const stl::byte*>(resp.data()), resp.size()));
    });

    auto ex_r = sap::async::Executor::create();
    ASSERT_TRUE(ex_r.has_value());
    auto& ex = ex_r.value();
    HttpClientAsync client(ex);

    auto handle = sap::async::spawn(ex, client.get(url_for(PORT_CCHUNK, "/")));
    auto result = sap::async::sync_wait(stl::move(handle));
    EXPECT_FALSE(result.has_value());
    if (!result)
        EXPECT_NE(stl::string(result.error()).find("Chunked"), stl::string::npos);

    if (server_t.joinable())
        server_t.join();
}

TEST(ClientAsyncTest, ConnectFailureReturnsError) {
    auto ex_r = sap::async::Executor::create();
    ASSERT_TRUE(ex_r.has_value());
    auto& ex = ex_r.value();
    HttpClientAsync client(ex);

    auto handle = sap::async::spawn(ex, client.get(url_for(PORT_CERR, "/nope")));
    auto result = sap::async::sync_wait(stl::move(handle));
    EXPECT_FALSE(result.has_value());
}
