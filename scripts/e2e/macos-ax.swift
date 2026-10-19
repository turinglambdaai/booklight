import AppKit
import ApplicationServices
import Foundation

enum DriverError: Error, CustomStringConvertible {
    case failure(String)
    var description: String { switch self { case .failure(let message): return message } }
}
func attribute(_ element: AXUIElement, _ name: String) -> CFTypeRef? { var value: CFTypeRef?; return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil }
func stringAttribute(_ element: AXUIElement, _ name: String) -> String { attribute(element, name) as? String ?? "" }
func children(_ element: AXUIElement) -> [AXUIElement] { attribute(element, kAXChildrenAttribute) as? [AXUIElement] ?? [] }
func walk(_ element: AXUIElement) -> [AXUIElement] { [element] + children(element).flatMap(walk) }
func contains(_ root: AXUIElement, _ fragment: String) -> Bool { walk(root).contains { node in [kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute, kAXHelpAttribute].contains { stringAttribute(node, $0).contains(fragment) } } }
func waitFor<T>(_ description: String, timeout: TimeInterval = 90, probe: () -> T?) throws -> T { let deadline = Date().addingTimeInterval(timeout); while Date() < deadline { if let value = probe() { return value }; Thread.sleep(forTimeInterval: 0.05) }; throw DriverError.failure("timed out waiting for \(description)") }
func snapshot(_ element: AXUIElement, depth: Int = 0) -> [String: Any] { var result: [String: Any] = ["role": stringAttribute(element, kAXRoleAttribute), "identifier": stringAttribute(element, kAXIdentifierAttribute), "title": stringAttribute(element, kAXTitleAttribute), "label": stringAttribute(element, kAXDescriptionAttribute), "value": stringAttribute(element, kAXValueAttribute)]; if depth < 10 { result["children"] = children(element).map { snapshot($0, depth: depth + 1) } }; return result }
func writeJSON(_ value: Any, to url: URL) throws { try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]).write(to: url) }

var diagnosticApplication: AXUIElement?
var diagnosticOutput: URL?
do {
    let arguments = CommandLine.arguments
    guard let pidIndex = arguments.firstIndex(of: "--pid"), pidIndex + 1 < arguments.count, let pid = Int32(arguments[pidIndex + 1]), let outputIndex = arguments.firstIndex(of: "--output"), outputIndex + 1 < arguments.count else { throw DriverError.failure("usage: macos-ax --pid PID --output DIRECTORY") }
    let output = URL(fileURLWithPath: arguments[outputIndex + 1], isDirectory: true); diagnosticOutput = output
    try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
    guard AXIsProcessTrusted() else { throw DriverError.failure("accessibility access is unavailable") }
    let application = AXUIElementCreateApplication(pid); diagnosticApplication = application
    let window: AXUIElement = try waitFor("Booklight window") { (attribute(application, kAXWindowsAttribute) as? [AXUIElement])?.first }
    _ = AXUIElementSetAttributeValue(application, kAXFrontmostAttribute as CFString, kCFBooleanTrue)
    _ = AXUIElementPerformAction(window, kAXRaiseAction as CFString)
    _ = try waitFor("EPUB title parsed by the embedded backend") { contains(window, "The Sample Book") ? true : nil }
    _ = try waitFor("EPUB table of contents parsed by the embedded backend") { contains(window, "First Chapter") ? true : nil }
    try writeJSON(snapshot(application), to: output.appendingPathComponent("accessibility.json"))
    try writeJSON(["platform": "macos", "actions": ["launch:sample.epub"], "assertions": ["packaged SwiftUI window activated", "sample EPUB crossed RVT1 into the embedded backend", "native accessibility tree contains The Sample Book", "native accessibility tree contains First Chapter"]], to: output.appendingPathComponent("interaction-trace.json"))
    guard let running = NSRunningApplication(processIdentifier: pid), running.terminate() else { throw DriverError.failure("native application rejected termination") }
} catch {
    if let application = diagnosticApplication, let output = diagnosticOutput { try? writeJSON(snapshot(application), to: output.appendingPathComponent("accessibility-failure.json")) }
    FileHandle.standardError.write(Data("Booklight macOS acceptance failed: \(error)\n".utf8)); exit(1)
}
