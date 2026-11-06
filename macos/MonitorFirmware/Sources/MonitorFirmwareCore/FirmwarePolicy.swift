import Foundation

public enum FirmwareWritePolicy: Sendable {
    case approvedPackages
    case simulationOnly
    public static let approvedPackageHashes: Set<String> = [
        "60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b",
        "3d5cda5eb49c6c0bb0378742aa1767becd31d3b3e85a6c0b9c023bd6ada2dac4"
    ]
    func validate(source: ValidatedImage, target: ValidatedImage, backup: VerifiedBackup) throws {
        switch self {
        case .approvedPackages:
            try require(Self.approvedPackageHashes.contains(source.report.package_sha256), "Current source package is not an approved 1004.5/1005.0 hash; inspect/verify only.")
            try require(Self.approvedPackageHashes.contains(target.report.package_sha256), "Selected package is not an approved 1004.5/1005.0 hash; valid CRCs alone do not authorize writing.")
        case .simulationOnly:
            try require(backup.manifest.simulated == true, "Simulation policy requires a simulated backup.")
        }
    }
}
