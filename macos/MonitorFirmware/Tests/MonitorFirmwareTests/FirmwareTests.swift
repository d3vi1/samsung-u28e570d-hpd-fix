import XCTest
import NativeSupport
@testable import MonitorFirmwareCore

private func syntheticPackage(version: String, bodyCount: Int = 1024) -> Data {
    var body = Array(repeating: UInt8(0x41), count: bodyCount)
    let text = Array(("m-E5728UMDB-" + version + "\0").utf8)
    body.replaceSubrange(0..<text.count, with: text)
    // A complete RFC1951 stored block, with a deliberately inspectable boundary.
    let length = UInt16(bodyCount), inverted = ~length
    let compressed: [UInt8] = [1,UInt8(length & 255),UInt8(length >> 8),UInt8(inverted & 255),UInt8(inverted >> 8)] + body
    let mainEnd = 0x23880 + compressed.count + 6
    let crcOffset = ((mainEnd + 7) & ~7) + 8
    var b = [UInt8](repeating: 0, count: crcOffset + 4)
    func le(_ value: Int, _ offset: Int) { for i in 0..<4 { b[offset+i] = UInt8((value >> (i*8)) & 255) } }
    func be16(_ value: UInt16, _ offset: Int) { b[offset] = UInt8(value >> 8); b[offset+1] = UInt8(value & 255) }
    le(0x20080,0x10000); le(0x200000,0x10004); le(0x200000+mainEnd-0x20080,0x10008)
    le(mainEnd,0x1000c); le(crcOffset,0x10020)
    b.replaceSubrange(0x211c7..<0x211ca, with: [0xbe,0xad,0xde])
    le(mainEnd-0x20080,0x211ca); le(0x3800,0x211ce)
    b.replaceSubrange(0x23880..<(mainEnd-6), with: compressed); le(body.count,mainEnd-6)
    be16(FirmwareImage.crc16(b,0x10000..<(mainEnd-2)),mainEnd-2)
    le(Int(FirmwareImage.crc32(b,0x20080..<mainEnd)),0x10014)
    b.replaceSubrange((crcOffset-8)..<(crcOffset-2), with: [0x55,0xaa,0xab,0xcd,0,0])
    be16(FirmwareImage.crc16(b,0x10000..<(crcOffset-2)),crcOffset-2)
    let crc = FirmwareImage.crc32(b,0..<crcOffset)
    for i in 0..<4 { b[crcOffset+i] = UInt8((crc >> ((3-i)*8)) & 255) }
    return Data(b)
}

private func candidateURL() -> URL {
    URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("firmware/m-E5728UMDB-1005.0-experimental.1.bin")
}
private func modifiedAppWithValidCRCs(_ package: Data) throws -> Data {
    let old = [UInt8](package)
    func le(_ bytes: [UInt8],_ offset: Int) -> Int { Int(bytes[offset]) | Int(bytes[offset+1]) << 8 | Int(bytes[offset+2]) << 16 | Int(bytes[offset+3]) << 24 }
    let oldEnd = le(old,0x1000c), expandedCount = le(old,oldEnd - 6)
    let compressed = Data(old[0x23880..<oldEnd-6])
    var expanded = Data(count: expandedCount)
    let status = expanded.withUnsafeMutableBytes { output in compressed.withUnsafeBytes { input in
        mf_inflate_exact(input.bindMemory(to: UInt8.self).baseAddress,compressed.count,output.bindMemory(to: UInt8.self).baseAddress,expandedCount)
    } }
    XCTAssertEqual(status,0)
    expanded[0] ^= 1 // Precisely modify app[0], then repack a structurally valid image.
    var stored: [UInt8] = []
    for offset in stride(from: 0,to: expanded.count,by: 65535) {
        let count = min(65535,expanded.count-offset), length = UInt16(count), complement = ~UInt16(count)
        stored += [offset + count == expanded.count ? 1 : 0,UInt8(length & 255),UInt8(length >> 8),UInt8(complement & 255),UInt8(complement >> 8)]
        stored += expanded[offset..<offset+count]
    }
    let end = 0x23880 + stored.count + 6, crcOffset = ((end + 7) & ~7) + 8
    var b = Array(old.prefix(0x23880)) + [UInt8](repeating: 0,count: crcOffset + 4 - 0x23880)
    func putLE(_ value: Int,_ p: Int) { for i in 0..<4 { b[p+i] = UInt8((value >> (i*8)) & 255) } }
    func putBE16(_ value: UInt16,_ p: Int) { b[p] = UInt8(value >> 8); b[p+1] = UInt8(value & 255) }
    putLE(0x200000 + end - 0x20080,0x10008); putLE(end,0x1000c); putLE(0,0x10014); putLE(crcOffset,0x10020)
    putLE(end-0x20080,0x211ca); b.replaceSubrange(0x23880..<end-6,with: stored); putLE(expandedCount,end-6)
    putBE16(FirmwareImage.crc16(b,0x10000..<end-2),end-2)
    putLE(Int(FirmwareImage.crc32(b,0x20080..<end)),0x10014)
    b.replaceSubrange(crcOffset-8..<crcOffset-2,with: [0x55,0xaa,0xab,0xcd,0,0])
    putBE16(FirmwareImage.crc16(b,0x10000..<crcOffset-2),crcOffset-2)
    let crc = FirmwareImage.crc32(b,0..<crcOffset)
    for i in 0..<4 { b[crcOffset+i] = UInt8((crc >> ((3-i)*8)) & 255) }
    return Data(b)
}

private final class MemoryDevice: FlashDevice, @unchecked Sendable {
    let isSimulated = true
    let selected = MonitorIdentity(manufacturer: "SAM", product_code: 0x0c4e, edid_serial: "TEST-ONLY-UNIT", edid_sha256: sha256(Data("synthetic-edid".utf8)), transport_locator: "mock:one")
    var bytes: Data
    var erases: [Int] = []
    var programs = 0
    var fullStarts = 0
    var shortRead = false
    var differingSecondRead = false
    var failNextProgram = false
    var failEveryProgram = false
    var corruptFinal = false
    var unknownProfile = false
    var changedStatus = false
    var exits = 0
    var failExit = false
    var beforeErase: (@Sendable () -> Void)?
    var afterExit: (@Sendable () -> Void)?
    var identityOverride: MonitorIdentity?
    var transientIdentityFailures = 0
    init(_ bytes: Data) { self.bytes = bytes }
    func assertQualified() throws {}
    func assertWritableQualified() throws {}
    func identity() throws -> MonitorIdentity {
        if transientIdentityFailures > 0 { transientIdentityFailures -= 1; throw IdentificationTransientError(stage: "Mock C3 framing",detail: "Injected startup frame",frame: Data([0,0])) }
        return identityOverride ?? selected
    }
    func profile() throws -> FlashProfile {
        // Explicitly synthetic SFDP tail; no claim this tail was observed on hardware.
        let synthetic = FlashProfile.reviewedSFDP + Data(repeating: 0xa5,count: 48)
        return FlashProfile(jedec: unknownProfile ? "ffffff" : "ef4015", sfdp_sha256: sha256(synthetic), status_before: [0,2,0x60], status_after: [0,2,0x60], sfdpSnapshot: synthetic)
    }
    func status() throws -> [UInt8] { changedStatus ? [0,2,0x61] : [0,2,0x60] }
    func read(address: Int, count: Int) throws -> Data {
        if address == 0 { fullStarts += 1 }
        var output = Data(bytes[address..<(address+count)])
        if shortRead { output.removeLast() }
        if differingSecondRead && fullStarts == 2 && address == 0 { output[0] ^= 1 }
        if corruptFinal && fullStarts >= 3 && address == 0 { output[0] ^= 1 }
        return output
    }
    func eraseSector(address: Int) throws { beforeErase?(); erases.append(address); bytes.replaceSubrange(address..<(address+flashSectorSize), with: repeatElement(UInt8(255), count: flashSectorSize)) }
    func program(address: Int, bytes: Data) throws {
        programs += 1
        if failEveryProgram || failNextProgram { failNextProgram = false; throw FirmwareError.rejected("Injected programming failure") }
        for i in 0..<bytes.count { self.bytes[address+i] &= bytes[i] }
    }
    func writeDisable() throws {}
    func exitISP(permit: VerifiedImagePermit) throws { if failExit { throw FirmwareError.rejected("Injected ISP exit failure") }; exits += 1; afterExit?() }
}

private final class EventRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var events: [String] = []
    func append(_ event: String) { lock.lock(); defer { lock.unlock() }; events.append(event) }
    var snapshot: [String] { lock.lock(); defer { lock.unlock() }; return events }
}

private final class HardwareSentinel: FlashDevice, @unchecked Sendable {
    let isSimulated = false
    var calls = 0
    private func forbidden<T>() throws -> T { calls += 1; throw FirmwareError.rejected("Hardware method must not be reached") }
    func assertQualified() throws { try forbidden() as Void }
    func assertWritableQualified() throws { try forbidden() as Void }
    func identity() throws -> MonitorIdentity { try forbidden() }
    func profile() throws -> FlashProfile { try forbidden() }
    func status() throws -> [UInt8] { try forbidden() }
    func read(address: Int,count: Int) throws -> Data { try forbidden() }
    func eraseSector(address: Int) throws { try forbidden() as Void }
    func program(address: Int,bytes: Data) throws { try forbidden() as Void }
    func writeDisable() throws { try forbidden() as Void }
    func exitISP(permit: VerifiedImagePermit) throws { try forbidden() as Void }
}

private final class ScriptBus: RawI2CBus, @unchecked Sendable {
    let isSimulated = true
    let legacy: Bool
    var writes: [Data] = []
    var reads: [(UInt8, Int)] = []
    var command = Data()
    var identityReads = 0
    init(legacy: Bool) { self.legacy = legacy }
    func assertQualified() throws {}
    func assertWritableQualified() throws {}
    func selectedIdentity() throws -> MonitorIdentity { identityReads += 1; return MemoryDevice(Data()).selected }
    func write(address: UInt8, bytes: Data) throws {
        XCTAssertEqual(address, 0x49); writes.append(bytes)
        if bytes.first == 0x10 { command = Data(bytes.dropFirst()) }
    }
    func read(address: UInt8, count: Int) throws -> Data {
        reads.append((address,count)); XCTAssertEqual(address,0x49)
        XCTAssertEqual(writes.last, Data([0x11]))
        switch command.first {
        case 0x9f: return Data([0xef,0x40,0x15])
        case 0x5a: return (legacy ? FlashProfile.legacySFDP : FlashProfile.reviewedSFDP) + Data(repeating: 0xa5,count: 48) // synthetic tail
        case 0x05: return Data([0])
        case 0x35: return Data([legacy ? 0 : 2])
        case 0x15: if legacy { throw FirmwareError.rejected("Legacy profile must never read 0x15") }; return Data([0x60])
        default: throw FirmwareError.rejected("Unexpected scripted command")
        }
    }
}

private func syntheticEDID(serial: String = "TESTROUTE001", extensions: Bool = false) -> Data {
    var bytes = [UInt8](repeating: 0,count: extensions ? 256 : 128)
    bytes.replaceSubrange(0..<8,with: [0,255,255,255,255,255,255,0])
    bytes[8] = 0x4c; bytes[9] = 0x2d; bytes[10] = 0x4e; bytes[11] = 0x0c
    bytes.replaceSubrange(12..<16,with: [0x78,0x56,0x34,0x12])
    bytes[57] = 0xff
    let serialBytes = Array(serial.utf8.prefix(13))
    bytes.replaceSubrange(59..<59+serialBytes.count,with: serialBytes)
    bytes[75] = 0xfc; bytes.replaceSubrange(77..<84,with: Array("U28E570".utf8))
    bytes[126] = extensions ? 1 : 0
    bytes[127] = UInt8(truncatingIfNeeded: -bytes[0..<127].reduce(0) { $0 + Int($1) })
    if extensions { bytes[128] = 2; bytes[129] = 3; bytes[130] = 4; bytes[255] = UInt8(truncatingIfNeeded: -bytes[128..<255].reduce(0) { $0 + Int($1) }) }
    return Data(bytes)
}
private final class IdentificationChannel: SubaddressedI2C {
    var frame: Data
    var edid: Data
    var writes: [Data] = []
    var reads: [(UInt32,UInt32,Int)] = []
    var unstable = false
    private var baseReads = 0
    init(edid: Data = syntheticEDID()) {
        self.edid = edid
        var frame = [UInt8](repeating: 0,count: 24)
        frame.replaceSubrange(0..<4,with: [0x6e,0x94,0xc3,0])
        frame.replaceSubrange(4..<22,with: Array("m-E5728UMDB-1005.0".utf8))
        frame[22] = frame.prefix(22).reduce(UInt8(0x50),^)
        self.frame = Data(frame)
    }
    func write(chip: UInt32,offset: UInt32,bytes: Data) throws { XCTAssertEqual(chip,0x37); XCTAssertEqual(offset,0x51); writes.append(bytes) }
    func read(chip: UInt32,offset: UInt32,count: Int) throws -> Data {
        reads.append((chip,offset,count))
        if chip == 0x37 { XCTAssertEqual(offset,0x51); XCTAssertEqual(count,24); return frame }
        XCTAssertEqual(chip,0x50); XCTAssertTrue(offset == 0 || offset == 128); XCTAssertEqual(count,128)
        if offset == 0 { baseReads += 1 }
        var result = Data(edid[Int(offset)..<(Int(offset)+count)])
        if unstable && offset == 0 && baseReads > 1 { result[20] ^= 1 }
        return result
    }
}

final class FirmwareTests: XCTestCase {
    var temporary: URL!
    override func setUpWithError() throws {
        temporary = FileManager.default.temporaryDirectory.appendingPathComponent("monitorfw-tests-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
    }
    override func tearDownWithError() throws { try FileManager.default.removeItem(at: temporary) }
    func archive(_ label: String = "test") throws -> EvidenceArchive { try EvidenceArchive(parent: temporary, label: label) }
    func source() -> Data {
        let package = syntheticPackage(version: "1004.5")
        return package + Data(repeating: 0xa5, count: flashCapacity - package.count)
    }
    private func backup(_ device: MemoryDevice) throws -> VerifiedBackup { try FlashEngine.backup(device: device, selected: device.selected, archive: archive("backup")) }
    private func plan(_ device: MemoryDevice) throws -> WritePlan { try WritePlan(backup: backup(device), input: FirmwareImage.validate(syntheticPackage(version: "1005.0")),policy: .simulationOnly) }

    func testPackageAndFullImageAreDistinct() throws {
        let package = try FirmwareImage.validate(syntheticPackage(version: "1005.0"))
        XCTAssertEqual(package.report.image_kind, "samsung_package")
        XCTAssertEqual(package.report.version, "m-E5728UMDB-1005.0")
        XCTAssertEqual(try FirmwareImage.validate(source()).report.image_kind, "spi_full")
    }
    func testCorruptionAndTruncationAreRejected() throws {
        var damaged = syntheticPackage(version: "1005.0"); damaged[0x23900] ^= 0x80
        XCTAssertThrowsError(try FirmwareImage.validate(damaged))
        XCTAssertThrowsError(try FirmwareImage.validate(Data(damaged.dropLast())))
        XCTAssertThrowsError(try FirmwareImage.validate(Data(repeating: 0xff, count: flashCapacity)))
    }
    func testRepositoryCandidateOffline() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let url = root.appendingPathComponent("firmware/m-E5728UMDB-1005.0-experimental.1.bin")
        let image = try FirmwareImage.load(url)
        XCTAssertEqual(image.report.version, "m-E5728UMDB-1005.0")
        XCTAssertEqual(image.report.package_sha256, "3d5cda5eb49c6c0bb0378742aa1767becd31d3b3e85a6c0b9c023bd6ada2dac4")
    }
    func testBackupTwoFullReadsAndManifestReload() throws {
        let device = MemoryDevice(source()); let result = try backup(device)
        XCTAssertEqual(device.fullStarts, 2)
        XCTAssertEqual(result.manifest.reads.count, 2)
        XCTAssertFalse(result.manifest.coding_eeprom.available)
        XCTAssertEqual(try BackupLoader.load(manifestURL: result.directory.appendingPathComponent("backup.json")).image, device.bytes)
        XCTAssertTrue(device.erases.isEmpty)
        XCTAssertEqual(device.exits, 1)
    }
    func testBackupMismatchRetainsBothFullReadsWithoutManifest() throws {
        let device = MemoryDevice(source()); device.differingSecondRead = true; let evidence = try archive()
        XCTAssertThrowsError(try FlashEngine.backup(device: device, selected: device.selected, archive: evidence))
        for name in ["backup-read-1.bin","backup-read-2.bin"] { XCTAssertEqual(try Data(contentsOf: evidence.directory.appendingPathComponent(name)).count, flashCapacity) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: evidence.directory.appendingPathComponent("backup.json").path))
    }
    func testShortReadAndUnknownProfileRejectWithoutErase() throws {
        let device = MemoryDevice(source()); device.shortRead = true
        XCTAssertThrowsError(try backup(device)); XCTAssertTrue(device.erases.isEmpty)
        device.shortRead = false; device.unknownProfile = true
        XCTAssertThrowsError(try backup(device)); XCTAssertTrue(device.erases.isEmpty)
    }
    func testIOAVRejectsBeforeAnyISPTransaction() throws {
        let identity = MemoryDevice(source()).selected
        let device = MStarISPDevice(bus: IOAVRawBus(target: identity))
        XCTAssertThrowsError(try FlashEngine.backup(device: device, selected: identity, archive: archive())) { error in
            XCTAssertTrue(error.localizedDescription.contains("not qualified"))
        }
    }
    func testPlanPreservesTailAndWritesHeaderLast() throws {
        let device = MemoryDevice(source()); let planned = try plan(device)
        XCTAssertEqual(planned.changedSectors.last, firmwareHeaderSector)
        XCTAssertEqual(planned.target.suffix(4096), device.bytes.suffix(4096))
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: try archive(), recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertTrue(result.completed); XCTAssertTrue(result.target_verified); XCTAssertTrue(result.reset_allowed)
        XCTAssertTrue(result.isp_exit_completed)
        XCTAssertEqual(device.bytes, planned.target); XCTAssertEqual(device.erases.last, firmwareHeaderSector)
        XCTAssertEqual(device.fullStarts, 6) // backup ×2, preflight ×2, final ×2
    }
    func testChangedLiveSourceRejectsBeforeErase() throws {
        let device = MemoryDevice(source()); let planned = try plan(device); device.bytes[0x1f0000] ^= 1
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: try archive(), recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertFalse(result.completed); XCTAssertFalse(result.reset_allowed); XCTAssertTrue(device.erases.isEmpty)
    }
    func testProgrammingFailureRestoresSourceAndVerifiesTwice() throws {
        let device = MemoryDevice(source()); let original = device.bytes; let planned = try plan(device)
        device.failNextProgram = true
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: try archive(), recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertFalse(result.completed); XCTAssertTrue(result.rollback_attempted); XCTAssertTrue(result.rollback_verified)
        XCTAssertTrue(result.reset_allowed); XCTAssertEqual(device.bytes, original)
    }
    func testUnresolvedWriteKeepsPersistentRecoveryGate() throws {
        let device = MemoryDevice(source()); let planned = try plan(device); device.failEveryProgram = true
        let recovery = temporary.appendingPathComponent("recovery")
        let exitsBefore = device.exits
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: try archive(), recoveryRoot: recovery)
        XCTAssertFalse(result.reset_allowed); XCTAssertFalse(result.rollback_verified)
        XCTAssertFalse(result.isp_exit_completed); XCTAssertEqual(device.exits, exitsBefore)
        let erases = device.erases.count
        let second = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: try archive(), recoveryRoot: recovery)
        XCTAssertFalse(second.mutation_started); XCTAssertEqual(device.erases.count, erases)
        XCTAssertTrue(second.error?.contains("Unresolved") == true)
    }
    func testNoOpFinalFailureWithholdsReset() throws {
        let device = MemoryDevice(source()); let verified = try backup(device)
        let planned = try WritePlan(backup: verified, input: FirmwareImage.validate(device.bytes),restoreBackup: verified,policy: .simulationOnly)
        device.fullStarts = 0; device.corruptFinal = true
        let exitsBefore = device.exits
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: try archive(), recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertFalse(result.completed); XCTAssertFalse(result.reset_allowed); XCTAssertTrue(device.erases.isEmpty)
        XCTAssertEqual(device.exits, exitsBefore)
    }
    func testDifferentUnitAndFullDumpTailRejected() throws {
        let device = MemoryDevice(source()); let verified = try backup(device)
        var altered = source(); altered[flashCapacity - 1] ^= 1
        XCTAssertThrowsError(try WritePlan(backup: verified, input: FirmwareImage.validate(altered),restoreBackup: verified,policy: .simulationOnly))
        let other = MonitorIdentity(manufacturer: "SAM", product_code: 0x0c4e, edid_serial: "OTHER", edid_sha256: device.selected.edid_sha256, transport_locator: "mock:one")
        let planned = try WritePlan(backup: verified, input: FirmwareImage.validate(syntheticPackage(version: "1005.0")),policy: .simulationOnly)
        let result = FlashEngine.execute(device: device, selected: other, plan: planned, archive: try archive(), recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertFalse(result.mutation_started); XCTAssertTrue(device.erases.isEmpty)
    }
    func testCancellationBeforeErase() async throws {
        let device = MemoryDevice(source()); let planned = try plan(device); let evidence = try archive()
        let recovery = temporary.appendingPathComponent("recovery")
        let task = Task.detached { () -> ExecutionResult in
            withUnsafeCurrentTask { $0?.cancel() }
            return FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: evidence, recoveryRoot: recovery)
        }
        let result = await task.value
        XCTAssertFalse(result.mutation_started); XCTAssertFalse(result.reset_allowed); XCTAssertTrue(device.erases.isEmpty)
    }
    func testCancellationAfterSectorRestoresThenExits() async throws {
        let device = MemoryDevice(source()); let original = device.bytes; let planned = try plan(device)
        let evidence = try archive(); let recovery = temporary.appendingPathComponent("recovery")
        let task = Task.detached {
            FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: evidence, recoveryRoot: recovery) { update in
                if update.stage == "Verified sectors" { withUnsafeCurrentTask { $0?.cancel() } }
            }
        }
        let result = await task.value
        XCTAssertFalse(result.completed); XCTAssertTrue(result.rollback_verified); XCTAssertTrue(result.isp_exit_completed)
        XCTAssertEqual(device.bytes, original)
    }
    func testExitFailureDoesNotRestartDestructiveRollback() throws {
        let device = MemoryDevice(source()); let planned = try plan(device); device.failExit = true
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: try archive(), recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertFalse(result.completed); XCTAssertTrue(result.target_verified); XCTAssertFalse(result.rollback_attempted)
        XCTAssertFalse(result.isp_exit_completed); XCTAssertEqual(device.erases, planned.changedSectors)
    }
    func testLegacyAndJVProfilesUseExactRawPackets() throws {
        for legacy in [false,true] {
            let bus = ScriptBus(legacy: legacy); let device = MStarISPDevice(bus: bus)
            let profile = try device.profile()
            XCTAssertEqual(profile.page_size,256)
            XCTAssertEqual(profile.status_before, legacy ? "0000" : "000260")
            XCTAssertEqual(bus.writes.first,Data("MSTAR".utf8))
            XCTAssertEqual(bus.writes.last,Data([0x12]))
            XCTAssertEqual(bus.writes.filter { $0 == Data([0x10,0x9f]) }.count,2)
            XCTAssertEqual(bus.writes.contains(Data([0x10,0x15])),!legacy)
            XCTAssertFalse(bus.writes.contains(Data([0x24])))
            _ = try device.identity()
            XCTAssertEqual(bus.identityReads,1,"No DDC identity query may be interleaved after ISP entry")
        }
    }
    func testPrimitiveRejectsEraseBeforeProfile() throws {
        let bus = ScriptBus(legacy: false); let device = MStarISPDevice(bus: bus)
        XCTAssertThrowsError(try device.eraseSector(address: 0x20000))
        XCTAssertThrowsError(try device.program(address: 0x20000,bytes: Data(repeating: 0,count: 32)))
        XCTAssertTrue(bus.writes.isEmpty)
    }
    func testProviderCapabilitiesAreExplicitAndUnavailableDriverFails() throws {
        let providers = TransportProviders.available
        XCTAssertEqual(Set(providers.map(\.id)).count, providers.count)
        XCTAssertFalse(IOAVAppleSiliconProvider().capabilities.qualifiedHardwareWrite)
        let intel = IntelFramebufferProvider()
        XCTAssertFalse(intel.capabilities.cachedEnumeration)
        XCTAssertFalse(intel.capabilities.experimentalReadOnlyISP)
        XCTAssertThrowsError(try intel.enumerateCached())
    }
    func testRawReadOnlyCandidateRejectsDestructivePacketsBeforeOpeningService() throws {
        let original = MemoryDevice(source()).selected
        let identity = MonitorIdentity(manufacturer: original.manufacturer,product_code: original.product_code,edid_serial: original.edid_serial,edid_sha256: original.edid_sha256,transport_locator: "ioav:\(UInt64.max)")
        // Registry ID is deliberately nonexistent. A destructive request must fail at
        // the pure allowlist before attempting any native open or hardware call.
        let bus = IOAVRawBus(target: identity, registryID: UInt64.max, routePath: "/dcpext2/DCPEXT2Endpoint9/test",experimentalReadOnlyOptIn: true)
        XCTAssertThrowsError(try bus.assertWritableQualified())
        for packet: [UInt8] in [[0x10,0x06],[0x10,0x20,0,0,0],[0x10,0x01,0],[0x10,0x02]+Array(repeating: 0,count: 35)] {
            XCTAssertThrowsError(try bus.write(address: 0x49,bytes: Data(packet))) { error in
                XCTAssertTrue(error.localizedDescription.contains("read-only"))
            }
        }
    }
    func testManifestUsesCommonTypesAndAll64SFDPBytes() throws {
        let device = MemoryDevice(source()); let verified = try backup(device)
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: verified.directory.appendingPathComponent("backup.json"))) as! [String: Any]
        let identity = json["identity"] as! [String: Any], flash = json["flash"] as! [String: Any]
        XCTAssertEqual(json["model"] as? String,"LU28E570DS/EN")
        XCTAssertTrue(identity["product_code"] is NSNumber)
        XCTAssertEqual(identity["ambiguous"] as? Bool,false)
        XCTAssertEqual(identity["ambiguity_reason"] as? String,"")
        XCTAssertEqual(flash["status_before"] as? String,"000260")
        XCTAssertEqual(flash["sfdp_bytes"] as? Int,64)
        XCTAssertEqual(flash["page_size"] as? Int,256)
        XCTAssertEqual(flash["programming_chunk_size"] as? Int,32)
        XCTAssertNil(flash["sfdpSnapshot"])
        let raw = try Data(contentsOf: verified.directory.appendingPathComponent("flash-sfdp.bin"))
        XCTAssertEqual(raw.count,64); XCTAssertEqual(sha256(raw),flash["sfdp_sha256"] as? String)
        if let path = ProcessInfo.processInfo.environment["MONITORFW_EXPORT_MOCK_DIR"] {
            let export = try EvidenceArchive(parent: URL(fileURLWithPath: path),label: "swift-synthetic-backup")
            try export.save("backup.json",value: verified.manifest)
            try export.save("flash-sfdp.bin",data: raw)
            for read in verified.manifest.reads { try export.save(read.file,data: verified.image) }
            print("MOCK_EXPORT=" + export.directory.path)
        }
        var corrupt = raw; corrupt[63] ^= 1
        try corrupt.write(to: verified.directory.appendingPathComponent("flash-sfdp.bin"))
        XCTAssertThrowsError(try BackupLoader.load(manifestURL: verified.directory.appendingPathComponent("backup.json")))
    }
    func testDurableMarkerPrecedesEraseAndRemovalFollowsVerifiedExit() throws {
        let device = MemoryDevice(source()); let planned = try plan(device); let recorder = EventRecorder()
        let evidence = try EvidenceArchive(parent: temporary.appendingPathComponent("new/nested"), label: "durability") { recorder.append($0) }
        device.beforeErase = { recorder.append("erase") }; device.afterExit = { recorder.append("exit") }
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: evidence, recoveryRoot: temporary.appendingPathComponent("new/recovery"))
        XCTAssertTrue(result.completed)
        let events = recorder.snapshot
        XCTAssertLessThan(events.firstIndex(of: "archive-directory-durable")!,events.firstIndex(of: "erase")!)
        XCTAssertLessThan(events.firstIndex(of: "marker-data-synced")!,events.firstIndex(of: "marker-entry-durable")!)
        XCTAssertLessThan(events.firstIndex(of: "marker-entry-durable")!,events.firstIndex(of: "erase")!)
        XCTAssertLessThan(events.firstIndex(of: "exit")!,events.firstIndex(of: "marker-removal-durable")!)
    }
    func testMarkerDirectorySyncFailureRejectsBeforeErase() throws {
        let device = MemoryDevice(source()); let planned = try plan(device)
        let evidence = try EvidenceArchive(parent: temporary, label: "injected-sync-failure") { event in
            if event == "before-marker-directory-sync" { throw FirmwareError.rejected("Injected directory sync failure") }
        }
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: evidence, recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertFalse(result.completed); XCTAssertFalse(result.mutation_started)
        XCTAssertTrue(device.erases.isEmpty)
        XCTAssertTrue(result.error?.contains("directory sync failure") == true)
    }
    func testExplicitRouteIdentificationUsesFixedQueryAndDoubleBoundedEDID() throws {
        let channel = IdentificationChannel(edid: syntheticEDID(extensions: true))
        let route = CachedRoute(registry_id: 123,path: "/dcpext2/DCPEXT2Endpoint9/dispext2")
        let result = try RouteIdentifier.identify(channel: channel,route: route,displays: [],operatorConfirmedAwake: true)
        XCTAssertEqual(channel.writes,[Data([0x82,0xc3,0,0x7e])])
        XCTAssertEqual(channel.reads.filter { $0.0 == 0x50 }.map { $0.1 },[0,128,0,128])
        XCTAssertEqual(result.target.identity.manufacturer,"SAM")
        XCTAssertEqual(result.target.identity.edid_serial,"TESTROUTE001")
        XCTAssertEqual(result.target.identity.product_code,0x0c4e)
        XCTAssertEqual(result.target.registryID,123)
        XCTAssertFalse(result.target.identity.ambiguous)
        XCTAssertTrue(result.target.active)
        XCTAssertNil(result.matchedCGDisplay)
    }
    func testIdentificationRejectsNoAwakeConsentAndInvalidOrUnstableReplies() throws {
        let route = CachedRoute(registry_id: 123,path: "/dcpext2/test")
        let untouched = IdentificationChannel()
        XCTAssertThrowsError(try RouteIdentifier.identify(channel: untouched,route: route,displays: [],operatorConfirmedAwake: false))
        XCTAssertTrue(untouched.writes.isEmpty); XCTAssertTrue(untouched.reads.isEmpty)
        let corrupt = IdentificationChannel(); corrupt.frame[22] ^= 1
        XCTAssertThrowsError(try RouteIdentifier.identify(channel: corrupt,route: route,displays: [],operatorConfirmedAwake: true))
        XCTAssertEqual(corrupt.reads.count,1)
        let unstable = IdentificationChannel(); unstable.unstable = true
        XCTAssertThrowsError(try RouteIdentifier.identify(channel: unstable,route: route,displays: [],operatorConfirmedAwake: true))
    }
    func testGenericIdentityRemainsAmbiguous() throws {
        let result = try RouteIdentifier.identify(channel: IdentificationChannel(edid: syntheticEDID(serial: "0000000000")),route: CachedRoute(registry_id: 123,path: "/dcpext2/test"),displays: [],operatorConfirmedAwake: true)
        XCTAssertTrue(result.target.identity.ambiguous)
        XCTAssertFalse(result.target.identity.matchesUnit(result.target.identity))
        let device = try IOAVAppleSiliconProvider().makeDevice(target: result.target,access: .experimentalReadOnly)
        XCTAssertThrowsError(try device.assertQualified())
        XCTAssertTrue(try RouteIdentifier.parseEDID(syntheticEDID(serial: "H1AK500000")).generic)
    }
    func testPostExitIdentityFailureWithholdsCompletionAndRetainsRecovery() throws {
        let device = MemoryDevice(source()); let planned = try plan(device)
        let other = MonitorIdentity(manufacturer: "SAM",product_code: 0x0c4e,edid_serial: "OTHER-UNIT",edid_sha256: device.selected.edid_sha256,transport_locator: device.selected.transport_locator)
        device.afterExit = { device.identityOverride = other }
        let recovery = temporary.appendingPathComponent("recovery")
        let evidence = try archive()
        let result = FlashEngine.execute(device: device, selected: device.selected, plan: planned, archive: evidence, recoveryRoot: recovery)
        XCTAssertTrue(result.target_verified); XCTAssertTrue(result.isp_exit_completed)
        XCTAssertFalse(result.completed); XCTAssertFalse(result.reset_allowed); XCTAssertFalse(result.rollback_attempted)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: recovery.path).contains { $0.hasSuffix(".recovery-required") })
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: evidence.directory.path).filter { $0.hasPrefix("post-exit-identity-") }.count,1)
    }
    func testUniqueCGMatchAndSleepingTargetCannotBeOverriddenByAwakeConsent() throws {
        let route = CachedRoute(registry_id: 123,path: "/dcpext2/test")
        let identity = MonitorIdentity(manufacturer: "SAM",product_code: 0x0c4e,edid_serial: "305419896",edid_sha256: "",transport_locator: "coregraphics:7",ambiguous: true)
        let display = CachedDisplay(id: 7,name: "Cached",identity: identity,active: false,asleep: true,identityWarning: "cached",registryID: nil,cachedManufacturerCode: 0x4c2d,cachedNumericSerial: 0x12345678,routePath: nil)
        let result = try RouteIdentifier.identify(channel: IdentificationChannel(),route: route,displays: [display],operatorConfirmedAwake: true)
        XCTAssertEqual(result.matchedCGDisplay,7); XCTAssertFalse(result.target.active); XCTAssertTrue(result.target.asleep)
        XCTAssertThrowsError(try IOAVAppleSiliconProvider().makeDevice(target: result.target,access: .experimentalReadOnly))
        let duplicate = try RouteIdentifier.identify(channel: IdentificationChannel(),route: route,displays: [display,display],operatorConfirmedAwake: true)
        XCTAssertTrue(duplicate.target.identity.ambiguous)
    }
    func testValidRepackedAppMutationIsInspectOnlyBeforeStaging() throws {
        let candidate = try Data(contentsOf: candidateURL())
        let device = MemoryDevice(candidate + Data(repeating: 0xa5,count: flashCapacity-candidate.count))
        let verified = try backup(device)
        let modified = try FirmwareImage.validate(modifiedAppWithValidCRCs(candidate))
        XCTAssertFalse(FirmwareWritePolicy.approvedPackageHashes.contains(modified.report.package_sha256))
        XCTAssertThrowsError(try WritePlan(backup: verified,input: modified)) { error in
            XCTAssertTrue(error.localizedDescription.contains("Selected package is not an approved"))
        }
        XCTAssertTrue(device.erases.isEmpty)
    }
    func testOwnUnitFullRestoreRestoresShorterPackageGap() throws {
        let device = MemoryDevice(source()); let original = try backup(device)
        let input = try FirmwareImage.validate(syntheticPackage(version: "1005.0",bodyCount: 8192))
        let forward = try WritePlan(backup: original,input: input,policy: .simulationOnly)
        let result = FlashEngine.execute(device: device,selected: device.selected,plan: forward,archive: try archive(),recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertTrue(result.completed)
        let current = try backup(device)
        let oldImage = try FirmwareImage.validate(original.image)
        XCTAssertThrowsError(try WritePlan(backup: current,input: oldImage,policy: .simulationOnly))
        let restore = try WritePlan(backup: current,input: oldImage,restoreBackup: original,policy: .simulationOnly)
        XCTAssertNotEqual(current.image[oldImage.package.count..<input.package.count],original.image[oldImage.package.count..<input.package.count])
        let restored = FlashEngine.execute(device: device,selected: device.selected,plan: restore,archive: try archive(),recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertTrue(restored.completed); XCTAssertEqual(device.bytes,original.image)
    }
    func testPostExitTransientFramingThenValidIdentityIsRetriedWithEvidence() throws {
        let device = MemoryDevice(source()); let planned = try plan(device)
        device.afterExit = { device.transientIdentityFailures = 2 }
        let evidence = try archive()
        let result = FlashEngine.execute(device: device,selected: device.selected,plan: planned,archive: evidence,recoveryRoot: temporary.appendingPathComponent("recovery"),retryPolicy: .init(maximumAttempts: 3,delaySeconds: 0))
        XCTAssertTrue(result.completed)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: evidence.directory.path).filter { $0.hasPrefix("post-exit-identity-") }.count,3)
    }
    func testPostExitPersistentInvalidReplyStopsAtBoundAndKeepsMarker() throws {
        let device = MemoryDevice(source()); let planned = try plan(device)
        device.afterExit = { device.transientIdentityFailures = 100 }
        let evidence = try archive(), recovery = temporary.appendingPathComponent("recovery")
        let result = FlashEngine.execute(device: device,selected: device.selected,plan: planned,archive: evidence,recoveryRoot: recovery,retryPolicy: .init(maximumAttempts: 3,delaySeconds: 0))
        XCTAssertFalse(result.completed); XCTAssertFalse(result.reset_allowed); XCTAssertFalse(result.rollback_attempted)
        XCTAssertEqual(device.transientIdentityFailures,97)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: evidence.directory.path).filter { $0.hasPrefix("post-exit-identity-") }.count,3)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: recovery.path).contains { $0.hasSuffix(".recovery-required") })
    }
    func testBackupLoaderRejectsMetadataAndOversizedRead() throws {
        let device = MemoryDevice(source()); let verified = try backup(device)
        let original = try JSONSerialization.jsonObject(with: Data(contentsOf: verified.directory.appendingPathComponent("backup.json"))) as! [String: Any]
        for (key,value) in [("model",""),("created_utc","invalid")] {
            var modified = original; modified[key] = value
            let path = verified.directory.appendingPathComponent("bad-" + key + ".json")
            try JSONSerialization.data(withJSONObject: modified).write(to: path)
            XCTAssertThrowsError(try BackupLoader.load(manifestURL: path))
        }
        var coding = original; coding["coding_eeprom"] = ["available": true,"reason": "Not supported"]
        let path = verified.directory.appendingPathComponent("bad-coding.json")
        try JSONSerialization.data(withJSONObject: coding).write(to: path)
        XCTAssertThrowsError(try BackupLoader.load(manifestURL: path))
        let handle = try FileHandle(forWritingTo: verified.directory.appendingPathComponent(verified.manifest.reads[0].file))
        try handle.truncate(atOffset: UInt64(flashCapacity + 1)); try handle.close()
        XCTAssertThrowsError(try BackupLoader.load(manifestURL: verified.directory.appendingPathComponent("backup.json"))) { error in
            XCTAssertTrue(error.localizedDescription.contains("file size before reading"))
        }
    }
    func testSimulationPlanCannotReachAnyHardwareMethod() throws {
        let mock = MemoryDevice(source()); let planned = try plan(mock); let hardware = HardwareSentinel()
        let result = FlashEngine.execute(device: hardware,selected: mock.selected,plan: planned,archive: try archive(),recoveryRoot: temporary.appendingPathComponent("recovery"))
        XCTAssertFalse(result.mutation_started); XCTAssertEqual(hardware.calls,0)
        XCTAssertTrue(result.error?.contains("Simulation-only plans") == true)
    }
    func testRecoveryMarkerFollowsUnitAcrossProductEDIDAndRouteChanges() throws {
        let first = MemoryDevice(source()).selected, evidence = try archive()
        let recovery = temporary.appendingPathComponent("recovery")
        var lease: DeviceLease? = try DeviceLease(identity: first,root: recovery,archive: evidence)
        try lease!.markMutation(); lease = nil
        let otherInput = MonitorIdentity(manufacturer: first.manufacturer,product_code: 0x0c4d,edid_serial: first.edid_serial.lowercased(),edid_sha256: sha256(Data("different-input-edid".utf8)),transport_locator: "mock:other-input")
        XCTAssertThrowsError(try DeviceLease(identity: otherInput,root: recovery,archive: evidence)) { error in
            XCTAssertTrue(error.localizedDescription.contains("Unresolved earlier write"))
        }
    }
    func testAmbiguousBundleLoadsForReadOnlyButCannotCreateWritePlan() throws {
        let device = MemoryDevice(source()); let original = try backup(device)
        var json = try JSONSerialization.jsonObject(with: Data(contentsOf: original.directory.appendingPathComponent("backup.json"))) as! [String: Any]
        var identity = json["identity"] as! [String: Any]
        identity["ambiguous"] = true; identity["ambiguity_reason"] = "Synthetic generic identity for interoperability testing"
        json["identity"] = identity
        let path = original.directory.appendingPathComponent("ambiguous-backup.json")
        try JSONSerialization.data(withJSONObject: json).write(to: path)
        let loaded = try BackupLoader.load(manifestURL: path)
        XCTAssertTrue(loaded.manifest.identity.ambiguous); XCTAssertEqual(loaded.image,original.image)
        XCTAssertThrowsError(try WritePlan(backup: loaded,input: FirmwareImage.validate(syntheticPackage(version: "1005.0")),policy: .simulationOnly)) { error in
            XCTAssertTrue(error.localizedDescription.contains("unambiguous"))
        }
        try FlashEngine.verify(device: device,selected: device.selected,expected: loaded.image,archive: archive())
        XCTAssertTrue(device.erases.isEmpty)
    }
    func testImportAcceptsSafeDistinctSFDPFilename() throws {
        let original = try backup(MemoryDevice(source()))
        var json = try JSONSerialization.jsonObject(with: Data(contentsOf: original.directory.appendingPathComponent("backup.json"))) as! [String: Any]
        var flash = json["flash"] as! [String: Any]; flash["sfdp_file"] = "portable-sfdp64.bin"; json["flash"] = flash
        try FileManager.default.moveItem(at: original.directory.appendingPathComponent("flash-sfdp.bin"),to: original.directory.appendingPathComponent("portable-sfdp64.bin"))
        let path = original.directory.appendingPathComponent("portable-backup.json")
        try JSONSerialization.data(withJSONObject: json).write(to: path)
        let loaded = try BackupLoader.load(manifestURL: path)
        XCTAssertEqual(loaded.sfdp,original.sfdp); XCTAssertEqual(loaded.image,original.image)
    }
}
