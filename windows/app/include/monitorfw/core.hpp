#pragma once
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace monitorfw {
typedef std::vector<std::uint8_t> Bytes;
static const std::uint32_t SpiCapacity = 0x200000;
static const std::uint32_t SectorSize = 4096;
static const std::uint32_t HeaderSector = 0x10000;
static const std::uint32_t PackageLimit = 0x90000;

struct Error : std::runtime_error { explicit Error(const std::string& text) : std::runtime_error(text) {} };
std::string sha256(const Bytes& bytes);
std::string jsonQuote(const std::string& value);

struct MonitorIdentity {
    std::string model, manufacturer, serial, edidSha256, transportLocator;
    std::uint16_t productCode = 0;
    bool serialAmbiguous = false;
    std::string ambiguityReason;
    bool operator==(const MonitorIdentity& rhs) const;
    bool operator!=(const MonitorIdentity& rhs) const { return !(*this == rhs); }
};
struct FlashIdentity {
    Bytes jedec, sfdp, status;
    std::uint32_t capacity = SpiCapacity, sectorSize = SectorSize, pageSize = 256;
    std::string profile;
    bool operator==(const FlashIdentity& rhs) const;
    bool operator!=(const FlashIdentity& rhs) const { return !(*this == rhs); }
};

// The device never resets in its destructor. Only the transaction engine calls
// resetVerified after a full image and unchanged flash status have been verified.
class IFlashDevice {
public:
    virtual ~IFlashDevice() {}
    virtual MonitorIdentity monitorIdentity() = 0; // Live EDID, before ISP only.
    virtual void enterIsp() = 0;
    virtual FlashIdentity flashIdentity() = 0;
    virtual Bytes read(std::uint32_t address, std::uint32_t length) = 0;
    virtual void eraseSector(std::uint32_t address) = 0;
    virtual void program(std::uint32_t address, const Bytes& data) = 0;
    virtual void waitReady() = 0;
    virtual void disableWrites() = 0;
    virtual void resetVerified() = 0;
};

// saveNew must durably persist a new file or throw; never overwrite a backup.
// armRecovery is persistent across process termination and must fail if a marker
// already exists for this monitor. A result directory alone is not the interlock.
class IArtifactStore {
public:
    virtual ~IArtifactStore() {}
    virtual void saveNew(const std::string& leaf, const Bytes& data) = 0;
    virtual void appendJournal(const std::string& event) = 0;
    virtual void armRecovery(const MonitorIdentity& identity) = 0;
    virtual void clearRecovery() = 0;
};

struct Progress { std::string phase; std::uint32_t done = 0, total = 0; };
typedef std::function<void(const Progress&)> ProgressSink;
typedef std::function<bool()> CancelCheck;

struct PackageInfo {
    std::uint32_t length = 0, mainEnd = 0, expandedLength = 0;
    std::string sha256, expandedSha256, version;
    std::uint16_t sum16 = 0;
};
// Exact container bounds, both CRC16/CRC32 checks, bounded raw DEFLATE with exact
// end-of-stream, and loader layout. Does not imply authorization to program.
PackageInfo inspectPackage(const Bytes& package);
Bytes packageFromDump(const Bytes& fullDump);

struct FirmwarePolicy {
    std::string model = "LU28E570DS/EN";
    std::vector<std::string> permittedPackageHashes;
    bool requireApplicationOnlyChange = true;
};
FirmwarePolicy reviewedPolicy();

struct Backup {
    MonitorIdentity monitor;
    FlashIdentity flash;
    Bytes image;
    std::string firstSha256, secondSha256;
    bool persisted = false;
};
enum class ImageKind { SamsungPackage, OwnSpiDump };
struct BackupEvidence {
    MonitorIdentity monitor;
    std::string imageSha256, flashJedecHex, sfdpSha256;
    std::uint32_t capacity = 0;
    bool readsEqual = false;
};
std::string backupManifest(const Backup& backup);
BackupEvidence parseBackupManifest(const std::string& json);

struct SectorChange {
    std::uint32_t address = 0;
    std::string beforeSha256, afterSha256;
};
// Values are copied into an immutable plan; editable manifests cannot override it.
class FlashPlan {
    Backup backup_;
    Bytes target_;
    std::vector<SectorChange> sectors_;
    std::string targetHash_, planHash_;
    ImageKind kind_;
    FlashPlan(const Backup&, const Bytes&, ImageKind);
    friend FlashPlan planPackageOverlay(const Backup&, const Bytes&, const FirmwarePolicy&);
    friend FlashPlan planOwnDump(const Backup&, const Bytes&, const BackupEvidence&, const FirmwarePolicy&);
public:
    const Backup& backup() const { return backup_; }
    const Bytes& target() const { return target_; }
    const std::vector<SectorChange>& sectors() const { return sectors_; }
    const std::string& targetHash() const { return targetHash_; }
    const std::string& planHash() const { return planHash_; }
    ImageKind imageKind() const { return kind_; }
};
FlashPlan planPackageOverlay(const Backup&, const Bytes& package, const FirmwarePolicy&);
FlashPlan planOwnDump(const Backup&, const Bytes& dump, const BackupEvidence&, const FirmwarePolicy&);
std::string planManifest(const FlashPlan& plan);

struct Result {
    bool success = false, backupVerified = false, targetVerified = false;
    bool rollbackAttempted = false, rollbackVerified = false;
    bool resetAttempted = false, resetSucceeded = false, recoveryRequired = true;
    std::string error, sourceSha256, targetSha256, planSha256;
    std::vector<std::uint32_t> touchedSectors;
};
std::string resultManifest(const Result& result);

// One operation owns one ISP session and one interlock. A failed preflight holds
// reset; users must inspect the report rather than auto-reset an unknown image.
class Session {
    IFlashDevice& device_;
    IArtifactStore& store_;
    MonitorIdentity expected_;
    ProgressSink progress_;
    CancelCheck cancel_;
    bool entered_ = false, armed_ = false, finished_ = false;
    Backup backup_;
    Bytes readFull(const std::string& phase, bool allowCancel);
    Bytes readPair(const std::string& phase, bool allowCancel, bool persist);
    void verifyIdentityAndStatus(const FlashIdentity& expected);
    void writeSector(std::uint32_t address, const Bytes& image, std::uint32_t chunk);
    void log(const std::string& event);
    void finishReset(Result& result);
    void saveResult(Result& result);
public:
    Session(IFlashDevice&, IArtifactStore&, const MonitorIdentity&, ProgressSink = ProgressSink(), CancelCheck = CancelCheck());
    ~Session(); // No hardware access, reset or implicit rollback.
    Backup backupTwice(); // Persists two complete independent reads and manifest.
    Result finishBackup(); // Rechecks status; resets only a verified double backup.
    Result verify(const Bytes& image, ImageKind kind, const BackupEvidence* ownDumpEvidence = 0);
    Result execute(const FlashPlan& plan); // Sector verify + full target verify.
};

const char* codingEepromUnavailableReason();
} // namespace monitorfw
