import SwiftUI
import AppKit
import UniformTypeIdentifiers
import MonitorFirmwareCore

@MainActor
@Observable final class AppModel {
    static let shared = AppModel()
    var displays: [CachedDisplay] = []
    let providers = TransportProviders.available
    var providerID = "apple-silicon-ioav"
    var provider: any DisplayTransportProvider { providers.first { $0.id == providerID }! }
    var experimentalReadOnly = false
    var confirmReadOnly = false
    var routes: [CachedRoute] = []
    var selectedRoute: UInt64?
    var identifiedTarget: CachedDisplay?
    var confirmIdentification = false
    var selected: UInt32?
    var fileURL: URL?
    var input: ValidatedImage?
    var backup: VerifiedBackup?
    var restoreBackup: VerifiedBackup?
    var outputRoot = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Monitor Firmware Archives")
    var archiveURL: URL?
    var busy = false
    var progress = ProgressUpdate("Ready")
    var message = "Select Refresh to list cached external displays. No device commands are sent automatically."
    var confirmWrite = false
    struct PreparedWrite: Sendable { let plan: WritePlan; let target: CachedDisplay; let providerID: String }
    var preparedWrite: PreparedWrite?
    var operation: Task<Void, Never>?
    var target: CachedDisplay? { identifiedTarget ?? displays.first { $0.id == selected } }
    func invalidatePlan() { preparedWrite = nil; confirmWrite = false }
    var summary: String {
        guard let input else { return "Choose a Samsung BIN package or full 2 MiB SPI image." }
        let policy = FirmwareWritePolicy.approvedPackageHashes.contains(input.report.package_sha256) ? "Package matches an approved firmware hash." : "Unknown package hash: inspect/verify only."
        return "\(input.report.image_kind) · \(input.report.file_bytes) bytes\n\(input.report.version ?? "Version string unavailable")\nSHA-256: \(input.report.file_sha256)\nAll four CRC fields and the exact DEFLATE boundary verified. \(policy) Device/transport qualification is still required."
    }
    func refresh() {
        guard !busy else { return }
        invalidatePlan()
        do { displays = try provider.enumerateCached(); routes = try provider.enumerateRoutesCached(); selected = nil; selectedRoute = nil; identifiedTarget = nil; experimentalReadOnly = false
            message = routes.isEmpty ? "No cached external IOAV route is currently present. Connect and wake the monitor, then Refresh." : "Select an external IOAV route and use Identify to bind its firmware/EDID explicitly. Refresh sent no device commands."
        } catch { message = error.localizedDescription }
    }
    func identifySelectedRoute() {
        guard let route = routes.first(where: { $0.registry_id == selectedRoute }) else { return }
        let provider = self.provider
        run(label: "identify") { archive, _ in .identification(try provider.identify(route: route,operatorConfirmedAwake: true,archive: archive)) }
    }
    func chooseInput() {
        let panel = NSOpenPanel(); panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        panel.title = "Choose firmware package or full SPI image"
        if panel.runModal() == .OK, let url = panel.url { invalidatePlan(); fileURL = url; input = nil; restoreBackup = nil; validateInput() }
    }
    func validateInput() {
        guard let fileURL, !busy else { return }
        run(label: "offline-validation") { archive, _ in
            let image = try FirmwareImage.load(fileURL)
            try archive.save("validation.json", value: image.report)
            return .input(image)
        }
    }
    func chooseBackup() {
        let panel = NSOpenPanel(); panel.allowedContentTypes = [.json]; panel.title = "Choose own-monitor backup.json"
        if panel.runModal() == .OK, let url = panel.url {
            invalidatePlan()
            do { backup = try BackupLoader.load(manifestURL: url); message = "Both full backup files match their manifest. Live identity/profile/source checks are still required before writing." }
            catch { backup = nil; message = error.localizedDescription }
        }
    }
    func chooseOutput() {
        let panel = NSOpenPanel(); panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
        panel.title = "Choose archive parent directory"
        if panel.runModal() == .OK, let url = panel.url { outputRoot = url }
    }
    func chooseRestoreBackup() {
        let panel = NSOpenPanel(); panel.allowedContentTypes = [.json]; panel.title = "Choose the own-unit backup to restore"
        if panel.runModal() == .OK, let url = panel.url {
            invalidatePlan()
            do {
                let value = try BackupLoader.load(manifestURL: url)
                let image = try FirmwareImage.validate(value.image)
                restoreBackup = value; input = image; fileURL = value.directory.appendingPathComponent(value.manifest.reads[0].file)
                message = "Restore target and both provenance reads verified. Load/download a separate current backup before writing; live preflight must match that current backup."
            } catch { restoreBackup = nil; message = error.localizedDescription }
        }
    }
    func download() {
        guard let target else { return }
        do {
            let device = try provider.makeDevice(target: target, access: experimentalReadOnly ? .experimentalReadOnly : .defaultBlocked)
            run(label: "backup") { archive, progress in
                .backup(try FlashEngine.backup(device: device, selected: target.identity, archive: archive, progress: progress))
            }
        } catch { message = error.localizedDescription }
    }
    func verifyMonitor() {
        guard let target, let input else { return }
        do {
            let expected: Data
            if input.report.image_kind == "spi_full" { expected = input.bytes }
            else {
                guard let backup else { message = "A package is not a full image. Load this monitor's verified backup to compose the expected full image."; return }
                var full = backup.image
                full.replaceSubrange(0..<input.package.count,with: input.package)
                expected = full // Read-only comparison intentionally does not require write-policy approval.
            }
            let device = try provider.makeDevice(target: target, access: experimentalReadOnly ? .experimentalReadOnly : .defaultBlocked)
            run(label: "verify") { archive, progress in
                try FlashEngine.verify(device: device, selected: target.identity, expected: expected, archive: archive, progress: progress)
                return .message("Two complete reads match the expected full image and status is unchanged.")
            }
        } catch { message = error.localizedDescription }
    }
    func prepareWrite() {
        guard let target, let backup, let input else { return }
        invalidatePlan(); busy = true; progress = ProgressUpdate("Preparing exact plan offline")
        let restore = restoreBackup, driver = providerID
        // Pure in-memory preparation: no archive, transport/provider call or service open.
        operation = Task.detached(priority: .userInitiated) { [weak self] in
            do {
                let plan = try WritePlan(backup: backup,input: input,restoreBackup: restore)
                try Task.checkCancellation()
                await self?.finish(.prepared(PreparedWrite(plan: plan,target: target,providerID: driver)))
            } catch { await self?.finish(.message(error.localizedDescription)) }
        }
    }
    func upload() {
        guard let prepared = preparedWrite, prepared.providerID == providerID,
              prepared.target.identity == target?.identity else { message = "The reviewed plan is no longer current. Prepare it again."; return }
        let plan = prepared.plan, target = prepared.target
        confirmWrite = false
        do {
            let device = try provider.makeDevice(target: target, access: experimentalReadOnly ? .experimentalReadOnly : .defaultBlocked)
            let recoveryRoot = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("MonitorFirmware/Recovery", isDirectory: true)
            run(label: "write") { archive, progress in
                .execution(FlashEngine.execute(device: device, selected: target.identity, plan: plan, archive: archive,
                    recoveryRoot: recoveryRoot, progress: progress))
            }
        } catch { message = error.localizedDescription }
    }
    enum Outcome: Sendable { case input(ValidatedImage), backup(VerifiedBackup), execution(ExecutionResult), identification(RouteIdentification), prepared(PreparedWrite), message(String) }
    func run(label: String, action: @escaping @Sendable (EvidenceArchive, ProgressHandler) throws -> Outcome) {
        guard !busy else { return }
        busy = true; progress = ProgressUpdate(label); message = "Preparing \(label)…"
        let parent = outputRoot
        operation = Task.detached(priority: .userInitiated) { [weak self] in
            let activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled, .idleDisplaySleepDisabled], reason: "Monitor firmware operation")
            defer { ProcessInfo.processInfo.endActivity(activity) }
            var archive: EvidenceArchive?
            do {
                let created = try EvidenceArchive(parent: parent, label: label); archive = created
                await self?.setArchive(created.directory)
                let handler: ProgressHandler = { [weak self] update in Task { @MainActor in self?.progress = update } }
                let outcome = try action(created, handler)
                await self?.finish(outcome)
            } catch {
                try? archive?.log("Operation rejected: " + error.localizedDescription)
                await self?.finish(.message(error.localizedDescription))
            }
        }
    }
    func setArchive(_ url: URL) { archiveURL = url }
    func finish(_ outcome: Outcome) {
        switch outcome {
        case .prepared(let value): preparedWrite = value; confirmWrite = true; message = "Exact offline plan prepared for review."
        case .identification(let value): invalidatePlan(); identifiedTarget = value.target; experimentalReadOnly = false; message = "Identified " + value.firmware + ". " + value.target.identityWarning
        case .input(let value): invalidatePlan(); input = value; message = "Offline image verification passed."
        case .backup(let value): invalidatePlan(); backup = value; message = "Two equal complete SPI reads saved. Coding EEPROM is not included."
        case .message(let value): message = value
        case .execution(let value):
            invalidatePlan()
            message = value.completed ? "Full target verified twice; status unchanged; ISP exit completed." : "Write not completed: \(value.error ?? "unknown error")\nRollback verified: \(value.rollback_verified). Reset allowed: \(value.reset_allowed). ISP exit completed: \(value.isp_exit_completed)."
            if !value.persistence_errors.isEmpty { message += "\nArchive errors: " + value.persistence_errors.joined(separator: "; ") }
        }
        busy = false; operation = nil
    }
    func cancel() { operation?.cancel(); message = "Cancellation requested. An in-progress sector finishes first; any required restoration continues before stopping." }
}

@MainActor final class ApplicationDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if AppModel.shared.busy { AppModel.shared.message = "An operation is active. Use Cancel and wait for completion before quitting."; return .terminateCancel }
        return .terminateNow
    }
}
@main struct MonitorFirmwareApp: App {
    @NSApplicationDelegateAdaptor(ApplicationDelegate.self) private var delegate
    @State private var model = AppModel.shared
    var body: some Scene {
        WindowGroup("Monitor Firmware") {
            ContentView(model: model).frame(minWidth: 960, minHeight: 700)
        }
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}
struct ContentView: View {
    @Bindable var model: AppModel
    var body: some View {
        NavigationSplitView {
            VStack(alignment: .leading, spacing: 12) {
                Picker("I²C driver", selection: $model.providerID) {
                    ForEach(model.providers, id: \.id) { provider in Text(provider.name).tag(provider.id) }
                }.disabled(model.busy)
                .onChange(of: model.providerID) { _, _ in model.invalidatePlan(); model.displays = []; model.routes = []; model.selected = nil; model.selectedRoute = nil; model.identifiedTarget = nil; model.experimentalReadOnly = false; model.message = model.provider.capabilities.reason }
                HStack { Text("External displays").font(.headline); Spacer(); Button("Refresh", systemImage: "arrow.clockwise", action: model.refresh).disabled(model.busy) }
                List(selection: $model.selected) {
                    ForEach(model.displays) { display in
                        VStack(alignment: .leading) { Text(display.name); Text(display.asleep ? "Asleep" : display.active ? "Active" : "Inactive").font(.caption).foregroundStyle(.secondary) }.tag(display.id)
                    }
                }.disabled(model.busy)
                Text("Cached OS data only").font(.caption).foregroundStyle(.secondary)
                Picker("Explicit IOAV route", selection: $model.selectedRoute) {
                    Text("Select route").tag(nil as UInt64?)
                    ForEach(model.routes,id: \.registry_id) { route in Text("\(route.registry_id) · \(route.path.components(separatedBy: "/").last ?? "DCP")").tag(Optional(route.registry_id)) }
                }.disabled(model.busy)
                Button("Identify selected route…") { model.confirmIdentification = true }.disabled(model.busy || model.selectedRoute == nil)
                DisclosureGroup("Cached IOAV routes (unassociated)") {
                    ForEach(model.routes, id: \.registry_id) { route in Text("\(route.registry_id)\n\(route.path)").font(.system(.caption, design: .monospaced)).textSelection(.enabled) }
                }
            }.padding().navigationSplitViewColumnWidth(min: 260, ideal: 310)
        } detail: {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    Text("Monitor Firmware").font(.largeTitle.bold())
                    Label("Hardware ISP transport is not qualified on macOS", systemImage: "exclamationmark.shield").font(.headline).foregroundStyle(.orange)
                    Text(model.provider.capabilities.reason).font(.callout).foregroundStyle(.secondary)
                    HStack {
                        Button(model.experimentalReadOnly ? "Experimental reads enabled for this selection" : "Allow experimental read-only ISP…") { model.confirmReadOnly = true }
                            .disabled(model.busy || model.target?.registryID == nil || model.target?.identity.ambiguous != false || !model.provider.capabilities.experimentalReadOnlyISP)
                        if model.experimentalReadOnly { Button("Disable") { model.experimentalReadOnly = false }.disabled(model.busy) }
                    }
                    if let target = model.target {
                        GroupBox("Selected target") {
                            VStack(alignment: .leading) {
                                Text(target.name).font(.headline)
                                Text("EDID serial: \(target.identity.edid_serial) · \(target.identity.transport_locator)").textSelection(.enabled)
                                Text(target.identityWarning).font(.caption).foregroundStyle(.orange)
                            }.frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    GroupBox("Input firmware") {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack { Text(model.fileURL?.path ?? "No file selected").lineLimit(2).textSelection(.enabled); Spacer(); Button("Browse…", action: model.chooseInput).disabled(model.busy) }
                            Text(model.summary).font(.system(.callout, design: .monospaced)).textSelection(.enabled)
                            Button("Verify file offline", action: model.validateInput).disabled(model.fileURL == nil || model.busy)
                        }.frame(maxWidth: .infinity, alignment: .leading)
                    }
                    GroupBox("Own-monitor backup and archive") {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack { Text(model.backup.map { "Backup serial: " + $0.manifest.identity.edid_serial } ?? "No verified backup loaded"); Spacer(); Button("Load backup.json…", action: model.chooseBackup).disabled(model.busy) }
                            if let backup = model.backup, backup.manifest.identity.ambiguous { Text("Ambiguous backup identity: inspect/verify only. " + backup.manifest.identity.ambiguity_reason).foregroundStyle(.orange).font(.caption) }
                            HStack { Text(model.restoreBackup.map { "Restore target: " + $0.manifest.identity.edid_serial } ?? "No restore target loaded"); Spacer(); Button("Load restore target…",action: model.chooseRestoreBackup).disabled(model.busy) }
                            Text("Archive folder: \(model.outputRoot.path)").font(.caption).textSelection(.enabled)
                            Button("Choose archive folder…", action: model.chooseOutput).disabled(model.busy)
                            Text("Coding EEPROM: unavailable. A SPI dump does not back up the separate coding EEPROM.").font(.caption).foregroundStyle(.secondary)
                        }.frame(maxWidth: .infinity, alignment: .leading)
                    }
                    HStack {
                        Button("Download backup ×2", systemImage: "arrow.down.doc", action: model.download).disabled(model.target == nil || model.busy)
                        Button("Verify monitor ×2", action: model.verifyMonitor).disabled(model.target == nil || model.input == nil || model.busy)
                        Button("Review upload / restore plan…",action: model.prepareWrite).disabled(model.target == nil || model.input == nil || model.backup == nil || model.busy)
                    }
                    if model.busy {
                        ProgressView(model.progress.stage, value: Double(model.progress.completed), total: Double(max(1, model.progress.total)))
                        Button("Cancel safely", action: model.cancel)
                    }
                    Text(model.message).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                    if let url = model.archiveURL { Button("Show evidence archive", systemImage: "folder") { NSWorkspace.shared.open(url) } }
                }.padding(24)
            }
        }
        .sheet(isPresented: $model.confirmWrite) {
            if let prepared = model.preparedWrite { WriteReviewSheet(prepared: prepared,execute: model.upload,cancel: { model.confirmWrite = false }) }
        }
        .confirmationDialog("Enable experimental read-only ISP", isPresented: $model.confirmReadOnly, titleVisibility: .visible) {
            Button("Enable read-only candidate") { model.experimentalReadOnly = true }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This enables only an explicit Download or Verify action for the selected target. It may enter ISP and temporarily interrupt display operation. Remote DCP raw I²C behavior is not qualified. Erase/program remain blocked. Exit/reset is sent only after complete image and status verification. An ambiguous identity or unresolved IOAV route still blocks access.")
        }
        .confirmationDialog("Identify the selected physical monitor", isPresented: $model.confirmIdentification, titleVisibility: .visible) {
            Button("Monitor is awake — identify this route", action: model.identifySelectedRoute)
            Button("Cancel",role: .cancel) {}
        } message: {
            Text("Only the selected IOAV registry ID is accessed. This sends the fixed Samsung C3/0 firmware query and two bounded EDID reads; it does not enter ISP or change settings. Confirm that the monitor attached to this route is awake. Identity ambiguity is retained and blocks ISP access.")
        }
        .onChange(of: model.selected) { _, _ in model.invalidatePlan(); model.experimentalReadOnly = false; model.identifiedTarget = nil }
        .onChange(of: model.selectedRoute) { _, _ in model.invalidatePlan(); model.experimentalReadOnly = false; model.identifiedTarget = nil }
    }
}

struct WriteReviewSheet: View {
    let prepared: AppModel.PreparedWrite
    let execute: () -> Void
    let cancel: () -> Void
    var body: some View {
        VStack(alignment: .leading,spacing: 16) {
            Text("Review exact write plan").font(.title.bold())
            Text(prepared.target.name).font(.headline)
            Text("Unit: \(prepared.target.identity.edid_serial)\nRoute: \(prepared.target.identity.transport_locator)\nDriver: \(prepared.providerID)").textSelection(.enabled)
            ScrollView {
                VStack(alignment: .leading,spacing: 12) {
                    Text("Source full SHA-256\n" + prepared.plan.sourceHash)
                    Text("Target full SHA-256\n" + prepared.plan.targetHash)
                    Text("Plan SHA-256\n" + prepared.plan.planHash)
                    Text("Mode: \(prepared.plan.summary.mode)\nChanged sectors: \(prepared.plan.changedSectors.count)\nHeader sector 0x010000 is last when changed.")
                    DisclosureGroup("Exact sector order") {
                        ForEach(Array(prepared.plan.changedSectors.enumerated()),id: \.offset) { entry in
                            Text(String(format: "%d. 0x%06X",entry.offset+1,entry.element)).frame(maxWidth: .infinity,alignment: .leading)
                        }
                    }
                }.font(.system(.callout,design: .monospaced)).textSelection(.enabled).frame(maxWidth: .infinity,alignment: .leading)
            }
            Text("This immutable plan will be passed unchanged to execution. Hardware writes are currently unqualified and will be rejected before ISP access.").foregroundStyle(.orange)
            HStack { Spacer(); Button("Cancel",action: cancel); Button("Execute this plan",role: .destructive,action: execute) }
        }.padding(24).frame(width: 740,height: 600)
    }
}
