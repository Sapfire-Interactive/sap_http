#pragma once

#include "sap_http/net/http.h"

#include <sap_core/stl/map.h>
#include <sap_core/stl/result.h>
#include <sap_core/stl/string.h>
#include <sap_core/stl/string_view.h>
#include <sap_core/stl/vector.h>

#include <sstream>
#include <string>

namespace sap::http::detail {

    inline stl::string to_lower(stl::string_view s) {
        stl::string out;
        out.reserve(s.size());
        for (char c : s)
            out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        return out;
    }

    inline bool is_http_1_0(const stl::string& header_section) {
        auto        eol  = header_section.find("\r\n");
        stl::string line = (eol == stl::string::npos) ? stl::string(header_section) : header_section.substr(0, eol);
        return line.find("HTTP/1.0") != stl::string::npos;
    }

    inline const char* status_reason_phrase(EStatusCode code) {
        switch (code) {
        case EStatusCode::OK:                          return "OK";
        case EStatusCode::Created:                     return "Created";
        case EStatusCode::NoContent:                   return "No Content";
        case EStatusCode::BadRequest:                  return "Bad Request";
        case EStatusCode::NotFound:                    return "Not Found";
        case EStatusCode::MethodNotAllowed:            return "Method Not Allowed";
        case EStatusCode::PayloadTooLarge:             return "Payload Too Large";
        case EStatusCode::RequestHeaderFieldsTooLarge: return "Request Header Fields Too Large";
        case EStatusCode::InternalServerError:         return "Internal Server Error";
        default:                                       return "Unknown";
        }
    }

    inline stl::result<Request> parse_request(const stl::string& raw_request) {
        std::istringstream stream(raw_request);
        stl::string        line;
        if (!std::getline(stream, line))
            return stl::make_error<Request>("Empty request");
        std::istringstream first_line(line);
        stl::string        method_str, path_str, version;
        first_line >> method_str >> path_str >> version;
        auto method     = string_to_method(method_str);
        auto url_result = URL::from_path(path_str);
        if (!url_result)
            return stl::make_error<Request>("{}", url_result.error());
        Request req(method, std::move(url_result.value()));
        while (std::getline(stream, line) && line != "\r" && !line.empty()) {
            if (line.back() == '\r')
                line.pop_back();
            auto colon = line.find(':');
            if (colon != stl::string::npos) {
                auto key   = line.substr(0, colon);
                auto value = line.substr(colon + 1);
                if (!value.empty() && value[0] == ' ')
                    value.erase(0, 1);
                req.headers.set(key, value);
            }
        }
        return req;
    }

    // Caller is responsible for the Content-Length / Connection auto-injection
    // contract — see comment in the original handler for why the auto-inject
    // matters for keep-alive framing.
    inline stl::string build_response(const Response& resp, bool keep_alive) {
        std::ostringstream ss;
        ss << "HTTP/1.1 " << static_cast<i32>(resp.status_code) << " " << status_reason_phrase(resp.status_code) << "\r\n";
        bool has_content_length = false;
        bool has_connection     = false;
        for (const auto& [key, value] : resp.headers.data) {
            auto lk = to_lower(key);
            if (lk == "content-length")
                has_content_length = true;
            else if (lk == "connection")
                has_connection = true;
            ss << key << ": " << value << "\r\n";
        }
        if (!has_content_length)
            ss << "Content-Length: " << resp.body.size() << "\r\n";
        if (!has_connection)
            ss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
        ss << "\r\n";
        if (!resp.body.empty())
            ss << resp.body;
        return ss.str();
    }

    template <typename R>
    struct RouteMatch {
        const R*                           route = nullptr;
        stl::map<stl::string, stl::string> params;
    };

    template <typename R>
    RouteMatch<R> match_route(const stl::vector<R>& routes, EMethod method, const stl::string& path) {
        stl::vector<stl::string> req_segments;
        {
            size_t start = 0;
            if (!path.empty() && path[0] == '/')
                start = 1;
            while (start <= path.size()) {
                size_t slash = path.find('/', start);
                size_t end   = (slash == stl::string::npos) ? path.size() : slash;
                if (end > start)
                    req_segments.push_back(path.substr(start, end - start));
                if (slash == stl::string::npos)
                    break;
                start = slash + 1;
            }
        }

        RouteMatch<R> out;
        int           best_score = -1;
        for (const auto& route : routes) {
            if (route.method != method)
                continue;
            if (route.has_params) {
                if (route.segments.size() != req_segments.size())
                    continue;
                stl::map<stl::string, stl::string> params;
                int                                literals = 0;
                bool                               ok       = true;
                for (size_t i = 0; i < route.segments.size(); ++i) {
                    if (route.segments[i].is_param) {
                        params[route.segments[i].text] = req_segments[i];
                    } else if (route.segments[i].text == req_segments[i]) {
                        ++literals;
                    } else {
                        ok = false;
                        break;
                    }
                }
                if (ok && literals > best_score) {
                    best_score  = literals;
                    out.route   = &route;
                    out.params  = std::move(params);
                }
            } else {
                if (route.path == path) {
                    int score = static_cast<int>(route.segments.size()) + 1000;
                    if (score > best_score) {
                        best_score = score;
                        out.route  = &route;
                        out.params.clear();
                    }
                } else if (path.size() > route.path.size() && path.substr(0, route.path.size()) == route.path &&
                           path[route.path.size()] == '/') {
                    int score = -1 + static_cast<int>(route.segments.size());
                    if (score > best_score) {
                        best_score = score;
                        out.route  = &route;
                        out.params.clear();
                    }
                }
            }
        }
        return out;
    }

} // namespace sap::http::detail
