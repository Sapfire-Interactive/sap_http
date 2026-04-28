#include <gtest/gtest.h>
#include "sap_http/net/http.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

    struct ServerFixture {
        sap::http::HttpServer server;
        stl::thread thread;

        ServerFixture(u16 port) : server(sap::http::HttpServerConfig{"127.0.0.1", port, false}) {}

        void run() {
            auto start = server.start();
            EXPECT_TRUE(start.has_value()) << "start failed: " << start.error();
            thread = stl::thread([this]() { server.run(); });
            // Give the accept loop a moment to come up before the client hits it.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        ~ServerFixture() {
            server.stop();
            if (thread.joinable())
                thread.join();
        }
    };

} // namespace

// Middleware that short-circuits must prevent the route handler from running,
// and the caller must see the middleware's response.
TEST(MiddlewareTest, ShortCircuitBlocksHandler) {
    ServerFixture fx(11000);
    std::atomic<int> handler_calls{0};

    fx.server.use([](sap::http::Request&) -> std::optional<sap::http::Response> {
        return sap::http::Response(sap::http::EStatusCode::Unauthorized, "blocked");
    });
    fx.server.route("/secret", sap::http::EMethod::GET,
                    [&](const sap::http::Request&) {
                        handler_calls.fetch_add(1);
                        return sap::http::Response(sap::http::EStatusCode::OK, "reached handler");
                    });
    fx.run();

    auto resp = sap::http::Client::get("http://127.0.0.1:11000/secret").get();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp.value().status_code, sap::http::EStatusCode::Unauthorized);
    EXPECT_EQ(resp.value().body, "blocked");
    EXPECT_EQ(handler_calls.load(), 0);
}

// Middleware that returns nullopt lets routing continue and the handler runs.
TEST(MiddlewareTest, PassThroughCallsHandler) {
    ServerFixture fx(11001);

    fx.server.use([](sap::http::Request&) -> std::optional<sap::http::Response> { return std::nullopt; });
    fx.server.route("/open", sap::http::EMethod::GET,
                    [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "ok"); });
    fx.run();

    auto resp = sap::http::Client::get("http://127.0.0.1:11001/open").get();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp.value().status_code, sap::http::EStatusCode::OK);
    EXPECT_EQ(resp.value().body, "ok");
}

// Routes registered via public_route() must skip all middleware, even
// short-circuiting middleware. Useful for /auth/login style endpoints.
TEST(MiddlewareTest, PublicRouteSkipsMiddleware) {
    ServerFixture fx(11002);
    std::atomic<int> mw_calls{0};
    std::atomic<int> handler_calls{0};

    fx.server.use([&](sap::http::Request&) -> std::optional<sap::http::Response> {
        mw_calls.fetch_add(1);
        return sap::http::Response(sap::http::EStatusCode::Unauthorized, "blocked");
    });
    fx.server.public_route("/login", sap::http::EMethod::POST,
                           [&](const sap::http::Request&) {
                               handler_calls.fetch_add(1);
                               return sap::http::Response(sap::http::EStatusCode::OK, "welcome");
                           });
    fx.run();

    auto resp = sap::http::Client::post("http://127.0.0.1:11002/login", "").get();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp.value().status_code, sap::http::EStatusCode::OK);
    EXPECT_EQ(resp.value().body, "welcome");
    EXPECT_EQ(mw_calls.load(), 0);
    EXPECT_EQ(handler_calls.load(), 1);
}

// When a route is registered normally alongside a public one, the regular route
// still goes through middleware. This confirms skip is per-route, not global.
TEST(MiddlewareTest, PublicAndProtectedCoexist) {
    ServerFixture fx(11003);

    fx.server.use([](sap::http::Request&) -> std::optional<sap::http::Response> {
        return sap::http::Response(sap::http::EStatusCode::Unauthorized, "blocked");
    });
    fx.server.public_route("/public", sap::http::EMethod::GET,
                           [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "pub"); });
    fx.server.route("/protected", sap::http::EMethod::GET,
                    [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "prot"); });
    fx.run();

    auto pub = sap::http::Client::get("http://127.0.0.1:11003/public").get();
    ASSERT_TRUE(pub.has_value());
    EXPECT_EQ(pub.value().status_code, sap::http::EStatusCode::OK);
    EXPECT_EQ(pub.value().body, "pub");

    auto prot = sap::http::Client::get("http://127.0.0.1:11003/protected").get();
    ASSERT_TRUE(prot.has_value());
    EXPECT_EQ(prot.value().status_code, sap::http::EStatusCode::Unauthorized);
    EXPECT_EQ(prot.value().body, "blocked");
}

// Middleware may mutate the request (e.g. stash the authenticated user's id
// in params for handlers to consume). The route handler must observe those
// mutations. Also confirms path params are populated before middleware runs,
// so middleware can authorize based on path-param ids.
TEST(MiddlewareTest, MutatesRequestForHandler) {
    ServerFixture fx(11004);

    fx.server.use([](sap::http::Request& req) -> std::optional<sap::http::Response> {
        req.params["_user_id"] = "42";
        return std::nullopt;
    });
    fx.server.route("/projects/:id", sap::http::EMethod::GET, [](const sap::http::Request& req) {
        auto path_id = req.params.find("id");
        auto user_id = req.params.find("_user_id");
        EXPECT_NE(path_id, req.params.end());
        EXPECT_NE(user_id, req.params.end());
        stl::string body = stl::string("project=") + path_id->second + stl::string(";user=") + user_id->second;
        return sap::http::Response(sap::http::EStatusCode::OK, body);
    });
    fx.run();

    auto resp = sap::http::Client::get("http://127.0.0.1:11004/projects/7").get();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp.value().status_code, sap::http::EStatusCode::OK);
    EXPECT_EQ(resp.value().body, "project=7;user=42");
}

// Multiple middleware run in registration order. The first one to short-circuit
// wins; later ones don't execute.
TEST(MiddlewareTest, FirstShortCircuitWins) {
    ServerFixture fx(11005);
    std::atomic<int> second_called{0};

    fx.server.use([](sap::http::Request&) -> std::optional<sap::http::Response> {
        return sap::http::Response(sap::http::EStatusCode::Forbidden, "first");
    });
    fx.server.use([&](sap::http::Request&) -> std::optional<sap::http::Response> {
        second_called.fetch_add(1);
        return sap::http::Response(sap::http::EStatusCode::Unauthorized, "second");
    });
    fx.server.route("/x", sap::http::EMethod::GET,
                    [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "x"); });
    fx.run();

    auto resp = sap::http::Client::get("http://127.0.0.1:11005/x").get();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp.value().status_code, sap::http::EStatusCode::Forbidden);
    EXPECT_EQ(resp.value().body, "first");
    EXPECT_EQ(second_called.load(), 0);
}
