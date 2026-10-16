import SwiftUI
import AppKit
import PDFKit
import UniformTypeIdentifiers

// MARK: - the shelf (library-first launch window)
//
// The launch window IS the shelf: cover grid, collections filter, search,
// import. Double-clicking a cover opens the reading window (one document
// per window — the Preview model). The shelf catalogs books in place;
// nothing here ever copies or moves a file.

struct LibraryView: View {
    @EnvironmentObject private var app: AppModel
    var body: some View {
        ZStack {
            Theme.bg.frame(maxWidth: .infinity, maxHeight: .infinity)
            shelfContent
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg)
        .foregroundStyle(Theme.text)
        .navigationTitle(L10n.t("ui.library.title"))
        .toolbar { shelfToolbar }
        .task { await app.loadLibrary() }
    }

    // MARK: content

    @ViewBuilder
    private var shelfContent: some View {
        if !app.ready {
            EmptyHint(L10n.t("ui.update.checking"))
        } else if app.shelfEntries.isEmpty {
            emptyState
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

    private var emptyState: some View {
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
    }

    // MARK: toolbar

    @ToolbarContentBuilder
    private var shelfToolbar: some ToolbarContent {
        ToolbarItem(placement: .navigation) {
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
            Picker("", selection: $app.shelfFilter) {
                Text(L10n.t("ui.library.all")).tag(String?.none)
                Text(L10n.t("ui.library.unfiled")).tag("unfiled" as String?)
                ForEach(app.shelfCollections, id: \.self) { name in
                    Text(name).tag(name as String?)
                }
            }
            .labelsHidden()
            .frame(width: 150)
            TextField(L10n.t("ui.library.search"), text: $app.shelfSearch)
                .textFieldStyle(.roundedBorder)
                .frame(width: 190)
        }
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

    // MARK: cover art

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

    // MARK: actions

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
            Button(L10n.t("ui.library.new-collection")) { newCollectionSheet = true }
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
        VStack(spacing: 14) {
            Text(L10n.t("ui.library.new-collection"))
                .font(.system(size: 14, weight: .semibold))
            TextField(L10n.t("ui.library.new-collection"), text: $newCollectionName)
                .textFieldStyle(.roundedBorder)
                .frame(width: 240)
                .onSubmit { createAndAssign() }
            HStack(spacing: 10) {
                Button(L10n.t("ui.password.cancel")) { newCollectionSheet = false }
                    .keyboardShortcut(.cancelAction)
                Button(L10n.t("ui.password.ok")) { createAndAssign() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(newCollectionName.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(24)
        .frame(width: 320)
    }

    private func createAndAssign() {
        let name = newCollectionName.trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty else { return }
        newCollectionSheet = false
        app.addToCollection(entry, name: name)
        newCollectionName = ""
    }
}

extension AppModel {
    /// Tools-style relocate flow for a missing book: pick the moved file,
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
