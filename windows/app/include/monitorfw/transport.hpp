#pragma once
#include "core.hpp"
#include <cstddef>
#include <memory>

namespace monitorfw {
struct ProviderInfo {
    std::string id, name;
    bool implemented = false;
    std::string reason;
};
struct TransportCapabilities {
    bool rawRead = false, separateWriteRead = false, combinedRead = false;
    std::uint32_t maxRead = 0, maxWrite = 0, maxPrefix = 0, maxCombinedRead = 0;
};
struct DisplayEndpoint {
    std::string providerId;
    MonitorIdentity identity;
    std::uintptr_t gpuHandle = 0;
    std::uint32_t outputMask = 0, displayId = 0;
    Bytes edid;
};
class II2cTransport {
public:
    virtual ~II2cTransport() {}
    // Metadata only; no library loading, probing, or device transactions.
    virtual TransportCapabilities capabilities() const = 0;
    // Inputs use seven-bit addresses. Implementations must reject requests above
    // their declared bounds, throw on transport errors, and return exact lengths.
    // No implicit retry of writes or hidden monitor-control side effects.
    virtual void write(std::uint8_t address7, const Bytes& data) = 0;
    virtual Bytes read(std::uint8_t address7, const Bytes& prefix, std::uint32_t length) = 0;
    virtual Bytes liveEdid() = 0;
};
class II2cProvider {
public:
    virtual ~II2cProvider() {}
    virtual ProviderInfo info() const = 0;
    // These are the only provider entry points that may initialize a driver or
    // access hardware. A provider must reject another provider's endpoint.
    virtual std::vector<DisplayEndpoint> enumerate() = 0;
    virtual std::unique_ptr<II2cTransport> open(const DisplayEndpoint& endpoint) = 0;
};
// Pure registry metadata and lazy factories, including unavailable providers.
std::vector<ProviderInfo> listProviders();
std::unique_ptr<II2cProvider> createProvider(const std::string& id);
void validateI2cRequest(const TransportCapabilities& capabilities, bool reading,
    std::uint8_t address7, const Bytes& prefix, std::size_t length);
MonitorIdentity decodeMonitorEdid(const Bytes& edid, const std::string& locator);
void markDuplicateMonitorSerials(std::vector<DisplayEndpoint>& endpoints);
// No library loading or hardware I/O occurs before these explicit calls.
std::vector<DisplayEndpoint> enumerateNvidiaDisplays();
std::unique_ptr<II2cTransport> openNvidiaTransport(const DisplayEndpoint& endpoint);
std::unique_ptr<IFlashDevice> makeMstarFlash(std::unique_ptr<II2cTransport> transport, const MonitorIdentity& selected);
} // namespace monitorfw
