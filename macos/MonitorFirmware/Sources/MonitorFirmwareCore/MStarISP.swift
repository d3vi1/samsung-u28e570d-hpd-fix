import Foundation
import NativeSupport

/// A raw I2C transaction has no implicit register/subaddress byte. Implementations
/// must preserve transaction boundaries and support a one-byte write and a raw read.
public protocol RawI2CBus: AnyObject, Sendable {
    var isSimulated: Bool { get }
    func assertQualified() throws
    func assertWritableQualified() throws
    func selectedIdentity() throws -> MonitorIdentity
    func write(address: UInt8, bytes: Data) throws
    func read(address: UInt8, count: Int) throws -> Data
}

/// Complete MStar command/state implementation. A qualifying transport is mandatory
/// before the first MSTAR packet. A serial operation owns one instance exclusively.
public final class MStarISPDevice: FlashDevice, @unchecked Sendable {
    private let bus: any RawI2CBus
    private var entered = false
    private var statusCount: Int?
    private var profileValidated = false
    private var boundIdentity: MonitorIdentity?
    public var isSimulated: Bool { bus.isSimulated }
    public init(bus: any RawI2CBus) { self.bus = bus }
    public func assertQualified() throws { try bus.assertQualified() }
    public func assertWritableQualified() throws { try bus.assertWritableQualified() }
    public func identity() throws -> MonitorIdentity {
        try bus.assertQualified()
        // DDC firmware/EDID identification belongs before ISP, never interleaved
        // with the raw ISP state machine. Preserve that handle-bound identity.
        if entered {
            guard let boundIdentity else { throw FirmwareError.rejected("ISP session has no bound identity; DDC is withheld.") }
            return boundIdentity
        }
        let value = try bus.selectedIdentity(); boundIdentity = value; return value
    }
    private func enter() throws {
        try bus.assertQualified()
        if !entered {
            if boundIdentity == nil { boundIdentity = try bus.selectedIdentity() }
            // Mark entry attempted before sending: a throwing call may have side effects.
            entered = true
            try bus.write(address: 0x49, bytes: Data("MSTAR".utf8))
            Thread.sleep(forTimeInterval: 0.1)
        }
    }
    private func spiRead(_ command: [UInt8], count: Int) throws -> Data {
        try enter()
        try require([0x9f,0x05,0x35,0x15,0x03,0x5a].contains(command.first ?? 0) && count > 0 && count <= 256, "Unsupported SPI read.")
        do {
            try bus.write(address: 0x49, bytes: Data([0x10] + command))
            try bus.write(address: 0x49, bytes: Data([0x11]))
            let data = try bus.read(address: 0x49, count: count)
            try bus.write(address: 0x49, bytes: Data([0x12]))
            try require(data.count == count, "Short raw ISP read.")
            return data
        } catch { try? bus.write(address: 0x49, bytes: Data([0x12])); throw error }
    }
    private func spiWrite(_ command: [UInt8]) throws {
        try enter()
        let op = command.first ?? 0
        try require(((op == 0x06 || op == 0x04) && command.count == 1) ||
                    (op == 0x20 && command.count == 4) || (op == 0x02 && command.count == 36), "Unsupported SPI write.")
        do {
            try bus.write(address: 0x49, bytes: Data([0x10] + command))
            try bus.write(address: 0x49, bytes: Data([0x12]))
        } catch { try? bus.write(address: 0x49, bytes: Data([0x12])); throw error }
    }
    private func waitReady() throws -> UInt8 {
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while true {
            let value = try spiRead([0x05], count: 1)[0]
            if value & 1 == 0 { return value }
            try require(ProcessInfo.processInfo.systemUptime < deadline, "Flash remained busy for five seconds.")
            Thread.sleep(forTimeInterval: 0.005)
        }
    }
    private func enableWrite() throws {
        let status = try waitReady()
        try require(status & 0x1c == 0, "Flash block protection is enabled; status writes are forbidden.")
        try spiWrite([0x06])
        try require(try spiRead([0x05], count: 1)[0] & 3 == 2, "Write-enable latch did not set.")
    }
    public func profile() throws -> FlashProfile {
        profileValidated = false
        let first = try spiRead([0x9f], count: 3)
        try require(try spiRead([0x9f], count: 3) == first && hex(first) == "ef4015", "Unknown or unstable flash JEDEC ID.")
        let sfdp = try spiRead([0x5a,0,0,0,0], count: 64)
        try require(try spiRead([0x5a,0,0,0,0], count: 64) == sfdp, "SFDP64 reads are unstable.")
        if sfdp.prefix(16) == FlashProfile.reviewedSFDP { statusCount = 3 }
        else if sfdp.prefix(16) == FlashProfile.legacySFDP { statusCount = 2 }
        else { throw FirmwareError.rejected("Unknown SFDP; no profile-specific status read or destructive command is permitted.") }
        let before = try status()
        let profile = FlashProfile(jedec: hex(first), sfdp_sha256: sha256(sfdp), status_before: before, status_after: before, sfdpSnapshot: sfdp)
        try profile.validate(); profileValidated = true; return profile
    }
    public func status() throws -> [UInt8] {
        try require(statusCount != nil, "Identify the SFDP profile before reading profile-specific status registers.")
        var result = [try waitReady(), try spiRead([0x35], count: 1)[0]]
        if statusCount == 3 { result.append(try spiRead([0x15], count: 1)[0]) }
        return result
    }
    public func read(address: Int, count: Int) throws -> Data {
        try require(address >= 0 && count > 0 && count <= 256 && address <= flashCapacity - count, "SPI read bounds exceeded.")
        return try spiRead([0x03,UInt8((address >> 16) & 255),UInt8((address >> 8) & 255),UInt8(address & 255)], count: count)
    }
    public func eraseSector(address: Int) throws {
        try bus.assertWritableQualified()
        try require(profileValidated, "Flash profile has not been qualified; erase is forbidden.")
        try require(address >= 0 && address % flashSectorSize == 0 && address <= flashCapacity - flashSectorSize, "Invalid erase sector.")
        try enableWrite()
        try spiWrite([0x20,UInt8((address >> 16) & 255),UInt8((address >> 8) & 255),UInt8(address & 255)])
        try require(try waitReady() & 2 == 0, "Erase command was not accepted.")
    }
    public func program(address: Int, bytes: Data) throws {
        try bus.assertWritableQualified()
        try require(profileValidated, "Flash profile has not been qualified; programming is forbidden.")
        try require(bytes.count == 32 && address >= 0 && address % 32 == 0 && address <= flashCapacity - 32, "Invalid 32-byte program block.")
        try enableWrite()
        try spiWrite([0x02,UInt8((address >> 16) & 255),UInt8((address >> 8) & 255),UInt8(address & 255)] + bytes)
        try require(try waitReady() & 2 == 0, "Program command was not accepted.")
    }
    public func writeDisable() throws { try spiWrite([0x04]); try require(try waitReady() & 3 == 0, "Write-disable latch was not cleared.") }
    /// Explicit lifecycle action only after the caller has durably verified a full image/status.
    /// It is never sent by deinit, exception cleanup, or cancellation.
    public func exitISP(permit: VerifiedImagePermit) throws {
        try require(permit.imageSHA256.count == 64 && !permit.status.isEmpty, "Reset requires a verified-image permit.")
        try writeDisable()
        try require(try status() == permit.status, "Status changed before ISP exit; reset withheld.")
        try bus.write(address: 0x49, bytes: Data([0x24])); entered = false
        statusCount = nil
        profileValidated = false
        boundIdentity = nil
        if !isSimulated { Thread.sleep(forTimeInterval: 3) }
    }
}

/// Local-kernel static analysis identifies UINT32_MAX as a no-subaddress sentinel.
/// Remote DCP/wire behavior remains unqualified. Only explicit experimental reads
/// may use this candidate; no manifest or read-only checkbox enables flash writes.
public final class IOAVRawBus: RawI2CBus, @unchecked Sendable {
    public static let limitation = "Remote DCP raw ISP transport is not qualified. An explicit experimental read-only action may use the UINT32_MAX no-subaddress candidate. Hardware erase/program remain disabled."
    public let isSimulated = false
    public let target: MonitorIdentity
    private let experimentalReadOnlyOptIn: Bool
    private let registryID: UInt64?
    private let routePath: String?
    private var session: IOAVSession?
    public init(target: MonitorIdentity, registryID: UInt64? = nil, routePath: String? = nil, experimentalReadOnlyOptIn: Bool = false) {
        self.target = target; self.registryID = registryID; self.routePath = routePath; self.experimentalReadOnlyOptIn = experimentalReadOnlyOptIn
    }
    public func assertQualified() throws {
        try require(experimentalReadOnlyOptIn, Self.limitation + " Experimental read-only access is not enabled; no service opened.")
        try require(target.matchesUnit(target) && registryID != nil && registryID != 0 &&
                    target.transport_locator == "ioav:\(registryID!)" && routePath?.lowercased().contains("dcpext") == true,
                    "Experimental read requires an unambiguous cached EDID identity and exact IOAV service route.")
    }
    public func assertWritableQualified() throws {
        throw FirmwareError.rejected("IOAV hardware write qualification is not installed. Backup evidence and experimental read-only opt-in cannot authorize erase/program.")
    }
    public func selectedIdentity() throws -> MonitorIdentity {
        try assertQualified()
        let channel = try openIfNeeded()
        let result = try RouteIdentifier.identify(channel: channel,route: CachedRoute(registry_id: registryID!,path: routePath!),
            displays: CachedDisplays.list(),operatorConfirmedAwake: true)
        try require(result.target.active && !result.target.asleep && result.target.identity.matchesUnit(target), "Selected route identity/active state changed before ISP entry.")
        return result.target.identity
    }
    private func openIfNeeded() throws -> IOAVSession {
        try assertQualified()
        if let session { return session }
        let opened = try IOAVSession(registryID: registryID!)
        session = opened; return opened
    }
    public func write(address: UInt8, bytes: Data) throws {
        try assertQualified()
        try require(address == 0x49, "Only the selected monitor's MStar ISP address is permitted.")
        let b = [UInt8](bytes)
        let delimiter = b.count == 1 && [UInt8(0x11),0x12,0x24].contains(b[0])
        let readCommand = b.count >= 2 && b[0] == 0x10 && (
            ([UInt8(0x9f),0x05,0x35,0x15,0x04].contains(b[1]) && b.count == 2) ||
            (b[1] == 0x03 && b.count == 5) || (b[1] == 0x5a && b.count == 6))
        try require(bytes == Data("MSTAR".utf8) || delimiter || readCommand,
                    "Experimental read-only transport rejected a destructive or unknown command.")
        let opened = try openIfNeeded()
        try opened.write(chip: UInt32(address),offset: UInt32.max,bytes: bytes)
    }
    public func read(address: UInt8, count: Int) throws -> Data {
        try assertQualified()
        try require(address == 0x49 && count > 0 && count <= 256, "Raw ISP read is outside supported bounds.")
        let opened = try openIfNeeded()
        // IOAV discards the returned byte count. Random prefill makes unfilled tails
        // unlikely to compare equal across independent full reads; this is detection
        // evidence, not proof of the private API's short-read semantics.
        return try opened.read(chip: UInt32(address),offset: UInt32.max,count: count)
    }
}
