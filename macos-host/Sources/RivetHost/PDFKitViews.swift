import SwiftUI
import AppKit
import PDFKit

/// Hosts the tab's long-lived PDFView. Each PDFTab owns exactly one PDFView so
/// selection, zoom and scroll survive tab switches; the representable only
/// re-attaches it to the hierarchy and forwards PDFKit notifications.
struct PDFViewport: NSViewRepresentable {
    @ObservedObject var tab: PDFTab

    func makeCoordinator() -> Coordinator { Coordinator(tab: tab) }

    func makeNSView(context: Context) -> GistPDFView {
        context.coordinator.install()
        if !tab.didInitialLayout {
            tab.didInitialLayout = true
            DispatchQueue.main.async { [weak tab] in
                guard let tab else { return }
                tab.fitWidth()
                if tab.resumePage > 1 {
                    tab.goToPage(tab.resumePage)
                    tab.resumePage = 1
                }
            }
        }
        return tab.pdfView
    }

    func updateNSView(_ view: GistPDFView, context: Context) {}

    final class Coordinator {
        private let tab: PDFTab
        private var observers: [NSObjectProtocol] = []

        init(tab: PDFTab) { self.tab = tab }

        func install() {
            guard observers.isEmpty else { return }
            let center = NotificationCenter.default
            let tab = self.tab
            observers.append(center.addObserver(
                forName: .PDFViewPageChanged, object: tab.pdfView, queue: .main
            ) { _ in
                MainActor.assumeIsolated { tab.reader?.pageChanged(tab) }
            })
            observers.append(center.addObserver(
                forName: .PDFViewScaleChanged, object: tab.pdfView, queue: .main
            ) { _ in
                MainActor.assumeIsolated { tab.reader?.scaleChanged(tab) }
            })
        }

        deinit {
            for observer in observers { NotificationCenter.default.removeObserver(observer) }
        }
    }
}

/// Horizontal PDFKit filmstrip bound to the tab's PDFView.
struct ThumbnailStrip: NSViewRepresentable {
    let tab: PDFTab

    func makeNSView(context: Context) -> PDFThumbnailView {
        let view = PDFThumbnailView()
        configure(view)
        return view
    }

    func updateNSView(_ view: PDFThumbnailView, context: Context) {
        if view.pdfView !== tab.pdfView { configure(view) }
    }

    private func configure(_ view: PDFThumbnailView) {
        view.pdfView = tab.pdfView
        // macOS PDFThumbnailView is a grid; one column reads like the classic
        // sidebar filmstrip.
        view.maximumNumberOfColumns = 1
        view.thumbnailSize = CGSize(width: 92, height: 118)
        view.backgroundColor = .clear
    }
}

/// Right-hand split view (v1 viewer-split): mirrors the main PDFView's
/// scroll position by ratio while the main view scrolls.
struct SplitViewport: NSViewRepresentable {
    @ObservedObject var tab: PDFTab

    func makeNSView(context: Context) -> GistPDFView {
        tab.splitPdfView
    }

    // install here, not in makeNSView: the clip view only exists once the
    // view is in the window and has laid out.
    func updateNSView(_ view: GistPDFView, context: Context) {
        context.coordinator.install()
    }

    func makeCoordinator() -> Coordinator { Coordinator(tab: tab) }

    @MainActor
    final class Coordinator {
        private let tab: PDFTab
        // NotificationCenter.removeObserver is thread-safe; the token is
        // only cleared from deinit.
        nonisolated(unsafe) private var observer: NSObjectProtocol?

        init(tab: PDFTab) { self.tab = tab }

        func install() {
            guard observer == nil,
                  let mainClip = Self.clipView(tab.pdfView) else { return }
            mainClip.postsBoundsChangedNotifications = true
            observer = NotificationCenter.default.addObserver(
                forName: NSView.boundsDidChangeNotification, object: mainClip, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.sync() }
            }
        }

        private func sync() {
            guard tab.splitOn,
                  let mainClip = Self.clipView(tab.pdfView),
                  let splitClip = Self.clipView(tab.splitPdfView),
                  let mainDoc = mainClip.documentView,
                  let splitDoc = splitClip.documentView else { return }
            let mainSpan = mainDoc.bounds.height - mainClip.bounds.height
            let ratio = mainSpan > 0 ? max(0, min(1, mainClip.bounds.origin.y / mainSpan)) : 0
            let splitSpan = splitDoc.bounds.height - splitClip.bounds.height
            splitClip.setBoundsOrigin(NSPoint(x: 0, y: max(0, ratio * splitSpan)))
        }

        deinit {
            if let observer { NotificationCenter.default.removeObserver(observer) }
        }

        private static func clipView(_ view: NSView) -> NSClipView? {
            view.subviews.compactMap { $0 as? NSClipView }.first
        }
    }
}
