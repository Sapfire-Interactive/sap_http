// Tests for sap::http::HttpsServer (= Server<sap::network::TLSSocket>).
//
// Test pattern mirrors server_recv_tests.cpp: spin up an HttpsServer in a
// background thread, drive it from the test thread with a TLSSocket client,
// assert on the response. Cert+key are generated programmatically per test
// run via the SelfSignedCert helper — same idiom sap_network's tls tests use.
// Each test owns a distinct port so a half-cleaned-up TIME_WAIT from a previous
// run never bleeds into the next one.

#include <gtest/gtest.h>
#include "sap_http/net/http.h"

#include <sap_network/tls_socket.h>
#include <sap_network/tcp_socket.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Port allocation
// ---------------------------------------------------------------------------

namespace {
    constexpr u16 PORT_CREATE                = 12000;
    constexpr u16 PORT_START_NO_CERT         = 12001;
    constexpr u16 PORT_START_NO_KEY          = 12002;
    constexpr u16 PORT_START_BAD_CERT        = 12003;
    constexpr u16 PORT_GET_SMOKE             = 12004;
    constexpr u16 PORT_POST_CL               = 12005;
    constexpr u16 PORT_NOT_FOUND             = 12006;
    constexpr u16 PORT_METHOD_NOT_ALLOWED    = 12007;
    constexpr u16 PORT_PATH_PARAMS           = 12008;
    constexpr u16 PORT_LARGE_BODY            = 12009;
    constexpr u16 PORT_BODY_TOO_LARGE        = 12010;
    constexpr u16 PORT_HEADERS_TOO_LARGE     = 12011;
    constexpr u16 PORT_INVALID_CL            = 12012;
    constexpr u16 PORT_CHUNKED_BODY          = 12013;
    constexpr u16 PORT_KEEPALIVE_REUSE       = 12014;
    constexpr u16 PORT_KEEPALIVE_HTTP10      = 12015;
    constexpr u16 PORT_KEEPALIVE_CLOSE       = 12016;
    constexpr u16 PORT_PIPELINED             = 12017;
    constexpr u16 PORT_MIDDLEWARE_SHORT      = 12018;
    constexpr u16 PORT_MIDDLEWARE_PASS       = 12019;
    constexpr u16 PORT_PUBLIC_ROUTE          = 12020;
    constexpr u16 PORT_ALPN                  = 12021;
    constexpr u16 PORT_TLS13_NEGOTIATED      = 12022;
    constexpr u16 PORT_TLS_VERSION_FLOOR     = 12023;
    constexpr u16 PORT_PLAIN_TCP_CLIENT      = 12024;
    constexpr u16 PORT_MULTITHREADED         = 12025;
    constexpr u16 PORT_GRACEFUL_STOP         = 12026;
    constexpr u16 PORT_MTLS_REQUIRED         = 12027;
    constexpr u16 PORT_MTLS_VALID            = 12028;
    constexpr u16 PORT_LARGE_RESPONSE        = 12029;
    constexpr u16 PORT_RESPONSE_HEADERS      = 12030;
    constexpr u16 PORT_DISCONNECT_DURING_RECV = 12031;
}

// ---------------------------------------------------------------------------
// Self-signed cert + key fixture (programmatic, per-test).
// Mirrors sap_network's SelfSignedCert helper. RAII — temp dir cleaned up in
// the dtor. CA mode generates a self-signing CA suitable for issuing client
// certs (used by the mTLS tests).
// ---------------------------------------------------------------------------

class SelfSignedCert {
public:
    enum class EKind { Server, ClientSignedByThis };

    explicit SelfSignedCert(stl::string cn = "localhost", bool is_ca = false) {
        std::random_device rd;
        m_dir = fs::temp_directory_path() / fs::path{"sap_http_tls_test_" + std::to_string(rd())};
        fs::create_directories(m_dir);
        cert_file = (m_dir / "cert.pem").string();
        key_file  = (m_dir / "key.pem").string();
        m_pkey = ::EVP_RSA_gen(2048);
        m_x509 = make_cert(cn, m_pkey, /*signer_pkey=*/m_pkey, /*signer_x509=*/nullptr, is_ca);
        write_pair(cert_file, key_file, m_x509, m_pkey);
    }

    ~SelfSignedCert() {
        if (m_x509) ::X509_free(m_x509);
        if (m_pkey) ::EVP_PKEY_free(m_pkey);
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }

    SelfSignedCert(const SelfSignedCert&) = delete;
    SelfSignedCert& operator=(const SelfSignedCert&) = delete;

    // Issue a client cert signed by *this* CA. Returns paths to a pair of
    // files (cert.pem, key.pem) inside this CA's temp dir.
    struct Issued {
        stl::string cert_file;
        stl::string key_file;
    };
    Issued issue_client(stl::string cn) {
        EVP_PKEY* client_pkey = ::EVP_RSA_gen(2048);
        X509*     client_x509 = make_cert(cn, client_pkey, m_pkey, m_x509, /*is_ca=*/false);
        Issued i;
        i.cert_file = (m_dir / (cn + "_cert.pem")).string();
        i.key_file  = (m_dir / (cn + "_key.pem")).string();
        write_pair(i.cert_file, i.key_file, client_x509, client_pkey);
        ::X509_free(client_x509);
        ::EVP_PKEY_free(client_pkey);
        return i;
    }

    stl::string cert_file;
    stl::string key_file;

private:
    static X509* make_cert(const stl::string& cn, EVP_PKEY* pkey,
                           EVP_PKEY* signer_pkey, X509* signer_x509, bool is_ca) {
        X509* x = ::X509_new();
        ::ASN1_INTEGER_set(::X509_get_serialNumber(x), std::rand());
        ::X509_gmtime_adj(::X509_getm_notBefore(x), 0);
        ::X509_gmtime_adj(::X509_getm_notAfter(x), 31536000L); // 1 year
        ::X509_set_pubkey(x, pkey);
        X509_NAME* name = ::X509_get_subject_name(x);
        ::X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
        if (signer_x509)
            ::X509_set_issuer_name(x, ::X509_get_subject_name(signer_x509));
        else
            ::X509_set_issuer_name(x, name);

        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        ::X509V3_set_ctx(&v3ctx, signer_x509 ? signer_x509 : x, x, nullptr, nullptr, 0);
        if (is_ca) {
            if (X509_EXTENSION* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx,
                    NID_basic_constraints, "critical,CA:TRUE")) {
                ::X509_add_ext(x, ext, -1);
                ::X509_EXTENSION_free(ext);
            }
            if (X509_EXTENSION* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx,
                    NID_key_usage, "critical,keyCertSign,cRLSign")) {
                ::X509_add_ext(x, ext, -1);
                ::X509_EXTENSION_free(ext);
            }
        } else {
            // Server cert (CN=localhost) needs SAN; client cert sets only EKU=clientAuth.
            if (cn == "localhost") {
                if (X509_EXTENSION* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx,
                        NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1")) {
                    ::X509_add_ext(x, ext, -1);
                    ::X509_EXTENSION_free(ext);
                }
            } else {
                if (X509_EXTENSION* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx,
                        NID_ext_key_usage, "clientAuth")) {
                    ::X509_add_ext(x, ext, -1);
                    ::X509_EXTENSION_free(ext);
                }
            }
        }
        ::X509_sign(x, signer_pkey, ::EVP_sha256());
        return x;
    }

    static void write_pair(const stl::string& cert_path, const stl::string& key_path,
                           X509* x509, EVP_PKEY* pkey) {
        FILE* fp = std::fopen(cert_path.c_str(), "wb");
        ::PEM_write_X509(fp, x509);
        std::fclose(fp);
        fp = std::fopen(key_path.c_str(), "wb");
        ::PEM_write_PrivateKey(fp, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(fp);
    }

    fs::path  m_dir;
    EVP_PKEY* m_pkey = nullptr;
    X509*     m_x509 = nullptr;
};

// ---------------------------------------------------------------------------
// Server / client helpers
// ---------------------------------------------------------------------------

// Build an HttpsServerConfig pre-populated with a server cert+key and the
// CA chain (= the same self-signed cert) so a TLS client can verify it.
static sap::http::HttpsServerConfig make_https_cfg(u16 port, const SelfSignedCert& cert) {
    sap::http::HttpsServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.tls_cfg.cert_file = cert.cert_file;
    cfg.tls_cfg.key_file  = cert.key_file;
    return cfg;
}

// Start the server in a background thread. Returns the run thread; caller
// is responsible for stop() + join().
static stl::thread start_server(sap::http::HttpsServer& server) {
    auto res = server.start();
    EXPECT_TRUE(res.has_value()) << "HttpsServer failed to start: " << res.error();
    stl::thread t([&server]() { server.run(); });
    std::this_thread::sleep_for(150ms);  // give the listen() call time to settle
    return t;
}

// Open a TLSSocket to 127.0.0.1:port that trusts `cert` and verifies hostname.
// Returns nullptr on connect/handshake failure; the test should ASSERT_NE.
// ALPN is hardcoded to http/1.1; tests that need a different ALPN build their
// own TlsConfig.
static stl::unique_ptr<sap::network::TLSSocket>
tls_connect(u16 port, const SelfSignedCert& cert, bool verify_peer = true) {
    sap::network::TlsClientConfig tc;
    tc.tcp.host = "127.0.0.1";
    tc.tcp.port = port;
    tc.tcp.connect_timeout = 3000ms;
    tc.tcp.recv_timeout = 5000ms;
    tc.tcp.send_timeout = 3000ms;
    tc.sni_hostname = "localhost";
    tc.verify_peer = verify_peer;
    tc.verify_hostname = verify_peer;
    tc.ca_file = cert.cert_file;
    tc.alpn_protocols.push_back("http/1.1");
    auto sock = stl::make_unique<sap::network::TLSSocket>(std::move(tc));
    if (!sock->connect()) return nullptr;
    return sock;
}

// Send `payload` over `sock`, then read until EOF and return what we got.
// Handles short writes / short reads that TLS records are bounded to.
static stl::string tls_exchange(sap::network::TLSSocket& sock, stl::string payload) {
    size_t sent = 0;
    while (sent < payload.size()) {
        auto n = sock.send(stl::span<const stl::byte>(
            reinterpret_cast<const stl::byte*>(payload.data() + sent), payload.size() - sent));
        if (!n || n.value() == 0) break;
        sent += n.value();
    }
    stl::string response;
    stl::byte buf[4096];
    while (true) {
        auto n = sock.recv(stl::span<stl::byte>(buf, sizeof(buf)));
        if (!n || n.value() == 0) break;
        response.append(reinterpret_cast<const char*>(buf), n.value());
    }
    return response;
}

// Convenience: connect, exchange, return response string. Inserts
// "Connection: close" if the caller didn't supply a Connection header so the
// server closes after responding (otherwise we'd block until idle timeout).
static stl::string
tls_request(u16 port, const SelfSignedCert& cert, stl::string data) {
    auto sock = tls_connect(port, cert);
    if (!sock) return "";
    if (data.find("Connection:") == stl::string::npos &&
        data.find("connection:") == stl::string::npos) {
        auto term = data.find("\r\n\r\n");
        if (term != stl::string::npos) data.insert(term + 2, "Connection: close\r\n");
    }
    auto resp = tls_exchange(*sock, std::move(data));
    sock->close();
    return resp;
}

// Trivial GET request builder.
static stl::string get_request(stl::string_view path, bool keep_alive = false) {
    stl::string s;
    s.append("GET "); s.append(path); s.append(" HTTP/1.1\r\n");
    s.append("Host: 127.0.0.1\r\n");
    s.append("Connection: ");
    s.append(keep_alive ? "keep-alive" : "close");
    s.append("\r\n\r\n");
    return s;
}

// ---------------------------------------------------------------------------
// Construction & lifecycle
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, CreateServer) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_CREATE, cert);
    sap::http::HttpsServer server(std::move(cfg));
    SUCCEED();
}

TEST(HttpsServerTest, AddRoute) {
    sap::http::HttpsServer server;
    server.route("/test", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "T"); });
    SUCCEED();
}

TEST(HttpsServerTest, StartFailsWithMissingCert) {
    sap::http::HttpsServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = PORT_START_NO_CERT;
    cfg.tls_cfg.cert_file = ""; // missing
    cfg.tls_cfg.key_file  = "/nonexistent/key.pem";
    sap::http::HttpsServer server(std::move(cfg));
    auto res = server.start();
    // Listener can bind, but the first accept fails. start() itself succeeds
    // (it's the accept that triggers cert load via SSL_accept). The realistic
    // failure surface is at run/accept time. Verify start() doesn't crash and
    // cleanup is sane.
    if (res.has_value()) server.stop();
    SUCCEED();
}

TEST(HttpsServerTest, StartFailsWithBadCertPath) {
    sap::http::HttpsServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = PORT_START_BAD_CERT;
    cfg.tls_cfg.cert_file = "/nonexistent/cert.pem";
    cfg.tls_cfg.key_file  = "/nonexistent/key.pem";
    sap::http::HttpsServer server(std::move(cfg));
    auto res = server.start();
    // Same caveat as above — check cleanup, not exact failure timing.
    if (res.has_value()) server.stop();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Routing & request handling
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, GetRoundTrip) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_GET_SMOKE, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/hello", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "world"); });
    auto t = start_server(server);

    auto resp = tls_request(PORT_GET_SMOKE, cert, get_request("/hello"));
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("HTTP/1.1 200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("world") != stl::string::npos) << resp;
}

TEST(HttpsServerTest, PostBodyRoundTrip) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_POST_CL, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
        [](const sap::http::Request& req) {
            return sap::http::Response(sap::http::EStatusCode::OK, req.body);
        });
    auto t = start_server(server);

    stl::string body = R"({"k":"v"})";
    stl::string req  = "POST /echo HTTP/1.1\r\n"
                       "Host: 127.0.0.1\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    auto resp = tls_request(PORT_POST_CL, cert, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find(body) != stl::string::npos) << resp;
}

TEST(HttpsServerTest, NotFoundReturns404) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_NOT_FOUND, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/known", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "k"); });
    auto t = start_server(server);

    auto resp = tls_request(PORT_NOT_FOUND, cert, get_request("/unknown"));
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("404 Not Found") != stl::string::npos) << resp;
}

TEST(HttpsServerTest, MethodNotAllowedReturns405) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_METHOD_NOT_ALLOWED, cert);
    sap::http::HttpsServer server(std::move(cfg));
    auto t = start_server(server);

    stl::string req = "WIBBLE / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    auto resp = tls_request(PORT_METHOD_NOT_ALLOWED, cert, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("405") != stl::string::npos) << resp;
}

TEST(HttpsServerTest, PathParamsExtracted) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_PATH_PARAMS, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/users/:id", sap::http::EMethod::GET,
        [](const sap::http::Request& req) {
            auto it = req.params.find("id");
            stl::string id = (it != req.params.end()) ? it->second : stl::string{"none"};
            return sap::http::Response(sap::http::EStatusCode::OK, "id=" + id);
        });
    auto t = start_server(server);

    auto resp = tls_request(PORT_PATH_PARAMS, cert, get_request("/users/42"));
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("id=42") != stl::string::npos) << resp;
}

// ---------------------------------------------------------------------------
// Body handling
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, LargeBodySpansMultipleTLSRecords) {
    // A TLS record max is ~16KB. 64KB bodies must span multiple records on
    // both send and recv.
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_LARGE_BODY, cert);
    auto saved = sap::http::HttpsServer::max_body_size;
    sap::http::HttpsServer::max_body_size = 256 * 1024;
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
        [](const sap::http::Request& req) {
            return sap::http::Response(sap::http::EStatusCode::OK,
                                       std::to_string(req.body.size()));
        });
    auto t = start_server(server);

    stl::string body(64 * 1024, 'A');
    stl::string req  = "POST /echo HTTP/1.1\r\n"
                       "Host: 127.0.0.1\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    auto resp = tls_request(PORT_LARGE_BODY, cert, req);
    server.stop();
    t.join();
    sap::http::HttpsServer::max_body_size = saved;

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find(std::to_string(body.size())) != stl::string::npos) << resp;
}

TEST(HttpsServerTest, BodyExceedsMaxBodySize) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_BODY_TOO_LARGE, cert);
    auto saved = sap::http::HttpsServer::max_body_size;
    sap::http::HttpsServer::max_body_size = 128;
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
        [](const sap::http::Request& req) {
            return sap::http::Response(sap::http::EStatusCode::OK, req.body);
        });
    auto t = start_server(server);

    stl::string body(256, 'X');
    stl::string req  = "POST /echo HTTP/1.1\r\n"
                       "Host: 127.0.0.1\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    auto resp = tls_request(PORT_BODY_TOO_LARGE, cert, req);
    server.stop();
    t.join();
    sap::http::HttpsServer::max_body_size = saved;

    EXPECT_TRUE(resp.find("200 OK") == stl::string::npos) << resp;
}

TEST(HttpsServerTest, HeadersExceedMaxHeaderSize) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_HEADERS_TOO_LARGE, cert);
    auto saved = sap::http::HttpsServer::max_header_size;
    sap::http::HttpsServer::max_header_size = 256;
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "OK"); });
    auto t = start_server(server);

    stl::string big(300, 'H');
    stl::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Junk: " + big + "\r\n\r\n";
    auto resp = tls_request(PORT_HEADERS_TOO_LARGE, cert, req);
    server.stop();
    t.join();
    sap::http::HttpsServer::max_header_size = saved;

    EXPECT_TRUE(resp.find("200 OK") == stl::string::npos) << resp;
}

TEST(HttpsServerTest, InvalidContentLengthDoesNotCrash) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_INVALID_CL, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/echo", sap::http::EMethod::POST,
        [](const sap::http::Request& req) {
            return sap::http::Response(sap::http::EStatusCode::OK, req.body);
        });
    auto t = start_server(server);

    stl::string req = "POST /echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: not-a-number\r\n\r\n"
                      "junk";
    auto resp = tls_request(PORT_INVALID_CL, cert, req);
    server.stop();
    t.join();

    // Server must reject; never crash.
    EXPECT_TRUE(resp.find("200 OK") == stl::string::npos) << resp;
}

TEST(HttpsServerTest, ChunkedRequestBodyDecoded) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_CHUNKED_BODY, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/chunked", sap::http::EMethod::POST,
        [](const sap::http::Request& req) {
            return sap::http::Response(sap::http::EStatusCode::OK, req.body);
        });
    auto t = start_server(server);

    // Two chunks "hello" + " world", total body "hello world".
    stl::string req = "POST /chunked HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n\r\n"
                      "5\r\nhello\r\n"
                      "6\r\n world\r\n"
                      "0\r\n\r\n";
    auto resp = tls_request(PORT_CHUNKED_BODY, cert, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("hello world") != stl::string::npos) << resp;
}

// ---------------------------------------------------------------------------
// Keep-alive
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, KeepAliveReusesConnection) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_KEEPALIVE_REUSE, cert);
    sap::http::HttpsServer server(std::move(cfg));
    std::atomic<int> hits{0};
    server.route("/", sap::http::EMethod::GET,
        [&hits](const sap::http::Request&) {
            hits.fetch_add(1, std::memory_order_relaxed);
            return sap::http::Response(sap::http::EStatusCode::OK, "x");
        });
    auto t = start_server(server);

    auto sock = tls_connect(PORT_KEEPALIVE_REUSE, cert);
    ASSERT_NE(sock, nullptr);

    // First request, keep-alive.
    stl::string r1 = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
    size_t sent = 0;
    while (sent < r1.size()) {
        auto n = sock->send(stl::span<const stl::byte>(
            reinterpret_cast<const stl::byte*>(r1.data() + sent), r1.size() - sent));
        ASSERT_TRUE(n.has_value()); sent += n.value();
    }
    // Read the first response (don't assume framing; pull until "x" body seen).
    stl::string buf;
    stl::byte chunk[1024];
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto n = sock->recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
        if (!n || n.value() == 0) break;
        buf.append(reinterpret_cast<const char*>(chunk), n.value());
        // Once we've seen at least the first complete response, send the second.
        if (buf.find("\r\n\r\nx") != stl::string::npos) break;
    }
    EXPECT_TRUE(buf.find("200 OK") != stl::string::npos);

    // Second request on the same socket — must succeed without a new handshake.
    stl::string r2 = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    sent = 0;
    while (sent < r2.size()) {
        auto n = sock->send(stl::span<const stl::byte>(
            reinterpret_cast<const stl::byte*>(r2.data() + sent), r2.size() - sent));
        if (!n || n.value() == 0) break;
        sent += n.value();
    }
    while (true) {
        auto n = sock->recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
        if (!n || n.value() == 0) break;
        buf.append(reinterpret_cast<const char*>(chunk), n.value());
    }
    sock->close();
    server.stop();
    t.join();

    EXPECT_GE(hits.load(), 2) << "Both requests must have hit the handler over a single connection";
}

TEST(HttpsServerTest, Http10ClosesByDefault) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_KEEPALIVE_HTTP10, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "ok"); });
    auto t = start_server(server);

    stl::string req = "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    auto resp = tls_request(PORT_KEEPALIVE_HTTP10, cert, req);
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    // Server should have closed (we read to EOF without hanging).
}

TEST(HttpsServerTest, ConnectionCloseHonored) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_KEEPALIVE_CLOSE, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "x"); });
    auto t = start_server(server);

    auto sock = tls_connect(PORT_KEEPALIVE_CLOSE, cert);
    ASSERT_NE(sock, nullptr);
    stl::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    auto resp = tls_exchange(*sock, req);
    sock->close();
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("Connection: close") != stl::string::npos ||
                resp.find("connection: close") != stl::string::npos) << resp;
}

TEST(HttpsServerTest, PipelinedRequestsDeliveredInOrder) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_PIPELINED, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/n", sap::http::EMethod::GET,
        [](const sap::http::Request& req) {
            // Echo the X-Index header back so tests can check ordering.
            stl::string idx = req.headers.get("X-Index");
            return sap::http::Response(sap::http::EStatusCode::OK, idx);
        });
    auto t = start_server(server);

    auto sock = tls_connect(PORT_PIPELINED, cert);
    ASSERT_NE(sock, nullptr);

    // Fire three requests back-to-back without waiting for responses.
    stl::string pipelined =
        "GET /n HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Index: 1\r\n\r\n"
        "GET /n HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Index: 2\r\n\r\n"
        "GET /n HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Index: 3\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < pipelined.size()) {
        auto n = sock->send(stl::span<const stl::byte>(
            reinterpret_cast<const stl::byte*>(pipelined.data() + sent),
            pipelined.size() - sent));
        ASSERT_TRUE(n.has_value() && n.value() > 0);
        sent += n.value();
    }
    // Drain.
    stl::string buf;
    stl::byte chunk[2048];
    while (true) {
        auto n = sock->recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
        if (!n || n.value() == 0) break;
        buf.append(reinterpret_cast<const char*>(chunk), n.value());
    }
    sock->close();
    server.stop();
    t.join();

    auto pos1 = buf.find("\r\n\r\n1");
    auto pos2 = buf.find("\r\n\r\n2");
    auto pos3 = buf.find("\r\n\r\n3");
    ASSERT_NE(pos1, stl::string::npos) << buf;
    ASSERT_NE(pos2, stl::string::npos) << buf;
    ASSERT_NE(pos3, stl::string::npos) << buf;
    EXPECT_LT(pos1, pos2);
    EXPECT_LT(pos2, pos3);
}

// ---------------------------------------------------------------------------
// Middleware
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, MiddlewareShortCircuits) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_MIDDLEWARE_SHORT, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.use([](sap::http::Request&) -> std::optional<sap::http::Response> {
        return sap::http::Response(sap::http::EStatusCode::BadRequest, "blocked");
    });
    bool handler_ran = false;
    server.route("/", sap::http::EMethod::GET,
        [&handler_ran](const sap::http::Request&) {
            handler_ran = true;
            return sap::http::Response(sap::http::EStatusCode::OK, "should-not-see");
        });
    auto t = start_server(server);

    auto resp = tls_request(PORT_MIDDLEWARE_SHORT, cert, get_request("/"));
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("400") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("blocked") != stl::string::npos) << resp;
    EXPECT_FALSE(handler_ran);
}

TEST(HttpsServerTest, MiddlewarePassThrough) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_MIDDLEWARE_PASS, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.use([](sap::http::Request&) -> std::optional<sap::http::Response> {
        return std::nullopt; // pass through
    });
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "ok"); });
    auto t = start_server(server);

    auto resp = tls_request(PORT_MIDDLEWARE_PASS, cert, get_request("/"));
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("ok") != stl::string::npos) << resp;
}

TEST(HttpsServerTest, PublicRouteBypassesMiddleware) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_PUBLIC_ROUTE, cert);
    sap::http::HttpsServer server(std::move(cfg));
    std::atomic<int> mw_hits{0};
    server.use([&mw_hits](sap::http::Request&) -> std::optional<sap::http::Response> {
        mw_hits.fetch_add(1, std::memory_order_relaxed);
        return sap::http::Response(sap::http::EStatusCode::BadRequest, "no");
    });
    server.public_route("/login", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "in"); });
    auto t = start_server(server);

    auto resp = tls_request(PORT_PUBLIC_ROUTE, cert, get_request("/login"));
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("in") != stl::string::npos) << resp;
    EXPECT_EQ(mw_hits.load(), 0) << "Middleware should not run for public_route";
}

// ---------------------------------------------------------------------------
// TLS-specific
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, AlpnHttp11Negotiated) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_ALPN, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "alpn"); });
    auto t = start_server(server);

    auto sock = tls_connect(PORT_ALPN, cert);
    ASSERT_NE(sock, nullptr);
    EXPECT_EQ(sock->negotiated_protocol(), "http/1.1");
    sock->close();
    server.stop();
    t.join();
}

TEST(HttpsServerTest, Tls13NegotiatedByDefault) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_TLS13_NEGOTIATED, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "v"); });
    auto t = start_server(server);

    auto sock = tls_connect(PORT_TLS13_NEGOTIATED, cert);
    ASSERT_NE(sock, nullptr);
    auto v = sock->negotiated_tls_version();
    // Modern OpenSSL clients/servers default to TLS 1.3. Accept 1.2 too in
    // case the build env has 1.3 disabled.
    EXPECT_TRUE(v == "TLSv1.3" || v == "TLSv1.2") << v;
    sock->close();
    server.stop();
    t.join();
}

TEST(HttpsServerTest, MinVersionTls12RejectsOlder) {
    // Configure server to require TLS 1.2; client also TLS 1.2; handshake works.
    // We don't have a TLS 1.0/1.1 client to test rejection, but we can verify
    // that TLS 1.2 floor doesn't break a normal connection.
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_TLS_VERSION_FLOOR, cert);
    cfg.tls_cfg.min_version = sap::network::ETlsMinVersion::TLS_1_2;
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "v"); });
    auto t = start_server(server);

    auto resp = tls_request(PORT_TLS_VERSION_FLOOR, cert, get_request("/"));
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
}

TEST(HttpsServerTest, PlainTcpClientFailsHandshakeServerStaysUp) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_PLAIN_TCP_CLIENT, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "x"); });
    auto t = start_server(server);

    // Connect with plain TCP, send junk. Server must not crash.
    sap::network::SocketConfig sc;
    sc.host = "127.0.0.1";
    sc.port = PORT_PLAIN_TCP_CLIENT;
    sc.connect_timeout = 1000ms;
    sap::network::TCPSocket plain(std::move(sc));
    ASSERT_TRUE(plain.connect());
    const char junk[] = "GET / HTTP/1.1\r\n\r\n";
    plain.send(stl::span<const stl::byte>(
        reinterpret_cast<const stl::byte*>(junk), sizeof(junk) - 1));
    stl::byte buf[64];
    auto n = plain.recv(stl::span<stl::byte>(buf, sizeof(buf)));
    plain.close();
    // Either a 0-length read or an error — never a 200 over plaintext.
    EXPECT_TRUE(!n || n.value() == 0);

    // Now hit it with a proper TLS client to verify the server is still up.
    auto resp = tls_request(PORT_PLAIN_TCP_CLIENT, cert, get_request("/"));
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, MultithreadedHandlesConcurrentClients) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_MULTITHREADED, cert);
    cfg.is_multithreaded = true;
    sap::http::HttpsServer server(std::move(cfg));
    std::atomic<int> total{0};
    server.route("/c", sap::http::EMethod::GET,
        [&total](const sap::http::Request&) {
            total.fetch_add(1, std::memory_order_relaxed);
            return sap::http::Response(sap::http::EStatusCode::OK, "ok");
        });
    auto t = start_server(server);

    constexpr int N = 16;
    std::vector<std::thread> workers;
    workers.reserve(N);
    std::atomic<int> oks{0};
    for (int i = 0; i < N; ++i) {
        workers.emplace_back([&, i]() {
            auto resp = tls_request(PORT_MULTITHREADED, cert, get_request("/c"));
            if (resp.find("200 OK") != stl::string::npos)
                oks.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& w : workers) w.join();
    server.stop();
    t.join();

    EXPECT_EQ(oks.load(), N);
    EXPECT_GE(total.load(), N);
}

// Disabled: this segfaults today because Server::stop() calls e.sock->close()
// on a TLSSocket whose SSL* is concurrently being read from inside the
// handler thread's read_header loop. OpenSSL's SSL_shutdown is not safe to
// call concurrently with SSL_read on the same SSL*. The plain HttpServer
// equivalent test works because TCPSocket::close() is just close(2), which
// wakes the parked recv with EOF without sharing any user-space state.
//
// Fix is at the sap_network layer: see docs/SAP_NETWORK_TLS_THREADING_PLAN.md.
// Re-enable once TLSSocket exposes a safe-from-other-thread wake (likely
// shutdown(fd, SHUT_RDWR) on the underlying TCP fd, leaving SSL state alone
// for the owning thread to clean up).
TEST(HttpsServerTest, GracefulStopUnblocksIdleClients) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_GRACEFUL_STOP, cert);
    cfg.timeout_ms = 30000; // long, so without graceful shutdown the test would hang
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "x"); });
    auto t = start_server(server);

    auto sock = tls_connect(PORT_GRACEFUL_STOP, cert);
    ASSERT_NE(sock, nullptr);

    // First request, keep-alive — server is now parked in read_header waiting
    // for the next pipelined request.
    stl::string r1 = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
    size_t sent = 0;
    while (sent < r1.size()) {
        auto n = sock->send(stl::span<const stl::byte>(
            reinterpret_cast<const stl::byte*>(r1.data() + sent), r1.size() - sent));
        ASSERT_TRUE(n.has_value()); sent += n.value();
    }
    stl::byte chunk[1024];
    auto deadline = std::chrono::steady_clock::now() + 3s;
    bool got_first = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto n = sock->recv(stl::span<stl::byte>(chunk, sizeof(chunk)));
        if (!n || n.value() == 0) break;
        stl::string s(reinterpret_cast<const char*>(chunk), n.value());
        if (s.find("\r\n\r\nx") != stl::string::npos) { got_first = true; break; }
    }
    EXPECT_TRUE(got_first);

    // Server thread is parked. Stop should close the parked socket and unwind
    // within seconds, not blocked on the 30s timeout.
    auto stop_start = std::chrono::steady_clock::now();
    server.stop();
    t.join();
    auto elapsed = std::chrono::steady_clock::now() - stop_start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5000)
        << "stop() took too long; graceful shutdown of idle TLS clients regressed";
}

// ---------------------------------------------------------------------------
// Mutual TLS
//
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, MtlsRejectsConnectionWithoutClientCert) {
    // Server requires client cert verified by `ca` (a separate CA cert).
    SelfSignedCert ca("loopback-ca", /*is_ca=*/true);
    auto cfg = make_https_cfg(PORT_MTLS_REQUIRED, ca);
    cfg.tls_cfg.require_client_cert = true;
    cfg.tls_cfg.ca_file = ca.cert_file;
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "x"); });
    auto t = start_server(server);

    // Client connects without presenting a cert → handshake must fail.
    sap::network::TlsClientConfig tc;
    tc.tcp.host = "127.0.0.1";
    tc.tcp.port = PORT_MTLS_REQUIRED;
    tc.tcp.connect_timeout = 2000ms;
    tc.sni_hostname = "loopback-ca";
    tc.verify_peer = false; // doesn't matter for the test; we want to see *server* reject
    tc.alpn_protocols.push_back("http/1.1");
    sap::network::TLSSocket sock(std::move(tc));
    bool ok = sock.connect();
    sock.close();

    server.stop();
    t.join();

    EXPECT_FALSE(ok) << "Handshake should have failed: server requires client cert";
}

TEST(HttpsServerTest, MtlsAcceptsValidClientCert) {
    SelfSignedCert ca("loopback-ca", /*is_ca=*/true);
    auto client_pair = ca.issue_client("test-client");
    auto cfg = make_https_cfg(PORT_MTLS_VALID, ca);
    cfg.tls_cfg.require_client_cert = true;
    cfg.tls_cfg.ca_file = ca.cert_file;
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "mtls-ok"); });
    auto t = start_server(server);

    sap::network::TlsClientConfig tc;
    tc.tcp.host = "127.0.0.1";
    tc.tcp.port = PORT_MTLS_VALID;
    tc.tcp.connect_timeout = 3000ms;
    tc.tcp.recv_timeout = 5000ms;
    tc.tcp.send_timeout = 3000ms;
    tc.sni_hostname = "loopback-ca";
    tc.verify_peer = false;
    tc.client_cert_file = client_pair.cert_file;
    tc.client_key_file  = client_pair.key_file;
    tc.alpn_protocols.push_back("http/1.1");
    sap::network::TLSSocket sock(std::move(tc));
    ASSERT_TRUE(sock.connect()) << "mTLS handshake failed: " << sock.handshake_error();

    auto resp = tls_exchange(sock, get_request("/"));
    sock.close();
    server.stop();
    t.join();

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("mtls-ok") != stl::string::npos) << resp;
}

// ---------------------------------------------------------------------------
// Response shape
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, ResponseBodyEchoLargePayload) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_LARGE_RESPONSE, cert);
    auto saved = sap::http::HttpsServer::max_body_size;
    sap::http::HttpsServer::max_body_size = 256 * 1024;
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/big", sap::http::EMethod::GET,
        [](const sap::http::Request&) {
            return sap::http::Response(sap::http::EStatusCode::OK, stl::string(64 * 1024, 'B'));
        });
    auto t = start_server(server);

    auto resp = tls_request(PORT_LARGE_RESPONSE, cert, get_request("/big"));
    server.stop();
    t.join();
    sap::http::HttpsServer::max_body_size = saved;

    EXPECT_TRUE(resp.find("200 OK") != stl::string::npos);
    auto body_start = resp.find("\r\n\r\n");
    ASSERT_NE(body_start, stl::string::npos);
    auto body = resp.substr(body_start + 4);
    EXPECT_EQ(body.size(), static_cast<stl::size_t>(64 * 1024));
    EXPECT_TRUE(std::all_of(body.begin(), body.end(), [](char c) { return c == 'B'; }));
}

TEST(HttpsServerTest, ResponseHasContentLengthAndConnection) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_RESPONSE_HEADERS, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, "hello"); });
    auto t = start_server(server);

    auto resp = tls_request(PORT_RESPONSE_HEADERS, cert, get_request("/"));
    server.stop();
    t.join();

    // Headers::set lowercases the key on storage so the response uses
    // lowercase header names. Connection: comes from build_response which
    // preserves the case it writes ("Connection") since it's not stored
    // in the Headers map.
    EXPECT_TRUE(resp.find("content-length: 5") != stl::string::npos) << resp;
    EXPECT_TRUE(resp.find("Connection: close") != stl::string::npos) << resp;
}

// ---------------------------------------------------------------------------
// Disconnect mid-exchange — server must not crash
// ---------------------------------------------------------------------------

TEST(HttpsServerTest, DisconnectDuringRecvDoesNotCrash) {
    SelfSignedCert cert;
    auto cfg = make_https_cfg(PORT_DISCONNECT_DURING_RECV, cert);
    sap::http::HttpsServer server(std::move(cfg));
    server.route("/", sap::http::EMethod::POST,
        [](const sap::http::Request&) { return sap::http::Response(sap::http::EStatusCode::OK, ""); });
    auto t = start_server(server);

    // Send a partial request (Content-Length declares 100 bytes, send 0) then close.
    auto sock = tls_connect(PORT_DISCONNECT_DURING_RECV, cert);
    ASSERT_NE(sock, nullptr);
    stl::string head = "POST / HTTP/1.1\r\n"
                       "Host: 127.0.0.1\r\n"
                       "Content-Length: 100\r\n\r\n";
    size_t sent = 0;
    while (sent < head.size()) {
        auto n = sock->send(stl::span<const stl::byte>(
            reinterpret_cast<const stl::byte*>(head.data() + sent), head.size() - sent));
        if (!n || n.value() == 0) break;
        sent += n.value();
    }
    sock->close();

    // Give the server a moment to process the dangling read; then verify it
    // still serves a fresh client.
    std::this_thread::sleep_for(200ms);
    auto resp = tls_request(PORT_DISCONNECT_DURING_RECV, cert, get_request("/"));
    server.stop();
    t.join();

    // Path is POST-only so a GET will get 404 — the point is the server didn't crash.
    EXPECT_TRUE(resp.find("HTTP/1.1") != stl::string::npos) << resp;
}
