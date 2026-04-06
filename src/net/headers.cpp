#include "sap_http/net/http.h"

namespace sap::http {

    void Headers::set(stl::string_view key, stl::string_view value) {
        stl::string lower_key(key);
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        data[lower_key] = value;
    }

    stl::string Headers::get(stl::string_view key) const {
        stl::string lower_key(key);
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        auto it = data.find(lower_key);
        return (it != data.end()) ? it->second : "";
    }

    bool Headers::has(stl::string_view key) const {
        stl::string lower_key(key);
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        return data.find(lower_key) != data.end();
    }
} // namespace sap::http