import Foundation

public struct WritePlanSummary: Codable, Sendable {
    public let source_full_sha256: String
    public let target_full_sha256: String
    public let plan_sha256: String
    public let changed_sectors: [Int]
    public let header_last: Bool
    public let mode: String
    public let identity: MonitorIdentity
}

public struct WritePlan: Sendable {
    public let backup: VerifiedBackup
    public let target: Data
    public let changedSectors: [Int]
    public let sourceHash: String
    public let targetHash: String
    public let planHash: String
    public let policy: FirmwareWritePolicy
    public let restoreBackup: VerifiedBackup?
    public var summary: WritePlanSummary {
        WritePlanSummary(source_full_sha256: sourceHash,target_full_sha256: targetHash,plan_sha256: planHash,
                         changed_sectors: changedSectors,header_last: !changedSectors.contains(firmwareHeaderSector) || changedSectors.last == firmwareHeaderSector,
                         mode: restoreBackup == nil ? "package-overlay" : "own-unit-full-restore",identity: backup.manifest.identity)
    }
    public init(backup: VerifiedBackup, input: ValidatedImage, restoreBackup: VerifiedBackup? = nil, policy: FirmwareWritePolicy = .approvedPackages) throws {
        try require(backup.image.count == flashCapacity, "Source must be a complete own-unit SPI backup.")
        try require(backup.manifest.identity.matchesUnit(backup.manifest.identity), "Write planning requires an unambiguous current-unit identity; this backup is inspect/verify only.")
        let sourcePackage = try FirmwareImage.validate(backup.image)
        try policy.validate(source: sourcePackage,target: input,backup: backup)
        let candidate = input.package
        let source = backup.image
        try require(source[0..<0x10000] == candidate[0..<0x10000], "Bootstrap differs; this tool does not replace the bootloader.")
        try require(source[0x10080..<0x20080] == candidate[0x10080..<0x20080], "PM51 slot differs; this tool does not replace PM firmware.")
        for offset in 0..<0x80 where !(8..<16).contains(offset) && !(20..<24).contains(offset) && !(32..<36).contains(offset) {
            try require(source[0x10000 + offset] == candidate[0x10000 + offset], "An invariant package header byte differs.")
        }
        for offset in 0x20080..<0x23880 where !(0x211ca..<0x211ce).contains(offset) {
            try require(source[offset] == candidate[offset], "Main loader differs beyond its embedded length; separate qualification is required.")
        }
        var target = source
        target.replaceSubrange(0..<candidate.count, with: candidate)
        if input.report.image_kind == "spi_full" {
            guard let restoreBackup else { throw FirmwareError.rejected("Full-image restoration requires a separate verified target-backup manifest, not just a BIN file.") }
            try restoreBackup.manifest.flash.validate(sfdp: restoreBackup.sfdp)
            try require(restoreBackup.image == input.bytes && restoreBackup.manifest.identity.matchesUnit(backup.manifest.identity), "Restore target does not match its own-unit backup provenance.")
            try require((restoreBackup.manifest.simulated == true) == (backup.manifest.simulated == true) && restoreBackup.sfdp == backup.sfdp, "Restore backup profile/provenance differs from the current unit.")
            target = restoreBackup.image
        } else { try require(restoreBackup == nil, "A restore manifest applies only to its complete SPI image.") }
        try require(source[0x90000..<flashCapacity] == target[0x90000..<flashCapacity], "Protected unit-specific SPI tail at 0x90000 and above differs.")
        var sectors = Set<Int>()
        for index in 0..<flashCapacity where source[index] != target[index] {
            try require((0x10000..<0x10080).contains(index) || (0x20080..<0x90000).contains(index), "Change outside the permitted header/main regions.")
            sectors.insert(index / flashSectorSize * flashSectorSize)
        }
        let ordered = sectors.filter { $0 != firmwareHeaderSector }.sorted() + (sectors.contains(firmwareHeaderSector) ? [firmwareHeaderSector] : [])
        self.backup = backup; self.target = target; changedSectors = ordered
        self.policy = policy; self.restoreBackup = restoreBackup
        sourceHash = sha256(source); targetHash = sha256(target)
        planHash = sha256(Data((sourceHash + "\n" + targetHash + "\n" + ordered.map(String.init).joined(separator: ",")).utf8))
    }
}
public struct ExecutionResult: Codable, Sendable {
    public var completed = false
    public var source_verified = false
    public var target_verified = false
    public var mutation_started = false
    public var rollback_attempted = false
    public var rollback_verified = false
    public var reset_allowed = false
    public var isp_exit_completed = false
    public var touched_sectors: [Int] = []
    public var error: String?
    public var persistence_errors: [String] = []
    public init() {}
}

public enum FlashEngine {
    private static func recoveryDirectory(_ device: any FlashDevice, _ archive: EvidenceArchive) -> URL {
        if device.isSimulated { return archive.directory.deletingLastPathComponent().appendingPathComponent("mock-recovery") }
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("MonitorFirmware/Recovery")
    }
    private static func cancelled(_ enabled: Bool = true) throws { if enabled { try Task.checkCancellation() } }
    private static func readFull(_ device: any FlashDevice, name: String, archive: EvidenceArchive,
                                 cancellable: Bool, progress: ProgressHandler) throws -> Data {
        var data = Data(); data.reserveCapacity(flashCapacity)
        do {
            for offset in stride(from: 0, to: flashCapacity, by: 256) {
                try cancelled(cancellable)
                let bytes = try device.read(address: offset, count: 256)
                try require(bytes.count == 256, "Short SPI read at \(offset).")
                data.append(bytes)
                if offset % 65536 == 0 { progress(ProgressUpdate(name, offset, flashCapacity)) }
            }
            try archive.save(name + ".bin", data: data)
            progress(ProgressUpdate(name, flashCapacity, flashCapacity))
            return data
        } catch {
            // Retain partial reads as evidence, never label them as a valid full backup.
            try? archive.save(name + ".partial.bin", data: data)
            throw error
        }
    }
    private static func readPair(_ device: any FlashDevice, prefix: String, archive: EvidenceArchive,
                                 cancellable: Bool = true, progress: ProgressHandler) throws -> Data {
        let first = try readFull(device, name: prefix + "-1", archive: archive, cancellable: cancellable, progress: progress)
        let second = try readFull(device, name: prefix + "-2", archive: archive, cancellable: cancellable, progress: progress)
        try require(first == second, "The two complete reads differ. Both files were retained; the operation is rejected.")
        return first
    }
    private static func validateDevice(_ device: any FlashDevice, selected: MonitorIdentity) throws -> FlashProfile {
        try device.assertQualified()
        let actual = try device.identity()
        try require(selected.matchesUnit(actual) && selected.transport_locator == actual.transport_locator, "Live target identity/route does not match the explicitly selected monitor.")
        let profile = try device.profile(); try profile.validate()
        try require(try device.status() == profile.beforeBytes, "Initial flash status does not match its profile.")
        return profile
    }
    public static func backup(device: any FlashDevice, selected: MonitorIdentity, archive: EvidenceArchive,
                              retryPolicy: PostExitRetryPolicy = .init(),progress: ProgressHandler = { _ in }) throws -> VerifiedBackup {
        try retryPolicy.validate()
        try device.assertQualified()
        let lease = try DeviceLease(identity: selected, root: recoveryDirectory(device, archive), archive: archive)
        defer { withExtendedLifetime(lease) {} }
        let profile = try validateDevice(device, selected: selected)
        try archive.save("identity.json", value: selected)
        try archive.save("flash-sfdp.bin", data: profile.sfdpSnapshot!)
        let bytes = try readPair(device, prefix: "backup-read", archive: archive, progress: progress)
        try require(try device.status() == profile.beforeBytes, "Flash status changed during backup.")
        try require(selected.matchesUnit(device.identity()), "Target identity changed during backup.")
        let hash = sha256(bytes)
        let manifest = BackupManifest(model: "LU28E570DS/EN", identity: selected, flash: profile,
            reads: [ReadEvidence(file: "backup-read-1.bin", bytes: bytes.count, sha256: hash), ReadEvidence(file: "backup-read-2.bin", bytes: bytes.count, sha256: hash)], simulated: device.isSimulated)
        // Preserve both read files on failure, but publish the completed own-unit
        // manifest only after validated image/status and post-exit identity evidence.
        _ = try FirmwareImage.validate(bytes)
        try device.exitISP(permit: VerifiedImagePermit(image: bytes, status: profile.beforeBytes))
        try PostExitSettling.confirm(device: device,selected: selected,archive: archive,policy: retryPolicy)
        try archive.log("ISP exit sent after two equal complete reads, image integrity and unchanged status.")
        try archive.save("backup.json", value: manifest)
        return VerifiedBackup(manifest: manifest, image: bytes, directory: archive.directory, sfdp: profile.sfdpSnapshot!)
    }
    public static func verify(device: any FlashDevice, selected: MonitorIdentity, expected: Data,
                              archive: EvidenceArchive,retryPolicy: PostExitRetryPolicy = .init(), progress: ProgressHandler = { _ in }) throws {
        try retryPolicy.validate()
        try require(expected.count == flashCapacity, "Live verify needs a full expected 2 MiB image, not a package prefix.")
        _ = try FirmwareImage.validate(expected)
        try device.assertQualified()
        let lease = try DeviceLease(identity: selected, root: recoveryDirectory(device, archive), archive: archive)
        defer { withExtendedLifetime(lease) {} }
        let profile = try validateDevice(device, selected: selected)
        try archive.save("flash-sfdp.bin", data: profile.sfdpSnapshot!)
        let actual = try readPair(device, prefix: "verify", archive: archive, progress: progress)
        try require(actual == expected, "Full SPI image differs from the expected image; both read files were retained.")
        try require(try device.status() == profile.beforeBytes, "Status changed during verification.")
        try archive.log("Verified two complete reads against SHA-256 " + sha256(expected))
        try device.exitISP(permit: VerifiedImagePermit(image: actual, status: profile.beforeBytes))
        try PostExitSettling.confirm(device: device,selected: selected,archive: archive,policy: retryPolicy)
    }
    private static func writeSector(_ device: any FlashDevice, address: Int, image: Data) throws {
        try device.eraseSector(address: address)
        // Full-sector erased verification; never infer erase from a small prefix.
        for offset in stride(from: 0, to: flashSectorSize, by: 256) {
            let erased = try device.read(address: address + offset, count: 256)
            try require(erased.count == 256 && erased.allSatisfy { $0 == 0xff }, "Erase verification failed.")
        }
        for offset in stride(from: 0, to: flashSectorSize, by: 32) {
            let bytes = Data(image[(address + offset)..<(address + offset + 32)])
            if !bytes.allSatisfy({ $0 == 0xff }) { try device.program(address: address + offset, bytes: bytes) }
        }
        for offset in stride(from: 0, to: flashSectorSize, by: 256) {
            let actual = try device.read(address: address + offset, count: 256)
            try require(actual == image[(address + offset)..<(address + offset + 256)], "Sector read-back failed at \(address + offset).")
        }
    }
    public static func execute(device: any FlashDevice, selected: MonitorIdentity, plan: WritePlan,
                               archive: EvidenceArchive, recoveryRoot: URL,retryPolicy: PostExitRetryPolicy = .init(), progress: ProgressHandler = { _ in }) -> ExecutionResult {
        var result = ExecutionResult()
        var lease: DeviceLease?
        var profile: FlashProfile?
        var finalVerificationStarted = false
        func record(_ message: String) { do { try archive.log(message) } catch { result.persistence_errors.append(error.localizedDescription) } }
        do {
            try retryPolicy.validate()
            if plan.policy == .simulationOnly { try require(device.isSimulated, "Simulation-only plans can never execute on a hardware transport.") }
            try require(plan.backup.manifest.identity.matchesUnit(selected), "Backup belongs to another or ambiguous monitor identity.")
            try require((plan.backup.manifest.simulated == true) == device.isSimulated, "Simulated and hardware backups cannot be mixed.")
            try device.assertQualified() // Before lock, ISP entry, or bus operations.
            try device.assertWritableQualified()
            lease = try DeviceLease(identity: selected, root: recoveryRoot, archive: archive)
            profile = try validateDevice(device, selected: selected)
            try plan.backup.manifest.flash.validate(sfdp: plan.backup.sfdp)
            try require(profile!.jedec == plan.backup.manifest.flash.jedec && profile!.sfdp_sha256 == plan.backup.manifest.flash.sfdp_sha256 && profile!.status_before == plan.backup.manifest.flash.status_before, "Current flash profile differs from backup evidence.")
            try archive.save("source-full.bin", data: plan.backup.image)
            try archive.save("flash-sfdp.bin", data: profile!.sfdpSnapshot!)
            try archive.save("target-full.bin", data: plan.target)
            try archive.save("backup-provenance.json", value: plan.backup.manifest)
            try archive.save("plan.json",value: plan.summary)
            if let targetBackup = plan.restoreBackup { try archive.save("restore-target-provenance.json",value: targetBackup.manifest) }
            try archive.log("Pinned plan " + plan.planHash)
            let source = try readPair(device, prefix: "preflight", archive: archive, progress: progress)
            try require(source == plan.backup.image, "Live source differs from the own-unit backup; no erase permitted.")
            try require(try device.status() == profile!.beforeBytes, "Status changed during preflight.")
            result.source_verified = true
            try cancelled()
            for (index, address) in plan.changedSectors.enumerated() {
                try cancelled() // Cancellation after mutation triggers restoration; never interrupts a sector halfway.
                if !result.mutation_started { try lease!.markMutation() }
                result.mutation_started = true; result.reset_allowed = false; result.touched_sectors.append(address)
                record(String(format: "Writing sector 0x%06X", address))
                try writeSector(device, address: address, image: plan.target)
                progress(ProgressUpdate("Verified sectors", index + 1, plan.changedSectors.count))
            }
            if result.mutation_started { try device.writeDisable() }
            // No-op transactions also require fresh complete final evidence before reset permission.
            result.reset_allowed = false
            finalVerificationStarted = true
            let target = try readPair(device, prefix: "final", archive: archive, progress: progress)
            try require(target == plan.target, "Final complete image does not match the target.")
            try require(try device.status() == profile!.beforeBytes, "Final flash status changed.")
            result.target_verified = true; result.completed = true; result.reset_allowed = true
        } catch {
            result.error = error.localizedDescription; result.reset_allowed = false
            record("Operation failed: " + error.localizedDescription)
            if !result.mutation_started && result.source_verified && !finalVerificationStarted {
                result.reset_allowed = true
            }
            if result.mutation_started {
                result.rollback_attempted = true
                // Cancellation is intentionally ignored during recovery. Header remains last.
                for address in result.touched_sectors {
                    do { try writeSector(device, address: address, image: plan.backup.image) }
                    catch { record("Rollback sector failure: " + error.localizedDescription) }
                }
                do {
                    try device.writeDisable()
                    let restored = try readPair(device, prefix: "rollback", archive: archive, cancellable: false, progress: progress)
                    try require(restored == plan.backup.image && device.status() == profile!.beforeBytes, "Rollback full-image/status mismatch.")
                    result.rollback_verified = true; result.reset_allowed = true
                } catch { result.reset_allowed = false; record("RECOVERY REQUIRED; reset withheld: " + error.localizedDescription) }
            }
        }
        // This is an explicit post-verification operation, not an unconditional
        // cleanup/finally reset. Failure here never starts another destructive rollback.
        if result.reset_allowed, let profile {
            do {
                let verified = result.target_verified ? plan.target : plan.backup.image
                try device.exitISP(permit: VerifiedImagePermit(image: verified, status: profile.beforeBytes))
                result.isp_exit_completed = true
                try PostExitSettling.confirm(device: device,selected: selected,archive: archive,policy: retryPolicy)
                if result.mutation_started {
                    do { try lease!.resolve() }
                    catch { result.persistence_errors.append("Verified image; recovery marker cleanup failed: " + error.localizedDescription) }
                }
            } catch {
                result.completed = false
                result.reset_allowed = false
                result.error = (result.error.map { $0 + "\n" } ?? "") + "Session finalization failed: " + error.localizedDescription
                record("Verified image, but ISP exit failed. Recovery marker retained.")
            }
        }
        do { try archive.save("result.json", value: result) }
        catch { result.persistence_errors.append(error.localizedDescription) }
        withExtendedLifetime(lease) {}
        return result
    }
}
