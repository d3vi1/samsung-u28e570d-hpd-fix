import Foundation

public struct DriverCapabilities: Sendable {
    public let cachedEnumeration: Bool
    public let experimentalReadOnlyISP: Bool
    public let qualifiedHardwareWrite: Bool
    public let reason: String
}
public enum TransportAccess: Sendable { case defaultBlocked, experimentalReadOnly }

/// Provider discovery/capabilities are independent of raw I2C framing, MStar SPI
/// commands, transactional flashing, and SwiftUI. A future provider supplies the
/// same RawI2CBus contract; it cannot silently replace the selected transport.
public protocol DisplayTransportProvider: Sendable {
    var id: String { get }
    var name: String { get }
    var capabilities: DriverCapabilities { get }
    func enumerateCached() throws -> [CachedDisplay]
    func enumerateRoutesCached() throws -> [CachedRoute]
    func identify(route: CachedRoute, operatorConfirmedAwake: Bool, archive: EvidenceArchive) throws -> RouteIdentification
    func makeDevice(target: CachedDisplay, access: TransportAccess) throws -> any FlashDevice
}
public struct IOAVAppleSiliconProvider: DisplayTransportProvider {
    public let id = "apple-silicon-ioav"
    public let name = "Apple Silicon · IOAV / DCP"
    public init() {}
    public var capabilities: DriverCapabilities {
        #if arch(arm64)
        DriverCapabilities(cachedEnumeration: true, experimentalReadOnlyISP: true, qualifiedHardwareWrite: false,
                           reason: IOAVRawBus.limitation)
        #else
        DriverCapabilities(cachedEnumeration: false, experimentalReadOnlyISP: false, qualifiedHardwareWrite: false,
                           reason: "This provider requires Apple Silicon DCP services.")
        #endif
    }
    public func enumerateCached() throws -> [CachedDisplay] {
        try require(capabilities.cachedEnumeration, capabilities.reason)
        return try CachedDisplays.list()
    }
    public func enumerateRoutesCached() throws -> [CachedRoute] {
        try require(capabilities.cachedEnumeration,capabilities.reason)
        return try CachedDisplays.routes().filter { $0.path.lowercased().contains("dcpext") }
    }
    public func identify(route: CachedRoute, operatorConfirmedAwake: Bool, archive: EvidenceArchive) throws -> RouteIdentification {
        try require(capabilities.cachedEnumeration,capabilities.reason)
        try require(operatorConfirmedAwake && route.path.lowercased().contains("dcpext"), "Select an external route and confirm its monitor is awake before opening IOAV.")
        try archive.save("selected-route.json",value: route)
        let channel = try IOAVSession(registryID: route.registry_id)
        let result = try RouteIdentifier.identify(channel: channel,route: route,displays: CachedDisplays.list(),operatorConfirmedAwake: operatorConfirmedAwake)
        try archive.save("edid.bin",data: result.edid)
        try archive.save("firmware-query.bin",data: result.firmwareFrame)
        try archive.save("identity.json",value: result.target.identity)
        try archive.log("Explicit route identification: " + result.firmware + "; " + result.target.identityWarning)
        return result
    }
    public func makeDevice(target: CachedDisplay, access: TransportAccess) throws -> any FlashDevice {
        try require(capabilities.cachedEnumeration, capabilities.reason)
        try require(target.active && !target.asleep, "Selected display is asleep/inactive; no ISP access is permitted.")
        return MStarISPDevice(bus: IOAVRawBus(target: target.identity, registryID: target.registryID, routePath: target.routePath,
                                            experimentalReadOnlyOptIn: access == .experimentalReadOnly))
    }
}
public struct IntelFramebufferProvider: DisplayTransportProvider {
    public let id = "intel-ioframebuffer"
    public let name = "Intel Mac · IOFramebuffer / IOI2C (unavailable)"
    public init() {}
    public var capabilities: DriverCapabilities {
        DriverCapabilities(cachedEnumeration: false, experimentalReadOnlyISP: false, qualifiedHardwareWrite: false,
                           reason: "Native Intel IOFramebuffer/IOI2C provider is reserved but not implemented or qualified. No fallback or driver access is attempted.")
    }
    public func enumerateCached() throws -> [CachedDisplay] { throw FirmwareError.rejected(capabilities.reason) }
    public func enumerateRoutesCached() throws -> [CachedRoute] { throw FirmwareError.rejected(capabilities.reason) }
    public func identify(route: CachedRoute, operatorConfirmedAwake: Bool, archive: EvidenceArchive) throws -> RouteIdentification { throw FirmwareError.rejected(capabilities.reason) }
    public func makeDevice(target: CachedDisplay, access: TransportAccess) throws -> any FlashDevice { throw FirmwareError.rejected(capabilities.reason) }
}
public enum TransportProviders {
    public static var available: [any DisplayTransportProvider] { [IOAVAppleSiliconProvider(), IntelFramebufferProvider()] }
}
