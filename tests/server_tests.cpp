#include <gtest/gtest.h>
#include "sap_http/net/http.h"

TEST(ServerTest, CreateServer) {
    sap::http::HttpServerConfig cfg{"127.0.0.1", 8080, false};
    sap::http::HttpServer server(std::move(cfg));
    SUCCEED();
}

TEST(ServerTest, AddRoute) {
    sap::http::HttpServer server;
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, "Test response"); });
    SUCCEED();
}

TEST(ServerTest, RouteHandlerWithJSON) {
    sap::http::HttpServer server;
    server.route("/api/data", sap::http::EMethod::POST, [](const sap::http::Request& req) {
        // Simulate JSON processing
        if (req.headers.get("Content-Type") == "application/json") {
            return sap::http::Response(sap::http::EStatusCode::Created, R"({"status": "created"})");
        }
        return sap::http::Response(sap::http::EStatusCode::BadRequest, "Bad Request");
    });

    SUCCEED();
}

TEST(ServerTest, MultipleRoutes) {
    sap::http::HttpServer server;
    server.route("/", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "Home"); });
    server.route("/api/users", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, R"([{"id": 1, "name": "John"}])"); });
    server.route("/api/users", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::Created, "User created"); });
    SUCCEED();
}

TEST(ServerTest, MultithreadedMode) {
    sap::http::HttpServerConfig cfg{"127.0.0.1", 8081, true};
    sap::http::HttpServer server{std::move(cfg)};
    SUCCEED();
}

// ----------------------------------------------------------------------------
// Registration tests for Task<Response> handlers.
// These are compile-time checks: they prove route()/public_route() instantiate
// for each accepted signature. No server is started.
// ----------------------------------------------------------------------------

#include <sap_core/async/task.h>

TEST(ServerTest, AddRouteAsyncByValue) {
    sap::http::HttpServer server;
    server.route("/async", sap::http::EMethod::GET,
                 [](sap::http::Request) -> sap::async::Task<sap::http::Response> {
                     co_return sap::http::Response(sap::http::EStatusCode::OK, "ok");
                 });
    SUCCEED();
}

TEST(ServerTest, AddRouteAsyncByConstRef) {
    sap::http::HttpServer server;
    server.route("/async-ref", sap::http::EMethod::GET,
                 [](const sap::http::Request&) -> sap::async::Task<sap::http::Response> {
                     co_return sap::http::Response(sap::http::EStatusCode::OK, "ok");
                 });
    SUCCEED();
}

TEST(ServerTest, AddPublicRouteAsyncByValue) {
    sap::http::HttpServer server;
    server.public_route("/login", sap::http::EMethod::POST,
                        [](sap::http::Request) -> sap::async::Task<sap::http::Response> {
                            co_return sap::http::Response(sap::http::EStatusCode::OK, "welcome");
                        });
    SUCCEED();
}

TEST(ServerTest, AddPublicRouteAsyncByConstRef) {
    sap::http::HttpServer server;
    server.public_route("/login", sap::http::EMethod::POST,
                        [](const sap::http::Request&) -> sap::async::Task<sap::http::Response> {
                            co_return sap::http::Response(sap::http::EStatusCode::OK, "welcome");
                        });
    SUCCEED();
}

TEST(ServerTest, MixedSyncAndAsyncHandlersRegister) {
    sap::http::HttpServer server;
    server.route("/sync", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "sync"); });
    server.route("/async", sap::http::EMethod::POST,
                 [](sap::http::Request) -> sap::async::Task<sap::http::Response> {
                     co_return sap::http::Response(sap::http::EStatusCode::Created, "async");
                 });
    SUCCEED();
}

TEST(ServerTest, MultipleAsyncRoutesDifferentMethods) {
    sap::http::HttpServer server;
    server.route("/r", sap::http::EMethod::GET,
                 [](sap::http::Request) -> sap::async::Task<sap::http::Response> {
                     co_return sap::http::Response(sap::http::EStatusCode::OK, "get");
                 });
    server.route("/r", sap::http::EMethod::POST,
                 [](sap::http::Request) -> sap::async::Task<sap::http::Response> {
                     co_return sap::http::Response(sap::http::EStatusCode::OK, "post");
                 });
    server.route("/r", sap::http::EMethod::DELETE,
                 [](sap::http::Request) -> sap::async::Task<sap::http::Response> {
                     co_return sap::http::Response(sap::http::EStatusCode::NoContent, "");
                 });
    SUCCEED();
}

TEST(ServerTest, AsyncHandlerCanCoAwaitInnerTask) {
    auto inner = []() -> sap::async::Task<int> { co_return 42; };

    sap::http::HttpServer server;
    server.route("/await", sap::http::EMethod::GET,
                 [inner](sap::http::Request) -> sap::async::Task<sap::http::Response> {
                     int n = co_await inner();
                     co_return sap::http::Response(sap::http::EStatusCode::OK, std::to_string(n));
                 });
    SUCCEED();
}

TEST(ServerTest, AsyncHandlerWithLambdaCaptures) {
    std::string greeting = "hello";
    sap::http::HttpServer server;
    server.route("/g", sap::http::EMethod::GET,
                 [greeting](sap::http::Request) -> sap::async::Task<sap::http::Response> {
                     co_return sap::http::Response(sap::http::EStatusCode::OK, greeting);
                 });
    SUCCEED();
}