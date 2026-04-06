#include <gtest/gtest.h>
#include "sap_http/net/http.h"

TEST(ServerTest, CreateServer) {
    sap::http::ServerConfig cfg{-1, "127.0.0.1", 8080, false};
    sap::http::Server server(std::move(cfg));
    SUCCEED();
}

TEST(ServerTest, AddRoute) {
    sap::http::Server server;
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request& req) { return sap::http::Response(200, "Test response"); });
    SUCCEED();
}

TEST(ServerTest, RouteHandlerWithJSON) {
    sap::http::Server server;
    server.route("/api/data", sap::http::EMethod::POST, [](const sap::http::Request& req) {
        // Simulate JSON processing
        if (req.headers.get("Content-Type") == "application/json") {
            return sap::http::Response(201, R"({"status": "created"})");
        }
        return sap::http::Response(400, "Bad Request");
    });

    SUCCEED();
}

TEST(ServerTest, MultipleRoutes) {
    sap::http::Server server;
    server.route("/", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(200, "Home"); });
    server.route("/api/users", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(200, R"([{"id": 1, "name": "John"}])"); });
    server.route("/api/users", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(201, "User created"); });
    SUCCEED();
}

TEST(ServerTest, MultithreadedMode) {
    sap::http::ServerConfig cfg{-1, "127.0.0.1", 8081, true};
    sap::http::Server server{std::move(cfg)};
    SUCCEED();
}