#include <gtest/gtest.h>
#include "sap_http/net/http.h"

TEST(RequestTest, DefaultConstructor) {
    auto url_result = sap::http::URL::parse("http://example.com/api");
    ASSERT_TRUE(url_result.has_value());

    sap::http::Request req(sap::http::EMethod::GET, std::move(url_result.value()));
    EXPECT_EQ(req.method, sap::http::EMethod::GET);
    EXPECT_EQ(req.url.host, "example.com");
}

TEST(RequestTest, SetHeader) {
    auto url_result = sap::http::URL::parse("http://example.com/api");
    ASSERT_TRUE(url_result.has_value());

    sap::http::Request req(sap::http::EMethod::GET, std::move(url_result.value()));
    req.set_header("Authorization", "Bearer token123");

    EXPECT_EQ(req.headers.get("Authorization"), "Bearer token123");
}

TEST(RequestTest, SetBody) {
    auto url_result = sap::http::URL::parse("http://example.com/api");
    ASSERT_TRUE(url_result.has_value());

    sap::http::Request req(sap::http::EMethod::POST, std::move(url_result.value()));
    req.set_body("test body content");

    EXPECT_EQ(req.body, "test body content");
    EXPECT_EQ(req.headers.get("Content-Length"), "17");
}

TEST(RequestTest, SetJsonBody) {
    auto url_result = sap::http::URL::parse("http://example.com/api");
    ASSERT_TRUE(url_result.has_value());

    sap::http::Request req(sap::http::EMethod::POST, std::move(url_result.value()));
    req.set_header("Content-Type", "application/json");
    req.set_body(R"({"key": "value"})");

    EXPECT_EQ(req.headers.get("Content-Type"), "application/json");
    EXPECT_EQ(req.body, R"({"key": "value"})");
}

TEST(RequestTest, MethodToString) {
    EXPECT_EQ(sap::http::method_to_string(sap::http::EMethod::GET), "GET");
    EXPECT_EQ(sap::http::method_to_string(sap::http::EMethod::POST), "POST");
    EXPECT_EQ(sap::http::method_to_string(sap::http::EMethod::PUT), "PUT");
    EXPECT_EQ(sap::http::method_to_string(sap::http::EMethod::DELETE), "DELETE");
    EXPECT_EQ(sap::http::method_to_string(sap::http::EMethod::HEAD), "HEAD");
    EXPECT_EQ(sap::http::method_to_string(sap::http::EMethod::PATCH), "PATCH");
    EXPECT_EQ(sap::http::method_to_string(sap::http::EMethod::OPTIONS), "OPTIONS");
}

TEST(RequestTest, StringToMethod) {
    EXPECT_EQ(sap::http::string_to_method("GET"), sap::http::EMethod::GET);
    EXPECT_EQ(sap::http::string_to_method("POST"), sap::http::EMethod::POST);
    EXPECT_EQ(sap::http::string_to_method("PUT"), sap::http::EMethod::PUT);
    EXPECT_EQ(sap::http::string_to_method("DELETE"), sap::http::EMethod::DELETE);
    EXPECT_EQ(sap::http::string_to_method("HEAD"), sap::http::EMethod::HEAD);
    EXPECT_EQ(sap::http::string_to_method("PATCH"), sap::http::EMethod::PATCH);
    EXPECT_EQ(sap::http::string_to_method("OPTIONS"), sap::http::EMethod::OPTIONS);
}

TEST(RequestTest, DefaultHeaders) {
    auto url_result = sap::http::URL::parse("http://example.com/api");
    ASSERT_TRUE(url_result.has_value());

    sap::http::Request req(sap::http::EMethod::GET, std::move(url_result.value()));

    EXPECT_TRUE(req.headers.has("User-Agent"));
    EXPECT_TRUE(req.headers.has("Accept"));
}

TEST(RequestTest, EmptyBody) {
    auto url_result = sap::http::URL::parse("http://example.com/api");
    ASSERT_TRUE(url_result.has_value());

    sap::http::Request req(sap::http::EMethod::GET, std::move(url_result.value()));
    EXPECT_TRUE(req.body.empty());
}