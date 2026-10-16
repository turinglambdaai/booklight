import SwiftUI
import AppKit

/// AI provider settings (设置 tab) — preset picker with per-preset stash,
/// base URL / API key / model, connection test, target language, storage and
/// appearance options. Split into small sections to keep the type-checker sane.
struct SettingsPane: View {
    @EnvironmentObject private var app: AppModel

    @State private var providerID = "deepseek"
    @State private var baseURL = ""
    @State private var modelField = ""
    @State private var apiKey = ""
    @State private var target: RivetTypes.TargetLanguage = .zh
    @State private var viewMode = "single"
    @State private var sidecar = false
    @State private var status = ""
    @State private var statusIsError = false
    @State private var modelSuggestions: [String] = []
    @State private var busy = false
    @State private var loaded = false
    @State private var presetStash: [String: (url: String, model: String)] = [:]
    @State private var loadedLanguage = "system"

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                providerSection
                Divider().overlay(Theme.border)
                readingSection
                Divider().overlay(Theme.border)
                languageSection
                Divider().overlay(Theme.border)
                privacyFooter
            }
            .padding(12)
        }
        .onAppear {
            if !loaded {
                loadFromSettings()
                loadedLanguage = app.languageSetting
                loaded = true
            }
        }
    }

    // MARK: sections

    private var providerSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            settingLabel(L10n.t("ui.settings.preset"))
            Picker("", selection: Binding(
                get: { providerID },
                set: { switchPreset($0) })) {
                ForEach(app.presets, id: \.id) { preset in
                    Text(preset.label).tag(preset.id)
                }
            }
            .labelsHidden()

            settingLabel(L10n.t("ui.settings.base-url"))
            TextField("https://…", text: $baseURL)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 12))

            settingLabel(L10n.t("ui.settings.model"))
            TextField("", text: $modelField)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 12))
            if !modelSuggestions.isEmpty {
                Picker("", selection: Binding(
                    get: { modelField },
                    set: { modelField = $0 })) {
                    ForEach(modelSuggestions, id: \.self) { suggestion in
                        Text(suggestion).tag(suggestion)
                    }
                }
                .labelsHidden()
                .font(.system(size: 11))
            }

            settingLabel(L10n.t("ui.settings.api-key"))
            HStack(spacing: 6) {
                SecureField(
                    app.settings.has_api_key ? L10n.t("ui.settings.api-key-saved") : "",
                    text: $apiKey)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 12))
                smallButton(L10n.t("ui.settings.save")) {
                    Task { await saveKey() }
                }
                .disabled(apiKey.isEmpty || busy)
            }

            HStack(spacing: 6) {
                smallButton(L10n.t("ui.settings.fetch-models")) {
                    Task { await testConnection() }
                }
                .disabled(busy || !canTest)
                smallButton(L10n.t("ui.settings.test")) {
                    Task { await testConnection() }
                }
                .disabled(busy || !canTest)
                if busy {
                    ProgressView().controlSize(.small)
                }
            }
            if !status.isEmpty {
                Text(status)
                    .font(.system(size: 11))
                    .foregroundStyle(statusIsError ? Theme.danger : Theme.textDim)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private var readingSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            settingLabel(L10n.t("ui.settings.target-language"))
            Picker("", selection: Binding(
                get: { target },
                set: { target = $0 })) {
                ForEach(RivetTypes.TargetLanguage.allCasesArray, id: \.self) { code in
                    Text(languageName(code)).tag(code)
                }
            }
            .labelsHidden()

            settingLabel(L10n.t("ui.settings.view-mode"))
            Picker("", selection: Binding(
                get: { viewMode },
                set: { viewMode = $0 })) {
                Text(L10n.t("ui.settings.view-single")).tag("single")
                Text(L10n.t("ui.settings.view-double")).tag("double")
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            Toggle(isOn: Binding(
                get: { sidecar },
                set: { sidecar = $0 })) {
                Text(L10n.t("ui.settings.sidecar"))
                    .font(.system(size: 12))
            }

            smallButton(L10n.t("ui.settings.save")) {
                Task { await save() }
            }
            .disabled(busy)
        }
    }

    private var languageSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            settingLabel(L10n.t("ui.settings.language"))
            Picker("", selection: Binding(
                get: { app.languageSetting },
                set: { app.setLanguage($0) })) {
                Text(L10n.t("ui.settings.language-system")).tag("system")
                Text("中文").tag("zh")
                Text("English").tag("en")
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            Text(L10n.t("ui.settings.language-note"))
                .font(.system(size: 11))
                .foregroundStyle(Theme.textFaint)
            if app.languageSetting != loadedLanguage {
                smallButton(L10n.t("ui.settings.relaunch")) {
                    app.relaunchNow()
                }
            }
        }
    }


    private var privacyFooter: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(L10n.t("ui.settings.privacy"))
                .font(.system(size: 11))
                .foregroundStyle(Theme.textFaint)
            Text("PDFGist \(RivetGeneratedConfig.version) · Rivet protocol v\(RivetGeneratedConfig.build)")
                .font(.system(size: 10))
                .foregroundStyle(Theme.textFaint)
            // Support surface (the taskly About facts): a bug report starts
            // one click away from the failure it describes.
            HStack(spacing: 14) {
                Link(L10n.t("ui.about.website"),
                     destination: URL(string: "https://booklight.jrtx.site")!)
                Link(L10n.t("ui.about.issue"),
                     destination: URL(string: "https://github.com/turinglambdaai/booklight/issues")!)
            }
            .font(.system(size: 11))
            Text(L10n.t("ui.about.license"))
                .font(.system(size: 10))
                .foregroundStyle(Theme.textFaint)
        }
    }

    // MARK: helpers

    private var canTest: Bool {
        !baseURL.trimmingCharacters(in: .whitespaces).isEmpty
            && (!modelField.trimmingCharacters(in: .whitespaces).isEmpty || app.settings.has_api_key)
    }

    private func settingLabel(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 11, weight: .semibold))
            .foregroundStyle(Theme.textDim)
    }

    private func smallButton(_ title: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 12))
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
        }
        .buttonStyle(.plain)
        .foregroundStyle(Theme.text)
        .background(RoundedRectangle(cornerRadius: 5).fill(Theme.panelAlt))
        .overlay(RoundedRectangle(cornerRadius: 5).stroke(Theme.border))
    }

    private func loadFromSettings() {
        providerID = app.settings.provider
        baseURL = app.settings.base_url
        modelField = app.settings.model
        target = app.settings.target_language
        viewMode = app.settings.view_mode == "double" ? "double" : "single"
        sidecar = app.settings.annotation_sidecar
    }

    /// v1 preset behavior: stash the edited URL/model under the old preset,
    /// restore the stash (or the preset defaults) for the new one.
    private func switchPreset(_ newID: String) {
        presetStash[providerID] = (baseURL, modelField)
        providerID = newID
        if let stash = presetStash[newID] {
            baseURL = stash.url
            modelField = stash.model
        } else if let preset = app.presets.first(where: { $0.id == newID }) {
            baseURL = preset.base_url
            modelField = preset.default_model
        } else {
            baseURL = ""
            modelField = ""
        }
        apiKey = ""
        modelSuggestions = []
    }

    private func saveKey() async {
        busy = true
        defer { busy = false }
        status = await app.saveApiKey(apiKey.trimmingCharacters(in: .whitespaces))
        statusIsError = !status.hasPrefix("✓")
        apiKey = ""
    }

    private func testConnection() async {
        busy = true
        status = L10n.t("ui.settings.fetching")
        statusIsError = false
        defer { busy = false }
        let result = await app.testConnection(
            baseURL: baseURL.trimmingCharacters(in: .whitespaces),
            apiKey: apiKey.trimmingCharacters(in: .whitespaces),
            model: modelField.trimmingCharacters(in: .whitespaces))
        status = result.message
        statusIsError = !result.message.hasPrefix("✓")
        modelSuggestions = result.models
    }

    private func save() async {
        busy = true
        defer { busy = false }
        status = await app.saveSettings(
            provider: providerID,
            baseURL: baseURL.trimmingCharacters(in: .whitespaces),
            model: modelField.trimmingCharacters(in: .whitespaces),
            target: target,
            viewMode: viewMode,
            sidecar: sidecar)
        statusIsError = !status.hasPrefix("✓")
    }
}

extension RivetTypes.TargetLanguage {
    static var allCasesArray: [RivetTypes.TargetLanguage] {
        [.zh, .zh_hant, .en, .ja, .ko, .fr, .de, .es]
    }


}

