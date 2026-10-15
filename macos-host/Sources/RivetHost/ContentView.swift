import SwiftUI
import AppKit
import PDFKit
import WebKit
import UniformTypeIdentifiers

/// One window = one document (Preview model). The window group hands us a
/// path (opened file) or nil (welcome state); each window owns a ReaderModel
/// for its lifetime.
struct DocumentWindowView: View {
    @EnvironmentObject private var app: AppModel
    @Environment(\.openWindow) private var openWindowEnv
    let path: String?

    @State private var reader: ReaderModel?

    var body: some View {
        ZStack {
            if let reader {
                ReaderWindowView(reader: reader)
            } else {
                Theme.bg.frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .task {
            guard reader == nil else { return }
            app.start()
            let model = ReaderModel(app: app)
            reader = model
            app.register(model)
            if let path {
                await model.openPath(path)
            } else if let launch = app.claimPendingLaunch() {
                await model.openPath(launch)
            }
        }
        .onChange(of: path) { newPath in
            guard let newPath, let reader,
                  reader.documentPath != newPath else { return }
            Task { await reader.openPath(newPath) }
        }
    }
}

/// Bridges the SwiftUI window to AppKit: document title + proxy icon, native
/// window tabbing (Preview model), key-window tracking for the menus, and
/// close-time flush of pending persistence.
struct WindowAccessor: NSViewRepresentable {
    @ObservedObject var reader: ReaderModel
    let app: AppModel

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSView {
        context.coordinator.attach()
        return NSView()
    }

    func updateNSView(_ view: NSView, context: Context) {
        context.coordinator.parent = self
        // attach() is idempotent: the window may not be key/visible yet at
        // first layout, so keep retrying until it latches.
        context.coordinator.attach()
        context.coordinator.refreshTitle()
    }

    @MainActor
    final class Coordinator {
        var parent: WindowAccessor
        private weak var window: NSWindow?
        // NotificationCenter.removeObserver is thread-safe; the tokens are
        // only touched from install (main actor) and deinit (nonisolated).
        nonisolated(unsafe) private var observers: [NSObjectProtocol] = []

        init(_ parent: WindowAccessor) {
            self.parent = parent
        }

        func attach() {
            guard window == nil else { return }
            let candidate = NSApp.keyWindow
                ?? NSApp.windows.first { $0.isVisible && $0.tabbingIdentifier == "site.jrtx.pdfgist.reader" }
                ?? NSApp.windows.first(where: \.isVisible)
            guard let window = candidate else { return }
            self.window = window
            // Native window tabs: share a tabbing identity so Window >
            // Merge All Windows works and the tab bar's + button offers the
            // welcome state, exactly like Preview's document windows.
            window.tabbingMode = .preferred
            window.tabbingIdentifier = "site.jrtx.pdfgist.reader"
            let center = NotificationCenter.default
            observers.append(center.addObserver(
                forName: NSWindow.didBecomeKeyNotification, object: window, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.parent.app.activeReader = self.parent.reader
                }
            })
            observers.append(center.addObserver(
                forName: NSWindow.willCloseNotification, object: window, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.parent.reader.closeDocument()
                    self.parent.app.unregister(self.parent.reader)
                }
            })
            refreshTitle()
        }

        func refreshTitle() {
            // SwiftUI's navigationTitle owns window.title; this only keeps
            // the proxy icon (drag to Finder/Dock) in step with the document.
            guard let window else { return }
            if let path = parent.reader.documentPath {
                let url = URL(fileURLWithPath: path)
                if window.representedURL != url {
                    window.representedURL = url
                }
            } else if window.representedURL != nil {
                window.representedURL = nil
            }
        }

        deinit {
            for observer in observers { NotificationCenter.default.removeObserver(observer) }
        }
    }
}

// MARK: - reader window

struct ReaderWindowView: View {
    @EnvironmentObject private var app: AppModel
    @ObservedObject var reader: ReaderModel
    @Environment(\.openWindow) private var openWindowEnv
    @State private var sidebarVisibility: NavigationSplitViewVisibility = .automatic
    @State private var isKeyWindow = false

    var body: some View {
        NavigationSplitView(columnVisibility: $sidebarVisibility) {
            ReaderSidebar(reader: reader)
                .navigationSplitViewColumnWidth(min: 180, ideal: 232, max: 440)
        } detail: {
            detail
        }
        .inspector(isPresented: $reader.aiVisible) {
            AISidebar(reader: reader)
        }
        .toolbar { readerToolbar }
        // The SwiftUI-owned window title (macOS: navigationTitle drives the
        // title bar and native tab labels).
        .navigationTitle(reader.documentTitle ?? RivetGeneratedConfig.displayName)
        .environmentObject(reader)
        .background(WindowAccessor(reader: reader, app: app))
        .overlay(alignment: .top) {
            if !app.status.isEmpty && !app.ready {
                backendBanner
            }
        }
        .overlay {
            if reader.findVisible {
                FindBarView(reader: reader)
                    .padding(.top, 4)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .animation(.easeInOut(duration: 0.15), value: reader.findVisible)
        .background(Theme.bg)
        .foregroundStyle(Theme.text)
        .dropDestination(for: URL.self) { urls, _ in
            dropOpened(urls)
            return true
        }
        .alert(reader.alertText, isPresented: $reader.showAlert) {}
        .sheet(item: $reader.passwordPrompt) { prompt in
            PasswordSheet(reader: reader, wrong: prompt.wrong)
                .interactiveDismissDisabled()
        }
        // Updater sheet only in the key window, so it never ghosts into
        // every open document at once.
        .sheet(isPresented: Binding(
            get: { isKeyWindow && app.showUpdateSheet },
            set: { app.showUpdateSheet = $0 })) {
            UpdateView()
        }
        .onAppear {
            app.openWindow = { value in
                if let value {
                    openWindowEnv(value: value)
                } else {
                    openWindowEnv(id: "reader")
                }
            }
        }
        .onReceive(NotificationCenter.default.publisher(
            for: NSWindow.didBecomeKeyNotification)) { notification in
            isKeyWindow = (notification.object as? NSWindow) === NSApp.keyWindow
        }
    }

    private func dropOpened(_ urls: [URL]) {
        let paths = urls
            .filter { ["pdf", "epub"].contains($0.pathExtension.lowercased()) }
            .map { $0.path }
        guard !paths.isEmpty else { return }
        if !reader.hasDocument {
            Task { await reader.openPath(paths[0]) }
            for path in paths.dropFirst() { app.openWindow?(path) }
        } else {
            for path in paths { app.openWindow?(path) }
        }
    }

    private var backendBanner: some View {
        Text(app.status)
            .font(.footnote)
            .foregroundStyle(Theme.danger)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 4)
            .background(Theme.accentSoft)
    }

    @ViewBuilder
    private var detail: some View {
        if reader.epubActive, let epub = reader.epub {
            EpubViewport(tab: epub)
                .ignoresSafeArea()
        } else if let tab = reader.pdf {
            CenteredDocument(tab: tab)
        } else {
            WelcomeView(reader: reader)
        }
    }

    @ToolbarContentBuilder
    private var readerToolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .primaryAction) {
            if let epub = reader.epub, reader.epubActive {
                PageNavControl(
                    current: epub.currentPage, total: max(epub.chapters, 1),
                    jump: { epub.goToChapter($0) })
            } else if let tab = reader.pdf {
                PageNavControl(
                    current: tab.currentPage, total: tab.pageCount,
                    jump: { tab.goToPage($0) })
                zoomMenu(tab)
                toolbarIcon("rectangle.split.2x1", active: tab.splitOn,
                            help: L10n.t("ui.toolbar.split")) {
                    reader.toggleSplit()
                }
            }
            toolbarIcon("magnifyingglass", active: reader.findVisible,
                        help: L10n.t("ui.toolbar.find")) {
                if reader.findVisible {
                    reader.closeFind()
                } else if reader.hasDocument {
                    reader.findVisible = true
                }
            }
            .disabled(!reader.hasDocument)
            if let tab = reader.pdf {
                toolbarIcon(
                    tab.isBookmarked ? "bookmark.fill" : "bookmark",
                    active: tab.isBookmarked,
                    help: L10n.t("ui.toolbar.bookmark")) {
                    reader.toggleBookmark()
                }
            }
            toolbarIcon("speaker.wave.2", active: reader.ttsSpeaking,
                        help: L10n.t("ui.toolbar.tts")) {
                reader.toggleTTS()
            }
            .disabled(!reader.hasDocument)
            toolbarIcon("printer", active: false, help: L10n.t("ui.menu.print")) {
                reader.printDocument()
            }
            .disabled(reader.pdf == nil)
            toolbarIcon("sparkles", active: reader.aiVisible,
                        help: L10n.t("ui.toolbar.ai")) {
                reader.aiVisible.toggle()
            }
        }
    }

    /// Zoom cluster in the Preview form: a percent label that opens the zoom
    /// menu, sitting between the page control and the view toggles.
    @ViewBuilder
    private func zoomMenu(_ tab: PDFTab) -> some View {
        Menu {
            Button(L10n.t("ui.menu.zoom-in")) { HostActions.zoomIn?() }
                .keyboardShortcut("+", modifiers: .command)
            Button(L10n.t("ui.menu.zoom-out")) { HostActions.zoomOut?() }
                .keyboardShortcut("-", modifiers: .command)
            Divider()
            Button(L10n.t("ui.menu.actual-size")) { HostActions.zoomReset?() }
            Button(L10n.t("ui.viewer.fit")) { HostActions.fitWidth?() }
        } label: {
            Text(verbatim: "\(tab.scalePercent)%")
                .font(.system(size: 12, weight: .medium).monospacedDigit())
                .foregroundStyle(Theme.textDim)
                .frame(minWidth: 40)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.visible)
        .fixedSize()
    }

    private func toolbarIcon(_ symbol: String, active: Bool, help: String,
                             action: @escaping () -> Void) -> some View {
        Button {
            action()
        } label: {
            Image(systemName: symbol)
                .font(.system(size: 13))
                .frame(width: 26, height: 24)
        }
        .buttonStyle(.plain)
        .foregroundStyle(active ? Theme.accent : Theme.textDim)
        .help(help)
    }
}

/// Toolbar page navigation in the Preview form: ‹ [page] / N ›.
struct PageNavControl: View {
    let current: Int
    let total: Int
    let jump: (Int) -> Void
    @State private var pageInput = "1"
    @FocusState private var fieldFocused: Bool

    var body: some View {
        HStack(spacing: 2) {
            Button {
                jump(max(1, current - 1))
            } label: {
                Image(systemName: "chevron.left")
                    .font(.system(size: 11, weight: .semibold))
            }
            .buttonStyle(.plain)
            .foregroundStyle(current > 1 ? Theme.textDim : Theme.textFaint)
            .disabled(current <= 1)
            TextField("", text: $pageInput)
                .textFieldStyle(.plain)
                .multilineTextAlignment(.center)
                .font(.system(size: 12).monospacedDigit())
                .frame(width: 30)
                .focused($fieldFocused)
                .onSubmit(submit)
            Text(verbatim: "/ \(total)")
                .font(.system(size: 12).monospacedDigit())
                .foregroundStyle(Theme.textFaint)
            Button {
                jump(min(total, current + 1))
            } label: {
                Image(systemName: "chevron.right")
                    .font(.system(size: 11, weight: .semibold))
            }
            .buttonStyle(.plain)
            .foregroundStyle(current < total ? Theme.textDim : Theme.textFaint)
            .disabled(current >= total)
        }
        .onAppear { pageInput = "\(current)" }
        .onChange(of: current) { pageInput = "\($0)" }
    }

    private func submit() {
        guard let page = Int(pageInput), page >= 1, page <= total else {
            pageInput = "\(current)"
            return
        }
        jump(page)
        fieldFocused = false
    }
}

/// Document area. Observes the tab directly so tab-level state (splitOn)
/// re-renders it — the environment model alone does not.
private struct CenteredDocument: View {
    @ObservedObject var tab: PDFTab

    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            PDFViewport(tab: tab)
                .ignoresSafeArea()
                .background(Theme.bg)
            if tab.splitOn {
                Divider().overlay(Theme.border)
                SplitViewport(tab: tab)
                    .ignoresSafeArea()
                    .background(Theme.panelAlt)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - find bar

struct FindBarView: View {
    @ObservedObject var reader: ReaderModel

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(Theme.textDim)
            TextField(L10n.t("ui.viewer.find-placeholder"), text: Binding(
                get: { reader.findQuery },
                set: { reader.findQuery = $0; reader.runFind() }))
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .frame(width: 180)
                .onSubmit { reader.findNext() }
            if reader.findTotal > 0 {
                Text("\(reader.findIndex + 1)/\(reader.findTotal)")
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textDim)
            } else if !reader.findQuery.isEmpty {
                Text("0")
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textFaint)
            }
            Button {
                reader.findPrevious()
            } label: {
                Image(systemName: "chevron.up")
            }
            .buttonStyle(.plain)
            .foregroundStyle(Theme.textDim)
            Button {
                reader.findNext()
            } label: {
                Image(systemName: "chevron.down")
            }
            .buttonStyle(.plain)
            .foregroundStyle(Theme.textDim)
            Button {
                reader.closeFind()
            } label: {
                Image(systemName: "xmark")
            }
            .buttonStyle(.plain)
            .foregroundStyle(Theme.textDim)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.panel))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Theme.borderStrong))
        .shadow(color: Color.black.opacity(0.18), radius: 8, y: 2)
    }
}

// MARK: - welcome / empty state (the document browser layer)

/// Shown in windows without a document: brand, open action and the
/// continue-reading list — the same role Books' library plays. Open Recent
/// lives here and in the File menu, nowhere near the reader sidebar.
struct WelcomeView: View {
    @EnvironmentObject private var app: AppModel
    let reader: ReaderModel

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                brandMark
                Text("PDFGist")
                    .font(.system(size: 30, weight: .bold))
                Text(L10n.t("ui.welcome.tagline"))
                    .font(.system(size: 14))
                    .foregroundStyle(Theme.textDim)
                featureRow("sparkles", L10n.t("ui.welcome.feature1"))
                featureRow("magnifyingglass", L10n.t("ui.welcome.feature2"))
                featureRow("key", L10n.t("ui.welcome.feature3"))
                openButton
                Text(L10n.t("ui.welcome.or-drag"))
                    .font(.footnote)
                    .foregroundStyle(Theme.textFaint)
                continueSection
                Text(L10n.t("ui.welcome.hints"))
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textFaint)
            }
            .frame(maxWidth: 460)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 40)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg)
    }

    private var brandMark: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 14)
                .fill(Theme.accent)
                .frame(width: 72, height: 72)
            Image(systemName: "doc.richtext.fill")
                .font(.system(size: 34))
                .foregroundStyle(Color.white)
        }
    }

    private func featureRow(_ icon: String, _ text: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: icon)
                .font(.system(size: 12))
                .foregroundStyle(Theme.accent)
                .frame(width: 16)
            Text(text)
                .font(.system(size: 12))
                .foregroundStyle(Theme.textDim)
        }
    }

    private var openButton: some View {
        Button {
            app.pickAndOpen()
        } label: {
            Text(L10n.t("ui.welcome.open-any"))
                .font(.system(size: 14, weight: .semibold))
                .padding(.horizontal, 22)
                .padding(.vertical, 9)
        }
        .buttonStyle(.plain)
        .foregroundStyle(Color.white)
        .background(RoundedRectangle(cornerRadius: 7).fill(Theme.accent))
        .disabled(!app.ready)
    }

    @ViewBuilder
    private var continueSection: some View {
        if !app.recents.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                Text(L10n.t("ui.welcome.continue"))
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(Theme.textDim)
                ForEach(Array(app.recents.prefix(6).enumerated()), id: \.element.path) { _, recent in
                    WelcomeRecentRow(recent: recent)
                }
            }
            .padding(.top, 10)
        }
    }
}

struct WelcomeRecentRow: View {
    @EnvironmentObject private var app: AppModel
    let recent: RivetTypes.RecentEntry

    var body: some View {
        Button {
            app.openWindow?(recent.path)
        } label: {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text((recent.path as NSString).lastPathComponent)
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(Theme.text)
                        .lineLimit(1)
                    Text(recentMeta)
                        .font(.system(size: 11))
                        .foregroundStyle(Theme.textFaint)
                }
                Spacer()
                Image(systemName: "chevron.right")
                    .font(.system(size: 10))
                    .foregroundStyle(Theme.textFaint)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.panel))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.border))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(recent.path)
    }

    private var recentMeta: String {
        let date = Date(timeIntervalSince1970: Double(recent.last_read_ms) / 1000)
        let calendar = Calendar.current
        return L10n.t(
            "ui.welcome.recent-meta", "\(recent.page)",
            "\(calendar.component(.month, from: date))",
            "\(calendar.component(.day, from: date))")
    }
}

// MARK: - password sheet

struct PasswordSheet: View {
    @ObservedObject var reader: ReaderModel
    let wrong: Bool
    @State private var password = ""

    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "lock")
                .font(.system(size: 26))
                .foregroundStyle(Theme.accent)
            Text(L10n.t("ui.password.title"))
                .font(.system(size: 14, weight: .semibold))
            if wrong {
                Text(L10n.t("ui.password.wrong"))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.danger)
            }
            SecureField("", text: $password)
                .textFieldStyle(.roundedBorder)
                .frame(width: 240)
                .onSubmit { submit() }
            HStack(spacing: 10) {
                Button(L10n.t("ui.password.cancel")) {
                    reader.cancelPassword()
                }
                .keyboardShortcut(.cancelAction)
                Button(L10n.t("ui.password.ok")) { submit() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(password.isEmpty)
            }
        }
        .padding(24)
        .frame(width: 320)
    }

    private func submit() {
        guard !password.isEmpty else { return }
        reader.submitPassword(password)
        password = ""
    }
}
