#include "monitorfw/transport.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

// Pure tests only: no call to enumerateNvidiaDisplays/openNvidiaTransport,
// LoadLibrary, NVIDIA, DDC, or a physical monitor. All bytes are synthetic.
namespace {
using namespace monitorfw;
int passed = 0;
void require(bool condition, const std::string& message) {
    if (!condition) throw Error("Assertion failed: " + message);
}
void rejects(const std::function<void()>& action, const std::string& fragment = "") {
    try { action(); }
    catch (const Error& error) {
        require(fragment.empty() || std::string(error.what()).find(fragment) != std::string::npos,
            "unexpected error: " + std::string(error.what()));
        return;
    }
    throw Error("Expected rejection.");
}
void test(const char* name, const std::function<void()>& action) {
    action(); ++passed; std::cout << "PASS " << name << '\n';
}
void checksums(Bytes& edid) {
    for (std::size_t start = 0; start < edid.size(); start += 128) {
        unsigned sum = 0;
        for (std::size_t i = start; i < start + 127; ++i) sum += edid[i];
        edid[start + 127] = static_cast<std::uint8_t>(0u - sum);
    }
}
void descriptor(Bytes& edid, std::size_t offset, std::uint8_t type, const std::string& value) {
    require(value.size() <= 13, "synthetic descriptor fits");
    std::fill(edid.begin() + offset, edid.begin() + offset + 18, 0);
    edid[offset + 3] = type;
    std::fill(edid.begin() + offset + 5, edid.begin() + offset + 18, ' ');
    std::copy(value.begin(), value.end(), edid.begin() + offset + 5);
    if (value.size() < 13) edid[offset + 5 + value.size()] = 10;
}
Bytes syntheticEdid(const std::string& serial = "TEST000001", std::uint16_t product = 0x0d71) {
    Bytes edid(256, 0);
    std::fill(edid.begin() + 1, edid.begin() + 7, 0xff);
    edid[8] = 0x4c; edid[9] = 0x2d;
    edid[10] = static_cast<std::uint8_t>(product); edid[11] = static_cast<std::uint8_t>(product >> 8);
    edid[18] = 1; edid[19] = 4; edid[126] = 1;
    descriptor(edid, 54, 0xfc, "U28E570");
    descriptor(edid, 72, 0xff, serial);
    edid[128] = 2; edid[129] = 3;
    checksums(edid); return edid;
}
const Bytes LegacySfdp = {0x53,0x46,0x44,0x50,0,1,0,0xff,0,0,1,9,0x80,0,0,0xff};
const Bytes JvSfdp = {0x53,0x46,0x44,0x50,5,1,0,0xff,0,5,1,16,0x80,0,0,0xff};
Bytes syntheticSfdp(const Bytes& prefix) {
    Bytes data = prefix; data.resize(64);
    for (std::size_t i=16; i<data.size(); ++i) data[i] = static_cast<std::uint8_t>(0xa0 ^ i);
    return data; // Synthetic evidence tail, not a flash-part specification.
}
struct Traffic {
    char kind; Bytes data; std::uint32_t length;
    Traffic(char k, const Bytes& d, std::uint32_t n = 0) : kind(k), data(d), length(n) {}
};
struct BridgeState {
    TransportCapabilities capabilities;
    Bytes edid = syntheticEdid();
    Bytes image = Bytes(SpiCapacity, 0xff);
    Bytes jedec = Bytes{0xef,0x40,0x15};
    Bytes sfdp = syntheticSfdp(LegacySfdp), status = Bytes{0,0};
    Bytes command;
    std::vector<Traffic> traffic;
    bool entered = false, readArmed = false, wel = false;
    bool throwEntry = false, throwCleanup = false, throwRead = false, shortRead = false;
    bool ignoreWren = false, changeSecondJedec = false;
    int failCommand = -1, failAfterSideEffect = -1, jedecReads = 0, busyReads = 0;
    unsigned destructors = 0;
    BridgeState() {
        capabilities.rawRead = true; capabilities.separateWriteRead = true;
        capabilities.maxRead = 256; capabilities.maxWrite = 37;
    }
    unsigned opcodes(std::uint8_t opcode) const {
        unsigned count = 0;
        for (const Traffic& t : traffic)
            if (t.kind == 'W' && t.data.size() >= 2 && t.data[0] == 0x10 && t.data[1] == opcode) ++count;
        return count;
    }
    unsigned packets(const Bytes& packet) const {
        unsigned count = 0;
        for (const Traffic& t : traffic) if (t.kind == 'W' && t.data == packet) ++count;
        return count;
    }
    std::uint32_t address() const {
        require(command.size() >= 4, "addressed command has three address bytes");
        return (std::uint32_t(command[1]) << 16) | (std::uint32_t(command[2]) << 8) | command[3];
    }
};
class ScriptedBridge : public II2cTransport {
    std::shared_ptr<BridgeState> s_;
public:
    explicit ScriptedBridge(const std::shared_ptr<BridgeState>& state) : s_(state) {}
    ~ScriptedBridge() { ++s_->destructors; }
    TransportCapabilities capabilities() const { return s_->capabilities; }
    Bytes liveEdid() { s_->traffic.push_back(Traffic('E', Bytes())); return s_->edid; }
    void write(std::uint8_t address7, const Bytes& packet) {
        require(address7 == 0x49, "only the reviewed ISP address reaches bridge");
        require(!packet.empty(), "nonempty bridge packet");
        s_->traffic.push_back(Traffic('W', packet));
        if (packet == Bytes{'M','S','T','A','R'}) {
            s_->entered = true;
            if (s_->throwEntry) throw Error("injected entry failure after possible effect");
            return;
        }
        require(s_->entered, "bridge operation follows entry");
        if (packet == Bytes{0x24}) { s_->entered = false; return; }
        if (packet == Bytes{0x12}) {
            s_->command.clear(); s_->readArmed = false;
            if (s_->throwCleanup) throw Error("injected cleanup failure");
            return;
        }
        if (packet == Bytes{0x11}) { require(!s_->command.empty(), "read arm follows command"); s_->readArmed = true; return; }
        require(packet[0] == 0x10 && packet.size() >= 2, "only reviewed bridge command framing");
        require(s_->command.empty(), "previous transaction was cleaned up");
        s_->command.assign(packet.begin() + 1, packet.end());
        const std::uint8_t opcode = s_->command[0];
        if (s_->failCommand == opcode) { s_->failCommand = -1; throw Error("injected command failure"); }
        if (opcode == 0x06) { if (!s_->ignoreWren) s_->wel = true; }
        else if (opcode == 0x04) s_->wel = false;
        else if (opcode == 0x20) {
            require(s_->wel, "erase requires WREN");
            const std::uint32_t a = s_->address();
            require(a % SectorSize == 0 && a <= SpiCapacity - SectorSize, "bounded aligned erase");
            std::fill(s_->image.begin() + a, s_->image.begin() + a + SectorSize, 0xff);
            s_->wel = false;
        } else if (opcode == 0x02) {
            require(s_->wel, "program requires WREN");
            const std::uint32_t a = s_->address();
            const std::size_t n = s_->command.size() - 4;
            require(n > 0 && n <= 32 && (a & 255) + n <= 256 && a + n <= SpiCapacity, "bounded 32-byte subpage");
            for (std::size_t i = 0; i < n; ++i) s_->image[a + i] &= s_->command[4 + i];
            s_->wel = false;
        } else require(opcode == 0x9f || opcode == 0x5a || opcode == 0x05 || opcode == 0x35 ||
            opcode == 0x15 || opcode == 0x03, "no status-register/config/chip-erase opcode");
        if (s_->failAfterSideEffect == opcode) {
            s_->failAfterSideEffect = -1; throw Error("injected failure after mutation");
        }
    }
    Bytes read(std::uint8_t address7, const Bytes& prefix, std::uint32_t length) {
        require(address7 == 0x49 && prefix.empty(), "SPI bridge uses raw read at address 49");
        require(s_->entered && s_->readArmed && !s_->command.empty(), "raw read is armed");
        s_->traffic.push_back(Traffic('R', prefix, length));
        if (s_->throwRead) throw Error("injected raw read failure");
        Bytes data;
        const std::uint8_t opcode = s_->command[0];
        if (opcode == 0x9f) {
            data = s_->jedec; ++s_->jedecReads;
            if (s_->changeSecondJedec && s_->jedecReads == 2) data[2] ^= 1;
        } else if (opcode == 0x5a) {
            require(s_->command == Bytes{0x5a,0,0,0,0}, "SFDP has exact address and dummy byte"); data = s_->sfdp;
        } else if (opcode == 0x05) {
            std::uint8_t status = s_->status[0] | (s_->wel ? 2 : 0);
            if (s_->busyReads > 0) { --s_->busyReads; status |= 1; }
            data = Bytes(1, status);
        } else if (opcode == 0x35) data = Bytes(1, s_->status.at(1));
        else if (opcode == 0x15) { require(s_->status.size() == 3, "legacy profile never reads SR3"); data = Bytes(1, s_->status[2]); }
        else if (opcode == 0x03) {
            const std::uint32_t a = s_->address();
            require(length <= 256 && a <= SpiCapacity && length <= SpiCapacity - a, "bounded flash read");
            data.assign(s_->image.begin() + a, s_->image.begin() + a + length);
        } else throw Error("unexpected simulated read opcode");
        require(data.size() == length, "expected read length");
        if (s_->shortRead) data.pop_back();
        return data;
    }
};
struct Fixture {
    std::shared_ptr<BridgeState> state = std::make_shared<BridgeState>();
    MonitorIdentity selected;
    std::unique_ptr<IFlashDevice> device;
    explicit Fixture(bool jv = false, const std::string& serial = "TEST000001") {
        state->edid = syntheticEdid(serial);
        if (jv) { state->sfdp = syntheticSfdp(JvSfdp); state->status = Bytes{0,2,0x60}; }
        selected = decodeMonitorEdid(state->edid, "mock:endpoint-1");
        device = makeMstarFlash(std::unique_ptr<II2cTransport>(new ScriptedBridge(state)), selected);
    }
    void identify() { device->enterIsp(); device->flashIdentity(); state->traffic.clear(); }
};
void noTrafficAfter(const std::shared_ptr<BridgeState>& state, const std::function<void()>& action) {
    const std::size_t count = state->traffic.size(); action();
    require(state->traffic.size() == count, "rejection or destruction has no transport traffic");
}
} // namespace

int main() {
    try {
        test("Provider registry/factories expose metadata without driver initialization", [] {
            const std::vector<ProviderInfo> registry = listProviders();
            require(registry.size() == 3, "three explicit providers");
            for (const ProviderInfo& info : registry) {
                const std::unique_ptr<II2cProvider> provider = createProvider(info.id);
                require(provider->info().id == info.id && !info.name.empty() && !info.reason.empty(), "metadata survives lazy factory");
                require(info.implemented == (info.id == "nvidia"), "only NVIDIA is implemented");
                // Never enumerate/open the NVIDIA provider in an offline test.
                if (!info.implemented) {
                    rejects([&] { provider->enumerate(); }, "unavailable");
                    rejects([&] { provider->open(DisplayEndpoint()); }, "unavailable");
                }
            }
            rejects([] { createProvider("unknown"); }, "Unknown I2C provider");
        });
        test("I2C capability contract rejects unsupported modes and lengths", [] {
            TransportCapabilities c;
            c.rawRead = true; c.separateWriteRead = true; c.combinedRead = true;
            c.maxRead = 256; c.maxWrite = 37; c.maxPrefix = 4; c.maxCombinedRead = 16;
            validateI2cRequest(c,true,0x49,Bytes(),256);
            validateI2cRequest(c,true,0x50,Bytes{0},16);
            validateI2cRequest(c,false,0x49,Bytes(),37);
            rejects([&] { validateI2cRequest(c,true,0x80,Bytes(),1); });
            rejects([&] { validateI2cRequest(c,true,0x49,Bytes(),0); });
            rejects([&] { validateI2cRequest(c,true,0x49,Bytes(),257); });
            rejects([&] { validateI2cRequest(c,false,0x49,Bytes(),38); });
            rejects([&] { validateI2cRequest(c,true,0x50,Bytes(5),1); });
            rejects([&] { validateI2cRequest(c,true,0x50,Bytes{0},17); });
            rejects([&] { validateI2cRequest(c,false,0x49,Bytes{0},1); });
            c.rawRead = false; rejects([&] { validateI2cRequest(c,true,0x49,Bytes(),1); });
            c.combinedRead = false; rejects([&] { validateI2cRequest(c,true,0x50,Bytes{0},1); });
            c.separateWriteRead = false; rejects([&] { validateI2cRequest(c,false,0x49,Bytes(),1); });
        });
        test("MStar rejects insufficient generic transport before any I/O", [] {
            for (unsigned field : {0u,1u,2u,3u}) {
                const std::shared_ptr<BridgeState> state = std::make_shared<BridgeState>();
                if (field == 0) state->capabilities.rawRead = false;
                if (field == 1) state->capabilities.separateWriteRead = false;
                if (field == 2) state->capabilities.maxRead = 255;
                if (field == 3) state->capabilities.maxWrite = 36;
                const MonitorIdentity selected = decodeMonitorEdid(state->edid,"mock:capabilities");
                rejects([&] { makeMstarFlash(std::unique_ptr<II2cTransport>(new ScriptedBridge(state)), selected); }, "MStar requires");
                require(state->traffic.empty() && state->destructors == 1, "unavailable contract rejected without transactions");
            }
        });
        test("EDID strict model/product/hash and exact endpoint", [] {
            for (std::uint16_t product : {0x0d6f,0x0d70,0x0d71}) {
                const Bytes edid = syntheticEdid("TEST000001", product);
                const MonitorIdentity id = decodeMonitorEdid(edid, "mock:endpoint-1");
                require(id.model == "LU28E570DS/EN" && id.manufacturer == "SAM" && id.serial == "TEST000001" &&
                    id.productCode == product && id.edidSha256 == sha256(edid) && id.transportLocator == "mock:endpoint-1" &&
                    !id.serialAmbiguous, "exact decoded identity");
            }
        });
        test("EDID corrupt lengths, header, both checksums, extension count and locator reject", [] {
            Bytes edid = syntheticEdid(); edid.pop_back(); rejects([&] { decodeMonitorEdid(edid, "mock:1"); });
            edid = syntheticEdid(); edid[0] = 1; checksums(edid); rejects([&] { decodeMonitorEdid(edid, "mock:1"); });
            for (unsigned offset : {20u,150u}) {
                edid = syntheticEdid(); edid[offset] ^= 1; rejects([&] { decodeMonitorEdid(edid, "mock:1"); }, "checksum");
            }
            edid = syntheticEdid(); edid[126] = 0; checksums(edid); rejects([&] { decodeMonitorEdid(edid, "mock:1"); });
            rejects([] { decodeMonitorEdid(syntheticEdid(), ""); }, "locator");
        });
        test("EDID manufacturer/product/model mismatch rejects", [] {
            Bytes edid = syntheticEdid(); edid[8] = 0; checksums(edid); rejects([&] { decodeMonitorEdid(edid, "mock:1"); });
            rejects([] { decodeMonitorEdid(syntheticEdid("TEST000001", 0x1234), "mock:1"); });
            edid = syntheticEdid(); descriptor(edid,54,0xfc,"OTHER"); checksums(edid);
            rejects([&] { decodeMonitorEdid(edid, "mock:1"); }, "model descriptor");
            edid = syntheticEdid(); descriptor(edid,90,0xfc,"U28E570"); checksums(edid);
            rejects([&] { decodeMonitorEdid(edid, "mock:1"); }, "uniquely");
        });
        test("EDID generic and missing serials remain explicitly ambiguous", [] {
            for (const char* serial : {"", "H1AK500000", "00000000", "FFFFFFFF", "123456789", "SERIAL NUMBER"}) {
                const MonitorIdentity id = decodeMonitorEdid(syntheticEdid(serial), "mock:1");
                require(id.serialAmbiguous && !id.ambiguityReason.empty(), "generic serial is ambiguous");
            }
            Bytes edid = syntheticEdid(); std::fill(edid.begin()+72,edid.begin()+90,0); checksums(edid);
            require(decodeMonitorEdid(edid,"mock:1").serialAmbiguous, "absent serial is ambiguous");
        });
        test("EDID duplicate descriptors are ambiguous, malformed text rejects", [] {
            Bytes edid = syntheticEdid(); descriptor(edid,90,0xff,"TEST000001"); checksums(edid);
            require(decodeMonitorEdid(edid,"mock:1").serialAmbiguous, "duplicate descriptor is ambiguous");
            edid = syntheticEdid(); edid[72+5] = 1; checksums(edid);
            rejects([&] { decodeMonitorEdid(edid,"mock:1"); }, "printable");
            edid = syntheticEdid(); edid[72+17] = 'X'; checksums(edid);
            rejects([&] { decodeMonitorEdid(edid,"mock:1"); }, "terminator");
        });
        test("Duplicate endpoint serials mark both without losing exact locators", [] {
            std::vector<DisplayEndpoint> endpoints(3);
            endpoints[0].identity = decodeMonitorEdid(syntheticEdid(), "mock:first");
            endpoints[1].identity = decodeMonitorEdid(syntheticEdid(), "mock:second");
            endpoints[2].identity = decodeMonitorEdid(syntheticEdid("TEST000002"), "mock:third");
            markDuplicateMonitorSerials(endpoints);
            require(endpoints[0].identity.serialAmbiguous && endpoints[1].identity.serialAmbiguous &&
                !endpoints[2].identity.serialAmbiguous, "only duplicate endpoints marked");
            require(endpoints[0].identity.transportLocator == "mock:first" && endpoints[1].identity.transportLocator == "mock:second", "locators retained");
            const std::string reason = endpoints[0].identity.ambiguityReason;
            markDuplicateMonitorSerials(endpoints);
            require(endpoints[0].identity.ambiguityReason == reason, "idempotent duplicate marking");
        });
        test("Construction/destruction never talks to a monitor", [] {
            Fixture f; require(f.state->traffic.empty(), "constructor is pure");
            noTrafficAfter(f.state,[&] { f.device.reset(); });
            require(f.state->destructors == 1, "transport destroyed without I/O");
            rejects([] { makeMstarFlash(std::unique_ptr<II2cTransport>(), MonitorIdentity()); });
        });
        test("MSTAR has exact live identity immediately before entry", [] {
            Fixture f; f.device->enterIsp();
            require(f.state->traffic.size() == 2 && f.state->traffic[0].kind == 'E' &&
                f.state->traffic[1].data == Bytes{'M','S','T','A','R'}, "EDID then one MSTAR");
            noTrafficAfter(f.state,[&] { rejects([&] { f.device->enterIsp(); }); });
            noTrafficAfter(f.state,[&] { rejects([&] { f.device->monitorIdentity(); }); });
            noTrafficAfter(f.state,[&] { f.device.reset(); });
            require(f.state->packets(Bytes{0x24}) == 0, "no destructor reset in ISP");
        });
        test("Live identity drift refuses ISP before MSTAR", [] {
            Fixture f; f.state->edid = syntheticEdid("TEST000002");
            rejects([&] { f.device->enterIsp(); }, "differs");
            require(f.state->traffic.size() == 1 && f.state->traffic[0].kind == 'E', "only live EDID read");
        });
        test("Ambiguous identity can be matched by exact live endpoint", [] {
            Fixture f(false,"H1AK500000");
            require(f.device->monitorIdentity().serialAmbiguous, "generic ambiguity persists");
            std::vector<DisplayEndpoint> endpoints(2);
            endpoints[0].identity = decodeMonitorEdid(syntheticEdid(),"mock:1");
            endpoints[1].identity = decodeMonitorEdid(syntheticEdid(),"mock:2");
            markDuplicateMonitorSerials(endpoints);
            std::unique_ptr<IFlashDevice> device = makeMstarFlash(
                std::unique_ptr<II2cTransport>(new ScriptedBridge(std::make_shared<BridgeState>())), endpoints[0].identity);
            require(device->monitorIdentity() == endpoints[0].identity, "enumeration duplicate evidence retained");
        });
        test("Ambiguous entry exception cannot retry or reset", [] {
            Fixture f; f.state->throwEntry = true;
            rejects([&] { f.device->enterIsp(); }, "entry failure");
            noTrafficAfter(f.state,[&] { rejects([&] { f.device->enterIsp(); }); rejects([&] { f.device->flashIdentity(); });
                rejects([&] { f.device->resetVerified(); }); f.device.reset(); });
            require(f.state->packets(Bytes{'M','S','T','A','R'}) == 1 && f.state->packets(Bytes{0x24}) == 0, "no entry retry/reset");
        });
        test("Legacy flash identifies twice and never reads unsupported SR3", [] {
            Fixture f; f.device->enterIsp(); const FlashIdentity id = f.device->flashIdentity();
            require(id.profile == "winbond-ef4015-legacy" && id.jedec == Bytes{0xef,0x40,0x15} &&
                id.sfdp == syntheticSfdp(LegacySfdp) && id.status == Bytes{0,0} && id.capacity == SpiCapacity &&
                id.sectorSize == 4096 && id.pageSize == 256, "legacy identity");
            require(f.state->opcodes(0x9f) == 2 && f.state->opcodes(0x15) == 0 && f.state->opcodes(0x06) == 0, "only reviewed reads");
        });
        test("JV flash identifies all three exact status bytes", [] {
            Fixture f(true); f.device->enterIsp(); const FlashIdentity id = f.device->flashIdentity();
            require(id.profile == "winbond-ef4015-jv" && id.sfdp == syntheticSfdp(JvSfdp) && id.status == Bytes{0,2,0x60}, "JV profile");
            require(f.state->opcodes(0x15) == 1 && f.state->opcodes(0x9f) == 2, "SR3 only after known JV SFDP");
        });
        test("Wrong or unstable JEDEC refuses profile and subsequent mutation", [] {
            for (bool unstable : {false,true}) {
                Fixture f; f.state->changeSecondJedec = unstable;
                if (!unstable) f.state->jedec[0] = 0;
                f.device->enterIsp(); rejects([&] { f.device->flashIdentity(); }, "JEDEC");
                noTrafficAfter(f.state,[&] { rejects([&] { f.device->eraseSector(0); }); rejects([&] { f.device->program(0,Bytes{1}); }); });
                require(f.state->opcodes(0x06) == 0 && f.state->opcodes(0x15) == 0, "no writes/unsupported reads");
            }
        });
        test("Unknown SFDP fails before any SR3 or mutation", [] {
            Fixture f; f.state->sfdp[4] = 0xfe; f.device->enterIsp();
            rejects([&] { f.device->flashIdentity(); }, "SFDP");
            require(f.state->opcodes(0x15) == 0 && f.state->opcodes(0x06) == 0, "no speculative SR3 or write");
        });
        test("Protected or mismatched status cannot establish flash profile", [] {
            for (unsigned byte : {0u,1u,2u}) {
                Fixture f(true); f.state->status[byte] ^= 4; f.device->enterIsp();
                rejects([&] { f.device->flashIdentity(); }, "status/protection");
                require(f.state->opcodes(0x06) == 0, "no WREN on unreviewed status");
            }
        });
        test("Read commands use 10, 11, raw read, 12 framing", [] {
            Fixture f; f.identify();
            f.state->image[0x12345] = 0x87;
            require(f.device->read(0x12345,1) == Bytes{0x87}, "read data correct");
            require(f.state->traffic.size() == 4 && f.state->traffic[0].data == Bytes{0x10,0x03,1,0x23,0x45} &&
                f.state->traffic[1].data == Bytes{0x11} && f.state->traffic[2].kind == 'R' &&
                f.state->traffic[2].length == 1 && f.state->traffic[3].data == Bytes{0x12}, "exact read framing");
        });
        test("Cleanup 12 is attempted even when initial bridge command throws", [] {
            Fixture f; f.identify(); f.state->failCommand = 0x03;
            rejects([&] { f.device->read(0,1); }, "command failure");
            require(f.state->traffic.size() == 2 && f.state->traffic.back().data == Bytes{0x12}, "cleanup after first write failure");
            require(f.state->packets(Bytes{0x24}) == 0, "no reset");
        });
        test("Raw read errors and short data still release bridge transaction", [] {
            for (bool shortData : {false,true}) {
                Fixture f; f.identify(); f.state->throwRead = !shortData; f.state->shortRead = shortData;
                rejects([&] { f.device->read(0,1); }, shortData ? "Short SPI" : "raw read failure");
                require(f.state->traffic.back().data == Bytes{0x12}, "read cleanup attempted");
            }
        });
        test("Cleanup failure preserves primary failure and never resets", [] {
            Fixture f; f.identify(); f.state->throwRead = true; f.state->throwCleanup = true;
            rejects([&] { f.device->read(0,1); }, "cleanup failed after injected raw read failure");
            require(f.state->packets(Bytes{0x12}) == 1 && f.state->packets(Bytes{0x24}) == 0, "one cleanup, no reset");
        });
        test("Read chunking, last byte, zero range and overflow bounds", [] {
            Fixture f; f.identify();
            for (std::size_t i=0; i<f.state->image.size(); ++i) f.state->image[i] = static_cast<std::uint8_t>(i*13+(i>>8));
            const Bytes expected(f.state->image.begin()+255,f.state->image.begin()+768);
            require(f.device->read(255,513) == expected && f.state->opcodes(0x03) == 3, "chunking preserves exact bytes");
            require(f.device->read(SpiCapacity-1,1) == Bytes(1,f.state->image.back()), "last byte read");
            noTrafficAfter(f.state,[&] {
                require(f.device->read(SpiCapacity,0).empty(), "zero length at end");
                rejects([&] { f.device->read(SpiCapacity,1); });
                rejects([&] { f.device->read(0xffffffff,2); });
                rejects([&] { f.device->read(1,0xffffffff); });
            });
        });
        test("Operations before profile and invalid erase/program bounds have zero traffic", [] {
            Fixture unopened;
            noTrafficAfter(unopened.state,[&] { rejects([&] { unopened.device->read(0,1); });
                rejects([&] { unopened.device->eraseSector(0); }); rejects([&] { unopened.device->program(0,Bytes{1}); }); });
            Fixture f; f.identify();
            noTrafficAfter(f.state,[&] {
                rejects([&] { f.device->eraseSector(1); }); rejects([&] { f.device->eraseSector(SpiCapacity); });
                rejects([&] { f.device->program(0,Bytes()); }); rejects([&] { f.device->program(0,Bytes(257)); });
                rejects([&] { f.device->program(255,Bytes(2)); }); rejects([&] { f.device->program(SpiCapacity,Bytes{1}); });
                rejects([&] { f.device->program(0xffffffff,Bytes{1}); });
            });
        });
        test("Erase 20 and 32-byte program subpages each receive WREN and verify WEL cleared", [] {
            Fixture f; f.identify();
            std::fill(f.state->image.begin()+SectorSize,f.state->image.begin()+2*SectorSize,0);
            f.device->eraseSector(SectorSize);
            require(f.state->opcodes(0x20) == 1 && f.state->opcodes(0x06) == 1 && !f.state->wel, "single sector erase accepted");
            Bytes data(256); for (unsigned i=0; i<256; ++i) data[i] = static_cast<std::uint8_t>(i);
            f.device->program(SectorSize,data);
            require(f.state->opcodes(0x02) == 8 && f.state->opcodes(0x06) == 9 && !f.state->wel, "eight separately enabled subpages");
            require(f.device->read(SectorSize,256) == data, "programmed bytes exact");
            for (const Traffic& t : f.state->traffic) if (t.kind == 'W') require(t.data.size() <= 37, "bounded bridge packet");
            require(f.state->opcodes(0x01) == 0 && f.state->opcodes(0x31) == 0 && f.state->opcodes(0x11) == 0 &&
                f.state->opcodes(0xc7) == 0 && f.state->packets(Bytes{0x24}) == 0, "no config/status/chip erase/reset");
        });
        test("Unaligned subpage ends at hardware page boundary", [] {
            Fixture f; f.identify(); const Bytes data(55,0x12);
            f.device->program(201,data);
            require(f.state->opcodes(0x02) == 2 && f.device->read(201,55) == data, "32+23 bounded subpages");
        });
        test("WREN failure prevents erase/program; unexpected SR1 blocks WREN", [] {
            Fixture f; f.identify(); f.state->ignoreWren = true;
            rejects([&] { f.device->eraseSector(0); }, "write-enable latch");
            require(f.state->opcodes(0x20) == 0 && f.state->opcodes(0x02) == 0, "mutation never submitted");
            f.state->traffic.clear(); f.state->ignoreWren = false; f.state->status[0] = 4;
            rejects([&] { f.device->program(0,Bytes{1}); }, "before WREN");
            require(f.state->opcodes(0x06) == 0, "status mismatch before enable");
        });
        test("Mutation may throw after side effect; backend never retries or resets", [] {
            for (std::uint8_t opcode : {0x20,0x02}) {
                Fixture f; f.identify(); f.state->failAfterSideEffect = opcode;
                if (opcode == 0x20) {
                    f.state->image[0] = 0; rejects([&] { f.device->eraseSector(0); }, "after mutation");
                    require(f.state->image[0] == 0xff, "erase side effect occurred");
                } else {
                    rejects([&] { f.device->program(0,Bytes(64,0x12)); }, "after mutation");
                    require(f.state->image[0] == 0x12 && f.state->image[32] == 0xff, "only first subpage may be changed");
                }
                require(f.state->opcodes(opcode) == 1 && f.state->traffic.back().data == Bytes{0x12}, "no retry, cleanup attempted");
                noTrafficAfter(f.state,[&] { f.device.reset(); });
                require(f.state->packets(Bytes{0x24}) == 0, "no reset during unwinding/destruction");
            }
        });
        test("Busy polling is bounded between commands and WRDI clears WEL", [] {
            Fixture f; f.identify(); f.state->busyReads = 2; f.device->waitReady();
            require(f.state->opcodes(0x05) == 3, "poll until idle");
            f.state->wel = true; f.device->disableWrites();
            require(!f.state->wel && f.state->opcodes(0x04) == 1, "WRDI explicitly sent");
        });
        test("Flash identity/status changes are rejected on later verification", [] {
            Fixture f(true); f.identify(); f.state->status[2] ^= 0x20;
            rejects([&] { f.device->flashIdentity(); }, "status/protection");
            require(f.state->packets(Bytes{0x24}) == 0, "identity failure does not reset");
        });
        test("All 64 observed SFDP bytes remain part of session identity", [] {
            Fixture f; f.identify(); f.state->sfdp[63] ^= 1;
            rejects([&] { f.device->flashIdentity(); }, "identity changed");
            require(f.state->packets(Bytes{0x24}) == 0, "SFDP tail change does not reset");
        });
        test("Only explicit verified-reset method sends 24 and invalidates session", [] {
            Fixture f; f.identify(); f.device->disableWrites(); f.device->resetVerified();
            require(f.state->packets(Bytes{0x24}) == 1, "explicit reset once");
            noTrafficAfter(f.state,[&] { rejects([&] { f.device->read(0,1); });
                rejects([&] { f.device->resetVerified(); }); f.device.reset(); });
        });
        std::cout << passed << " offline transport tests passed. No real transport was opened.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << passed << " tests: " << error.what() << '\n'; return 1;
    }
}
