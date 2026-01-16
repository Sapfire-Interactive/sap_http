#include "sap_http/net/http.h"
#include <gtest/gtest.h>

TEST(IntegrationTest, HttpBinGet) {
  auto future = sap::http::Client::get("http://httpbingo.org/get");
  auto result = future.get();

  ASSERT_TRUE(result.has_value());
  auto &response = result.value();
  EXPECT_TRUE(response.is_success());
  EXPECT_EQ(response.status_code, 200);
  EXPECT_FALSE(response.body.empty());
}

TEST(IntegrationTest, HttpBinPost) {
  auto future =
      sap::http::Client::post("http://httpbingo.org/post", R"({"test": "data"})");
  auto result = future.get();

  ASSERT_TRUE(result.has_value());
  auto &response = result.value();
  EXPECT_TRUE(response.is_success());
  EXPECT_EQ(response.status_code, 200);
}

TEST(IntegrationTest, HttpBinStatus404) {
  auto future = sap::http::Client::get("http://httpbingo.org/status/404");
  auto result = future.get();

  ASSERT_TRUE(result.has_value());
  auto &response = result.value();
  EXPECT_FALSE(response.is_success());
  EXPECT_EQ(response.status_code, 404);
}

TEST(IntegrationTest, HttpBinHeaders) {
  auto url_result = sap::http::URL::parse("http://httpbingo.org/headers");
  ASSERT_TRUE(url_result.has_value());

  sap::http::Request req(sap::http::EMethod::GET, std::move(url_result.value()));
  req.set_header("X-Custom-Header", "test-value");

  auto future = sap::http::Client::async_send(std::move(req));
  auto result = future.get();

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value().is_success());
}

TEST(IntegrationTest, ServerClientIntegration) {
  sap::http::ServerConfig cfg{-1, "127.0.0.1", 9999};
  sap::http::Server server{std::move(cfg)};
  server.route("/test", sap::http::EMethod::GET, [](const sap::http::Request &) {
    return sap::http::Response(200, "Integration test response");
  });
  auto start_result = server.start();
  ASSERT_TRUE(start_result.has_value())
      << "Server failed to start: " << start_result.error();
  std::thread server_thread([&server]() { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto future = sap::http::Client::get("http://127.0.0.1:9999/test");
  auto result = future.get();
  server.stop();
  server_thread.join();
  ASSERT_TRUE(result.has_value())
      << "Client request failed: " << result.error();
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().body, "Integration test response");
}

TEST(IntegrationTest, ServerPostRequest) {
  sap::http::ServerConfig cfg{-1, "127.0.0.1", 10000};
  sap::http::Server server{std::move(cfg)};
  server.route("/api/echo", sap::http::EMethod::POST,
               [](const sap::http::Request &req) {
                 sap::http::Response resp(200, req.body);
                 resp.headers.set("Content-Type", "application/json");
                 return resp;
               });
  auto start_result = server.start();
  ASSERT_TRUE(start_result.has_value())
      << "Server failed to start: " << start_result.error();
  std::thread server_thread([&server]() { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto future = sap::http::Client::post("http://127.0.0.1:10000/api/echo",
                                   R"({"test": "data"})");
  auto result = future.get();
  server.stop();
  server_thread.join();
  ASSERT_TRUE(result.has_value())
      << "Client request failed: " << result.error();
  EXPECT_EQ(result.value().body, R"({"test": "data"})");
}