#pragma once

#include <chrono>
#include <future>
#include <optional>
#include <sap_core/stl/map.h>
#include <sap_core/stl/unordered_map.h>
#include <sap_core/stl/vector.h>
#include <sap_core/types.h>

#include <sap_core/job_system.h>
#include <sap_core/stl/result.h>
#include <sap_core/stl/string.h>
#include <sap_core/stl/unique_ptr.h>
#include <sap_core/stl/vector.h>
#include <sap_network/socket_concept.h>

#include "sap_http/net/status_codes.h"
#include "sap_network/tcp_socket.h"
#include "sap_network/tls_socket.h"

namespace sap::http {

    enum class EMethod { GET, POST, PUT, DELETE, HEAD, PATCH, OPTIONS, UNKNOWN };

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
        default:
            return "UNKNOWN";
        }
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
        return EMethod::UNKNOWN;
    }

    struct URL {
        stl::string scheme;
        stl::string host;
        stl::string port;
        stl::string path;
        stl::string query;

        static stl::result<URL> parse(stl::string_view raw_url);
        stl::string full_path() const { return path + query; }
        static stl::result<URL> from_path(stl::string_view path_and_query);
    };

    struct Headers {
        stl::map<stl::string, stl::string> data;

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
        stl::map<stl::string, stl::string> params;

        Request() = default;
        Request(sap::http::EMethod m, sap::http::URL u);

        void set_header(stl::string_view key, stl::string_view value);
        void set_body(stl::string data);
    };

    struct Response {
        EStatusCode status_code{};
        stl::string status_text;
        Headers headers;
        stl::string body;
        Response() = default;
        Response(EStatusCode code, stl::string body_content = "");
        inline bool is_success() const {
            auto c = static_cast<i32>(status_code);
            return c >= 200 && c < 300;
        }
    };

    template <sap::network::Socket S>
    struct client_config_for;

    struct HttpClientConfig {};
    template <> struct client_config_for<sap::network::TCPSocket> { using type = HttpClientConfig; };

    struct HttpsClientConfig {
        bool verify_peer{true};
        bool verify_hostname{true};
        stl::string ca_file;
        stl::string ca_dir;
        stl::string client_cert_file;
        stl::string client_key_file;
        stl::vector<stl::string> alpn_protocols;
    };
    template <> struct client_config_for<sap::network::TLSSocket> { using type = HttpsClientConfig; };

    template <sap::network::Socket S>
    class Client {
    public:
        using Config = typename client_config_for<S>::type;

        // Maximum response size the client will accept. Defaults to 10MB.
        // Set higher for large downloads, lower for tighter resource limits.
        static inline stl::size_t max_response_size{10 * 1024 * 1024};

        // How long an idle pooled connection may sit before being evicted on next checkout.
        // Matches nginx's default keepalive_timeout. Set to zero to disable pooling.
        static inline std::chrono::seconds idle_timeout{90};

        Client() = default;
        explicit Client(Config cfg);
        ~Client() = default;
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        // Drop all pooled connections on this instance.
        void clear_pool();

        std::future<stl::result<Response>> async_send_req(Request req);
        stl::result<Response> send_req(const Request& req);

        // Default shared instance backing the static convenience methods below.
        static Client& default_instance();

        static std::future<stl::result<Response>> async_send(Request req);
        static stl::result<Response> send(const Request& req);
        static std::future<stl::result<Response>> get(stl::string_view url_str);
        static std::future<stl::result<Response>> post(stl::string_view url_str, stl::string body);

    private:
        struct PooledConn {
            stl::unique_ptr<S> sock;
            std::chrono::steady_clock::time_point last_used;
        };

        stl::result<Response> do_exchange(const Request& req);

        Config m_Config;
        stl::mutex m_Mu;
        stl::unordered_map<stl::string, stl::vector<PooledConn>> m_Pool;
    };

    using HttpClient  = Client<sap::network::TCPSocket>;
    using HttpsClient = Client<sap::network::TLSSocket>;

    using RouteHandler = stl::function<Response(const Request&)>;
    using Middleware = stl::function<std::optional<Response>(Request&)>;

    struct RouteSegment {
        stl::string text;  // literal text or param name (without ':')
        bool is_param{false};
    };

    struct Route {
        stl::string path;
        EMethod method;
        RouteHandler handler;
        bool is_regex{false};
        stl::vector<RouteSegment> segments;
        bool has_params{false};
        // When true, middleware registered via Server::use() is skipped for this route.
        // Use for endpoints that must run without auth/CORS/etc. gating (e.g. login).
        bool skip_middleware{false};
    };

    template <sap::network::Socket S>
        struct server_config_for;

    struct HttpServerConfig {
        stl::string host{"127.0.0.1"};
        u16 port{8080};
        bool is_multithreaded{false};
        u32 timeout_ms = 10000;
    };
    template<> struct server_config_for<sap::network::TCPSocket> {
        using type = HttpServerConfig;
    };

    struct HttpsServerConfig {
        stl::string host{"127.0.0.1"};
        u16 port{8080};
        bool is_multithreaded{false};
        u32 timeout_ms = 10000;
        sap::network::TlsServerConfig tls_cfg;
    };
    template<> struct server_config_for<sap::network::TLSSocket> {
        using type = HttpsServerConfig;
    };


    template<sap::network::Socket S>
    class Server {
    public:
        using Config = typename server_config_for<S>::type;

        Server() = default;
        explicit Server(Config cfg);
        ~Server();

        static inline stl::size_t max_header_size{8192};
        static inline stl::size_t max_body_size{1024 * 1024}; // 1MB

        stl::result<> start();
        void run();
        void run_async();
        void stop();

        template <typename M>
        void use(M&& middleware) {
            m_Middleware.emplace_back(std::forward<M>(middleware));
        }

        template <typename Handler>
        void route(stl::string_view path, EMethod method, Handler&& handler) {
            add_route(path, method, std::forward<Handler>(handler), /*skip_middleware=*/false);
        }

        // Registers a route that bypasses all middleware. Use for endpoints that must
        // run without auth/CORS/etc. (e.g. /auth/login). Matching semantics are
        // identical to route().
        template <typename Handler>
        void public_route(stl::string_view path, EMethod method, Handler&& handler) {
            add_route(path, method, std::forward<Handler>(handler), /*skip_middleware=*/true);
        }

    private:
        void handle_client(stl::unique_ptr<S> client_socket);

        template <typename Handler>
        void add_route(stl::string_view path, EMethod method, Handler&& handler, bool skip_middleware) {
            Route r;
            r.path = path;
            r.method = method;
            r.handler = std::forward<Handler>(handler);
            r.skip_middleware = skip_middleware;
            // Pre-split path into segments and detect param segments (":name")
            stl::string p(path);
            size_t start = 0;
            if (!p.empty() && p[0] == '/')
                start = 1;
            while (start <= p.size()) {
                size_t slash = p.find('/', start);
                size_t end = (slash == stl::string::npos) ? p.size() : slash;
                if (end > start) {
                    RouteSegment seg;
                    if (p[start] == ':') {
                        seg.is_param = true;
                        seg.text = p.substr(start + 1, end - start - 1);
                        r.has_params = true;
                    } else {
                        seg.text = p.substr(start, end - start);
                    }
                    r.segments.push_back(std::move(seg));
                }
                if (slash == stl::string::npos)
                    break;
                start = slash + 1;
            }
            m_Routes.push_back(std::move(r));
        }

    private:
        Config m_Config;
        stl::optional<S> m_ServerSocket;
        stl::vector<Route> m_Routes;
        stl::vector<Middleware> m_Middleware;
        stl::atomic<bool> m_IsRunning{false};
        sap::job_system m_JobSystem;
        stl::thread m_RunThread;

        // Tracks in-flight client sockets so stop() can close those parked in
        // read_header() waiting for the next keep-alive request. Sockets currently
        // executing a handler are left alone so graceful shutdown lets them finish.
        struct ClientEntry {
            S* sock;
            stl::atomic<bool>* idle; // true while parked in read_header
        };
        stl::mutex m_ClientsMutex;
        stl::vector<ClientEntry> m_ActiveClients;
    };

    using HttpServer = Server<sap::network::TCPSocket>;
    using HttpsServer = Server<sap::network::TLSSocket>;

} // namespace sap::http
