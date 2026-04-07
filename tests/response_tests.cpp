#include <gtest/gtest.h>
#include "sap_http/net/http.h"

TEST(ResponseTest, IsSuccessFor2xx) {
    sap::http::Response resp;
    resp.status_code = sap::http::EStatusCode::OK;
    EXPECT_TRUE(resp.is_success());

    resp.status_code = sap::http::EStatusCode::Created;
    EXPECT_TRUE(resp.is_success());

    resp.status_code = sap::http::EStatusCode::NoContent;
    EXPECT_TRUE(resp.is_success());

    resp.status_code = static_cast<sap::http::EStatusCode>(299);
    EXPECT_TRUE(resp.is_success());
}

TEST(ResponseTest, IsSuccessForNon2xx) {
    sap::http::Response resp;
    resp.status_code = static_cast<sap::http::EStatusCode>(199);
    EXPECT_FALSE(resp.is_success());

    resp.status_code = sap::http::EStatusCode::MultipleChoices;
    EXPECT_FALSE(resp.is_success());

    resp.status_code = sap::http::EStatusCode::NotFound;
    EXPECT_FALSE(resp.is_success());

    resp.status_code = sap::http::EStatusCode::InternalServerError;
    EXPECT_FALSE(resp.is_success());
}

TEST(ResponseTest, ConstructorWithBody) {
    sap::http::Response resp(sap::http::EStatusCode::OK, "Hello World");
    EXPECT_EQ(resp.status_code, sap::http::EStatusCode::OK);
    EXPECT_EQ(resp.body, "Hello World");
    EXPECT_EQ(resp.headers.get("Content-Length"), "11");
}