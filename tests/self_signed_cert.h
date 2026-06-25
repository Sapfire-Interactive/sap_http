#pragma once

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <sap_core/stl/string.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

class SelfSignedCert {
public:
    explicit SelfSignedCert(stl::string cn = "localhost", bool is_ca = false) {
        namespace fs = std::filesystem;
        std::random_device rd;
        m_dir = fs::temp_directory_path() / fs::path{"sap_http_tls_test_" + std::to_string(rd())};
        fs::create_directories(m_dir);
        cert_file = (m_dir / "cert.pem").string();
        key_file  = (m_dir / "key.pem").string();
        m_pkey    = ::EVP_RSA_gen(2048);
        m_x509    = make_cert(cn, m_pkey, m_pkey, nullptr, is_ca);
        write_pair(cert_file, key_file, m_x509, m_pkey);
    }

    ~SelfSignedCert() {
        if (m_x509) ::X509_free(m_x509);
        if (m_pkey) ::EVP_PKEY_free(m_pkey);
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    SelfSignedCert(const SelfSignedCert&)            = delete;
    SelfSignedCert& operator=(const SelfSignedCert&) = delete;

    struct Issued {
        stl::string cert_file;
        stl::string key_file;
    };
    Issued issue_client(stl::string cn) {
        EVP_PKEY* client_pkey = ::EVP_RSA_gen(2048);
        X509*     client_x509 = make_cert(cn, client_pkey, m_pkey, m_x509, false);
        Issued    i;
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
    static X509* make_cert(const stl::string& cn, EVP_PKEY* pkey, EVP_PKEY* signer_pkey, X509* signer_x509, bool is_ca) {
        X509* x = ::X509_new();
        ::ASN1_INTEGER_set(::X509_get_serialNumber(x), std::rand());
        ::X509_gmtime_adj(::X509_getm_notBefore(x), 0);
        ::X509_gmtime_adj(::X509_getm_notAfter(x), 31536000L);
        ::X509_set_pubkey(x, pkey);
        X509_NAME* name = ::X509_get_subject_name(x);
        ::X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
        if (signer_x509)
            ::X509_set_issuer_name(x, ::X509_get_subject_name(signer_x509));
        else
            ::X509_set_issuer_name(x, name);

        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        ::X509V3_set_ctx(&v3ctx, signer_x509 ? signer_x509 : x, x, nullptr, nullptr, 0);
        if (is_ca) {
            if (X509_EXTENSION* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_basic_constraints, "critical,CA:TRUE")) {
                ::X509_add_ext(x, ext, -1);
                ::X509_EXTENSION_free(ext);
            }
            if (X509_EXTENSION* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_key_usage, "critical,keyCertSign,cRLSign")) {
                ::X509_add_ext(x, ext, -1);
                ::X509_EXTENSION_free(ext);
            }
        } else {
            if (cn == "localhost") {
                if (X509_EXTENSION* ext =
                        ::X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1")) {
                    ::X509_add_ext(x, ext, -1);
                    ::X509_EXTENSION_free(ext);
                }
            } else {
                if (X509_EXTENSION* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_ext_key_usage, "clientAuth")) {
                    ::X509_add_ext(x, ext, -1);
                    ::X509_EXTENSION_free(ext);
                }
            }
        }
        ::X509_sign(x, signer_pkey, ::EVP_sha256());
        return x;
    }

    static void write_pair(const stl::string& cert_path, const stl::string& key_path, X509* x509, EVP_PKEY* pkey) {
        FILE* fp = std::fopen(cert_path.c_str(), "wb");
        ::PEM_write_X509(fp, x509);
        std::fclose(fp);
        fp = std::fopen(key_path.c_str(), "wb");
        ::PEM_write_PrivateKey(fp, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(fp);
    }

    std::filesystem::path m_dir;
    EVP_PKEY*             m_pkey = nullptr;
    X509*                 m_x509 = nullptr;
};
