import SwiftUI
import AppKit
import PDFKit
import UniformTypeIdentifiers

// MARK: - the shelf window (Books model)
//
// Sidebar navigation (home / smart groups / PDF / EPUB / collections) over
// a detail area that is either the Home digest (today's reading, streak,
// continue-reading) or a cover grid for the selected scope. Books are
// cataloged in place; nothing here ever copies or moves a file.

struct LibraryView: View {
    @EnvironmentObject private var app: AppModel
    @State private var newCollectionRequested = false

    var body: some View {
        NavigationSplitView {
            shelfSidebar
                .navigationSplitViewColumnWidth(min: 190, ideal: 220, max: 300)
        } detail: {
            Group {
                if app.shelfSelection == "home" {
                    HomeDigest()
                } else {
                    ShelfGrid()
                }
            }
            .navigationTitle(titleFor(app.shelfSelection))
        }
    }

    private func titleFor(_ selection: String) -> String {
        switch selection {
        case "home": return L10n.t("ui.shelf.home")
        case "search", "all": return L10n.t("ui.shelf.all")
        case "want": return L10n.t("ui.shelf.want")
        case "reading": return L10n.t("ui.shelf.continue")
        case "finished": return L10n.t("ui.shelf.finished")
        case "pdf": return L10n.t("ui.shelf.pdf")
        case "epub": return L10n.t("ui.shelf.epub")
        case let sel where sel.hasPrefix("c:"): return String(sel.dropFirst(2))
        default: return L10n.t("ui.shelf.all")
        }
    }

    // MARK: sidebar (source-list, Books arrangement)

    @ViewBuilder
    private var shelfSidebar: some View {
        List(selection: Binding(
            get: { app.shelfSelection },
            set: { newValue in
                if newValue == "search" {
                    // the search row selects the all-scope and focuses its field
                    app.shelfSelection = "all"
                    return
                }
                if newValue == "new-collection" {
                    newCollectionRequested = true
                    return
                }
                app.shelfSelection = newValue
            })) {
            Label(L10n.t("ui.shelf.search"), systemImage: "magnifyingglass")
                .tag("search")

            Section(L10n.t("ui.shelf.home")) {
                Label(L10n.t("ui.shelf.home"), systemImage: "house")
                    .tag("home")
            }

            Section(L10n.t("ui.shelf.lib")) {
                Label(L10n.t("ui.shelf.all"), systemImage: "books.vertical")
                    .tag("all")
                Label(L10n.t("ui.shelf.want"), systemImage: "circle.dashed")
                    .tag("want")
                Label(L10n.t("ui.shelf.finished"), systemImage: "checkmark.circle")
                    .tag("finished")
                Label(L10n.t("ui.shelf.pdf"), systemImage: "doc.richtext")
                    .tag("pdf")
                Label(L10n.t("ui.shelf.epub"), systemImage: "book")
                    .tag("epub")
            }

            Section(L10n.t("ui.shelf.mine")) {
                ForEach(app.shelfCollections, id: \.self) { name in
                    Label(name, systemImage: "folder")
                        .tag("c:\(name)")
                }
                Label(L10n.t("ui.shelf.new-collection"), systemImage: "plus")
                    .tag("new-collection")
            }
        }
        .listStyle(.sidebar)
        .scrollContentBackground(.hidden)
        .background(Theme.bg)
        .sheet(isPresented: $newCollectionRequested) {
            NewCollectionSheet { name in
                app.createCollection(name)
                app.shelfSelection = "c:\(name)"
            }
        }
    }
}

// MARK: - new collection sheet

struct NewCollectionSheet: View {
    let oncreate: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""

    var body: some View {
        VStack(spacing: 14) {
            Text(L10n.t("ui.shelf.new-collection"))
                .font(.system(size: 14, weight: .semibold))
            TextField(L10n.t("ui.shelf.new-collection"), text: $name)
                .textFieldStyle(.roundedBorder)
                .frame(width: 240)
                .onSubmit { commit() }
            HStack(spacing: 10) {
                Button(L10n.t("ui.password.cancel")) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(L10n.t("ui.password.ok")) { commit() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(24)
        .frame(width: 320)
    }

    private func commit() {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return }
        dismiss()
        oncreate(trimmed)
    }
}

// MARK: - grid detail (every non-home scope)

struct ShelfGrid: View {
    @EnvironmentObject private var app: AppModel
    @FocusState var searchFocused: Bool

    var body: some View {
        ZStack {
            Theme.bg.frame(maxWidth: .infinity, maxHeight: .infinity)
            gridContent
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg)
        .foregroundStyle(Theme.text)
        .toolbar { gridToolbar }
    }

    @ViewBuilder
    private var gridContent: some View {
        if !app.ready {
            EmptyHint(L10n.t("ui.update.checking"))
        } else if app.shelfEntries.isEmpty {
            VStack(spacing: 12) {
                Image(systemName: "books.vertical")
                    .font(.system(size: 44))
                    .foregroundStyle(Theme.accent)
                Text(L10n.t("ui.library.empty"))
                    .font(.system(size: 17, weight: .semibold))
                Text(L10n.t("ui.library.empty-hint"))
                    .font(.system(size: 13))
                    .foregroundStyle(Theme.textDim)
                Button(L10n.t("ui.library.import")) {
                    app.pickAndOpen()
                }
                .buttonStyle(.borderedProminent)
                .tint(Theme.accent)
                .disabled(!app.ready)
                .padding(.top, 6)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ScrollView {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 136, maximum: 180), spacing: 22)],
                          spacing: 26) {
                    ForEach(app.shelfEntries, id: \.path) { entry in
                        BookCoverView(entry: entry)
                    }
                }
                .padding(.horizontal, 32)
                .padding(.vertical, 28)
            }
        }
    }

    @ToolbarContentBuilder
    private var gridToolbar: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            Button {
                app.pickAndOpen()
            } label: {
                Label(L10n.t("ui.library.import"), systemImage: "plus")
            }
            .disabled(!app.ready || app.libraryImporting)
            .help(L10n.t("ui.library.import"))
        }
        ToolbarItemGroup(placement: .primaryAction) {
            if app.libraryImporting {
                ProgressView().controlSize(.small)
            }
            TextField(L10n.t("ui.library.search"), text: $app.shelfSearch)
                .textFieldStyle(.roundedBorder)
                .frame(width: 190)
                .focused($searchFocused)
            todayRing
        }
    }

    /// The Books toolbar mini-gauge: today's minutes over the goal.
    private var todayRing: some View {
        let ratio = min(1.0, Double(app.todaySeconds) / Double(app.dailyGoalSeconds))
        return ZStack {
            Circle()
                .stroke(Theme.border, lineWidth: 2.5)
            Circle()
                .trim(from: 0, to: CGFloat(ratio))
                .stroke(Theme.accent, style: StrokeStyle(lineWidth: 2.5, lineCap: .round))
                .rotationEffect(.degrees(-90))
            VStack(spacing: 0) {
                Text("\(app.todaySeconds / 60)")
                    .font(.system(size: 9, weight: .bold).monospacedDigit())
                Text("/ \(app.dailyGoalSeconds / 60)")
                    .font(.system(size: 7))
                    .foregroundStyle(Theme.textFaint)
            }
        }
        .frame(width: 30, height: 30)
        .help(L10n.t("ui.shelf.today"))
    }
}

// MARK: - home digest (continue-reading + recently added)

struct HomeDigest: View {
    @EnvironmentObject private var app: AppModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                Text(L10n.t("ui.shelf.home"))
                    .font(.system(size: 34, weight: .bold))
                    .padding(.top, 12)

                goalsCard

                if !readingNow.isEmpty {
                    Text(L10n.t("ui.shelf.continue"))
                        .font(.system(size: 17, weight: .semibold))
                    continueRow
                }

                if !recentlyAdded.isEmpty {
                    Text(L10n.t("ui.home.recent"))
                        .font(.system(size: 17, weight: .semibold))
                    recentRow
                }
            }
            .padding(.horizontal, 32)
            .padding(.bottom, 32)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg)
        .foregroundStyle(Theme.text)
    }

    private var goalRatio: Double {
        min(1.0, Double(app.todaySeconds) / Double(app.dailyGoalSeconds))
    }

    // MARK: goals card (Books form: gauge + daily-goal caption + black pill)

    private var goalsCard: some View {
        VStack(spacing: 0) {
            ZStack {
                SemiGauge()
                    .stroke(Theme.border, style: StrokeStyle(lineWidth: 10, lineCap: .round))
                SemiGauge(trim: CGFloat(goalRatio))
                    .stroke(Color.primary,
                            style: StrokeStyle(lineWidth: 10, lineCap: .round))
                VStack(spacing: 4) {
                    Text(L10n.t("ui.shelf.today"))
                        .font(.system(size: 15, weight: .semibold))
                    Text(AppModel.mmss(app.todaySeconds))
                        .font(.system(size: 46, weight: .bold).monospacedDigit())
                    Text(L10n.t("ui.shelf.goal-of"))
                        .font(.system(size: 12))
                        .foregroundStyle(Theme.textDim)
                    if goalRatio >= 1 {
                        Text(L10n.t("ui.shelf.goal-hit"))
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundStyle(Theme.accent)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 2)
                            .background(Capsule().fill(Theme.accentSoft))
                    }
                }
            }
            .frame(height: 210)
            .padding(.top, 24)

            continuePill
                .padding(.bottom, 24)

            Divider().overlay(Theme.border)

            streakRow
                .padding(.vertical, 18)
        }
        .background(RoundedRectangle(cornerRadius: 14).fill(Theme.panel))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(Theme.border))
        .frame(maxWidth: 760)
    }

    /// The Books card carries a black store button; ours opens the book the
    /// user last read (or the open panel when there is none).
    @ViewBuilder
    private var continuePill: some View {
        if let last = app.recents.first {
            Button {
                app.openWindow?(last.path)
            } label: {
                Text(L10n.t("ui.shelf.continue"))
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Color.white)
                    .padding(.horizontal, 28)
                    .padding(.vertical, 12)
                    .background(Capsule().fill(Color.primary))
            }
            .buttonStyle(.plain)
        } else {
            Button(L10n.t("ui.library.import")) {
                app.pickAndOpen()
            }
            .buttonStyle(.borderedProminent)
            .tint(Theme.accent)
        }
    }

    private var streakRow: some View {
        VStack(spacing: 8) {
            HStack(spacing: 10) {
                let symbols = ["一", "二", "三", "四", "五", "六", "日"]
                ForEach(0..<7, id: \.self) { index in
                    let read = app.weekSeconds.indices.contains(index)
                        && app.weekSeconds[index] >= 60
                    ZStack {
                        Circle()
                            .fill(read ? Color.primary : Theme.panelAlt)
                            .overlay(Circle().stroke(Theme.border))
                            .frame(width: 30, height: 30)
                        Text(symbols[index])
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundStyle(read ? Color.white : Theme.textDim)
                    }
                }
            }
            if app.streakDays > 0 {
                Text(L10n.t("ui.shelf.streak-record", "\(app.streakDays)"))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.textDim)
            } else {
                Text(L10n.t("ui.shelf.streak-start"))
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.textDim)
            }
        }
        .frame(maxWidth: .infinity)
    }

    private var readingNow: [RivetTypes.LibraryEntry] {
        app.library.filter { entry in
            guard let p = app.shelfProgress(entry) else { return false }
            return p.page < p.of
        }
    }

    private var recentlyAdded: [RivetTypes.LibraryEntry] {
        app.library.sorted { $0.added_at_ms > $1.added_at_ms }.prefix(12).map { $0 }
    }

    private var continueRow: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(alignment: .top, spacing: 22) {
                ForEach(readingNow, id: \.path) { entry in
                    BookCoverView(entry: entry)
                        .frame(width: 150)
                }
            }
            .padding(.vertical, 4)
        }
    }

    private var recentRow: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(alignment: .top, spacing: 22) {
                ForEach(recentlyAdded, id: \.path) { entry in
                    BookCoverView(entry: entry)
                        .frame(width: 150)
                }
            }
            .padding(.vertical, 4)
        }
    }
}

/// Top-half arc, starting at the left horizon sweeping right; `trim`
/// fraction is relative to the half-circle.
struct SemiGauge: Shape {
    var trim: CGFloat = 1.0

    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.addArc(center: CGPoint(x: rect.midX, y: rect.maxY),
                    radius: min(rect.width / 2, rect.height),
                    startAngle: .degrees(180),
                    endAngle: .degrees(0),
                    clockwise: false)
        return path.trimmedPath(from: 0.0001, to: max(0.0002, trim))
    }
}


// MARK: - one shelf cell

struct BookCoverView: View {
    @EnvironmentObject private var app: AppModel
    let entry: RivetTypes.LibraryEntry

    @State private var newCollectionSheet = false
    @State private var newCollectionName = ""

    private var progress: (page: Int, of: Int)? { app.shelfProgress(entry) }
    private var isMissing: Bool { entry.status == "missing" }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            cover
            Text(entry.title)
                .font(.system(size: 12, weight: .medium))
                .lineLimit(2)
                .foregroundStyle(isMissing ? Theme.textFaint : Theme.text)
            if !entry.author.isEmpty {
                Text(entry.author)
                    .font(.system(size: 11))
                    .lineLimit(1)
                    .foregroundStyle(Theme.textFaint)
            }
            progressFooter
        }
        .frame(width: 140)
        .contentShape(Rectangle())
        .onTapGesture(count: 2) { open() }
        .contextMenu { contextMenu }
        .sheet(isPresented: $newCollectionSheet) { newCollectionSheetView }
        .help(isMissing ? L10n.t("ui.library.missing") : entry.title)
    }

    @ViewBuilder
    private var cover: some View {
        let image = entry.cover.isEmpty
            ? nil
            : NSImage(contentsOfFile: entry.cover)
        ZStack {
            RoundedRectangle(cornerRadius: 6)
                .fill(image == nil
                      ? AnyShapeStyle(LinearGradient(
                          colors: [Theme.accentSoft, Theme.panelAlt],
                          startPoint: .top, endPoint: .bottom))
                      : AnyShapeStyle(Color.clear))
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(Theme.border))
                .frame(width: 128, height: 172)
            if isMissing {
                RoundedRectangle(cornerRadius: 6)
                    .fill(Color.black.opacity(0.55))
                    .frame(width: 128, height: 172)
                Label(L10n.t("ui.library.missing"), systemImage: "questionmark.circle")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(.white)
            } else if let image {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: 128, height: 172)
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                    .shadow(color: .black.opacity(0.18), radius: 5, y: 2)
            } else {
                VStack(spacing: 8) {
                    Image(systemName: entry.kind == "epub" ? "book" : "doc.richtext")
                        .font(.system(size: 34))
                        .foregroundStyle(Theme.accent)
                    Text(entry.title)
                        .font(.system(size: 11, weight: .semibold))
                        .lineLimit(3)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 10)
                        .foregroundStyle(Theme.textDim)
                }
                .frame(width: 128, height: 172)
            }
        }
    }

    @ViewBuilder
    private var progressFooter: some View {
        if let progress {
            VStack(alignment: .leading, spacing: 3) {
                ProgressView(value: Double(progress.page), total: Double(progress.of))
                    .progressViewStyle(.linear)
                    .tint(Theme.accent)
                Text(L10n.t(entry.kind == "epub"
                            ? "ui.library.chapter-progress"
                            : "ui.library.progress",
                            "\(progress.page)", "\(progress.of)"))
                    .font(.system(size: 10))
                    .foregroundStyle(Theme.textFaint)
            }
        } else {
            Text(L10n.t(entry.kind == "epub"
                        ? "ui.library.chapter-progress"
                        : "ui.library.progress",
                        "1", "\(max(entry.pages, 1))"))
                .font(.system(size: 10))
                .foregroundStyle(Color.clear) // keep row heights aligned
        }
    }

    private func open() {
        guard !isMissing else { return }
        app.openWindow?(entry.path)
    }

    @ViewBuilder
    private var contextMenu: some View {
        if isMissing {
            Button(L10n.t("ui.library.relink")) { app.relocateEntry(entry) }
            Divider()
        }
        Menu(L10n.t("ui.library.add-to")) {
            Button(L10n.t("ui.shelf.new-collection")) { newCollectionSheet = true }
            if !app.shelfCollections.isEmpty { Divider() }
            ForEach(app.shelfCollections, id: \.self) { name in
                Button(name) { app.addToCollection(entry, name: name) }
            }
        }
        if !entry.collections.isEmpty {
            Menu(L10n.t("ui.library.remove-from")) {
                ForEach(entry.collections, id: \.self) { name in
                    Button(name) { app.removeFromCollection(entry, name: name) }
                }
            }
        }
        Divider()
        Button(L10n.t("ui.library.reveal")) {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: entry.path)])
        }
        .disabled(isMissing)
        Button(L10n.t("ui.library.remove"), role: .destructive) {
            app.removeFromShelf(entry)
        }
    }

    private var newCollectionSheetView: some View {
        NewCollectionSheet { name in
            app.addToCollection(entry, name: name)
        }
    }
}
extension AppModel {
    /// Books-style relocate flow for a missing book: pick the moved file,
    /// the shelf re-links to it (books are never re-copied).
    func relocateEntry(_ entry: RivetTypes.LibraryEntry) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.message = L10n.t("ui.library.relink")
        let handle: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.relinkEntry(entry, to: url)
        }
        if let window = NSApp.keyWindow {
            panel.beginSheetModal(for: window, completionHandler: handle)
        } else {
            handle(panel.runModal())
        }
    }
}
