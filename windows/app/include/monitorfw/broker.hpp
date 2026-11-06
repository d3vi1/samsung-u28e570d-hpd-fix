#pragma once
#include "core.hpp"
namespace monitorfw {
struct BrokerRequest {
    std::string id, action, mode, providerId, endpointLocator, edidSha256, firmwareSha256;
    ImageKind imageKind=ImageKind::SamsungPackage;
    std::uint64_t createdUnix=0;
};
bool validRequestId(const std::string& value);
std::string requestIdFromUri(const std::string& uri);
BrokerRequest parseBrokerRequest(const std::string& json,const std::string& uriId,std::uint64_t nowUnix);
}
