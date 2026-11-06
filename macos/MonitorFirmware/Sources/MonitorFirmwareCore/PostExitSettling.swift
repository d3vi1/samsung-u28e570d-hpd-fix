import Foundation

public struct IdentificationTransientError: Error, LocalizedError, Sendable {
    public let stage: String
    public let detail: String
    public let frame: Data?
    public init(stage: String, detail: String, frame: Data? = nil) { self.stage = stage; self.detail = detail; self.frame = frame }
    public var errorDescription: String? { stage + ": " + detail }
}
public struct PostExitRetryPolicy: Sendable {
    public let maximumAttempts: Int
    public let delaySeconds: Double
    public let maximumElapsedSeconds: Double
    public init(maximumAttempts: Int = 6, delaySeconds: Double = 2, maximumElapsedSeconds: Double = 12) {
        self.maximumAttempts = maximumAttempts; self.delaySeconds = delaySeconds; self.maximumElapsedSeconds = maximumElapsedSeconds
    }
    func validate() throws {
        try require((1...6).contains(maximumAttempts) && delaySeconds >= 0 && delaySeconds <= 2 &&
                    maximumElapsedSeconds > 0 && maximumElapsedSeconds <= 12, "Post-exit settling must remain within the bounded retry policy.")
    }
}
private struct IdentityAttempt: Codable {
    let attempt: Int
    let utc: String
    let outcome: String
    let detail: String?
    let frame_hex: String?
    let identity: MonitorIdentity?
}
enum PostExitSettling {
    static func confirm(device: any FlashDevice, selected: MonitorIdentity, archive: EvidenceArchive, policy: PostExitRetryPolicy) throws {
        try policy.validate()
        let deadline = ProcessInfo.processInfo.systemUptime + policy.maximumElapsedSeconds
        for attempt in 1...policy.maximumAttempts {
            let utc = ISO8601DateFormatter().string(from: Date())
            let name = String(format: "post-exit-identity-%02d.json",attempt)
            let identity: MonitorIdentity
            do { identity = try device.identity() }
            catch let transient as IdentificationTransientError {
                try archive.save(name,value: IdentityAttempt(attempt: attempt,utc: utc,outcome: "transient-transport-or-framing-error",detail: transient.localizedDescription,frame_hex: transient.frame.map(hex),identity: nil))
                if attempt == policy.maximumAttempts || ProcessInfo.processInfo.systemUptime + policy.delaySeconds >= deadline { throw transient }
                Thread.sleep(forTimeInterval: policy.delaySeconds)
                continue
            } catch {
                try archive.save(name,value: IdentityAttempt(attempt: attempt,utc: utc,outcome: "fatal-identification-error",detail: error.localizedDescription,frame_hex: nil,identity: nil))
                throw error
            }
            let matches = selected.matchesUnit(identity) && selected.transport_locator == identity.transport_locator
            try archive.save(name,value: IdentityAttempt(attempt: attempt,utc: utc,outcome: matches ? "identity-matches" : "valid-wrong-identity",detail: nil,frame_hex: nil,identity: identity))
            // A valid wrong identity is never retried or accepted as startup settling.
            try require(matches,"Post-exit target identity differs from the selected monitor.")
            return
        }
        throw FirmwareError.rejected("Post-exit identification attempts exhausted.")
    }
}
