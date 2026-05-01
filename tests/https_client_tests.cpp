// Tests for sap::http::HttpsClient (= Client<sap::network::TLSSocket>).
//
// Each test spins up an HttpsServer in a background thread and drives it via
// an HttpsClient instance. Self-signed certs are generated per test run using
// the same SelfSignedCert helper pattern as https_server_tests.cpp.
// Ports start at 14001 to avoid conflicts with other test suites.

#include <gtest/gtest.h>
#include "sap_http/net/http.h"

#include <sap_network/tls_socket.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr u16 PORT_BASIC_GET           = 14001;
constexpr u16 PORT_POST                = 14002;
constexpr u16 PORT_VERIFY_PEER_FALSE   = 14003;
constexpr u16 PORT_VERIFY_PEER_REJECTS = 14004;
constexpr u16 PORT_POOLING             = 14005;
constexpr u16 PORT_CLEAR_POOL          = 14006;
constexpr u16 PORT_MTLS                = 14007;

// ---------------------------------------------------------------------------
// Self-signed cert fixture — mirrors the one in https_server_tests.cpp.
// Wrapped in an anonymous namespace to keep linkage internal.
// ---------------------------------------------------------------------------

class SelfSignedCert {
public:
    explicit SelfSignedCert(stl::string cn = "localhost", bool is_ca = false) {
        std::random_device rd;
        m_dir = fs::temp_directory_path() / fs::path{"sap_http_cli_test_" + std::to_string(rd())};
        fs::create_directories(m_dir);
        cert_file = (m_dir / "cert.pem").string();
        key_file  = (m_dir / "key.pem").string();
        m_pkey = ::EVP_RSA_gen(2048);
        m_x509 = make_cert(cn, m_pkey, m_pkey, nullptr, is_ca);
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

    struct Issued { stl::string cert_file; stl::string key_file; };
    Issued issue_client(stl::string cn) {
        EVP_PKEY* cpkey = ::EVP_RSA_gen(2048);
        X509*     cx509 = make_cert(cn, cpkey, m_pkey, m_x509, false);
        Issued i;
        i.cert_file = (m_dir / (cn + "_cert.pem")).string();
        i.key_file  = (m_dir / (cn + "_key.pem")).string();
        write_pair(i.cert_file, i.key_file, cx509, cpkey);
        ::X509_free(cx509);
        ::EVP_PKEY_free(cpkey);
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
        ::X509_gmtime_adj(::X509_getm_notAfter(x), 31536000L);
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
            if (auto* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_basic_constraints, "critical,CA:TRUE")) {
                ::X509_add_ext(x, ext, -1); ::X509_EXTENSION_free(ext);
            }
            if (auto* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_key_usage, "critical,keyCertSign,cRLSign")) {
                ::X509_add_ext(x, ext, -1); ::X509_EXTENSION_free(ext);
            }
        } else if (cn == "localhost") {
            if (auto* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1")) {
                ::X509_add_ext(x, ext, -1); ::X509_EXTENSION_free(ext);
            }
        } else {
            if (auto* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_ext_key_usage, "clientAuth")) {
                ::X509_add_ext(x, ext, -1); ::X509_EXTENSION_free(ext);
            }
        }
        ::X509_sign(x, signer_pkey, ::EVP_sha256());
        return x;
    }

    static void write_pair(const stl::string& cp, const stl::string& kp, X509* x509, EVP_PKEY* pkey) {
        FILE* fp = std::fopen(cp.c_str(), "wb");
        ::PEM_write_X509(fp, x509); std::fclose(fp);
        fp = std::fopen(kp.c_str(), "wb");
        ::PEM_write_PrivateKey(fp, pkey, nullptr, nullptr, 0, nullptr, nullptr); std::fclose(fp);
    }

    fs::path  m_dir;
    EVP_PKEY* m_pkey = nullptr;
    X509*     m_x509 = nullptr;
};

// Build an HttpsServerConfig with the given cert/key.
static sap::http::HttpsServerConfig make_server_cfg(u16 port, const SelfSignedCert& cert) {
    sap::http::HttpsServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.tls_cfg.cert_file = cert.cert_file;
    cfg.tls_cfg.key_file  = cert.key_file;
    return cfg;
}

// Build an HttpsClientConfig that trusts the given self-signed cert as the CA.
static sap::http::HttpsClientConfig make_client_cfg(const SelfSignedCert& cert) {
    sap::http::HttpsClientConfig cfg;
    cfg.ca_file = cert.cert_file;
    return cfg;
}

static sap::http::Request make_get(u16 port, stl::string_view path = "/") {
    auto url = sap::http::URL::parse(
        "https://127.0.0.1:" + std::to_string(port) + stl::string(path)).value();
    return sap::http::Request(sap::http::EMethod::GET, std::move(url));
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(HttpsClientTest, BasicGetRequest) {
    SelfSignedCert cert;
    sap::http::HttpsServer server{make_server_cfg(PORT_BASIC_GET, cert)};
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) {
            return sap::http::Response(sap::http::EStatusCode::OK, "hello tls");
        });
    ASSERT_TRUE(server.start().has_value());
    server.run_async();
    std::this_thread::sleep_for(100ms);

    sap::http::HttpsClient client{make_client_cfg(cert)};
    auto result = client.send_req(make_get(PORT_BASIC_GET));

    server.stop();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().status_code, sap::http::EStatusCode::OK);
    EXPECT_EQ(result.value().body, "hello tls");
}

TEST(HttpsClientTest, PostRequest) {
    SelfSignedCert cert;
    sap::http::HttpsServer server{make_server_cfg(PORT_POST, cert)};
    server.route("/echo", sap::http::EMethod::POST,
        [](const sap::http::Request& req) {
            return sap::http::Response(sap::http::EStatusCode::OK, req.body);
        });
    ASSERT_TRUE(server.start().has_value());
    server.run_async();
    std::this_thread::sleep_for(100ms);

    sap::http::HttpsClient client{make_client_cfg(cert)};
    auto url = sap::http::URL::parse("https://127.0.0.1:" + std::to_string(PORT_POST) + "/echo").value();
    sap::http::Request req(sap::http::EMethod::POST, std::move(url));
    req.set_body("payload");
    auto result = client.send_req(req);

    server.stop();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, "payload");
}

// Static convenience methods on the default instance must reject invalid URLs
// without hitting any network, same as HttpClient.
TEST(HttpsClientTest, InvalidUrlGet) {
    auto future = sap::http::HttpsClient::get("not-a-valid-url");
    auto result = future.get();
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.has_error());
}

TEST(HttpsClientTest, InvalidUrlPost) {
    auto future = sap::http::HttpsClient::post("not-a-valid-url", "body");
    auto result = future.get();
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.has_error());
}

// verify_peer=false skips certificate validation — connection to a server
// presenting a self-signed cert must succeed.
TEST(HttpsClientTest, VerifyPeerFalse) {
    SelfSignedCert cert;
    sap::http::HttpsServer server{make_server_cfg(PORT_VERIFY_PEER_FALSE, cert)};
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) {
            return sap::http::Response(sap::http::EStatusCode::OK, "ok");
        });
    ASSERT_TRUE(server.start().has_value());
    server.run_async();
    std::this_thread::sleep_for(100ms);

    sap::http::HttpsClientConfig cfg;
    cfg.verify_peer = false;
    sap::http::HttpsClient client{cfg};
    auto result = client.send_req(make_get(PORT_VERIFY_PEER_FALSE));

    server.stop();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, "ok");
}

// Default verify_peer=true with no CA supplied must refuse a self-signed cert.
TEST(HttpsClientTest, VerifyPeerTrueRejectsSelfSigned) {
    SelfSignedCert cert;
    sap::http::HttpsServer server{make_server_cfg(PORT_VERIFY_PEER_REJECTS, cert)};
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) {
            return sap::http::Response(sap::http::EStatusCode::OK, "ok");
        });
    ASSERT_TRUE(server.start().has_value());
    server.run_async();
    std::this_thread::sleep_for(100ms);

    // No ca_file — peer verification must fail.
    sap::http::HttpsClient client; // default config: verify_peer=true
    auto result = client.send_req(make_get(PORT_VERIFY_PEER_REJECTS));

    server.stop();
    EXPECT_FALSE(result.has_value()) << "expected TLS verification failure";
}

// Three sequential requests to the same HTTPS endpoint over a single client
// instance must all succeed (connection pooling keeps the TLS session alive).
TEST(HttpsClientTest, ConnectionPooling) {
    SelfSignedCert cert;
    sap::http::HttpsServer server{make_server_cfg(PORT_POOLING, cert)};
    stl::atomic<int> hits{0};
    server.route("/", sap::http::EMethod::GET,
        [&](const sap::http::Request&) {
            ++hits;
            return sap::http::Response(sap::http::EStatusCode::OK, "ok");
        });
    ASSERT_TRUE(server.start().has_value());
    server.run_async();
    std::this_thread::sleep_for(100ms);

    sap::http::HttpsClient client{make_client_cfg(cert)};
    for (int i = 0; i < 3; ++i) {
        auto r = client.send_req(make_get(PORT_POOLING));
        ASSERT_TRUE(r.has_value()) << r.error();
        EXPECT_EQ(r.value().body, "ok");
    }

    server.stop();
    EXPECT_EQ(hits.load(), 3);
}

// clear_pool() must discard all pooled TLS sockets so the next request opens
// a fresh connection (new TLS handshake).
TEST(HttpsClientTest, ClearPoolForcesReconnect) {
    SelfSignedCert cert;
    sap::http::HttpsServer server{make_server_cfg(PORT_CLEAR_POOL, cert)};
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) {
            return sap::http::Response(sap::http::EStatusCode::OK, "ok");
        });
    ASSERT_TRUE(server.start().has_value());
    server.run_async();
    std::this_thread::sleep_for(100ms);

    sap::http::HttpsClient client{make_client_cfg(cert)};
    ASSERT_TRUE(client.send_req(make_get(PORT_CLEAR_POOL)).has_value());
    client.clear_pool();
    // Second request must succeed after pool was cleared.
    auto r = client.send_req(make_get(PORT_CLEAR_POOL));
    server.stop();
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r.value().body, "ok");
}

// Mutual TLS: server requires a client cert signed by its CA; client supplies one.
// The server cert is the CA cert itself (CN="mtls-ca"), so the client skips peer
// verification — the test exercises the client-cert handshake path, not server-cert
// validation (that is already covered by VerifyPeerTrueRejectsSelfSigned).
TEST(HttpsClientTest, MutualTLS) {
    SelfSignedCert ca{"mtls-ca", /*is_ca=*/true};
    auto client_creds = ca.issue_client("test-client");

    sap::http::HttpsServerConfig server_cfg = make_server_cfg(PORT_MTLS, ca);
    server_cfg.tls_cfg.ca_file             = ca.cert_file;
    server_cfg.tls_cfg.require_client_cert = true;

    sap::http::HttpsServer server{server_cfg};
    server.route("/", sap::http::EMethod::GET,
        [](const sap::http::Request&) {
            return sap::http::Response(sap::http::EStatusCode::OK, "authed");
        });
    ASSERT_TRUE(server.start().has_value());
    server.run_async();
    std::this_thread::sleep_for(100ms);

    sap::http::HttpsClientConfig client_cfg;
    client_cfg.verify_peer      = false; // CA cert has no IP SAN for 127.0.0.1
    client_cfg.client_cert_file = client_creds.cert_file;
    client_cfg.client_key_file  = client_creds.key_file;

    sap::http::HttpsClient client{client_cfg};
    auto result = client.send_req(make_get(PORT_MTLS));

    server.stop();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().body, "authed");
}
