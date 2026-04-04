#pragma once

#include <sap_core/types.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <sstream>

#include <sap_core/stl/result.h>
#include <sap_core/stl/string.h>
#include <sap_core/stl/vector.h>
#include <sap_core/job_system.h>

namespace sap::http {

enum class EMethod { GET, POST, PUT, DELETE, HEAD, PATCH, OPTIONS };

inline stl::string method_to_string(EMethod m) {
  switch (m) {
  case EMethod::GET:
    return "GET";
  case EMethod::POST:
    return "POST";
  case EMethod::PUT:
    return "PUT";
  case EMethod::DELETE:
    return "DELETE";
  case EMethod::HEAD:
    return "HEAD";
  case EMethod::PATCH:
    return "PATCH";
  case EMethod::OPTIONS:
    return "OPTIONS";
  }
  return "GET";
}

inline EMethod string_to_method(stl::string_view s) {
  if (s == "GET")
    return EMethod::GET;
  if (s == "POST")
    return EMethod::POST;
  if (s == "PUT")
    return EMethod::PUT;
  if (s == "DELETE")
    return EMethod::DELETE;
  if (s == "HEAD")
    return EMethod::HEAD;
  if (s == "PATCH")
    return EMethod::PATCH;
  if (s == "OPTIONS")
    return EMethod::OPTIONS;
  return EMethod::GET;
}

struct URL {
  stl::string scheme;
  stl::string host;
  stl::string port;
  stl::string path;
  stl::string query;

  static stl::result<URL> parse(stl::string_view raw_url);
  stl::string full_path() const { return path + query; }
  static URL from_path(stl::string_view path_and_query);
};

struct Headers {
  std::map<stl::string, stl::string> data;

  void set(stl::string_view key, stl::string_view value);
  stl::string get(stl::string_view key) const;
  bool has(stl::string_view key) const;
};

struct Request {
  EMethod method = EMethod::GET;
  URL url;
  Headers headers;
  stl::string body;
  std::chrono::milliseconds timeout{30000};

  // Optional: route params extracted by server routing (e.g., /users/:id)
  std::map<stl::string, stl::string> params;

  Request() = default;
  Request(sap::http::EMethod m, sap::http::URL u);

  void set_header(stl::string_view key, stl::string_view value);
  void set_body(stl::string data);
};

struct Response {
  i32 status_code{0};
  stl::string status_text;
  Headers headers;
  stl::string body;
  Response() = default;
  Response(i32 code, stl::string body_content = "");
  inline bool is_success() const {
    return status_code >= 200 && status_code < 300;
  }
};

class Client {
private:
  static stl::result<i32> connect_socket(const URL &u);
  static stl::result<> send_request(i32 sock, const Request &req);
  static stl::result<Response> read_response(i32 sock);

public:
  static std::future<stl::result<Response>> async_send(Request req);
  static stl::result<Response> send(const Request &req);
  static std::future<stl::result<Response>> get(stl::string_view url_str);
  static std::future<stl::result<Response>> post(stl::string_view url_str,
                                                 stl::string body);
};

using RouteHandler = std::function<Response(const Request &)>;

struct Route {
  stl::string path;
  EMethod method;
  RouteHandler handler;
  bool is_regex{false};
};

struct ServerConfig {
  i32 server_socket{-1};
  stl::string host{"127.0.0.1"};
  u16 port{8080};
  bool is_multithreaded{false};
  u32 timeout_ms = 10000;
  stl::size_t max_header_size{8192};
  stl::size_t max_body_size{1024 * 1024}; // 1MB
};

class Server {
public:
  Server() = default;
  Server(ServerConfig cfg);
  ~Server();
  stl::result<> start();
  void run();
  void stop();

  template <typename Handler>
  void route(stl::string_view path, EMethod method, Handler &&handler) {
    Route r;
    r.path = path;
    r.method = method;
    r.handler = std::forward<Handler>(handler);
    m_Routes.push_back(std::move(r));
  }

private:
  void handle_client(i32 client_socket);

private:
  ServerConfig m_Config;
  std::vector<Route> m_Routes;
  std::atomic<bool> m_IsRunning{false};
  sap::job_system m_JobSystem;
};

} // namespace sap::http