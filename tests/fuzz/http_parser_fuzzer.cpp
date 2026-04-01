#include "neustack/app/http_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

using namespace neustack;

namespace {

void touch_request(const HttpRequest &request) {
    (void)request.method;
    (void)request.path.size();
    (void)request.version.size();
    (void)request.body.size();
    (void)request.query_params.size();
    for (const auto &[key, values] : request.headers) {
        (void)key.size();
        for (const auto &value : values) {
            (void)value.size();
        }
    }
}

void touch_response(const HttpResponse &response) {
    (void)response.status;
    (void)response.body.size();
    for (const auto &[key, values] : response.headers) {
        (void)key.size();
        for (const auto &value : values) {
            (void)value.size();
        }
    }
}

template <typename Parser, typename Consumer>
void feed_incrementally(Parser &parser, const uint8_t *data, size_t len,
                        uint8_t stride_seed, Consumer consumer) {
    size_t offset = 0;
    size_t stride = static_cast<size_t>(stride_seed % 31) + 1;

    while (offset < len && !parser.is_complete() && !parser.has_error()) {
        size_t chunk = std::min(stride, len - offset);
        parser.feed(data + offset, chunk);
        offset += chunk;
        stride = ((stride * 5) + 3) % 31 + 1;
    }

    if (parser.is_complete()) {
        consumer();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    HttpRequestParser request_parser;
    request_parser.feed(data, len);
    if (request_parser.is_complete()) {
        touch_request(request_parser.request());
    }

    request_parser.reset();
    feed_incrementally(request_parser, data, len, len > 0 ? data[0] : 0,
                       [&request_parser]() { touch_request(request_parser.request()); });

    HttpResponseParser response_parser;
    response_parser.feed(data, len);
    if (response_parser.is_complete()) {
        touch_response(response_parser.response());
    }

    response_parser.reset();
    feed_incrementally(response_parser, data, len, len > 1 ? data[1] : 17,
                       [&response_parser]() { touch_response(response_parser.response()); });

    return 0;
}
