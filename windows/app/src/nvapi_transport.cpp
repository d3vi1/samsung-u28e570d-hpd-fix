#include "monitorfw/transport.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace monitorfw {
namespace {
// Minimal public ABI facts, checked against NVIDIA/nvapi nvapi.h (pack 8).
// This custom QueryInterface binding is experimental. NVIDIA documents the SDK
// static stub as its supported integration; no legacy OS compatibility is implied.
// https://github.com/NVIDIA/nvapi/blob/main/nvapi.h
// https://github.com/NVIDIA/nvapi/blob/main/nvapi_interface.h
#pragma pack(push, 8)
struct I2cV3 {
    std::uint32_t version, displayMask;
    std::uint8_t ddc, deviceAddress;
    std::uint8_t* registerAddress;
    std::uint32_t registerSize;
    std::uint8_t* data;
    std::uint32_t dataSize, speedDeprecated, speedEnum;
    std::uint8_t portId;
    std::uint32_t portIdSet;
};
struct DisplayIds { std::uint32_t version, connectorType, displayId, flags; };
struct EdidV3 {
    std::uint32_t version;
    std::uint8_t data[256];
    std::uint32_t totalSize, edidId, offset;
};
#pragma pack(pop)
static_assert(sizeof(I2cV3) == (sizeof(void*) == 8 ? 64 : 44), "NVAPI I2C V3 size");
static_assert(offsetof(I2cV3, registerAddress) == (sizeof(void*) == 8 ? 16 : 12), "NVAPI register pointer");
static_assert(offsetof(I2cV3, registerSize) == (sizeof(void*) == 8 ? 24 : 16), "NVAPI register size");
static_assert(offsetof(I2cV3, data) == (sizeof(void*) == 8 ? 32 : 20), "NVAPI data pointer");
static_assert(offsetof(I2cV3, dataSize) == (sizeof(void*) == 8 ? 40 : 24), "NVAPI buffer size");
static_assert(offsetof(I2cV3, speedDeprecated) == (sizeof(void*) == 8 ? 44 : 28), "NVAPI deprecated speed");
static_assert(offsetof(I2cV3, speedEnum) == (sizeof(void*) == 8 ? 48 : 32), "NVAPI speed enum");
static_assert(offsetof(I2cV3, portId) == (sizeof(void*) == 8 ? 52 : 36), "NVAPI port ID");
static_assert(offsetof(I2cV3, portIdSet) == (sizeof(void*) == 8 ? 56 : 40), "NVAPI port flag");
static_assert(sizeof(DisplayIds) == 16 && sizeof(EdidV3) == 272, "NVAPI display/EDID ABI");

bool reviewedProduct(std::uint16_t product) {
    return product == 0x0d6f || product == 0x0d70 || product == 0x0d71;
}
std::string descriptorText(const Bytes& edid, std::size_t offset) {
    std::string text;
    bool ended = false;
    for (std::size_t i = offset + 5; i < offset + 18; ++i) {
        const unsigned char value = edid[i];
        if (value == 0 || value == 10 || value == 13) { ended = true; continue; }
        if (ended) {
            if (value != ' ') throw Error("EDID descriptor contains data after its terminator.");
        } else {
            if (value < 32 || value > 126) throw Error("EDID descriptor is not printable ASCII.");
            text.push_back(static_cast<char>(value));
        }
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    return text;
}
bool genericSerial(const std::string& serial) {
    if (serial.empty() || serial == "H1AK500000" || serial == "123456789" ||
        serial == "1234567890" || serial == "SERIAL NUMBER" || serial == "Serial Number") return true;
    return serial.find_first_not_of("0 ") == std::string::npos ||
        serial.find_first_not_of("Ff ") == std::string::npos;
}
void markAmbiguous(MonitorIdentity& identity, const std::string& reason) {
    identity.serialAmbiguous = true;
    if (!identity.ambiguityReason.empty()) identity.ambiguityReason += "; ";
    identity.ambiguityReason += reason;
}

#ifdef _WIN32
typedef int (__cdecl* InitFn)();
typedef void* (__cdecl* QueryFn)(std::uint32_t);
typedef int (__cdecl* GpusFn)(void**, std::uint32_t*);
typedef int (__cdecl* DisplaysFn)(void*, DisplayIds*, std::uint32_t*, std::uint32_t);
typedef int (__cdecl* OutputFn)(std::uint32_t, void**, std::uint32_t*);
typedef int (__cdecl* EdidFn)(void*, std::uint32_t, EdidV3*);
typedef int (__cdecl* I2cFn)(void*, I2cV3*);
typedef int (__cdecl* ErrorFn)(int, char*);

struct Nvapi {
    HMODULE module;
    QueryFn query;
    GpusFn gpus;
    DisplaysFn displays;
    OutputFn output;
    EdidFn edid;
    I2cFn read, write;
    ErrorFn error;
    template<class T> T require(std::uint32_t id) {
        void* function = query(id);
        if (!function) throw Error("Required NVAPI entry point is unavailable; no legacy fallback is attempted.");
        return reinterpret_cast<T>(function);
    }
    Nvapi() : module(0), query(0), gpus(0), displays(0), output(0), edid(0), read(0), write(0), error(0) {
        wchar_t directory[MAX_PATH];
        const UINT count = GetSystemDirectoryW(directory, MAX_PATH);
        if (!count || count >= MAX_PATH) throw Error("Cannot resolve the Windows system directory.");
        const std::wstring path = std::wstring(directory, count) +
            (sizeof(void*) == 8 ? L"\\nvapi64.dll" : L"\\nvapi.dll");
        module = LoadLibraryExW(path.c_str(), 0, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) throw Error("The matching NVIDIA driver DLL is unavailable.");
        try {
            const FARPROC entry = GetProcAddress(module, "nvapi_QueryInterface");
            static_assert(sizeof(entry) == sizeof(query), "Windows function-pointer representation");
            std::memcpy(&query, &entry, sizeof(query));
            if (!query) throw Error("The NVIDIA driver does not export nvapi_QueryInterface.");
            InitFn initialize = require<InitFn>(0x0150e828);
            gpus = require<GpusFn>(0xe5ac921f);
            displays = require<DisplaysFn>(0x0078dba2);
            output = require<OutputFn>(0x112ba1a5);
            edid = require<EdidFn>(0x37d32e69);
            read = require<I2cFn>(0x2fde12c5);
            write = require<I2cFn>(0xe812eb07);
            error = reinterpret_cast<ErrorFn>(query(0x6c2d048c));
            const int status = initialize();
            if (status != 0) throw Error("NvAPI_Initialize failed with status " + std::to_string(status));
        } catch (...) {
            FreeLibrary(module); module = 0;
            throw;
        }
    }
    void check(int status, const char* operation) const {
        if (!status) return;
        char message[64] = {};
        if (error) error(status, message);
        message[63] = 0;
        throw Error(std::string(operation) + " failed: " + std::to_string(status) + " " + message);
    }
};
Nvapi& nvapi() {
    // Explicit enumeration/open only. Retain the binding for process lifetime:
    // destruction of a transport never calls NVAPI, unloads its DLL, or resets ISP.
    static Nvapi* binding = new Nvapi();
    return *binding;
}
std::string locator(std::uint32_t display, std::uint32_t mask) {
    std::ostringstream out;
    out << "nvapi:display=" << std::hex << std::setw(8) << std::setfill('0') << display
        << ":output=" << std::setw(8) << mask;
    return out.str();
}
void validMask(std::uint32_t mask) {
    if (!mask || (mask & (mask - 1))) throw Error("A single NVIDIA output bit is required.");
}
class NvidiaTransport : public II2cTransport {
    DisplayEndpoint endpoint_;
    Nvapi& api_;
    Bytes transfer(bool reading, std::uint8_t address7, const Bytes& prefix, const Bytes& payload, std::uint32_t length) {
        validateI2cRequest(capabilities(), reading, address7, prefix, length);
        if (!reading && payload.size() != length) throw Error("I2C write length does not match its payload.");
        Bytes data = reading ? Bytes(length, 0xcc) : payload;
        Bytes registerBytes = prefix;
        I2cV3 info = {};
        info.version = static_cast<std::uint32_t>(sizeof(info)) | 0x30000;
        info.displayMask = endpoint_.outputMask;
        info.ddc = 1;
        // NVIDIA's sample distinguishes direct read (LSB 1) from combined read
        // (LSB 0). A nonzero prefix with LSB 1 can otherwise be ignored.
        info.deviceAddress = static_cast<std::uint8_t>((address7 << 1) | (reading && prefix.empty() ? 1 : 0));
        info.registerAddress = registerBytes.empty() ? 0 : &registerBytes[0];
        info.registerSize = static_cast<std::uint32_t>(registerBytes.size());
        info.data = &data[0]; info.dataSize = length;
        info.speedDeprecated = 0xffff; info.speedEnum = 0;
        api_.check((reading ? api_.read : api_.write)(reinterpret_cast<void*>(endpoint_.gpuHandle), &info),
            reading ? "NvAPI_I2CRead" : "NvAPI_I2CWrite");
        if (info.dataSize != length) throw Error("NVAPI changed the requested I2C buffer length.");
        return data;
    }
public:
    explicit NvidiaTransport(const DisplayEndpoint& endpoint) : endpoint_(endpoint), api_(nvapi()) { validMask(endpoint.outputMask); }
    TransportCapabilities capabilities() const {
        TransportCapabilities c;
        c.rawRead = true; c.separateWriteRead = true; c.combinedRead = true;
        c.maxRead = 4096; c.maxWrite = 4096; c.maxPrefix = 4; c.maxCombinedRead = 16;
        return c;
    }
    virtual void write(std::uint8_t address, const Bytes& data) {
        transfer(false, address, Bytes(), data, static_cast<std::uint32_t>(data.size()));
    }
    virtual Bytes read(std::uint8_t address, const Bytes& prefix, std::uint32_t length) {
        return transfer(true, address, prefix, Bytes(), length);
    }
    virtual Bytes liveEdid() {
        Bytes result;
        for (unsigned offset = 0; offset < 256; offset += 16) {
            write(0x50, Bytes(1, static_cast<std::uint8_t>(offset)));
            const Bytes part = read(0x50, Bytes(), 16);
            result.insert(result.end(), part.begin(), part.end());
        }
        // Positional and repeated reads guard against the observed stale-data
        // behavior where NVAPI reports success but the slave did not respond.
        for (unsigned offset : {0u, 8u, 0u}) {
            const Bytes part = read(0x50, Bytes(1, static_cast<std::uint8_t>(offset)), 16);
            if (!std::equal(part.begin(), part.end(), result.begin() + offset))
                throw Error("Combined/live EDID transport checks disagree.");
        }
        decodeMonitorEdid(result, endpoint_.identity.transportLocator);
        return result;
    }
};
#endif
} // namespace

MonitorIdentity decodeMonitorEdid(const Bytes& edid, const std::string& location) {
    const std::uint8_t header[] = {0,255,255,255,255,255,255,0};
    if (edid.size() != 256 || !std::equal(header, header + 8, edid.begin()) || edid[126] != 1)
        throw Error("Expected the reviewed two-block EDID structure.");
    for (std::size_t start = 0; start < edid.size(); start += 128) {
        unsigned sum = 0;
        for (std::size_t i = start; i < start + 128; ++i) sum += edid[i];
        if (sum & 255) throw Error("EDID checksum mismatch.");
    }
    const std::uint16_t product = static_cast<std::uint16_t>(edid[10] | (edid[11] << 8));
    if (edid[8] != 0x4c || edid[9] != 0x2d || !reviewedProduct(product))
        throw Error("Monitor manufacturer/product is outside the reviewed U28E570 profile.");
    unsigned serialCount = 0, nameCount = 0;
    std::string serial, name;
    for (std::size_t offset = 54; offset <= 108; offset += 18) {
        if (edid[offset] || edid[offset + 1] || edid[offset + 2] || edid[offset + 4]) continue;
        if (edid[offset + 3] == 0xff) { ++serialCount; serial = descriptorText(edid, offset); }
        if (edid[offset + 3] == 0xfc) { ++nameCount; name = descriptorText(edid, offset); }
    }
    if (nameCount != 1 || name != "U28E570") throw Error("The EDID model descriptor is not uniquely U28E570.");
    if (location.empty()) throw Error("An exact transport locator is required.");
    MonitorIdentity result;
    // This is the application's reviewed firmware compatibility profile. EDID
    // establishes SAM/U28E570 and the product code, not a regional SKU suffix.
    result.model = "LU28E570DS/EN"; result.manufacturer = "SAM";
    result.serial = serial; result.productCode = product;
    result.edidSha256 = sha256(edid); result.transportLocator = location;
    if (serialCount != 1) markAmbiguous(result, "EDID does not contain exactly one serial descriptor");
    if (genericSerial(serial)) markAmbiguous(result, "EDID serial is absent or a generic template value");
    return result;
}

void markDuplicateMonitorSerials(std::vector<DisplayEndpoint>& endpoints) {
    const std::string reason = "Serial is shared by multiple enumerated endpoints";
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        bool duplicated = false;
        for (std::size_t j = 0; j < endpoints.size(); ++j)
            if (i != j && !endpoints[i].identity.serial.empty() &&
                endpoints[i].identity.serial == endpoints[j].identity.serial) duplicated = true;
        if (duplicated && endpoints[i].identity.ambiguityReason.find(reason) == std::string::npos)
            markAmbiguous(endpoints[i].identity, reason);
    }
}

std::vector<DisplayEndpoint> enumerateNvidiaDisplays() {
#ifdef _WIN32
    Nvapi& api = nvapi();
    void* handles[64] = {};
    std::uint32_t count = 0;
    api.check(api.gpus(handles, &count), "NvAPI_EnumPhysicalGPUs");
    if (count > 64) throw Error("Invalid NVAPI GPU count.");
    std::vector<DisplayEndpoint> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t displayCount = 0;
        api.check(api.displays(handles[i], 0, &displayCount, 0), "NvAPI connected display count");
        if (!displayCount) continue;
        if (displayCount > 256) throw Error("NVAPI display count exceeds its bounded allocation.");
        std::vector<DisplayIds> displays(displayCount);
        for (DisplayIds& item : displays) item.version = 0x30010;
        const std::uint32_t capacity = displayCount;
        api.check(api.displays(handles[i], &displays[0], &displayCount, 0), "NvAPI connected displays");
        if (displayCount > capacity) throw Error("Display topology changed during enumeration.");
        for (std::uint32_t n = 0; n < displayCount; ++n) {
            void* gpu = 0; std::uint32_t mask = 0;
            api.check(api.output(displays[n].displayId, &gpu, &mask), "NvAPI display output mapping");
            validMask(mask);
            EdidV3 data = {}; data.version = 0x30110;
            api.check(api.edid(gpu, mask, &data), "NvAPI cached EDID");
            Bytes edid(data.data, data.data + 256);
            const std::uint16_t product = static_cast<std::uint16_t>(edid[10] | (edid[11] << 8));
            if (edid[8] != 0x4c || edid[9] != 0x2d || !reviewedProduct(product)) continue;
            if (data.totalSize != 256 || data.offset != 0)
                throw Error("The reviewed monitor must return exactly one 256-byte EDID page.");
            DisplayEndpoint endpoint;
            endpoint.providerId = "nvidia";
            endpoint.gpuHandle = reinterpret_cast<std::uintptr_t>(gpu);
            endpoint.outputMask = mask; endpoint.displayId = displays[n].displayId; endpoint.edid = edid;
            endpoint.identity = decodeMonitorEdid(edid, locator(endpoint.displayId, mask));
            result.push_back(endpoint);
        }
    }
    markDuplicateMonitorSerials(result);
    return result;
#else
    throw Error("NVAPI transport is unavailable outside Windows; use an offline mock.");
#endif
}

std::unique_ptr<II2cTransport> openNvidiaTransport(const DisplayEndpoint& endpoint) {
#ifdef _WIN32
    if (endpoint.providerId != "nvidia") throw Error("An NVIDIA endpoint is required by this provider.");
    // Revalidate cached enumeration before accepting caller-owned endpoint fields.
    const std::vector<DisplayEndpoint> current = enumerateNvidiaDisplays();
    const DisplayEndpoint* found = 0;
    for (const DisplayEndpoint& item : current) {
        if (item.providerId == endpoint.providerId && item.gpuHandle == endpoint.gpuHandle && item.outputMask == endpoint.outputMask &&
            item.displayId == endpoint.displayId && item.edid == endpoint.edid && item.identity == endpoint.identity) {
            if (found) throw Error("The selected endpoint is not unique.");
            found = &item;
        }
    }
    if (!found) throw Error("The selected NVIDIA endpoint changed; select it again.");
    return std::unique_ptr<II2cTransport>(new NvidiaTransport(*found));
#else
    (void)endpoint;
    throw Error("NVAPI transport is unavailable outside Windows; use an offline mock.");
#endif
}
} // namespace monitorfw
