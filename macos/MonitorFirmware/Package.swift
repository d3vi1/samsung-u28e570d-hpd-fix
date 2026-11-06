// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "MonitorFirmware",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "MonitorFirmware", targets: ["MonitorFirmwareApp"]),
               .library(name: "MonitorFirmwareCore", targets: ["MonitorFirmwareCore"])],
    targets: [
        .target(name: "NativeSupport", linkerSettings: [.linkedLibrary("z"), .linkedFramework("IOKit"), .linkedFramework("CoreGraphics")]),
        .target(name: "MonitorFirmwareCore", dependencies: ["NativeSupport"]),
        .executableTarget(name: "MonitorFirmwareApp", dependencies: ["MonitorFirmwareCore"]),
        .testTarget(name: "MonitorFirmwareTests", dependencies: ["MonitorFirmwareCore"])
    ]
)
