import Foundation
import CryptoKit

public enum FirmwareError: Error, LocalizedError, Sendable {
    case rejected(String)
    public var errorDescription: String? { switch self { case .rejected(let value): value } }
}
func require(_ condition: Bool, _ message: String) throws {
    if !condition { throw FirmwareError.rejected(message) }
}
public func sha256(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
public func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }
public let flashCapacity = 0x200000
public let flashSectorSize = 0x1000
public let firmwareHeaderSector = 0x10000

public struct MonitorIdentity: Codable, Equatable, Sendable {
    public let manufacturer: String
    public let product_code: UInt32
    public let edid_serial: String
    public let edid_sha256: String
    public let transport_locator: String
    public let ambiguous: Bool
    public let ambiguity_reason: String
    public init(manufacturer: String, product_code: UInt32, edid_serial: String, edid_sha256: String, transport_locator: String,
                ambiguous: Bool = false, ambiguity_reason: String = "") {
        self.manufacturer = manufacturer; self.product_code = product_code; self.edid_serial = edid_serial
        self.edid_sha256 = edid_sha256; self.transport_locator = transport_locator
        self.ambiguous = ambiguous; self.ambiguity_reason = ambiguity_reason
    }
    public func matchesUnit(_ other: Self) -> Bool {
        !ambiguous && !other.ambiguous && product_code <= 65535 &&
        manufacturer.range(of: "^[A-Z]{3}$", options: .regularExpression) != nil &&
        edid_sha256.range(of: "^[0-9a-f]{64}$", options: .regularExpression) != nil &&
        !transport_locator.isEmpty && !edid_serial.isEmpty && edid_serial != "0" &&
        manufacturer == other.manufacturer && product_code == other.product_code &&
        edid_serial == other.edid_serial && edid_sha256 == other.edid_sha256
    }
    public func validateStructure() throws {
        try require(manufacturer.range(of: "^[A-Z]{3}$",options: .regularExpression) != nil && product_code <= 65535,
                    "Invalid manufacturer/product fields in backup identity.")
        try require(edid_sha256.range(of: "^[0-9a-f]{64}$",options: .regularExpression) != nil && !transport_locator.isEmpty,
                    "Invalid EDID hash or transport locator in backup identity.")
        // Ambiguity is valid evidence. Hardware eligibility is a separate decision.
    }
}
public struct FlashProfile: Codable, Equatable, Sendable {
    public let jedec: String
    public let sfdp_sha256: String
    public let sfdp_bytes: Int
    public let sfdp_file: String
    public let capacity: Int
    public let sector_size: Int
    public let page_size: Int
    public let programming_chunk_size: Int
    public let status_before: String
    public let status_after: String
    public var sfdpSnapshot: Data? = nil
    enum CodingKeys: String, CodingKey {
        case jedec, sfdp_sha256, sfdp_bytes, sfdp_file, capacity, sector_size, page_size, programming_chunk_size, status_before, status_after
    }
    public var beforeBytes: [UInt8] { Self.bytes(status_before) }
    public var afterBytes: [UInt8] { Self.bytes(status_after) }
    private static func bytes(_ text: String) -> [UInt8] {
        let characters = Array(text)
        guard characters.count % 2 == 0 else { return [] }
        var result: [UInt8] = []
        for offset in stride(from: 0, to: characters.count, by: 2) {
            guard let value = UInt8(String(characters[offset...offset+1]), radix: 16) else { return [] }
            result.append(value)
        }
        return result
    }
    public init(jedec: String, sfdp_sha256: String, capacity: Int = flashCapacity,
                sector_size: Int = flashSectorSize, page_size: Int = 256, status_before: [UInt8], status_after: [UInt8], sfdpSnapshot: Data? = nil) {
        self.jedec = jedec; self.sfdp_sha256 = sfdp_sha256; self.capacity = capacity
        self.sector_size = sector_size; self.page_size = page_size
        self.status_before = hex(Data(status_before)); self.status_after = hex(Data(status_after)); programming_chunk_size = 32
        sfdp_bytes = 64; sfdp_file = "flash-sfdp.bin"; self.sfdpSnapshot = sfdpSnapshot
    }
    public func validate(sfdp: Data? = nil) throws {
        try require(jedec == "ef4015", "Unknown flash profile. Only the reviewed W25Q16 profile is implemented.")
        guard let raw = sfdp ?? sfdpSnapshot else { throw FirmwareError.rejected("The complete 64-byte SFDP evidence file is required.") }
        try require(sfdp_bytes == 64 && raw.count == 64 && sha256(raw) == sfdp_sha256 &&
                    sfdp_file.range(of: "^[A-Za-z0-9][A-Za-z0-9._-]*$",options: .regularExpression) != nil, "SFDP64 evidence does not match its manifest.")
        let expected: [UInt8]
        if raw.prefix(16) == Self.reviewedSFDP { expected = [0, 2, 0x60] }
        else if raw.prefix(16) == Self.legacySFDP { expected = [0, 0] }
        else { throw FirmwareError.rejected("Unknown SFDP profile; refusing flash operations.") }
        try require(capacity == flashCapacity && sector_size == flashSectorSize && page_size == 256 && programming_chunk_size == 32, "Unsupported physical flash geometry. Programming uses 32-byte chunks within 256-byte pages.")
        try require(status_before == hex(Data(expected)) && status_after == status_before, "Status/protection registers differ from the reviewed profile.")
    }
    // The reviewed 16-byte SFDP header, not a claim that the complete SFDP table was captured.
    public static let reviewedSFDP = Data([0x53,0x46,0x44,0x50,5,1,0,0xff,0,5,1,16,0x80,0,0,0xff])
    public static let legacySFDP = Data([0x53,0x46,0x44,0x50,0,1,0,0xff,0,0,1,9,0x80,0,0,0xff])
}
public struct ReadEvidence: Codable, Sendable {
    public let file: String
    public let bytes: Int
    public let sha256: String
    public init(file: String, bytes: Int, sha256: String) { self.file = file; self.bytes = bytes; self.sha256 = sha256 }
}
public struct CodingEEPROM: Codable, Sendable {
    public let available: Bool
    public let reason: String
    public static let unsupported = Self(available: false, reason: "No qualified read-only coding EEPROM path. SPI backup is not a coding EEPROM backup.")
}
public struct BackupManifest: Codable, Sendable {
    public let schema: String
    public let image_kind: String
    public let model: String
    public let identity: MonitorIdentity
    public let flash: FlashProfile
    public let reads: [ReadEvidence]
    public let reads_equal: Bool
    public let created_utc: String
    public let coding_eeprom: CodingEEPROM
    public let simulated: Bool?
    public init(model: String, identity: MonitorIdentity, flash: FlashProfile, reads: [ReadEvidence], simulated: Bool) {
        schema = "monitorfw.backup.v1"; image_kind = "spi_full"; self.model = model; self.identity = identity
        self.flash = flash; self.reads = reads; reads_equal = true
        created_utc = ISO8601DateFormatter().string(from: Date()); coding_eeprom = .unsupported; self.simulated = simulated
    }
}
public struct VerifiedBackup: Sendable {
    public let manifest: BackupManifest
    public let image: Data
    public let directory: URL
    public let sfdp: Data
    init(manifest: BackupManifest, image: Data, directory: URL, sfdp: Data) { self.manifest = manifest; self.image = image; self.directory = directory; self.sfdp = sfdp }
}
public struct ProgressUpdate: Sendable {
    public let stage: String
    public let completed: Int
    public let total: Int
    public init(_ stage: String, _ completed: Int = 0, _ total: Int = 1) { self.stage = stage; self.completed = completed; self.total = total }
}
public typealias ProgressHandler = @Sendable (ProgressUpdate) -> Void

/// Only this module can mint permission after full-image and status verification.
/// It is not Codable, and cannot be loaded from an editable manifest.
public struct VerifiedImagePermit: Sendable {
    let imageSHA256: String
    let status: [UInt8]
    init(image: Data, status: [UInt8]) { imageSHA256 = sha256(image); self.status = status }
}

public protocol FlashDevice: AnyObject, Sendable {
    var isSimulated: Bool { get }
    /// Must reject unsupported transport before entering ISP or sending any bus command.
    func assertQualified() throws
    func assertWritableQualified() throws
    func identity() throws -> MonitorIdentity
    func profile() throws -> FlashProfile
    func status() throws -> [UInt8]
    func read(address: Int, count: Int) throws -> Data
    func eraseSector(address: Int) throws
    func program(address: Int, bytes: Data) throws
    func writeDisable() throws
    func exitISP(permit: VerifiedImagePermit) throws
}
