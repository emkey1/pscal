import Foundation
import UIKit
import Darwin

// Forward declaration of C function
@_silgen_name("PSCALRuntimeApplyPathTruncation")
func PSCALRuntimeApplyPathTruncation(_ path: UnsafePointer<CChar>?)

struct PathTruncationPreferences {
    static let enabledDefaultsKey = "com.pscal.terminal.pathTruncateEnabled"
    static let pathDefaultsKey = "com.pscal.terminal.pathTruncateValue"
}

final class PathTruncationManager {
    static let shared = PathTruncationManager()

    private init() { }

    func normalize(_ path: String) -> String {
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            return ""
        }

        let absolute = trimmed.hasPrefix("/") ? trimmed : "/" + trimmed
        let resolved = URL(fileURLWithPath: absolute).resolvingSymlinksInPath().path
        var normalized = resolved
        if normalized.isEmpty {
            return ""
        }
        if normalized == "/" {
            return normalized
        }

        while normalized.count > 1 && normalized.hasSuffix("/") {
            normalized.removeLast()
        }
        return normalized
    }

    func defaultDocumentsPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first
        return normalize(docs?.path ?? "")
    }

    func apply(enabled: Bool, path: String) {
        if enabled {
            let normalized = normalize(path)
            if !normalized.isEmpty {
                withCStringPointer(normalized) { PSCALRuntimeApplyPathTruncation($0) }
                let tmpURL = URL(fileURLWithPath: normalized, isDirectory: true)
                    .appendingPathComponent("tmp", isDirectory: true)
                try? FileManager.default.createDirectory(at: tmpURL,
                                                         withIntermediateDirectories: true,
                                                         attributes: nil)
                return
            }
        }

        PSCALRuntimeApplyPathTruncation(nil)
    }

    func applyStoredPreference() {
        let defaults = UserDefaults.standard
        let storedPath = defaults.string(
            forKey: PathTruncationPreferences.pathDefaultsKey
        )

        let normalizedPath: String
        if let storedPath, !storedPath.isEmpty {
            normalizedPath = normalize(storedPath)
        } else {
            normalizedPath = normalize(defaultDocumentsPath())
        }

        let enabled: Bool
        if defaults.object(forKey: PathTruncationPreferences.enabledDefaultsKey) != nil {
            enabled = defaults.bool(forKey: PathTruncationPreferences.enabledDefaultsKey)
        } else {
            enabled = false
        }

        apply(enabled: enabled, path: normalizedPath)
    }
}
