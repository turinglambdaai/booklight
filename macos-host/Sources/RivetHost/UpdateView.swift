import SwiftUI
import AppKit

// Update sheet driving the signed-manifest updater. The Racket backend
// verifies and downloads; this native adapter owns installation (mount the
// DMG, replace the app, relaunch) — the payback family pattern.

// Wire shapes of the backend's check-updates / update-state jsexpr payloads.
struct UpdateCheckResult: Codable {
    let status: String
    let message: String?
    let currentVersion: String?
    let availableVersion: String?
    let publishedAt: String?
    let installer: String?
    let sizeBytes: Int64?
}

struct UpdateState: Codable {
    let phase: String        // idle | checking | downloading | downloaded | error
    let percent: Int
    let message: String?
    let downloadedPath: String?
    let availableVersion: String?

    static let idle = UpdateState(phase: "idle", percent: 0, message: nil,
                                  downloadedPath: nil, availableVersion: nil)

    static let upToDate = UpdateState.idle

    static func error(_ message: String) -> UpdateState {
        UpdateState(phase: "error", percent: 0, message: message,
                    downloadedPath: nil, availableVersion: nil)
    }
}

struct UpdateView: View {
    @EnvironmentObject private var model: PDFGistModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "arrow.down.circle")
                .font(.system(size: 44))
                .foregroundStyle(Theme.accent)

            switch model.updateState.phase {
            case "checking":
                ProgressView()
                Text(L10n.t("ui.update.checking"))

            case "downloading":
                Text(L10n.t("ui.update.downloading"))
                ProgressView(value: Double(model.updateState.percent), total: 100)
                    .frame(width: 260)
                Text("\(model.updateState.percent)%")
                    .font(.caption)
                    .foregroundStyle(Theme.textDim)

            case "downloaded":
                Text(L10n.t("ui.update.available"))
                    .font(.headline)
                if let version = model.updateState.availableVersion {
                    Text("PDFGist \(version)")
                        .font(.title3)
                }
                if let size = model.updateInfo?.sizeBytes {
                    Text(Self.size(size))
                        .font(.caption)
                        .foregroundStyle(Theme.textDim)
                }
                Button(L10n.t("ui.update.install-now")) {
                    model.installUpdate()
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)

            case "error":
                Text("⚠️ " + L10n.t("ui.update.error"))
                    .font(.headline)
                Text(model.updateState.message ?? "")
                    .font(.caption)
                    .foregroundStyle(Theme.textDim)
                    .textSelection(.enabled)
                    .frame(maxWidth: 340)
                    .lineLimit(6)
                HStack {
                    Button(L10n.t("ui.update.retry")) { model.checkForUpdates() }
                    Button(L10n.t("ui.update.close")) { dismiss() }
                        .keyboardShortcut(.defaultAction)
                }

            default: // idle: either a verified candidate is waiting or up-to-date
                if model.updateInfo?.status == "available" {
                    Text(L10n.t("ui.update.available"))
                        .font(.headline)
                    if let version = model.updateInfo?.availableVersion {
                        Text("PDFGist \(version)")
                            .font(.title3)
                    }
                    if let size = model.updateInfo?.sizeBytes {
                        Text(Self.size(size))
                            .font(.caption)
                            .foregroundStyle(Theme.textDim)
                    }
                    Button(L10n.t("ui.update.download")) {
                        model.startDownload()
                    }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
                } else {
                    Text("✅ " + L10n.t("ui.update.up-to-date"))
                    Button(L10n.t("ui.update.close")) { dismiss() }
                        .keyboardShortcut(.defaultAction)
                }
            }
        }
        .padding(28)
        .frame(width: 380)
        .tint(Theme.accent)
    }

    private static func size(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}

// Native installation adapter: replaces the running .app with the freshly
// downloaded one and relaunches. The previous bundle stays beside the new
// one as a rollback copy: restored if the copy fails, and removed on the
// next launch that runs from the target bundle (rollback no longer needed).
enum UpdaterInstaller {
    /// Running from the installation target means the last update worked:
    /// its rollback copy is stale and can go.
    static func cleanupStaleBackup() {
        let targetURL = installationTargetURL()
        guard Bundle.main.bundleURL.standardizedFileURL.path
                == targetURL.standardizedFileURL.path else { return }
        try? FileManager.default.removeItem(at: targetURL.appendingPathExtension("old"))
    }

    /// Dispatch on the downloaded artifact's container. The family update
    /// feed serves portable zips; DMG manifests stay supported.
    static func install(artifactAt artifactURL: URL) throws {
        switch artifactURL.pathExtension.lowercased() {
        case "zip": try installZip(artifactURL)
        default:    try installDmg(artifactURL)
        }
    }

    static func installDmg(_ dmgURL: URL) throws {
        let fileManager = FileManager.default
        let mountPoint = fileManager.temporaryDirectory
            .appendingPathComponent("pdfgist-update-\(UUID().uuidString)")

        try fileManager.createDirectory(at: mountPoint, withIntermediateDirectories: true)
        defer { try? run("/usr/bin/hdiutil", ["detach", mountPoint.path, "-force"]) }

        try run("/usr/bin/hdiutil", ["attach", "-nobrowse", "-readonly",
                                     "-mountpoint", mountPoint.path,
                                     dmgURL.path])

        let contents = try fileManager.contentsOfDirectory(at: mountPoint,
                                                           includingPropertiesForKeys: nil)
        guard let appURL = contents.first(where: { $0.pathExtension == "app" }) else {
            throw InstallerError.noAppFound(mountPoint.path)
        }

        try swapIn(appURL)
        finishInstall()
    }

    /// Portable-zip manifests (the family feed format): unpack with ditto,
    /// which preserves the bundle's metadata and signature, and install the
    /// .app found inside — same payload the DMG carries.
    static func installZip(_ zipURL: URL) throws {
        let fileManager = FileManager.default
        let unpackDir = fileManager.temporaryDirectory
            .appendingPathComponent("pdfgist-update-\(UUID().uuidString)")

        try fileManager.createDirectory(at: unpackDir, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: unpackDir) }

        try run("/usr/bin/ditto", ["-x", "-k", zipURL.path, unpackDir.path])

        let contents = try fileManager.contentsOfDirectory(at: unpackDir,
                                                           includingPropertiesForKeys: nil)
        guard let appURL = contents.first(where: { $0.pathExtension == "app" }) else {
            throw InstallerError.noAppFound(unpackDir.path)
        }

        try swapIn(appURL)
        finishInstall()
    }

    /// Backup → copy → rollback-on-failure, shared by both container paths.
    private static func swapIn(_ appURL: URL) throws {
        let fileManager = FileManager.default
        let targetURL = installationTargetURL()
        let backupURL = targetURL.appendingPathExtension("old")
        let hadPrevious = fileManager.fileExists(atPath: targetURL.path)
        if hadPrevious {
            try? fileManager.removeItem(at: backupURL)
            try fileManager.moveItem(at: targetURL, to: backupURL)
        }

        do {
            try fileManager.copyItem(at: appURL, to: targetURL)
        } catch {
            // put the previous version back before surfacing the failure
            if hadPrevious {
                try? fileManager.moveItem(at: backupURL, to: targetURL)
            }
            throw error
        }
    }

    /// The artifact arrived over the network, so the quarantine xattr rides
    /// into the copied bundle and Gatekeeper would block the very update
    /// the user just approved. The backend already verified the artifact
    /// (Ed25519 manifest + SHA-256) — clearing it here is safe. Then
    /// relaunch from the new bundle and end the old process.
    private static func finishInstall() {
        try? run("/usr/bin/xattr", ["-cr", installationTargetURL().path])
        NSWorkspace.shared.open(installationTargetURL())
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            NSApp.terminate(nil)
        }
    }

    private static func installationTargetURL() -> URL {
        let bundle = Bundle.main.bundleURL
        let applications = URL(fileURLWithPath: "/Applications")
        // already installed in /Applications: replace in place
        if bundle.deletingLastPathComponent() == applications {
            return bundle
        }
        return applications.appendingPathComponent("PDFGist.app")
    }

    @discardableResult
    static func run(_ launchPath: String, _ arguments: [String]) throws -> String {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: launchPath)
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        try process.run()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        guard process.terminationStatus == 0 else {
            let output = String(data: data, encoding: .utf8) ?? ""
            throw InstallerError.commandFailed(launchPath, process.terminationStatus, output)
        }
        return String(data: data, encoding: .utf8) ?? ""
    }

    enum InstallerError: LocalizedError {
        case noAppFound(String)
        case commandFailed(String, Int32, String)

        var errorDescription: String? {
            switch self {
            case .noAppFound(let path):
                return "no .app bundle found in \(path)"
            case .commandFailed(let command, let status, let output):
                return "\(command) exited \(status): \(output)"
            }
        }
    }
}
