#include "sap_http/net/http.h"
#include <gtest/gtest.h>

TEST(HeadersTest, SetAndGet) {
  sap::http::Headers h;
  h.set("Content-Type", "application/json");
  EXPECT_EQ(h.get("Content-Type"), "application/json");
}

TEST(HeadersTest, CaseInsensitiveGet) {
  sap::http::Headers h;
  h.set("Content-Type", "application/json");
  EXPECT_EQ(h.get("content-type"), "application/json");
  EXPECT_EQ(h.get("CONTENT-TYPE"), "application/json");
  EXPECT_EQ(h.get("CoNtEnT-tYpE"), "application/json");
}

TEST(HeadersTest, CaseInsensitiveSet) {
  sap::http::Headers h;
  h.set("Content-Type", "text/html");
  h.set("content-type", "application/json");
  EXPECT_EQ(h.get("Content-Type"), "application/json");
}

TEST(HeadersTest, GetNonExistent) {
  sap::http::Headers h;
  EXPECT_EQ(h.get("NonExistent"), "");
}

TEST(HeadersTest, HasHeader) {
  sap::http::Headers h;
  h.set("Authorization", "Bearer token");
  EXPECT_TRUE(h.has("Authorization"));
  EXPECT_TRUE(h.has("authorization"));
  EXPECT_FALSE(h.has("Content-Type"));
}

TEST(HeadersTest, MultipleHeaders) {
  sap::http::Headers h;
  h.set("Content-Type", "application/json");
  h.set("Authorization", "Bearer token");
  h.set("Accept", "*/*");

  EXPECT_EQ(h.get("Content-Type"), "application/json");
  EXPECT_EQ(h.get("Authorization"), "Bearer token");
  EXPECT_EQ(h.get("Accept"), "*/*");
}

TEST(HeadersTest, OverwriteHeader) {
  sap::http::Headers h;
  h.set("Content-Type", "text/html");
  EXPECT_EQ(h.get("Content-Type"), "text/html");

  h.set("Content-Type", "application/json");
  EXPECT_EQ(h.get("Content-Type"), "application/json");
}

TEST(HeadersTest, EmptyValue) {
  sap::http::Headers h;
  h.set("Empty-Header", "");
  EXPECT_TRUE(h.has("Empty-Header"));
  EXPECT_EQ(h.get("Empty-Header"), "");
}