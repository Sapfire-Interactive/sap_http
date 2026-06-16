# sap_http

C++20 HTTP/HTTPS library built on [sap_network](https://github.com/Sapfire-Interactive/sap_network). Provides a templated client and server parameterised over the socket type — swap `TCPSocket` for `TLSSocket` to get TLS for free.

Part of the [Sapfire](https://github.com/Sapfire-Interactive) library ecosystem.

## What's inside

### Client

`Client<S>` is templated on any type satisfying the `sap::network::Socket` concept:

```cpp
using HttpClient  = Client<sap::network::TCPSocket>;
using HttpsClient = Client<sap::network::TLSSocket>;
```

Features:
- **Connection pooling** — idle connections are reused per host:port. Connections idle longer than `idle_timeout` (default 90s, matching nginx's keepalive default) are evicted on next checkout.
- **Async requests** — `async_send_req()` returns `std::future<stl::result<Response>>`.
- **Configurable limits** — `max_response_size` (default 10MB), per-request `timeout`.
- **Static convenience API** — `HttpClient::get(url)`, `HttpClient::post(url, body)` backed by a shared default instance.

HTTPS client config exposes peer verification, hostname verification, CA file/dir, client certificates, and ALPN protocol list.

### Server

`Server<S>` is also templated, giving `HttpServer` and `HttpsServer` from the same implementation.

Features:
- **Route matching** — path segments are pre-split at registration time. Parameterised routes (`:id`) are detected and extracted into `Request::params` at match time — no regex at runtime for typical routes.
- **Middleware chain** — `use(fn)` registers middleware applied before route handlers. `public_route()` bypasses the middleware chain for routes that must run ungated (e.g. `/auth/login`).
- **Multithreaded dispatch** — backed by sap_core's `job_system`; set `is_multithreaded = true` in config.
- **Graceful shutdown** — `stop()` closes idle keep-alive sockets parked in header reads; in-flight handlers are allowed to finish.
- **Size guards** — `max_header_size` (8KB) and `max_body_size` (1MB) are enforced before any handler runs.
- **Chunked transfer encoding** — `read_chunked_body<S>()` decodes HTTP/1.1 chunked bodies with a configurable size cap.

## Build

Requires CMake 3.20+, C++20, [sap_core](https://github.com/Sapfire-Interactive/sap_core), and [sap_network](https://github.com/Sapfire-Interactive/sap_network).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Usage examples

```cpp
#include <sap_http/net/http.h>

// HTTP GET
auto future = sap::http::HttpClient::get("http://example.com/api/data");
auto response = future.get();
if (response && response->is_success())
    std::println("{}", response->body);

// HTTPS POST
sap::http::HttpsClient client{sap::http::HttpsClientConfig{.verify_peer = true}};
sap::http::Request req{sap::http::EMethod::POST, *sap::http::URL::parse("https://api.example.com/users")};
req.set_body(R"({"name":"dominik"})");
req.set_header("Content-Type", "application/json");
auto res = client.send_req(req);
```

```cpp
// HTTP server with middleware
sap::http::HttpServer server{sap::http::HttpServerConfig{.host = "0.0.0.0", .port = 8080}};

server.use([](sap::http::Request& req) -> std::optional<sap::http::Response> {
    if (!req.headers.has("Authorization"))
        return sap::http::Response{sap::http::EStatusCode::Unauthorized};
    return std::nullopt; // continue
});

server.route("/users/:id", sap::http::EMethod::GET, [](const sap::http::Request& req) {
    auto id = req.params.at("id");
    return sap::http::Response{sap::http::EStatusCode::OK, "user: " + id};
});

server.public_route("/auth/login", sap::http::EMethod::POST, [](const sap::http::Request& req) {
    return sap::http::Response{sap::http::EStatusCode::OK, "token"};
});

server.start();
server.run();
```
