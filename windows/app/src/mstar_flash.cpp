#include "monitorfw/transport.hpp"
#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

namespace monitorfw {
namespace {
const Bytes LegacySfdp = {0x53,0x46,0x44,0x50,0,1,0,0xff,0,0,1,9,0x80,0,0,0xff};
const Bytes JvSfdp = {0x53,0x46,0x44,0x50,5,1,0,0xff,0,5,1,16,0x80,0,0,0xff};
void range(std::uint32_t address, std::uint32_t length) {
    if (address > SpiCapacity || length > SpiCapacity - address) throw Error("SPI range exceeds the reviewed 2 MiB array.");
}
std::string exceptionText(const std::exception_ptr& error) {
    try { if (error) std::rethrow_exception(error); }
    catch (const std::exception& value) { return value.what(); }
    catch (...) { return "unknown exception"; }
    return "none";
}
class MstarFlash : public IFlashDevice {
    std::unique_ptr<II2cTransport> transport_;
    TransportCapabilities capabilities_;
    MonitorIdentity selected_;
    bool entryAttempted_ = false, entered_ = false, identified_ = false, resetSent_ = false;
    FlashIdentity profile_;

    void requireEntered() const {
        if (!entered_ || resetSent_) throw Error("An established MStar ISP session is required.");
    }
    void requireProfile() const {
        requireEntered();
        if (!identified_) throw Error("The SPI profile has not been identified.");
    }
    void bridge(const Bytes& packet) {
        validateI2cRequest(capabilities_, false, 0x49, Bytes(), packet.size());
        transport_->write(0x49, packet);
    }
    Bytes spi(const Bytes& command, std::uint32_t length, bool reading) {
        requireEntered();
        if (command.empty()) throw Error("Empty SPI command.");
        const std::uint8_t opcode = command[0];
        if (reading) {
            const bool valid = ((opcode == 0x9f || opcode == 0x05 || opcode == 0x35 || opcode == 0x15) && command.size() == 1) ||
                (opcode == 0x03 && command.size() == 4) || (opcode == 0x5a && command.size() == 5);
            if (!valid || length == 0 || length > 256) throw Error("SPI read command is outside the reviewed protocol.");
        } else {
            const bool valid = ((opcode == 0x06 || opcode == 0x04) && command.size() == 1) ||
                (opcode == 0x20 && command.size() == 4) || (opcode == 0x02 && command.size() >= 5 && command.size() <= 260);
            if (!valid) throw Error("SPI write opcode is outside the erase/program/volatile-latch whitelist.");
        }
        Bytes packet(1, 0x10); packet.insert(packet.end(), command.begin(), command.end());
        Bytes result;
        std::exception_ptr primary;
        try {
            bridge(packet);
            if (reading) {
                bridge(Bytes(1, 0x11));
                validateI2cRequest(capabilities_, true, 0x49, Bytes(), length);
                result = transport_->read(0x49, Bytes(), length);
                if (result.size() != length) throw Error("Short SPI bridge read.");
            }
        } catch (...) { primary = std::current_exception(); }
        try { bridge(Bytes(1, 0x12)); }
        catch (...) {
            throw Error("SPI bridge cleanup failed after " + exceptionText(primary) + "; cleanup: " + exceptionText(std::current_exception()));
        }
        if (primary) std::rethrow_exception(primary);
        return result;
    }
    Bytes spiRead(const Bytes& command, std::uint32_t length) { return spi(command, length, true); }
    void spiWrite(const Bytes& command) { spi(command, 0, false); }
    std::uint8_t readyStatus() {
        requireEntered();
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const std::uint8_t status = spiRead(Bytes(1, 0x05), 1)[0];
            if (!(status & 1)) return status;
            if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(5))
                throw Error("SPI remained busy beyond the five-second polling deadline.");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    void enableWrite() {
        requireProfile();
        if (readyStatus() != profile_.status[0]) throw Error("SPI status changed before WREN.");
        spiWrite(Bytes(1, 0x06));
        if (spiRead(Bytes(1, 0x05), 1)[0] != static_cast<std::uint8_t>(profile_.status[0] | 2))
            throw Error("SPI write-enable latch was not set as expected.");
    }
    Bytes addressed(std::uint8_t opcode, std::uint32_t address) {
        return Bytes{opcode, static_cast<std::uint8_t>(address >> 16),
            static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address)};
    }
public:
    MstarFlash(std::unique_ptr<II2cTransport> transport, const MonitorIdentity& selected)
        : transport_(std::move(transport)), selected_(selected) {
        if (!transport_) throw Error("An I2C transport is required.");
        capabilities_ = transport_->capabilities();
        if (!capabilities_.rawRead || !capabilities_.separateWriteRead ||
            capabilities_.maxRead < 256 || capabilities_.maxWrite < 37)
            throw Error("MStar requires separate writes/raw reads, read capacity of 256 bytes, and write capacity of 37 bytes; no transport adaptation is assumed.");
    }
    // Default destruction only frees memory. No ISP exit, reset, or hidden I2C.
    virtual MonitorIdentity monitorIdentity() {
        if (entryAttempted_) throw Error("Live EDID cannot be queried after ISP entry was attempted.");
        MonitorIdentity live = decodeMonitorEdid(transport_->liveEdid(), selected_.transportLocator);
        // Duplicate-serial evidence comes from enumeration, not from one EDID.
        if (selected_.serialAmbiguous) {
            live.serialAmbiguous = true;
            live.ambiguityReason = selected_.ambiguityReason;
        }
        if (live != selected_) throw Error("Live EDID differs from the selected endpoint.");
        return live;
    }
    virtual void enterIsp() {
        if (entryAttempted_) throw Error("ISP entry cannot be retried on this device object.");
        monitorIdentity(); // Exact live endpoint immediately before MSTAR.
        entryAttempted_ = true;
        bridge(Bytes{'M','S','T','A','R'});
        entered_ = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    virtual FlashIdentity flashIdentity() {
        requireEntered();
        const Bytes jedec = spiRead(Bytes(1, 0x9f), 3);
        if (jedec != Bytes{0xef,0x40,0x15} || spiRead(Bytes(1, 0x9f), 3) != jedec)
            throw Error("SPI JEDEC identity is not the reviewed EF4015 device.");
        // Preserve all 64 observed bytes as identity evidence. Only the reviewed
        // 16-byte prefix selects a profile; no unobserved tail is fabricated.
        const Bytes sfdp = spiRead(Bytes{0x5a,0,0,0,0}, 64);
        const bool legacy = std::equal(LegacySfdp.begin(), LegacySfdp.end(), sfdp.begin());
        const bool jv = std::equal(JvSfdp.begin(), JvSfdp.end(), sfdp.begin());
        if (!legacy && !jv) throw Error("SPI SFDP does not match either reviewed flash profile.");
        Bytes status{readyStatus(), spiRead(Bytes(1, 0x35), 1)[0]};
        if (jv) status.push_back(spiRead(Bytes(1, 0x15), 1)[0]);
        const Bytes expected = legacy ? Bytes{0,0} : Bytes{0,2,0x60};
        if (status != expected) throw Error("SPI status/protection differs from the reviewed profile.");
        FlashIdentity result;
        result.jedec = jedec; result.sfdp = sfdp; result.status = status;
        result.profile = legacy ? "winbond-ef4015-legacy" : "winbond-ef4015-jv";
        if (identified_ && result != profile_) throw Error("SPI identity changed within the ISP session.");
        profile_ = result; identified_ = true;
        return result;
    }
    virtual Bytes read(std::uint32_t address, std::uint32_t length) {
        requireProfile(); range(address, length);
        Bytes result; result.reserve(length);
        while (length) {
            const std::uint32_t count = std::min<std::uint32_t>(length, 256);
            const Bytes part = spiRead(addressed(0x03, address), count);
            result.insert(result.end(), part.begin(), part.end());
            address += count; length -= count;
        }
        return result;
    }
    virtual void eraseSector(std::uint32_t address) {
        requireProfile(); range(address, SectorSize);
        if (address % SectorSize) throw Error("SPI erase must be aligned to a 4 KiB sector.");
        enableWrite();
        spiWrite(addressed(0x20, address));
        if (readyStatus() != profile_.status[0]) throw Error("SPI erase did not clear WEL or changed status.");
    }
    virtual void program(std::uint32_t address, const Bytes& data) {
        requireProfile();
        if (data.empty() || data.size() > 256) throw Error("Program data must contain 1 to 256 bytes.");
        range(address, static_cast<std::uint32_t>(data.size()));
        if ((address & 255) + data.size() > 256) throw Error("Program operation crosses a hardware page boundary.");
        // Conservative 32-byte transport payloads; do not presume a 261-byte I2C
        // packet works on an untested driver. Every subpage gets its own WREN.
        for (std::size_t offset = 0; offset < data.size(); offset += 32) {
            const std::size_t count = std::min<std::size_t>(32, data.size() - offset);
            Bytes command = addressed(0x02, address + static_cast<std::uint32_t>(offset));
            command.insert(command.end(), data.begin() + offset, data.begin() + offset + count);
            enableWrite(); spiWrite(command);
            if (readyStatus() != profile_.status[0]) throw Error("SPI program did not clear WEL or changed status.");
        }
    }
    virtual void waitReady() { readyStatus(); }
    virtual void disableWrites() {
        requireEntered(); readyStatus(); spiWrite(Bytes(1, 0x04));
        if (spiRead(Bytes(1, 0x05), 1)[0] & 3) throw Error("SPI WRDI did not leave the device idle and write-disabled.");
    }
    virtual void resetVerified() {
        requireProfile();
        // Authorization is the core engine's verified-image state, not this layer.
        bridge(Bytes(1, 0x24));
        resetSent_ = true; entered_ = false;
    }
};
} // namespace

std::unique_ptr<IFlashDevice> makeMstarFlash(std::unique_ptr<II2cTransport> transport, const MonitorIdentity& selected) {
    return std::unique_ptr<IFlashDevice>(new MstarFlash(std::move(transport), selected));
}
} // namespace monitorfw
