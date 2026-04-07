#include <gtest/gtest.h>
#include "sap_http/net/http.h"

TEST(ServerTest, CreateServer) {
    sap::http::ServerConfig cfg{"127.0.0.1", 8080, false};
    sap::http::Server server(std::move(cfg));
    SUCCEED();
}

TEST(ServerTest, AddRoute) {
    sap::http::Server server;
    server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::OK, "Test response"); });
    SUCCEED();
}

TEST(ServerTest, RouteHandlerWithJSON) {
    sap::http::Server server;
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
    sap::http::Server server;
    server.route("/", sap::http::EMethod::GET, [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "Home"); });
    server.route("/api/users", sap::http::EMethod::GET,
                 [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, R"([{"id": 1, "name": "John"}])"); });
    server.route("/api/users", sap::http::EMethod::POST,
                 [](const sap::http::Request& req) { return sap::http::Response(sap::http::EStatusCode::Created, "User created"); });
    SUCCEED();
}

TEST(ServerTest, MultithreadedMode) {
    sap::http::ServerConfig cfg{"127.0.0.1", 8081, true};
    sap::http::Server server{std::move(cfg)};
    SUCCEED();
}