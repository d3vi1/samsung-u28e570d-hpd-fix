import Foundation
import NativeSupport
import Darwin

public protocol SubaddressedI2C: AnyObject {
    func write(chip: UInt32, offset: UInt32, bytes: Data) throws
    func read(chip: UInt32, offset: UInt32, count: Int) throws -> Data
}

/// Opens only one explicitly chosen registry ID. The per-route lock also serializes
/// identification with ISP sessions across instances of this application.
final class IOAVSession: SubaddressedI2C {
    let service: OpaquePointer
    private let lockDescriptor: Int32
    init(registryID: UInt64) throws {
        try require(registryID > 0, "An exact IOAV route is required.")
        let root = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("MonitorFirmware/RouteLocks")
        try DurableFilesystem.createDirectories(root)
        let fd = Darwin.open(root.appendingPathComponent("\(registryID).lock").path, O_CREAT | O_RDWR | O_NOFOLLOW, S_IRUSR | S_IWUSR)
        guard fd >= 0 else { throw FirmwareError.rejected("Cannot create the selected-route lock.") }
        do {
            try require(fsync(fd) == 0, "Cannot synchronize selected-route lock.")
            try DurableFilesystem.syncDirectory(root)
            try require(flock(fd,LOCK_EX | LOCK_NB) == 0, "Another operation owns the selected IOAV route.")
            guard let opened = mf_ioav_candidate_open(registryID) else { throw FirmwareError.rejected("The exact selected DCPAVServiceProxy is unavailable. No fallback is attempted.") }
            service = opened; lockDescriptor = fd
        } catch { Darwin.close(fd); throw error }
    }
    func write(chip: UInt32, offset: UInt32, bytes: Data) throws {
        let status = bytes.withUnsafeBytes { mf_ioav_candidate_write(service,chip,offset,$0.bindMemory(to: UInt8.self).baseAddress,UInt32(bytes.count)) }
        try require(status == 0, String(format: "IOAV write failed: 0x%08x",UInt32(bitPattern: status)))
    }
    func read(chip: UInt32, offset: UInt32, count: Int) throws -> Data {
        try require(count > 0 && count <= 4096, "IOAV read bounds exceeded.")
        var bytes = Data((0..<count).map { _ in UInt8.random(in: .min ... .max) })
        let status = bytes.withUnsafeMutableBytes { mf_ioav_candidate_read(service,chip,offset,$0.bindMemory(to: UInt8.self).baseAddress,UInt32(count)) }
        try require(status == 0, String(format: "IOAV read failed: 0x%08x",UInt32(bitPattern: status)))
        return bytes
    }
    deinit { mf_ioav_candidate_close(service); Darwin.close(lockDescriptor) } // Never sends exit/reset.
}

public struct EDIDDetails: Sendable {
    public let manufacturer: String
    public let manufacturer_code: UInt32
    public let product_code: UInt32
    public let numeric_serial: UInt32
    public let serial: String
    public let name: String
    public let generic: Bool
}
public struct RouteIdentification: Sendable {
    public let target: CachedDisplay
    public let firmware: String
    public let edid: Data
    public let firmwareFrame: Data
    public let matchedCGDisplay: UInt32?
}
public enum RouteIdentifier {
    public static func parseEDID(_ data: Data) throws -> EDIDDetails {
        let b = [UInt8](data)
        try require(b.count == 128 || b.count == 256, "EDID must contain one or two complete blocks.")
        try require(Array(b.prefix(8)) == [0,255,255,255,255,255,255,0], "Invalid EDID header.")
        try require(Int(b[126]) + 1 == b.count / 128 && b[126] <= 1, "Unsupported EDID extension count.")
        for start in stride(from: 0,to: b.count,by: 128) { try require(b[start..<start+128].reduce(0) { $0 + Int($1) } & 255 == 0, "EDID block checksum mismatch.") }
        let vendor = UInt32(b[8]) << 8 | UInt32(b[9])
        let letters = [Int((vendor >> 10) & 31),Int((vendor >> 5) & 31),Int(vendor & 31)]
        try require(letters.allSatisfy { (1...26).contains($0) }, "Invalid EDID manufacturer code.")
        let manufacturer = String(bytes: letters.map { UInt8($0 + 64) }, encoding: .ascii)!
        let product = UInt32(b[10]) | UInt32(b[11]) << 8
        let numeric = UInt32(b[12]) | UInt32(b[13]) << 8 | UInt32(b[14]) << 16 | UInt32(b[15]) << 24
        var serial = "", name = ""
        for offset in stride(from: 54,to: 126,by: 18) where b[offset] == 0 && b[offset+1] == 0 && b[offset+2] == 0 {
            let text = String(bytes: b[(offset+5)..<(offset+18)].prefix { $0 != 0 && $0 != 10 }, encoding: .ascii)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            if b[offset+3] == 0xff { serial = text }
            if b[offset+3] == 0xfc { name = text }
        }
        if serial.isEmpty { serial = String(numeric) }
        let generic = serial.isEmpty || ["0","1","4294967295","123456789","1234567890","SERIAL","SERIALNUMBER","H1AK500000"].contains(serial.uppercased()) || serial.allSatisfy { $0 == "0" }
        return EDIDDetails(manufacturer: manufacturer, manufacturer_code: vendor, product_code: product,
                           numeric_serial: numeric, serial: serial, name: name, generic: generic)
    }
    public static func identify(channel: any SubaddressedI2C, route: CachedRoute, displays: [CachedDisplay],
                                operatorConfirmedAwake: Bool) throws -> RouteIdentification {
        try require(route.registry_id > 0 && route.path.lowercased().contains("dcpext"), "Select a cached external DCP route explicitly.")
        try require(operatorConfirmedAwake, "Confirm the selected physical monitor is awake before identification.")
        func read(chip: UInt32, offset: UInt32, count: Int) throws -> Data {
            do { return try channel.read(chip: chip,offset: offset,count: count) }
            catch { throw IdentificationTransientError(stage: "I2C transport",detail: error.localizedDescription) }
        }
        func framing(_ valid: Bool, _ message: String, _ frame: Data) throws {
            if !valid { throw IdentificationTransientError(stage: "Identification framing",detail: message,frame: frame) }
        }
        // Exact fixed query from macos/read_firmware_id.m; no arbitrary DDC setter.
        do { try channel.write(chip: 0x37,offset: 0x51,bytes: Data([0x82,0xc3,0x00,0x7e])) }
        catch { throw IdentificationTransientError(stage: "I2C transport",detail: error.localizedDescription) }
        Thread.sleep(forTimeInterval: 0.1)
        let frame = try read(chip: 0x37,offset: 0x51,count: 24)
        try framing(frame.count == 24, "Short firmware identification reply.",frame)
        let f = [UInt8](frame)
        try framing(Array(f.prefix(4)) == [0x6e,0x94,0xc3,0] && f.prefix(23).reduce(UInt8(0x50),^) == 0, "Invalid C3/0 reply/checksum.",frame)
        let firmware = String(bytes: f[4..<22],encoding: .ascii) ?? ""
        try require(firmware.hasPrefix("m-E5728UMDB-"), "Selected route does not report the supported Samsung firmware family.")
        func readEDID() throws -> Data {
            let base = try read(chip: 0x50,offset: 0,count: 128)
            try framing(base.count == 128 && Array(base.prefix(8)) == [0,255,255,255,255,255,255,0] && base.reduce(0) { $0 + Int($1) } & 255 == 0, "Invalid base EDID block.",base)
            try require(base[126] <= 1, "EDID needs unsupported extension/segment reads.")
            if base[126] == 1 { return base + (try read(chip: 0x50,offset: 128,count: 128)) }
            return base
        }
        let first = try readEDID(), second = try readEDID()
        try framing(first == second, "Independent EDID reads differ; route identity is not stable.",second)
        let details: EDIDDetails
        do { details = try parseEDID(first) }
        catch { throw IdentificationTransientError(stage: "EDID framing",detail: error.localizedDescription,frame: first) }
        try require(details.manufacturer == "SAM", "Selected route's EDID is not Samsung.")
        let matches = displays.filter { $0.cachedManufacturerCode == details.manufacturer_code && $0.identity.product_code == details.product_code && $0.cachedNumericSerial == details.numeric_serial }
        let match = matches.count == 1 ? matches[0] : nil
        let ambiguous = details.generic || matches.count > 1
        let reason = details.generic ? "EDID serial is generic or missing." : matches.count > 1 ? "Multiple cached CG displays share this EDID triple." : ""
        let identity = MonitorIdentity(manufacturer: details.manufacturer,product_code: details.product_code,edid_serial: details.serial,
            edid_sha256: sha256(first),transport_locator: "ioav:\(route.registry_id)",ambiguous: ambiguous,ambiguity_reason: reason)
        let warning = ambiguous ? reason : match == nil ? "Exact IOAV route bound by C3/0 and two EDID reads. CG association unavailable; awake state is operator-confirmed." : "Exact IOAV route and a unique cached CoreGraphics EDID triple match."
        let target = CachedDisplay(id: match?.id ?? 0,name: details.name.isEmpty ? "Samsung · " + firmware : details.name + " · " + firmware,
            identity: identity,active: match.map { $0.active && !$0.asleep } ?? operatorConfirmedAwake,asleep: match?.asleep ?? false,
            identityWarning: warning,registryID: route.registry_id,cachedManufacturerCode: details.manufacturer_code,cachedNumericSerial: details.numeric_serial,routePath: route.path)
        return RouteIdentification(target: target,firmware: firmware,edid: first,firmwareFrame: frame,matchedCGDisplay: match?.id)
    }
}
