#include "monitorfw/transport.hpp"

namespace monitorfw {
namespace {
ProviderInfo descriptor(const char* id, const char* name, bool implemented, const char* reason) {
    ProviderInfo info; info.id = id; info.name = name; info.implemented = implemented; info.reason = reason;
    return info;
}
class UnavailableProvider : public II2cProvider {
    ProviderInfo info_;
    void unavailable() const {
        throw Error(info_.name + " I2C provider unavailable: " + info_.reason);
    }
public:
    explicit UnavailableProvider(const ProviderInfo& info) : info_(info) { info_.implemented = false; }
    ProviderInfo info() const { return info_; }
    std::vector<DisplayEndpoint> enumerate() {
        unavailable(); return std::vector<DisplayEndpoint>();
    }
    std::unique_ptr<II2cTransport> open(const DisplayEndpoint&) {
        unavailable(); return std::unique_ptr<II2cTransport>();
    }
};
class NvidiaProvider : public II2cProvider {
    ProviderInfo info_;
public:
    explicit NvidiaProvider(const ProviderInfo& info) : info_(info) {}
    ProviderInfo info() const { return info_; }
    std::vector<DisplayEndpoint> enumerate() { return enumerateNvidiaDisplays(); }
    std::unique_ptr<II2cTransport> open(const DisplayEndpoint& endpoint) {
        if (endpoint.providerId != "nvidia") throw Error("Endpoint provider does not match the selected NVIDIA I2C provider.");
        return openNvidiaTransport(endpoint);
    }
};
} // namespace

std::vector<ProviderInfo> listProviders() {
    return {
        descriptor("nvidia", "NVIDIA NVAPI", true,
            "Experimental custom binding. Driver availability is checked only by explicit enumeration/open; current NVIDIA documentation specifies Windows 10 or later."),
        descriptor("intel", "Intel", false,
            "No reviewed Intel I2C implementation is included. Selection performs no driver or monitor probe."),
        descriptor("amd", "AMD", false,
            "No reviewed AMD I2C implementation is included. Selection performs no driver or monitor probe.")
    };
}

std::unique_ptr<II2cProvider> createProvider(const std::string& id) {
    for (const ProviderInfo& info : listProviders()) {
        if (info.id != id) continue;
        // An implementation requires its own explicit factory. Changing an
        // availability flag can never route another vendor through NVIDIA.
        if (id == "nvidia") return std::unique_ptr<II2cProvider>(new NvidiaProvider(info));
        return std::unique_ptr<II2cProvider>(new UnavailableProvider(info));
    }
    throw Error("Unknown I2C provider ID: " + id);
}

void validateI2cRequest(const TransportCapabilities& c, bool reading,
    std::uint8_t address7, const Bytes& prefix, std::size_t length) {
    if (address7 > 0x7f || length == 0 || prefix.size() > c.maxPrefix)
        throw Error("Invalid I2C address, empty transfer, or prefix above the provider limit.");
    if (reading) {
        if (length > c.maxRead) throw Error("I2C read exceeds the provider limit.");
        if (prefix.empty()) {
            if (!c.rawRead) throw Error("The provider does not support raw I2C reads.");
        } else if (!c.combinedRead || length > c.maxCombinedRead) {
            throw Error("Combined I2C read is unsupported or exceeds the provider limit.");
        }
    } else {
        // This interface carries write packets in data, never in a read prefix.
        if (!prefix.empty() || !c.separateWriteRead || length > c.maxWrite)
            throw Error("I2C write is unsupported or exceeds the provider limit.");
    }
}
} // namespace monitorfw
