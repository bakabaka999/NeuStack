#include "neustack/app/dns_client.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace neustack;

namespace {

void touch_record(const DNSRecord &record) {
    (void)record.name.size();
    (void)record.ttl;

    if (auto ip = record.as_ipv4()) {
        (void)*ip;
        return;
    }

    if (auto name = record.as_name()) {
        (void)name->size();
        return;
    }

    if (auto raw = std::get_if<std::vector<uint8_t>>(&record.data)) {
        (void)raw->size();
    }
}

void touch_response(const DNSResponse &response) {
    (void)response.id;
    (void)response.rcode;
    (void)response.get_ip();

    for (const auto &record : response.answers) {
        touch_record(record);
    }
    for (const auto &record : response.authorities) {
        touch_record(record);
    }
    for (const auto &record : response.additionals) {
        touch_record(record);
    }
}

void exercise(const uint8_t *data, size_t len) {
    auto response = DNSClient::parse_message(data, len);
    if (response) {
        touch_response(*response);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    exercise(data, len);

    if (len >= sizeof(DNSHeader)) {
        std::vector<uint8_t> normalized(data, data + len);
        auto *header = reinterpret_cast<DNSHeader *>(normalized.data());
        header->flags = htons(static_cast<uint16_t>(ntohs(header->flags) | 0x8000));
        exercise(normalized.data(), normalized.size());
    }

    return 0;
}
