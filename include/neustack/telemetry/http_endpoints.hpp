#ifndef NEUSTACK_TELEMETRY_HTTP_ENDPOINTS_HPP
#define NEUSTACK_TELEMETRY_HTTP_ENDPOINTS_HPP

#include "neustack/app/http_server.hpp"
#include "neustack/telemetry/telemetry_api.hpp"

namespace neustack::telemetry {

void register_http_endpoints(HttpServer& server, TelemetryAPI& api);

} // namespace neustack::telemetry

#endif // NEUSTACK_TELEMETRY_HTTP_ENDPOINTS_HPP