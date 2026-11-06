import Foundation
import Darwin

enum DurableFilesystem {
    static func syncDirectory(_ directory: URL) throws {
        let descriptor = Darwin.open(directory.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { Darwin.close(descriptor) }
        guard fsync(descriptor) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    }
    static func createDirectories(_ directory: URL) throws {
        var missing: [URL] = []
        var cursor = directory.standardizedFileURL
        var isDirectory: ObjCBool = false
        while !FileManager.default.fileExists(atPath: cursor.path, isDirectory: &isDirectory) {
            missing.append(cursor)
            let parent = cursor.deletingLastPathComponent()
            try require(parent.path != cursor.path, "Cannot find an existing archive ancestor.")
            cursor = parent
        }
        try require(isDirectory.boolValue, "Archive ancestor is not a directory.")
        for next in missing.reversed() {
            try FileManager.default.createDirectory(at: next, withIntermediateDirectories: false)
            try syncDirectory(next)
            try syncDirectory(next.deletingLastPathComponent())
        }
        // Also persist linkage to an already-existing requested directory. These
        // are OS fsync calls, not a guarantee about a cloud provider or power loss.
        try syncDirectory(directory)
        try syncDirectory(directory.deletingLastPathComponent())
    }
}

public final class EvidenceArchive: @unchecked Sendable {
    public let directory: URL
    private let logLock = NSLock()
    private var logIndex = 0
    private let durabilityObserver: (@Sendable (String) throws -> Void)?
    public init(parent: URL, label: String, durabilityObserver: (@Sendable (String) throws -> Void)? = nil) throws {
        self.durabilityObserver = durabilityObserver
        directory = parent.appendingPathComponent("\(label)-\(UUID().uuidString)", isDirectory: true)
        try DurableFilesystem.createDirectories(directory)
        try observe("archive-directory-durable")
    }
    func observe(_ event: String) throws { try durabilityObserver?(event) }
    public func save(_ name: String, data: Data) throws {
        try require(name == URL(fileURLWithPath: name).lastPathComponent && !name.isEmpty, "Archive filename must be a leaf name.")
        let path = directory.appendingPathComponent(name).path
        let fd = Darwin.open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { Darwin.close(fd) }
        try data.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                let count = Darwin.write(fd, raw.baseAddress!.advanced(by: offset), raw.count - offset)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
                offset += count
            }
        }
        guard fsync(fd) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        try observe("file-data-synced:" + name)
        try DurableFilesystem.syncDirectory(directory)
        try observe("file-entry-durable:" + name)
    }
    public func save<T: Encodable>(_ name: String, value: T) throws {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try save(name, data: encoder.encode(value))
    }
    public func log(_ message: String) throws {
        logLock.lock(); defer { logLock.unlock() }
        logIndex += 1
        try save(String(format: "event-%06d.txt", logIndex), data: Data("\(ISO8601DateFormatter().string(from: Date())) \(message)\n".utf8))
    }
}

/// A per-unit process lock plus persistent unresolved-write marker. Neither a manifest
/// nor an application restart clears the marker. The operator must review recovery evidence.
public final class DeviceLease {
    private var descriptor: Int32 = -1
    private let marker: URL
    private let archive: EvidenceArchive
    public init(identity: MonitorIdentity, root: URL, archive: EvidenceArchive) throws {
        self.archive = archive
        try DurableFilesystem.createDirectories(root)
        try archive.observe("recovery-directory-durable")
        // Recovery belongs to the physical unit, not its current port's EDID.
        // Product/EDID hash/route can change across inputs and must not create a
        // fresh namespace that bypasses an unresolved operation on the same unit.
        let unit = identity.manufacturer.uppercased() + ":" + identity.edid_serial.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        let key = sha256(Data(unit.utf8))
        marker = root.appendingPathComponent(key + ".recovery-required")
        let path = root.appendingPathComponent(key + ".lock").path
        descriptor = Darwin.open(path, O_RDWR | O_CREAT | O_NOFOLLOW, S_IRUSR | S_IWUSR)
        guard descriptor >= 0 else { throw FirmwareError.rejected("Cannot open the per-monitor operation lock.") }
        if fsync(descriptor) != 0 { Darwin.close(descriptor); descriptor = -1; throw FirmwareError.rejected("Cannot synchronize the operation lock.") }
        do { try DurableFilesystem.syncDirectory(root) }
        catch { Darwin.close(descriptor); descriptor = -1; throw error }
        if flock(descriptor, LOCK_EX | LOCK_NB) != 0 { Darwin.close(descriptor); descriptor = -1; throw FirmwareError.rejected("Another operation owns this monitor.") }
        do { try require(!FileManager.default.fileExists(atPath: marker.path), "Unresolved earlier write: review \(marker.path). Reset and new operations are withheld.") }
        catch { Darwin.close(descriptor); descriptor = -1; throw error }
    }
    public func markMutation() throws {
        // This is written and synchronized before the first potentially destructive command.
        let fd = Darwin.open(marker.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR)
        guard fd >= 0 else { throw FirmwareError.rejected("Cannot persist recovery marker; no erase permitted.") }
        defer { Darwin.close(fd) }
        let data = Data((archive.directory.path + "\n").utf8)
        let written = data.withUnsafeBytes { Darwin.write(fd, $0.baseAddress, $0.count) }
        guard written == data.count && fsync(fd) == 0 else { throw FirmwareError.rejected("Cannot synchronize recovery marker; no erase permitted.") }
        try archive.observe("marker-data-synced")
        try archive.observe("before-marker-directory-sync")
        try DurableFilesystem.syncDirectory(marker.deletingLastPathComponent())
        try archive.observe("marker-entry-durable")
    }
    public func resolve() throws {
        try FileManager.default.removeItem(at: marker)
        try DurableFilesystem.syncDirectory(marker.deletingLastPathComponent())
        try archive.observe("marker-removal-durable")
    }
    deinit { if descriptor >= 0 { Darwin.close(descriptor) } }
}

public enum BackupLoader {
    private static func boundedRead(_ url: URL, expected: Int? = nil, maximum: Int) throws -> Data {
        let fd = Darwin.open(url.path,O_RDONLY | O_NOFOLLOW)
        guard fd >= 0 else { throw FirmwareError.rejected("Cannot open backup evidence file without following a symlink: " + url.lastPathComponent) }
        defer { Darwin.close(fd) }
        var info = stat()
        try require(fstat(fd,&info) == 0 && (info.st_mode & S_IFMT) == S_IFREG, "Backup evidence must be a regular file.")
        try require(info.st_size >= 0 && info.st_size <= maximum && (expected == nil || info.st_size == expected!), "Unexpected evidence file size before reading: " + url.lastPathComponent)
        var data = Data(); var buffer = [UInt8](repeating: 0,count: min(maximum + 1,65536))
        while true {
            let limit = min(buffer.count,maximum + 1 - data.count)
            try require(limit > 0,"Evidence grew beyond its allowed size.")
            let count = Darwin.read(fd,&buffer,limit)
            if count < 0 && errno == EINTR { continue }
            try require(count >= 0,"Backup evidence read failed.")
            if count == 0 { break }
            data.append(contentsOf: buffer.prefix(count))
            try require(data.count <= maximum,"Evidence grew beyond its allowed size.")
        }
        try require(expected == nil || data.count == expected!,"Evidence was truncated during the read.")
        return data
    }
    public static func load(manifestURL: URL) throws -> VerifiedBackup {
        let manifest = try JSONDecoder().decode(BackupManifest.self, from: boundedRead(manifestURL,maximum: 1024 * 1024))
        try require(manifest.schema == "monitorfw.backup.v1" && manifest.image_kind == "spi_full" && manifest.reads_equal && manifest.reads.count == 2, "Not a verified dual-read SPI backup manifest.")
        try require(!manifest.model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, "Backup model is empty.")
        let plainDate = ISO8601DateFormatter()
        let fractionalDate = ISO8601DateFormatter(); fractionalDate.formatOptions = [.withInternetDateTime,.withFractionalSeconds]
        try require(plainDate.date(from: manifest.created_utc) != nil || fractionalDate.date(from: manifest.created_utc) != nil, "Backup creation time is not an ISO 8601 timestamp.")
        try require(!manifest.coding_eeprom.available && !manifest.coding_eeprom.reason.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, "Backup claims unsupported coding EEPROM availability or omits its reason.")
        try manifest.identity.validateStructure()
        let directory = manifestURL.deletingLastPathComponent()
        try require(manifest.flash.sfdp_file.range(of: "^[A-Za-z0-9][A-Za-z0-9._-]*$",options: .regularExpression) != nil &&
                    !manifest.reads.contains { $0.file == manifest.flash.sfdp_file }, "Unsafe or non-distinct SFDP evidence path.")
        let sfdp = try boundedRead(directory.appendingPathComponent(manifest.flash.sfdp_file),expected: 64,maximum: 64)
        try manifest.flash.validate(sfdp: sfdp)
        var images: [Data] = []
        for read in manifest.reads {
            try require(read.file.range(of: "^[A-Za-z0-9][A-Za-z0-9._-]*$",options: .regularExpression) != nil && read.bytes == flashCapacity, "Invalid backup read reference.")
            let data = try boundedRead(directory.appendingPathComponent(read.file),expected: flashCapacity,maximum: flashCapacity)
            try require(data.count == flashCapacity && sha256(data) == read.sha256, "Backup bytes do not match the manifest.")
            images.append(data)
        }
        try require(manifest.reads[0].file != manifest.reads[1].file && images[0] == images[1], "Backup must contain two distinct equal full read files.")
        return VerifiedBackup(manifest: manifest, image: images[0], directory: directory, sfdp: sfdp)
    }
}
