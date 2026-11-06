#pragma once
#include "monitorfw/core.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <utility>

// Generated fixtures and a strict in-memory NOR device. No real firmware,
// monitor identifiers, system discovery or native transport calls are present.
namespace monitorfw { namespace testing {

inline std::uint32_t fixtureLe32(const Bytes& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) throw Error("Synthetic fixture bounds");
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset+1]) << 8) |
        (std::uint32_t(bytes[offset+2]) << 16) | (std::uint32_t(bytes[offset+3]) << 24);
}
inline void fixturePutLe32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i=0; i<4; ++i) bytes.at(offset+i) = std::uint8_t(value >> (8*i));
}
inline void fixturePutBe16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset)=std::uint8_t(value>>8); bytes.at(offset+1)=std::uint8_t(value);
}
inline void fixturePutBe32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i=0; i<4; ++i) bytes.at(offset+i) = std::uint8_t(value >> (24-8*i));
}
// Deliberately bitwise and independent of the production CRC implementation.
inline std::uint32_t fixtureCrc32(const Bytes& bytes, std::size_t start, std::size_t end) {
    if (start>end || end>bytes.size()) throw Error("Synthetic CRC bounds");
    std::uint32_t value=0;
    for (std::size_t i=start; i<end; ++i) {
        value ^= std::uint32_t(bytes[i]) << 24;
        for (unsigned bit=0; bit<8; ++bit) value=(value<<1)^((value&0x80000000U)?0x04c11db7U:0U);
    }
    return value;
}
inline std::uint16_t fixtureCrc16(const Bytes& bytes, std::size_t start, std::size_t end, bool zeroApCrc=true) {
    if (start>end || end>bytes.size()) throw Error("Synthetic CRC bounds");
    std::uint16_t value=0;
    for (std::size_t i=start; i<end; ++i) {
        const auto byte=(zeroApCrc && i>=0x10014 && i<0x10018)?std::uint8_t(0):bytes[i];
        value ^= std::uint16_t(std::uint16_t(byte)<<8);
        for (unsigned bit=0; bit<8; ++bit) value=std::uint16_t((value<<1)^((value&0x8000U)?0x8005U:0U));
    }
    return value;
}
inline void repairSyntheticIntegrity(Bytes& package) {
    const auto end=fixtureLe32(package,0x1000c), final=fixtureLe32(package,0x10020);
    fixturePutBe16(package,end-2,fixtureCrc16(package,0x10000,end-2));
    fixturePutBe16(package,final-2,fixtureCrc16(package,0x10000,final-2));
    fixturePutLe32(package,0x10014,fixtureCrc32(package,0x20080,end));
    fixturePutBe32(package,final,fixtureCrc32(package,0,final));
}
inline Bytes syntheticPackage(const std::string& version="1000.0", std::size_t extraPayload=0) {
    const std::string text="SYNTHETIC TEST FIXTURE, NOT FIRMWARE\nm-E5728UMDB-"+version;
    Bytes body(text.begin(),text.end()); body.push_back(0);
    body.insert(body.end(),extraPayload,std::uint8_t('X'));
    Bytes compressed;
    for (std::size_t offset=0; offset<body.size();) {
        const auto count=std::uint16_t(std::min<std::size_t>(65535,body.size()-offset));
        compressed.push_back(offset+count==body.size()?1:0); // Stored DEFLATE block, byte-aligned.
        compressed.push_back(std::uint8_t(count)); compressed.push_back(std::uint8_t(count>>8));
        const auto inverse=std::uint16_t(~count);
        compressed.push_back(std::uint8_t(inverse)); compressed.push_back(std::uint8_t(inverse>>8));
        compressed.insert(compressed.end(),body.begin()+offset,body.begin()+offset+count);
        offset+=count;
    }
    const auto end=std::uint32_t(0x23880+compressed.size()+6);
    const auto final=((end+7U)&~7U)+8U;
    if (final+4>PackageLimit) throw Error("Synthetic fixture exceeds package limit");
    Bytes package(final+4,0xff);
    const std::string warning="SYNTHETIC TEST FIXTURE, NOT FIRMWARE";
    std::copy(warning.begin(),warning.end(),package.begin());
    std::fill(package.begin()+0x10000,package.begin()+0x10080,0);
    std::fill(package.begin()+0x20080,package.begin()+0x23880,0);
    fixturePutLe32(package,0x10000,0x20080);
    fixturePutLe32(package,0x10004,0x200000);
    fixturePutLe32(package,0x10008,0x200000+end-0x20080);
    fixturePutLe32(package,0x1000c,end);
    fixturePutLe32(package,0x10020,final);
    fixturePutLe32(package,0x10024,0x10000);
    package[0x211c7]=0xbe; package[0x211c8]=0xad; package[0x211c9]=0xde;
    fixturePutLe32(package,0x211ca,end-0x20080);
    fixturePutLe32(package,0x211ce,0x3800);
    std::copy(compressed.begin(),compressed.end(),package.begin()+0x23880);
    fixturePutLe32(package,end-6,std::uint32_t(body.size()));
    std::fill(package.begin()+end,package.begin()+final,0);
    const std::uint8_t marker[]={0x55,0xaa,0xab,0xcd,0,0};
    std::copy(marker,marker+6,package.begin()+final-8);
    repairSyntheticIntegrity(package);
    return package;
}
inline Bytes syntheticDump(const Bytes& package=syntheticPackage()) {
    if (package.size()>SpiCapacity) throw Error("Synthetic package too large");
    Bytes dump(SpiCapacity,0xff); std::copy(package.begin(),package.end(),dump.begin());
    return dump;
}
inline MonitorIdentity syntheticIdentity(const std::string& serial="SYNTHETIC-UNIT-A") {
    MonitorIdentity identity;
    identity.model="LU28E570DS/EN"; identity.manufacturer="SAM"; identity.serial=serial;
    identity.productCode=0x0d71; identity.transportLocator="mock://synthetic/"+serial;
    const std::string text="SYNTHETIC EDID, NOT A DEVICE: "+serial;
    identity.edidSha256=sha256(Bytes(text.begin(),text.end()));
    return identity;
}
inline FlashIdentity syntheticFlashIdentity() {
    FlashIdentity flash;
    flash.jedec={0xef,0x40,0x15}; flash.status={0,0};
    flash.sfdp={0x53,0x46,0x44,0x50,0,1,0,0xff,0,0,1,9,0x80,0,0,0xff};
    flash.sfdp.resize(64,0xa5); // Synthetic evidence tail; not real device data.
    flash.profile="SYNTHETIC-LEGACY-2M";
    return flash;
}
inline FirmwarePolicy syntheticPolicy(const std::vector<Bytes>& packages) {
    FirmwarePolicy policy;
    for (const auto& package:packages) policy.permittedPackageHashes.push_back(sha256(package));
    return policy;
}

class MockFlash : public IFlashDevice {
    void requireIsp() const { if (!entered) throw Error("Mock access outside ISP"); }
    void range(std::uint32_t address,std::uint32_t length) const {
        if (address>image.size() || length>image.size()-address) throw Error("Mock access outside array");
    }
public:
    struct ReadRecord { std::uint32_t address,length; };
    Bytes image;
    MonitorIdentity identity;
    FlashIdentity flash;
    bool entered=false, failEnter=false, failReset=false;
    unsigned identityCalls=0, enterCalls=0, flashIdentityCalls=0, readCalls=0, readPass=0;
    unsigned eraseCalls=0, programCalls=0, waitCalls=0, disableCalls=0, resetCalls=0;
    unsigned failReadCall=0, shortReadCall=0, corruptReadCall=0, invertReadPass=0;
    unsigned failWaitCall=0, failDisableCall=0;
    std::set<unsigned> failEraseCalls,failProgramCalls;
    std::vector<ReadRecord> reads;
    std::vector<std::uint32_t> erasedAddresses,programmedAddresses;
    std::vector<std::size_t> programSizes;
    std::function<void(MockFlash&)> onFlashIdentity,onErase,onProgram,onReset;
    std::function<void(MockFlash&,std::uint32_t,std::uint32_t)> onRead;
    explicit MockFlash(Bytes initial=syntheticDump(),MonitorIdentity monitor=syntheticIdentity())
        : image(std::move(initial)),identity(std::move(monitor)),flash(syntheticFlashIdentity()) {
        if (image.size()!=SpiCapacity) throw Error("Mock array must be exactly 2 MiB");
    }
    MonitorIdentity monitorIdentity() override {
        ++identityCalls; if (entered) throw Error("Mock EDID query inside ISP"); return identity;
    }
    void enterIsp() override {
        ++enterCalls; if (entered) throw Error("Mock ISP already entered");
        entered=true; if (failEnter) throw Error("Injected ISP-entry failure after side effect");
    }
    FlashIdentity flashIdentity() override {
        requireIsp(); ++flashIdentityCalls; if (onFlashIdentity) onFlashIdentity(*this); return flash;
    }
    Bytes read(std::uint32_t address,std::uint32_t length) override {
        requireIsp(); range(address,length); ++readCalls; reads.push_back({address,length});
        if (address==0) ++readPass;
        if (onRead) onRead(*this,address,length);
        if (readCalls==failReadCall) throw Error("Injected read failure");
        Bytes bytes(image.begin()+address,image.begin()+address+length);
        if (length==256 && readPass==invertReadPass) for (auto& byte:bytes) byte^=0xff;
        if (readCalls==corruptReadCall && !bytes.empty()) bytes[0]^=1;
        if (readCalls==shortReadCall && !bytes.empty()) bytes.pop_back();
        return bytes;
    }
    void eraseSector(std::uint32_t address) override {
        requireIsp(); range(address,SectorSize);
        if (address%SectorSize) throw Error("Mock erase misaligned");
        ++eraseCalls; erasedAddresses.push_back(address);
        std::fill(image.begin()+address,image.begin()+address+SectorSize,0xff);
        if (onErase) onErase(*this);
        if (failEraseCalls.count(eraseCalls)) throw Error("Injected failure after erase");
    }
    void program(std::uint32_t address,const Bytes& bytes) override {
        requireIsp(); range(address,std::uint32_t(bytes.size()));
        if (bytes.empty() || bytes.size()>256 || address/256!=(address+bytes.size()-1)/256)
            throw Error("Mock invalid NOR page program");
        ++programCalls; programmedAddresses.push_back(address); programSizes.push_back(bytes.size());
        for (std::size_t i=0;i<bytes.size();++i) {
            if ((image[address+i]&bytes[i])!=bytes[i]) throw Error("Mock attempted NOR zero-to-one programming");
            image[address+i]&=bytes[i];
        }
        if (onProgram) onProgram(*this);
        if (failProgramCalls.count(programCalls)) throw Error("Injected failure after page program");
    }
    void waitReady() override {
        requireIsp(); if (++waitCalls==failWaitCall) throw Error("Injected ready timeout");
    }
    void disableWrites() override {
        requireIsp(); if (++disableCalls==failDisableCall) throw Error("Injected write-disable failure");
    }
    void resetVerified() override {
        requireIsp(); ++resetCalls; if (onReset) onReset(*this);
        if (failReset) throw Error("Injected reset failure");
        entered=false;
    }
};

class MemoryStore : public IArtifactStore {
public:
    std::map<std::string,Bytes> files;
    std::vector<std::string> journal;
    unsigned saveCalls=0,journalCalls=0,armCalls=0,clearCalls=0;
    unsigned failSaveCall=0,failJournalCall=0;
    bool recoveryArmed=false,failArm=false,failClear=false;
    MonitorIdentity recoveryIdentity;
    std::function<void(MemoryStore&,const std::string&)> onSave;
    void saveNew(const std::string& leaf,const Bytes& data) override {
        ++saveCalls; if (onSave) onSave(*this,leaf);
        if (saveCalls==failSaveCall) throw Error("Injected persistence failure");
        if (files.count(leaf)) throw Error("Mock artifact overwrite rejected");
        files.emplace(leaf,data);
    }
    void appendJournal(const std::string& event) override {
        if (++journalCalls==failJournalCall) throw Error("Injected journal failure");
        journal.push_back(event);
    }
    void armRecovery(const MonitorIdentity& identity) override {
        ++armCalls; if (failArm || recoveryArmed) throw Error("Mock recovery interlock blocks entry");
        recoveryArmed=true; recoveryIdentity=identity;
    }
    void clearRecovery() override {
        ++clearCalls; if (failClear) throw Error("Injected interlock-clear failure"); recoveryArmed=false;
    }
};
}} // namespace monitorfw::testing
