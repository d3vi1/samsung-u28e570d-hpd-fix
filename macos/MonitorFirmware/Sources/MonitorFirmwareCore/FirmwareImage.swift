import Foundation
import NativeSupport

public struct PackageReport: Codable, Sendable {
    public let image_kind: String
    public let file_bytes: Int
    public let package_bytes: Int
    public let file_sha256: String
    public let package_sha256: String
    public let version: String?
    public let expanded_bytes: Int
    public let expanded_sha256: String
    public let checksum16: String
}
public struct ValidatedImage: Sendable {
    public let bytes: Data
    public let package: Data
    public let report: PackageReport
    fileprivate init(bytes: Data, package: Data, report: PackageReport) { self.bytes = bytes; self.package = package; self.report = report }
}
public enum FirmwareImage {
    public static func load(_ url: URL) throws -> ValidatedImage {
        let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        try require(size > 0 && size <= flashCapacity, "Input must be a Samsung package or exactly 2 MiB full SPI dump.")
        return try validate(Data(contentsOf: url))
    }
    public static func validate(_ data: Data) throws -> ValidatedImage {
        try require(data.count >= 0x23880 + 18 && data.count <= flashCapacity, "Invalid image size.")
        let b = [UInt8](data)
        func le32(_ p: Int) -> Int { Int(b[p]) | Int(b[p+1]) << 8 | Int(b[p+2]) << 16 | Int(b[p+3]) << 24 }
        func be32(_ p: Int) -> UInt32 { UInt32(b[p]) << 24 | UInt32(b[p+1]) << 16 | UInt32(b[p+2]) << 8 | UInt32(b[p+3]) }
        func be16(_ p: Int) -> UInt16 { UInt16(b[p]) << 8 | UInt16(b[p+1]) }
        let mainStart = le32(0x10000), load = le32(0x10004), loadEnd = le32(0x10008), mainEnd = le32(0x1000c)
        let crcOffset = le32(0x10020)
        try require(mainStart == 0x20080 && mainEnd >= 0x23887 && mainEnd <= data.count - 12, "Invalid main-payload bounds.")
        try require(crcOffset == ((mainEnd + 7) & ~7) + 8 && crcOffset <= data.count - 4, "Invalid package CRC offset/alignment.")
        let packageLength = crcOffset + 4
        try require(data.count == flashCapacity || data.count == packageLength, "Trailing bytes: only a package or a full 2 MiB image is accepted.")
        try require(load == 0x200000 && loadEnd == load + mainEnd - mainStart, "Invalid load bounds.")
        try require(Array(b[0x211c7..<0x211ca]) == [0xbe,0xad,0xde] && le32(0x211ca) == mainEnd - mainStart && le32(0x211ce) == 0x3800, "Invalid embedded loader metadata.")
        try require(b[mainEnd..<(crcOffset-8)].allSatisfy { $0 == 0 }, "Nonzero package alignment padding.")
        try require(Array(b[(crcOffset-8)..<(crcOffset-2)]) == [0x55,0xaa,0xab,0xcd,0,0], "Invalid package marker.")
        try require(crc16(b, 0x10000..<(mainEnd-2)) == be16(mainEnd-2), "Inner CRC16 mismatch.")
        try require(crc16(b, 0x10000..<(crcOffset-2)) == be16(crcOffset-2), "Outer CRC16 mismatch.")
        try require(crc32(b, mainStart..<mainEnd) == UInt32(le32(0x10014)), "Main CRC32 mismatch.")
        try require(crc32(b, 0..<crcOffset) == be32(crcOffset) && crc32(b, 0..<packageLength) == 0, "Package CRC32 mismatch.")
        let expandedCount = le32(mainEnd - 6)
        try require(expandedCount > 0 && expandedCount <= 8 * 1024 * 1024, "Invalid bounded expanded size.")
        let compressed = Data(b[0x23880..<(mainEnd-6)])
        var expanded = Data(count: expandedCount)
        let result = expanded.withUnsafeMutableBytes { out in compressed.withUnsafeBytes { input in
            mf_inflate_exact(input.bindMemory(to: UInt8.self).baseAddress, compressed.count,
                             out.bindMemory(to: UInt8.self).baseAddress, expandedCount)
        } }
        try require(result == 0, "DEFLATE is truncated, has trailing data, or has an incorrect expanded length.")
        var version: String?
        let prefix = Data("m-E5728UMDB-".utf8)
        if let range = expanded.range(of: prefix) {
            let suffix = expanded[range.lowerBound..<min(range.lowerBound + 64, expanded.count)].prefix { $0 >= 32 && $0 < 127 }
            version = String(data: suffix, encoding: .ascii)
        }
        let package = Data(data.prefix(packageLength))
        let report = PackageReport(image_kind: data.count == flashCapacity ? "spi_full" : "samsung_package",
            file_bytes: data.count, package_bytes: packageLength, file_sha256: sha256(data), package_sha256: sha256(package),
            version: version, expanded_bytes: expandedCount, expanded_sha256: sha256(expanded),
            checksum16: String(format: "%04X", package.reduce(UInt16(0)) { $0 &+ UInt16($1) }))
        return ValidatedImage(bytes: data, package: package, report: report)
    }
    public static func crc32(_ b: [UInt8], _ range: Range<Int>) -> UInt32 {
        var crc: UInt32 = 0
        for i in range { crc ^= UInt32(b[i]) << 24; for _ in 0..<8 { crc = (crc &<< 1) ^ ((crc & 0x80000000) != 0 ? 0x04c11db7 : 0) } }
        return crc
    }
    public static func crc16(_ b: [UInt8], _ range: Range<Int>) -> UInt16 {
        var crc: UInt16 = 0
        for i in range { let byte = (0x10014..<0x10018).contains(i) ? 0 : b[i]
            crc ^= UInt16(byte) << 8; for _ in 0..<8 { crc = (crc &<< 1) ^ ((crc & 0x8000) != 0 ? 0x8005 : 0) }
        }
        return crc
    }
}
