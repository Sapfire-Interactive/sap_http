# sap_http

A modern, lightweight C++20 HTTP library with both client and server support.

## Features

* **Modern C++20**: Clean, type-safe API using modern C++ features
* **HTTP Client & Server**: Full-featured client and server in one library
* **Async I/O**: Promise/future-based asynchronous client operations
* **Type-Safe Error Handling**: Comprehensive `stl::result<T>` type for all operations
* **Cross-Platform**: Windows, Linux, and macOS support
* **Lightweight**: No external dependencies, minimal overhead
* **Multithreaded Server**: Optional multithreaded request handling

## Quick Start

### Installation

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

### Simple HTTP Client

```cpp
#include <sap_http/net/http.h>
#include <iostream>

int main() {
    auto future = sap::http::Client::get("http://example.com/api");
    auto result = future.get();
    
    if (result) {
        auto& response = result.value();
        std::cout << "Status: " << response.status_code << '\n';
        std::cout << "Body: " << response.body << '\n';
    } else {
        std::cerr << "Error: " << result.error() << '\n';
    }
    return 0;
}
```

### Simple HTTP Server

```cpp
#include <sap_http/net/http.h>
#include <iostream>

int main() {
    sap::http::ServerConfig config;
    config.port = 8080;
    config.is_multithreaded = true;
    
    sap::http::Server server(config);
    
    // Add routes - handler receives const Request&
    server.route("/", sap::http::EMethod::GET, [](const sap::http::Request& req) {
        return sap::http::Response(200, "Hello, World!");
    });
    
    server.route("/api/data", sap::http::EMethod::POST, [](const sap::http::Request& req) {
        // Echo the request body
        sap::http::Response resp(200, req.body);
        resp.headers.set("Content-Type", "application/json");
        return resp;
    });
    
    // Start server
    auto result = server.start();
    if (!result) {
        std::cerr << "Failed to start server: " << result.error() << '\n';
        return 1;
    }
    
    std::cout << "Server running on port 8080\n";
    server.run();  // Blocks until server.stop() is called
    
    return 0;
}
```

## Building Your Project

**CMakeLists.txt:**

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(sap_http REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE sap::http)
```

## API Reference

### HTTP Client

#### Making Requests

```cpp
// GET request
auto future = sap::http::Client::get("http://api.example.com/users");
auto result = future.get();

// POST request with body
std::string json = R"({"name": "John", "age": 30})";
auto future = sap::http::Client::post("http://api.example.com/users", json);

// Custom request with headers
auto url_result = sap::http::URL::parse("http://api.example.com/resource");
if (url_result) {
    sap::http::Request req(sap::http::EMethod::PUT, std::move(url_result.value()));
    req.set_header("Authorization", "Bearer token123");
    req.set_header("Content-Type", "application/json");
    req.set_body(R"({"status": "updated"})");
    
    auto future = sap::http::Client::async_send(std::move(req));
    auto result = future.get();
}

// Synchronous send
auto result = sap::http::Client::send(req);
```

#### Supported Methods

```cpp
enum class EMethod {
    GET,
    POST, 
    PUT,
    DELETE,
    HEAD,
    PATCH,
    OPTIONS
};

// Convert to/from string
std::string method_str = sap::http::method_to_string(sap::http::EMethod::POST);  // "POST"
sap::http::EMethod method = sap::http::string_to_method("GET");  // EMethod::GET
```

#### URL Parsing

```cpp
auto result = sap::http::URL::parse("http://example.com:8080/path?query=value");
if (result) {
    auto& url = result.value();
    std::cout << "Scheme: " << url.scheme << '\n';  // "http"
    std::cout << "Host: " << url.host << '\n';      // "example.com"
    std::cout << "Port: " << url.port << '\n';      // "8080"
    std::cout << "Path: " << url.path << '\n';      // "/path"
    std::cout << "Query: " << url.query << '\n';    // "?query=value"
    
    // Get full path with query
    std::string full = url.full_path();  // "/path?query=value"
}

// Create URL from path only
auto url = sap::http::URL::from_path("/api/users?id=123");
```

#### Headers Management

```cpp
sap::http::Headers h;
h.set("Content-Type", "application/json");
h.set("Authorization", "Bearer token");

// Case-insensitive access
std::string content_type = h.get("content-type");
bool has_auth = h.has("Authorization");
```

### HTTP Server

#### Server Configuration

```cpp
sap::http::ServerConfig config;
config.port = 8080;              // Port to listen on (default: 8080)
config.is_multithreaded = true;  // Enable multithreaded mode (default: false)

sap::http::Server server(config);

// Or use default config
sap::http::Server server;  // Uses port 8080, single-threaded
```

#### Defining Routes

Route handlers receive `const Request&` and return `Response`:

```cpp
// Simple GET endpoint
server.route("/health", sap::http::EMethod::GET, [](const sap::http::Request& req) {
    return sap::http::Response(200, R"({"status": "healthy"})");
});

// POST with request processing
server.route("/api/users", sap::http::EMethod::POST, [](const sap::http::Request& req) {
    // Access request data
    std::string body = req.body;
    std::string content_type = req.headers.get("Content-Type");
    std::string path = req.url.path;
    std::string query = req.url.query;
    
    // Create response
    sap::http::Response resp(201, R"({"id": 123, "created": true})");
    resp.headers.set("Content-Type", "application/json");
    resp.headers.set("Location", "/api/users/123");
    return resp;
});

// DELETE endpoint
server.route("/api/users", sap::http::EMethod::DELETE, [](const sap::http::Request& req) {
    return sap::http::Response(204);  // No content
});
```

#### Request Object

```cpp
struct Request {
    EMethod method;                            // HTTP method
    URL url;                                   // Parsed URL (use url.path, url.query)
    Headers headers;                           // Request headers
    std::string body;                          // Request body
    std::map<std::string, std::string> params; // Route parameters (e.g., :id)
    std::chrono::milliseconds timeout{30000};  // Request timeout
};
```

#### Response Object

```cpp
// Simple response
sap::http::Response resp(200, "Hello World");

// Response with custom headers
sap::http::Response resp(201, R"({"id": 1})");
resp.headers.set("Content-Type", "application/json");
resp.headers.set("X-Custom-Header", "value");

// Check success (2xx status codes)
if (resp.is_success()) {
    // ...
}
```

#### Running the Server

```cpp
sap::http::ServerConfig config;
config.port = 8080;
config.is_multithreaded = true;

sap::http::Server server(config);

// Add routes...
server.route("/", sap::http::EMethod::GET, [](const sap::http::Request&) {
    return sap::http::Response(200, "OK");
});

// Start and run
auto result = server.start();
if (result) {
    server.run();  // Blocking - runs until stop() is called
}

// To stop from another thread:
server.stop();
```

## Complete Examples

### REST API Server

```cpp
#include <sap_http/net/http.h>
#include <iostream>
#include <string>
#include <map>
#include <mutex>

struct Database {
    std::map<int, std::string> users;
    std::mutex mtx;
    int next_id = 1;
};

int main() {
    Database db;
    
    sap::http::ServerConfig config;
    config.port = 8000;
    config.is_multithreaded = true;
    
    sap::http::Server server(config);
    
    // GET /api/users - List all users
    server.route("/api/users", sap::http::EMethod::GET, [&db](const sap::http::Request&) {
        std::lock_guard<std::mutex> lock(db.mtx);
        
        std::string json = "[";
        bool first = true;
        for (const auto& [id, name] : db.users) {
            if (!first) json += ",";
            json += R"({"id":)" + std::to_string(id) + R"(,"name":")" + name + R"("})";
            first = false;
        }
        json += "]";
        
        sap::http::Response resp(200, json);
        resp.headers.set("Content-Type", "application/json");
        return resp;
    });
    
    // POST /api/users - Create user
    server.route("/api/users", sap::http::EMethod::POST, [&db](const sap::http::Request& req) {
        std::lock_guard<std::mutex> lock(db.mtx);
        
        std::string name = req.body;
        int id = db.next_id++;
        db.users[id] = name;
        
        std::string response = R"({"id":)" + std::to_string(id) + 
                              R"(,"name":")" + name + R"(","created":true})";
        
        sap::http::Response resp(201, response);
        resp.headers.set("Content-Type", "application/json");
        return resp;
    });
    
    // DELETE /api/users - Delete all users
    server.route("/api/users", sap::http::EMethod::DELETE, [&db](const sap::http::Request&) {
        std::lock_guard<std::mutex> lock(db.mtx);
        db.users.clear();
        return sap::http::Response(204);
    });
    
    std::cout << "Starting REST API server on port 8000...\n";
    if (server.start()) {
        server.run();
    }
    
    return 0;
}
```

### Concurrent HTTP Requests

```cpp
#include <sap_http/net/http.h>
#include <iostream>
#include <vector>

int main() {
    std::vector<std::string> urls = {
        "http://api.example.com/endpoint1",
        "http://api.example.com/endpoint2",
        "http://api.example.com/endpoint3"
    };
    
    std::vector<std::future<stl::result<sap::http::Response>>> futures;
    
    // Launch all requests asynchronously
    for (const auto& url : urls) {
        futures.push_back(sap::http::Client::get(url));
    }
    
    // Collect results
    for (size_t i = 0; i < futures.size(); ++i) {
        auto result = futures[i].get();
        if (result && result.value().is_success()) {
            std::cout << "Request " << i << " succeeded\n";
            std::cout << "Body length: " << result.value().body.size() << '\n';
        } else {
            std::cout << "Request " << i << " failed\n";
        }
    }
    
    return 0;
}
```

### POST JSON with Custom Headers

```cpp
#include <sap_http/net/http.h>
#include <iostream>

int main() {
    std::string json_data = R"({
        "username": "john_doe",
        "email": "john@example.com"
    })";
    
    auto url_result = sap::http::URL::parse("http://api.example.com/users");
    if (!url_result) {
        std::cerr << "Invalid URL\n";
        return 1;
    }
    
    sap::http::Request req(sap::http::EMethod::POST, std::move(url_result.value()));
    req.set_header("Content-Type", "application/json");
    req.set_header("Authorization", "Bearer your_token");
    req.set_body(std::move(json_data));
    
    auto result = sap::http::Client::send(req);
    
    if (result) {
        auto& response = result.value();
        std::cout << "Status: " << response.status_code << '\n';
        std::cout << "Response: " << response.body << '\n';
    }
    
    return 0;
}
```

## CMake Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `SAP_HTTP_BUILD_SHARED` | `ON` | Build shared library |
| `SAP_HTTP_BUILD_STATIC` | `ON` | Build static library |
| `SAP_HTTP_BUILD_TESTS` | `ON` | Build test suite |
| `SAP_HTTP_INSTALL` | `ON` | Enable installation |

## Naming Conventions

| Type | Convention | Example |
|------|------------|---------|
| **Enums** | PascalCase with `E` prefix | `EMethod` |
| **Structs/Classes** | PascalCase | `URL`, `Headers`, `Request`, `Response`, `Client`, `Server` |
| **Functions** | snake_case | `method_to_string()`, `set_header()` |
| **Member variables** | snake_case (public), m_PascalCase (private) | `status_code`, `m_Routes` |
| **Types from sap_core** | lowercase | `i32`, `u16`, `stl::result<T>` |

## Platform Support

| Platform | Client | Server | Notes |
|----------|--------|--------|-------|
| Linux | ✅ | ✅ | Tested on Ubuntu 20.04+ |
| macOS | ✅ | ✅ | Tested on macOS 12+ |
| Windows | ✅ | ✅ | MSVC 2019+, MinGW-w64 |

## License

MIT License - see LICENSE file for details.

## Roadmap

### Client
- HTTPS/TLS support
- HTTP/2 support
- Connection pooling
- Request/response compression

### Server
- Path parameter extraction (`/users/:id`)
- Middleware support
- Static file serving
- WebSocket support