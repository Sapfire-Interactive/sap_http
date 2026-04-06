#include <gtest/gtest.h>
#include "sap_http/net/http.h"

TEST(ClientTest, InvalidUrlGet) {
    auto future = sap::http::Client::get("not-a-valid-url");
    auto result = future.get();

    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.has_error());
}

TEST(ClientTest, InvalidUrlPost) {
    auto future = sap::http::Client::post("not-a-valid-url", "body");
    auto result = future.get();

    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.has_error());
}