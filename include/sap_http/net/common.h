#pragma once

#include <sap_core/stl/result.h>
#include <sap_core/stl/string.h>
#include <sap_core/types.h>

namespace sap::http {

    // Decode an HTTP/1.1 chunked transfer-encoded body from `sock`. `buffer` holds any bytes
    // already read past the headers and is consumed/refilled as needed. Returns the fully
    // decoded body, or an error if the stream is malformed or exceeds `max_size`.
    stl::result<stl::string> read_chunked_body(i32 sock, stl::string& buffer, stl::size_t max_size);

} // namespace sap::http
