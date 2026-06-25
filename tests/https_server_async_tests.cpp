#include <gtest/gtest.h>

#include "sap_http/net/http.h"
#include "self_signed_cert.h"

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

    constexpr u16 PORT_HSA_GET   = 12300;
    constexpr u16 PORT_HSA_POST  = 12301;
    constexpr u16 PORT_HSA_ROUND = 12302;

    HttpsServerConfig make_cfg(u16 port, const SelfSignedCert& cert) {
        HttpsServerConfig cfg;
        cfg.host                  = "127.0.0.1";
        cfg.port                  = port;
        cfg.tls_cfg.cert_file     = cert.cert_file;
        cfg.tls_cfg.key_file      = cert.key_file;
        cfg.tls_cfg.alpn_protocols.emplace_back("http/1.1");
        return cfg;
    }

    stl::thread start(HttpsServerAsync& server) {
        auto res = server.start();
        EXPECT_TRUE(res.has_value()) << "HttpsServerAsync failed to start: " << res.error();
        stl::thread t([&server]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        return t;
    }

    void route_shutdown(HttpsServerAsync& server) {
        server.public_route("/_shutdown", EMethod::GET, [&server](const Request&) -> Task<Response> {
            server.stop();
            co_return Response(EStatusCode::OK, "bye");
        });
    }

    void trigger_shutdown(u16 port, const SelfSignedCert& cert) {
        netw::TlsClientConfig tc;
        tc.tcp.host            = "127.0.0.1";
        tc.tcp.port            = port;
        tc.tcp.connect_timeout = std::chrono::milliseconds{2000};
        tc.sni_hostname        = "localhost";
        tc.verify_peer         = true;
        tc.verify_hostname     = true;
        tc.ca_file             = cert.cert_file;
        tc.alpn_protocols.emplace_back("http/1.1");
        netw::TLSSocket sock(stl::move(tc));
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

    HttpsClientConfig make_client_cfg(const SelfSignedCert& cert) {
        HttpsClientConfig cfg;
        cfg.verify_peer     = true;
        cfg.verify_hostname = true;
        cfg.ca_file         = cert.cert_file;
        cfg.alpn_protocols.emplace_back("http/1.1");
        return cfg;
    }

    std::string url_for(u16 port, std::string_view path) {
        return "https://localhost:" + std::to_string(port) + std::string(path);
    }

} // namespace

TEST(HttpsServerAsyncTest, GetReturnsBodyOverTls) {
    SelfSignedCert cert;
    auto _sr = HttpsServerAsync::create(make_cfg(PORT_HSA_GET, cert)); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.route("/hello", EMethod::GET, [](const Request&) -> Task<Response> { co_return Response(EStatusCode::OK, "secure hi"); });
    route_shutdown(server);
    auto t = start(server);

    auto ex_r = sap::async::Executor::create();
    ASSERT_TRUE(ex_r.has_value());
    auto& ex = ex_r.value();
    HttpsClientAsync client(ex, make_client_cfg(cert));

    auto handle = sap::async::spawn(ex, client.get(url_for(PORT_HSA_GET, "/hello")));
    auto result = sap::async::sync_wait(stl::move(handle));
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(static_cast<int>(result.value().status_code), 200);
    EXPECT_EQ(result.value().body, "secure hi");

    trigger_shutdown(PORT_HSA_GET, cert);
    if (t.joinable())
        t.join();
}

TEST(HttpsServerAsyncTest, PostEchoesBodyOverTls) {
    SelfSignedCert cert;
    auto _sr = HttpsServerAsync::create(make_cfg(PORT_HSA_POST, cert)); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.route("/echo", EMethod::POST, [](Request req) -> Task<Response> { co_return Response(EStatusCode::OK, req.body); });
    route_shutdown(server);
    auto t = start(server);

    auto ex_r = sap::async::Executor::create();
    ASSERT_TRUE(ex_r.has_value());
    auto& ex = ex_r.value();
    HttpsClientAsync client(ex, make_client_cfg(cert));

    auto handle = sap::async::spawn(ex, client.post(url_for(PORT_HSA_POST, "/echo"), "encrypted pong"));
    auto result = sap::async::sync_wait(stl::move(handle));
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(static_cast<int>(result.value().status_code), 200);
    EXPECT_EQ(result.value().body, "encrypted pong");

    trigger_shutdown(PORT_HSA_POST, cert);
    if (t.joinable())
        t.join();
}

TEST(HttpsServerAsyncTest, RouteParamOverTls) {
    SelfSignedCert cert;
    auto _sr = HttpsServerAsync::create(make_cfg(PORT_HSA_ROUND, cert)); ASSERT_TRUE(_sr.has_value()); auto& server = _sr.value();
    server.route("/users/:id", EMethod::GET,
                 [](Request req) -> Task<Response> { co_return Response(EStatusCode::OK, "user=" + req.params["id"]); });
    route_shutdown(server);
    auto t = start(server);

    auto ex_r = sap::async::Executor::create();
    ASSERT_TRUE(ex_r.has_value());
    auto& ex = ex_r.value();
    HttpsClientAsync client(ex, make_client_cfg(cert));

    auto handle = sap::async::spawn(ex, client.get(url_for(PORT_HSA_ROUND, "/users/77")));
    auto result = sap::async::sync_wait(stl::move(handle));
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(static_cast<int>(result.value().status_code), 200);
    EXPECT_EQ(result.value().body, "user=77");

    trigger_shutdown(PORT_HSA_ROUND, cert);
    if (t.joinable())
        t.join();
}
