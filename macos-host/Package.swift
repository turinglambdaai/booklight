// swift-tools-version: 6.0

import Foundation
import PackageDescription

guard let rivetRoot = ProcessInfo.processInfo.environment["RIVET_ROOT"],
      !rivetRoot.isEmpty else {
    fatalError("RIVET_ROOT is not set. Build this app through raco rivet build/dev.")
}

guard let racketFrameworkDir = ProcessInfo.processInfo.environment["RIVET_RACKET_FRAMEWORK_DIR"],
      !racketFrameworkDir.isEmpty else {
    fatalError("RIVET_RACKET_FRAMEWORK_DIR is not set. Build this app through raco rivet build/dev.")
}

let macosMinVersion = ProcessInfo.processInfo.environment["RIVET_MACOS_MIN_VERSION"] ?? "14.0"

let package = Package(
    name: "RivetHost",
    platforms: [.macOS(macosMinVersion)],
    dependencies: [
        // The root rivet package (issue rivet#238): consumed as a package,
        // never as compiled-in sources.
        .package(path: rivetRoot)
    ],
    targets: [
        .executableTarget(
            name: "RivetHost",
            dependencies: [
                .product(name: "RivetRuntime", package: "rivet"),
                .product(name: "RivetDevice", package: "rivet"),
                .product(name: "RivetEmbedding", package: "rivet"),
                .product(name: "RivetSystem", package: "rivet")
            ],
            path: "Sources/RivetHost",
            linkerSettings: [
                .unsafeFlags(["-F", racketFrameworkDir]),
                .linkedFramework("Racket")
            ]
        )
    ]
)
