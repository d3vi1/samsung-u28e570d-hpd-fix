import Foundation
import CoreGraphics
import NativeSupport

public struct CachedDisplay: Identifiable, Sendable {
    public let id: UInt32
    public let name: String
    public let identity: MonitorIdentity
    public let active: Bool
    public let asleep: Bool
    public let identityWarning: String
    public let registryID: UInt64?
    public let cachedManufacturerCode: UInt32
    public let cachedNumericSerial: UInt32
    public let routePath: String?
}
public struct CachedRoute: Codable, Sendable {
    public let registry_id: UInt64
    public let path: String
}
public enum CachedDisplays {
    /// Explicit user-triggered cached OS query. No IOAV object, EDID bus read, or ISP access.
    public static func list() throws -> [CachedDisplay] {
        var ids = [CGDirectDisplayID](repeating: 0, count: 64); var count: UInt32 = 0
        try require(CGGetOnlineDisplayList(64, &ids, &count) == .success, "Cached CoreGraphics enumeration failed.")
        return ids.prefix(Int(count)).filter { CGDisplayIsBuiltin($0) == 0 }.map { id in
            let vendor = CGDisplayVendorNumber(id), product = CGDisplayModelNumber(id), serial = CGDisplaySerialNumber(id)
            return CachedDisplay(id: id, name: String(format: "Display %u · vendor %04X / product %04X", id, vendor, product),
                identity: MonitorIdentity(manufacturer: String(format: "%04x", vendor), product_code: product,
                    edid_serial: String(serial), edid_sha256: "", transport_locator: "coregraphics:\(id)",
                    ambiguous: true, ambiguity_reason: "No verified cached EDID-to-IOAV route association."),
                active: CGDisplayIsActive(id) != 0, asleep: CGDisplayIsAsleep(id) != 0,
                identityWarning: "Cached numeric EDID serial may be generic. Use explicit selected-route identification to bind the EDID and IOAV service.", registryID: nil,
                cachedManufacturerCode: vendor,cachedNumericSerial: serial,routePath: nil)
        }
    }
    public static func routes() throws -> [CachedRoute] {
        guard let pointer = mf_copy_cached_routes() else { throw FirmwareError.rejected("Cached DCPAVServiceProxy enumeration failed.") }
        defer { mf_free(pointer) }
        return try JSONDecoder().decode([CachedRoute].self, from: Data(String(cString: pointer).utf8))
    }
}
