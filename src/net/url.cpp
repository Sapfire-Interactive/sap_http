#include "sap_http/net/http.h"

namespace sap::http {

    // Returns 0-15 for a valid hex digit, -1 otherwise.
    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    }

    // Percent-decode a path. Rejects:
    //   - malformed % escapes (%, %X, %XZ)
    //   - encoded slashes (%2F / %2f) — would confuse segment-based routing
    static stl::result<stl::string> percent_decode_path(stl::string_view s) {
        stl::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%') {
                if (i + 2 >= s.size())
                    return stl::make_error<stl::string>("Truncated percent escape");
                int hi = hex_value(s[i + 1]);
                int lo = hex_value(s[i + 2]);
                if (hi < 0 || lo < 0)
                    return stl::make_error<stl::string>("Invalid hex in percent escape");
                char decoded = static_cast<char>((hi << 4) | lo);
                if (decoded == '/')
                    return stl::make_error<stl::string>("Encoded slash (%2F) not allowed in path");
                out.push_back(decoded);
                i += 2;
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }

    // Returns true if the path contains a ".." segment (path traversal attempt).
    // A ".." segment is two literal dots bounded by slashes or string edges.
    // Does NOT flag "/users/file..txt" (the ".." is not a full segment).
    static bool has_traversal_segment(stl::string_view path) {
        size_t start = 0;
        for (size_t i = 0; i <= path.size(); ++i) {
            if (i == path.size() || path[i] == '/') {
                if (i - start == 2 && path[start] == '.' && path[start + 1] == '.')
                    return true;
                start = i + 1;
            }
        }
        return false;
    }

    stl::result<URL> URL::from_path(stl::string_view path_and_query) {
        URL u;
        stl::string_view raw_path;
        auto query_pos = path_and_query.find('?');
        if (query_pos != stl::string_view::npos) {
            raw_path = path_and_query.substr(0, query_pos);
            u.query = path_and_query.substr(query_pos);
        } else {
            raw_path = path_and_query;
        }

        auto decoded = percent_decode_path(raw_path);
        if (!decoded)
            return stl::make_error<URL>("{}", decoded.error());
        u.path = std::move(decoded.value());

        if (has_traversal_segment(u.path))
            return stl::make_error<URL>("Path traversal attempt rejected");

        return u;
    }

    stl::result<URL> URL::parse(stl::string_view raw_url) {
        URL u;
        size_t pos = 0;
        // Parse scheme
        auto scheme_end = raw_url.find("://");
        if (scheme_end == stl::string_view::npos) {
            return stl::make_error<URL>("Invalid URL: missing scheme");
        }
        u.scheme = raw_url.substr(0, scheme_end);
        pos = scheme_end + 3;
        // Parse host and optional port
        auto path_start = raw_url.find('/', pos);
        auto query_start = raw_url.find('?', pos);
        auto host_end = std::min(path_start, query_start);
        if (host_end == stl::string_view::npos) {
            host_end = raw_url.length();
        }
        auto host_port = raw_url.substr(pos, host_end - pos);
        auto port_pos = host_port.find(':');
        if (port_pos != stl::string_view::npos) {
            u.host = host_port.substr(0, port_pos);
            u.port = host_port.substr(port_pos + 1);
        } else {
            u.host = host_port;
            u.port = (u.scheme == "https") ? "443" : "80";
        }
        // Parse path
        if (path_start != stl::string_view::npos) {
            auto path_end = (query_start != stl::string_view::npos) ? query_start : raw_url.length();
            u.path = raw_url.substr(path_start, path_end - path_start);
        } else {
            u.path = "/";
        }
        // Parse query
        if (query_start != stl::string_view::npos) {
            u.query = raw_url.substr(query_start);
        }
        return u;
    }
} // namespace sap::http