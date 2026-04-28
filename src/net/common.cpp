#include "sap_http/net/common.h"

#include <sap_network/tcp_socket.h>
#include <sap_network/tls_socket.h>

namespace sap::http {

    template <sap::network::Socket S>
    stl::result<stl::string> read_chunked_body(S& sock, stl::string& buffer, stl::size_t max_size) {
        stl::string body;
        stl::byte tmp[4096];
        while (true) {
            stl::size_t crlf;
            while ((crlf = buffer.find("\r\n")) == stl::string::npos) {
                auto n = sock.recv(stl::span<stl::byte>(tmp, sizeof(tmp)));
                if (!n || n.value() == 0)
                    return stl::make_error<stl::string>("Connection closed in chunk size");
                buffer.append(reinterpret_cast<const char*>(tmp), n.value());
                if (buffer.size() > max_size)
                    return stl::make_error<stl::string>("Chunked data exceeds max size");
            }
            stl::string size_line = buffer.substr(0, crlf);
            auto semi = size_line.find(';');
            if (semi != stl::string::npos)
                size_line.resize(semi);
            stl::size_t chunk_size = 0;
            try {
                chunk_size = std::stoull(size_line, nullptr, 16);
            } catch (...) {
                return stl::make_error<stl::string>("Invalid chunk size");
            }
            buffer.erase(0, crlf + 2);
            if (chunk_size == 0) {
                // Consume optional trailer headers up to and including the final empty line.
                while (true) {
                    stl::size_t end;
                    while ((end = buffer.find("\r\n")) == stl::string::npos) {
                        auto n = sock.recv(stl::span<stl::byte>(tmp, sizeof(tmp)));
                        if (!n || n.value() == 0)
                            return stl::make_error<stl::string>("Connection closed in trailer");
                        buffer.append(reinterpret_cast<const char*>(tmp), n.value());
                    }
                    bool empty_line = (end == 0);
                    buffer.erase(0, end + 2);
                    if (empty_line)
                        break;
                }
                return body;
            }
            if (body.size() + chunk_size > max_size)
                return stl::make_error<stl::string>("Chunked body exceeds max size");
            while (buffer.size() < chunk_size + 2) {
                auto n = sock.recv(stl::span<stl::byte>(tmp, sizeof(tmp)));
                if (!n || n.value() == 0)
                    return stl::make_error<stl::string>("Connection closed in chunk data");
                buffer.append(reinterpret_cast<const char*>(tmp), n.value());
            }
            body.append(buffer, 0, chunk_size);
            if (buffer[chunk_size] != '\r' || buffer[chunk_size + 1] != '\n')
                return stl::make_error<stl::string>("Missing CRLF after chunk");
            buffer.erase(0, chunk_size + 2);
        }
    }

    template stl::result<stl::string> read_chunked_body<sap::network::TCPSocket>(
        sap::network::TCPSocket&, stl::string&, stl::size_t);
    template stl::result<stl::string> read_chunked_body<sap::network::TLSSocket>(
        sap::network::TLSSocket&, stl::string&, stl::size_t);

} // namespace sap::http
