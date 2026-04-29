#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <sap_network/tcp_socket.h>

#include "sap_http/net/http.h"

namespace {

    // Minimal HTTP/1.1 keep-alive listener used only for these tests.
    //
    // Why not reuse sap::http::HttpServer? The server in this repo closes the socket
    // after every response (see server.cpp handle_client), so it cannot be used
    // to exercise connection reuse on the client side.
    //
    // This listener accepts one TCP connection at a time and serves up to
    // `max_requests` requests on each accepted socket before moving on. It
    // records how many distinct accept() calls happened so tests can verify
    // whether the client reused a pooled connection or opened a new one.
    class KeepAliveListener {
    public:
        struct Options {
            u16 port;
            int max_requests_per_conn = 16;
            // If true, send "Connection: close" on the Nth response (1-indexed) so
            // the client must not pool it.
            int close_after_n = 0;
            // If true, close the socket after sending the 1st response (before the
            // client can send a 2nd). Simulates a server-side idle timeout racing
            // with the client's reuse attempt.
            bool close_after_first_response = false;
            // Optional body for responses.
            stl::string body = "ok";
        };

        explicit KeepAliveListener(Options o) : m_Opts(std::move(o)) {
            sap::network::SocketConfig sc;
            sc.host = "127.0.0.1";
            sc.port = m_Opts.port;
            sc.reuse_addr = true;
            sc.listen_backlog = 4;
            sc.recv_timeout = std::chrono::milliseconds{2000};
            sc.send_timeout = std::chrono::milliseconds{2000};
            m_Server = std::make_unique<sap::network::TCPSocket>(std::move(sc));
            if (!m_Server->valid() || !m_Server->bind() || !m_Server->listen()) {
                m_Ready = false;
                return;
            }
            m_Ready = true;
            m_Thread = stl::thread([this] { run(); });
        }

        ~KeepAliveListener() {
            m_Stop.store(true);
            if (m_Server) m_Server->close();
            if (m_Thread.joinable()) m_Thread.join();
        }

        bool ready() const { return m_Ready; }
        int accept_count() const { return m_AcceptCount.load(); }
        int request_count() const { return m_RequestCount.load(); }

    private:
        void run() {
            while (!m_Stop.load()) {
                auto client = m_Server->accept();
                if (!client) return;
                m_AcceptCount.fetch_add(1);
                auto& c = client.value();
                serve_connection(c);
                c.close();
            }
        }

        // Very small HTTP/1.1 request parser: reads until "\r\n\r\n", then pulls
        // Content-Length bytes if present. Returns false on error/close.
        bool read_one_request(sap::network::TCPSocket& sock) {
            stl::string buf;
            stl::byte chunk[1024];
            while (buf.find("\r\n\r\n") == stl::string::npos) {
                auto n = sock.recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
                if (!n || n.value() == 0) return false;
                buf.append(reinterpret_cast<const char*>(chunk), n.value());
                if (buf.size() > 64 * 1024) return false;
            }
            auto header_end = buf.find("\r\n\r\n");
            stl::string headers = buf.substr(0, header_end);
            // Find Content-Length (case-insensitive enough for our client)
            std::size_t content_length = 0;
            stl::string lower = headers;
            for (auto& c : lower) c = static_cast<char>(std::tolower(c));
            auto pos = lower.find("content-length:");
            if (pos != stl::string::npos) {
                auto eol = lower.find("\r\n", pos);
                auto val = headers.substr(pos + 15, eol - (pos + 15));
                content_length = std::stoull(val);
            }
            std::size_t already = buf.size() - (header_end + 4);
            while (already < content_length) {
                auto n = sock.recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
                if (!n || n.value() == 0) return false;
                already += n.value();
            }
            return true;
        }

        void write_response(sap::network::TCPSocket& sock, bool close_hdr) {
            stl::string resp = "HTTP/1.1 200 OK\r\n";
            resp += "Content-Length: " + std::to_string(m_Opts.body.size()) + "\r\n";
            resp += "Connection: ";
            resp += close_hdr ? "close" : "keep-alive";
            resp += "\r\n\r\n";
            resp += m_Opts.body;
            std::size_t sent = 0;
            while (sent < resp.size()) {
                auto n = sock.send(stl::span<const stl::byte>(
                    reinterpret_cast<const stl::byte*>(resp.data() + sent), resp.size() - sent));
                if (!n || n.value() == 0) return;
                sent += n.value();
            }
        }

        void serve_connection(sap::network::TCPSocket& sock) {
            for (int i = 0; i < m_Opts.max_requests_per_conn; ++i) {
                if (!read_one_request(sock)) return;
                m_RequestCount.fetch_add(1);
                bool send_close = (m_Opts.close_after_n > 0 && (i + 1) == m_Opts.close_after_n);
                write_response(sock, send_close);
                if (send_close) return;
                if (m_Opts.close_after_first_response && i == 0) return;
            }
        }

        Options m_Opts;
        stl::unique_ptr<sap::network::TCPSocket> m_Server;
        stl::thread m_Thread;
        stl::atomic<bool> m_Stop{false};
        stl::atomic<int> m_AcceptCount{0};
        stl::atomic<int> m_RequestCount{0};
        bool m_Ready{false};
    };

    sap::http::Request make_get(u16 port) {
        auto url = sap::http::URL::parse("http://127.0.0.1:" + std::to_string(port) + "/").value();
        return sap::http::Request(sap::http::EMethod::GET, std::move(url));
    }

} // namespace

// Three sequential requests to the same endpoint must reuse a single TCP
// connection when the server advertises keep-alive.
TEST(ClientPoolTest, ReusesConnectionForSequentialRequests) {
    KeepAliveListener server({.port = 11001});
    ASSERT_TRUE(server.ready());

    sap::http::HttpClient client;
    for (int i = 0; i < 3; ++i) {
        auto r = client.send_req(make_get(11001));
        ASSERT_TRUE(r.has_value()) << r.error();
        EXPECT_EQ(r.value().body, "ok");
    }

    EXPECT_EQ(server.request_count(), 3);
    EXPECT_EQ(server.accept_count(), 1) << "expected a single pooled connection";
}

// Setting idle_timeout to zero disables pooling — every request opens a fresh conn.
TEST(ClientPoolTest, ZeroIdleTimeoutDisablesPooling) {
    auto saved = sap::http::HttpClient::idle_timeout;
    sap::http::HttpClient::idle_timeout = std::chrono::seconds{0};

    KeepAliveListener server({.port = 11002});
    ASSERT_TRUE(server.ready());

    sap::http::HttpClient client;
    for (int i = 0; i < 3; ++i) {
        auto r = client.send_req(make_get(11002));
        ASSERT_TRUE(r.has_value()) << r.error();
    }

    sap::http::HttpClient::idle_timeout = saved;
    EXPECT_EQ(server.accept_count(), 3);
}

// A response carrying "Connection: close" must not end up in the pool.
TEST(ClientPoolTest, ConnectionCloseHeaderBypassesPool) {
    KeepAliveListener server({.port = 11003, .close_after_n = 1});
    ASSERT_TRUE(server.ready());

    sap::http::HttpClient client;
    for (int i = 0; i < 2; ++i) {
        auto r = client.send_req(make_get(11003));
        ASSERT_TRUE(r.has_value()) << r.error();
    }
    EXPECT_EQ(server.accept_count(), 2);
}

// Server drops its end after the first response. The client's second request
// must transparently retry on a fresh connection instead of surfacing the error.
TEST(ClientPoolTest, RetriesOnStalePooledConnection) {
    KeepAliveListener server({.port = 11004, .close_after_first_response = true});
    ASSERT_TRUE(server.ready());

    sap::http::HttpClient client;
    auto r1 = client.send_req(make_get(11004));
    ASSERT_TRUE(r1.has_value()) << r1.error();

    // Give the OS a moment to deliver the FIN so the retry path is exercised
    // (without this the client may not yet see the socket as dead).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto r2 = client.send_req(make_get(11004));
    ASSERT_TRUE(r2.has_value()) << r2.error();

    EXPECT_EQ(server.accept_count(), 2);
    EXPECT_EQ(server.request_count(), 2);
}

// End-to-end: the real sap::http::HttpServer must also keep the connection open
// across requests so the client's pool actually has something to reuse.
TEST(ClientPoolTest, EndToEndServerHonorsKeepAlive) {
    sap::http::HttpServerConfig cfg{"127.0.0.1", 11099};
    sap::http::HttpServer server{std::move(cfg)};
    stl::atomic<int> hits{0};
    server.route("/", sap::http::EMethod::GET,
                 [&](const sap::http::Request&) {
                     ++hits;
                     return sap::http::Response(sap::http::EStatusCode::OK, "ok");
                 });
    ASSERT_TRUE(server.start().has_value());
    server.run_async();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sap::http::HttpClient client;
    for (int i = 0; i < 3; ++i) {
        auto r = client.send_req(make_get(11099));
        ASSERT_TRUE(r.has_value()) << r.error();
        EXPECT_EQ(r.value().body, "ok");
    }

    server.stop();
    EXPECT_EQ(hits.load(), 3);
}

// clear_pool() drops any pooled sockets — subsequent requests must reconnect.
TEST(ClientPoolTest, ClearPoolForcesReconnect) {
    KeepAliveListener server({.port = 11005});
    ASSERT_TRUE(server.ready());

    sap::http::HttpClient client;
    ASSERT_TRUE(client.send_req(make_get(11005)).has_value());
    client.clear_pool();
    ASSERT_TRUE(client.send_req(make_get(11005)).has_value());

    EXPECT_EQ(server.accept_count(), 2);
}
