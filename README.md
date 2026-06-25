# sap_http

Modern C++20 HTTP/1.1 library — client and server, plain TCP and TLS, blocking and reactor-driven async. Error handling is `stl::result<T>` (from [sap_core](https://github.com/Sapfire-Interactive/sap_core)) — no exceptions.

Part of the [Sapfire](https://github.com/Sapfire-Interactive) library ecosystem.

## Features

- **Two flavors of every type.** Blocking thread-per-connection (`HttpClient`, `HttpServer`) and async coroutine-based (`HttpClientAsync`, `HttpServerAsync`) sharing the same `Request`/`Response`/`Headers`/`URL` types.
- **HTTPS / TLS** via OpenSSL — `HttpsClient` / `HttpsServer` / `HttpsClientAsync` / `HttpsServerAsync` use the same surfaces as the plain-TCP variants; only the socket type differs.
- **Middleware** registered with `use()`, runs before the route handler and can short-circuit with a `Response`. Async server accepts both sync and `Task`-returning middleware.
- **Path parameters** (`/users/:id`) extracted into `req.params`.
- **Connection pooling** in the blocking client, keyed per `host:port`, nginx-style idle timeout.
- **HTTP/1.1 keep-alive + pipelining** in the server.
- **Public routes** that bypass middleware (`public_route`) for endpoints like `/auth/login`.
- `stl::result<T>` everywhere — no exceptions in the surface API.

## Quick start

### Blocking client

```cpp
#include <sap_http/net/http.h>

int main() {
    auto fut    = sap::http::HttpClient::get("http://example.com/api");
    auto result = fut.get();
    if (!result)
        return std::fprintf(stderr, "error: %s\n", result.error().c_str()), 1;
    auto& resp = result.value();
    std::printf("status: %d\nbody: %s\n", static_cast<int>(resp.status_code), resp.body.c_str());
}
```

### Blocking server

```cpp
#include <sap_http/net/http.h>

int main() {
    sap::http::HttpServerConfig cfg;
    cfg.host             = "0.0.0.0";
    cfg.port             = 8080;
    cfg.is_multithreaded = true;

    sap::http::HttpServer server(cfg);

    server.route("/", sap::http::EMethod::GET, [](const sap::http::Request&) {
        return sap::http::Response(sap::http::EStatusCode::OK, "Hello, World!");
    });

    server.route("/api/users/:id", sap::http::EMethod::GET, [](const sap::http::Request& req) {
        sap::http::Response r(sap::http::EStatusCode::OK, "user=" + req.params.at("id"));
        r.headers.set("Content-Type", "text/plain");
        return r;
    });

    if (auto r = server.start(); !r) return 1;
    server.run();           // blocks until server.stop()
}
```

### Async server (reactor + coroutines)

`HttpServerAsync` runs a single-threaded `sap::async::Executor` driven by a `sap::io::Reactor`. Handlers are coroutines returning `Task<Response>`; nothing blocks the event loop.

```cpp
#include <sap_http/net/http.h>
#include <sap_core/async/sleep_for.h>

using namespace sap::http;
using sap::async::Task;
using sap::async::sleep_for;

int main() {
    auto sr = HttpServerAsync::create({.host = "127.0.0.1", .port = 8080});
    if (!sr) return 1;
    auto& server = sr.value();

    server.route("/slow", EMethod::GET, [&](const Request&) -> Task<Response> {
        co_await sleep_for(server.executor(), std::chrono::milliseconds(50));
        co_return Response(EStatusCode::OK, "took 50ms but didn't block the loop");
    });

    if (auto r = server.start(); !r) return 1;
    server.run();
}
```

### Async client

```cpp
#include <sap_http/net/http.h>
#include <sap_core/async/spawn.h>

int main() {
    auto exr = sap::async::Executor::create();
    auto& ex = exr.value();

    sap::http::HttpClientAsync client(ex);

    auto handle = sap::async::spawn(ex, client.get("http://127.0.0.1:8080/slow"));
    auto result = sap::async::sync_wait(stl::move(handle));   // drives the executor
    if (result)
        std::puts(result.value().body.c_str());
}
```

## HTTPS

The TLS variants share their entire dispatch path with their plain-TCP siblings; only the socket type differs.

```cpp
sap::http::HttpsServerConfig cfg;
cfg.host                  = "0.0.0.0";
cfg.port                  = 443;
cfg.tls_cfg.cert_file     = "/etc/ssl/server.pem";
cfg.tls_cfg.key_file      = "/etc/ssl/server.key";
cfg.tls_cfg.alpn_protocols.emplace_back("http/1.1");

sap::http::HttpsServer server(cfg);
// or:
auto sr = sap::http::HttpsServerAsync::create(cfg);
```

```cpp
sap::http::HttpsClientConfig ccfg;
ccfg.verify_peer     = true;
ccfg.verify_hostname = true;
ccfg.ca_file         = "/etc/ssl/ca.pem";
ccfg.alpn_protocols.emplace_back("http/1.1");

sap::http::HttpsClient sync_client(ccfg);
sap::http::HttpsClientAsync async_client(ex, ccfg);
```

## Middleware

Middleware runs before the route handler. Returning a `Response` short-circuits the chain; returning `nullopt` continues to the next middleware (or the handler).

```cpp
server.use([](sap::http::Request& req) -> std::optional<sap::http::Response> {
    if (req.headers.get("X-Api-Key").empty())
        return sap::http::Response(sap::http::EStatusCode::Unauthorized, "missing key");
    return std::nullopt;
});

// public_route bypasses use()-registered middleware
server.public_route("/auth/login", sap::http::EMethod::POST, login_handler);
```

`HttpServerAsync` also accepts `Task<std::optional<Response>>`-returning middleware so middleware can suspend on I/O:

```cpp
server.use([&ex](sap::http::Request& req) -> Task<std::optional<sap::http::Response>> {
    auto token = req.headers.get("Authorization");
    auto ok    = co_await verify_with_remote_auth_service(ex, token);
    if (!ok)
        co_return sap::http::Response(sap::http::EStatusCode::Unauthorized, "");
    co_return std::nullopt;
});
```

## API surface at a glance

| Type                | Socket                    | Driver         |
|---------------------|---------------------------|----------------|
| `HttpClient`        | `TCPSocket`               | blocking (`std::future` for async-ish) |
| `HttpsClient`       | `TLSSocket`               | blocking |
| `HttpClientAsync`   | `TCPSocketAsync`          | `sap::async::Executor` |
| `HttpsClientAsync`  | `TLSSocketAsync`          | `sap::async::Executor` |
| `HttpServer`        | `TCPSocket`               | single-threaded or `job_system` worker pool (set `is_multithreaded`) |
| `HttpsServer`       | `TLSSocket`               | single-threaded or `job_system` worker pool |
| `HttpServerAsync`   | `TCPSocketAsync`          | `sap::async::Executor` |
| `HttpsServerAsync`  | `TLSSocketAsync`          | `sap::async::Executor` |

`Client<S>` and `Server<S>` are templates parameterized on `sap::network::Socket`. `ClientAsync<S>` and `ServerAsync<S>` are templates parameterized on `sap::network::SocketAsync`. Adding a third socket type means adding a `client_config_for` / `server_config_for` trait specialization and an explicit instantiation in the relevant `.cpp`.

## Build

CMake 3.20+, C++20 (GCC 12+, Clang 15+, MSVC 19.34+), OpenSSL 3.0+.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Submodules first:

```sh
git submodule update --init --recursive
```

### Build options

| Option                  | Default | Description                |
|-------------------------|---------|----------------------------|
| `SAP_HTTP_BUILD_SHARED` | `ON`    | Build `sap_http` shared lib |
| `SAP_HTTP_BUILD_STATIC` | `ON`    | Build `sap_http` static lib |
| `SAP_HTTP_BUILD_TESTS`  | `ON`    | Build the gtest suite       |
| `SAP_HTTP_INSTALL`      | `ON`    | Enable `cmake --install`    |

### Consuming as a dependency

```cmake
find_package(sap_http REQUIRED)
target_link_libraries(my_app PRIVATE sap::http)
```

## Conventions

| Kind                   | Convention                     | Example                                  |
|------------------------|--------------------------------|------------------------------------------|
| Enums                  | `EPascalCase`                  | `EMethod`, `EStatusCode`                 |
| Types                  | `PascalCase`                   | `URL`, `Request`, `HttpServerAsync`      |
| Functions / fields     | `snake_case`                   | `set_header()`, `status_code`            |
| Private members        | `m_PascalCase`                 | `m_Routes`, `m_Executor`                 |
| Integer aliases        | `i32`, `u16`, …                | from `sap_core/types.h`                  |
| Containers / strings   | `stl::`                        | `stl::string`, `stl::vector<stl::byte>`  |

## Platforms

| Platform | Status |
|----------|--------|
| Linux    | Supported; primary target. |
| Windows  | Supported; the IOCP+AFD reactor backend exists but hasn't been compile-verified on Windows yet. |
| macOS / BSD | Out of scope. |

## License

MIT — see [LICENSE](LICENSE).
