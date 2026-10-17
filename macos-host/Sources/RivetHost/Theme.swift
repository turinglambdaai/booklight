import SwiftUI
import AppKit

// Color system: Apple semantic colors for every surface — window and
// control backgrounds, separators, label hierarchy — so light mode reads
// as white-on-black and dark mode follows the system automatically. The
// only branded color is the terracotta accent, reserved for interactive
// elements (selection, progress, buttons); reading surfaces stay neutral.
// Annotation colors are content colors (the user chose them) and keep
// their fixed palette.

enum Theme {
    // iBooks form: the whole window reads as one white document surface
    // (textBackgroundColor = white in light, near-black in dark); cards
    // float on it as light gray.
    static let bg = Color(nsColor: .textBackgroundColor)
    static let panel = Color(nsColor: .windowBackgroundColor)
    static let panelAlt = Color(nsColor: .quaternarySystemFill)
    static let border = Color(nsColor: .separatorColor)
    static let borderStrong = Color.primary.opacity(0.16)
    static let text = Color.primary
    static let textDim = Color(nsColor: .secondaryLabelColor)
    static let textFaint = Color(nsColor: .tertiaryLabelColor)
    static let accent = Color.dynamic(light: 0xD97757, dark: 0xE8926A)
    static let accentHover = Color.dynamic(light: 0xC4623E, dark: 0xF0A07C)
    static let accentSoft = Color.accentNonLinear(0.12)
    static let accentBorder = Color.accentNonLinear(0.28)
    static let danger = Color(nsColor: .systemRed)

    // Annotation chips (v1 .note-dot colors).
    static func annotationColor(_ name: String) -> NSColor {
        switch name {
        case "green": return NSColor(hex: 0x4CC383, alpha: 0.85)
        case "blue": return NSColor(hex: 0x569CFF, alpha: 0.85)
        default: return NSColor(hex: 0xFFD500, alpha: 0.9)
        }
    }

    static func annotationSwiftUIColor(_ name: String) -> Color {
        Color(nsColor: annotationColor(name))
    }
}

extension Color {
    /// Accent tint at the given opacity (keeps working across light/dark
    /// because the accent itself is a dynamic color).
    static func accentNonLinear(_ opacity: Double) -> Color {
        Color.dynamic(light: 0xD97757, dark: 0xE8926A).opacity(opacity)
    }

    static func dynamic(light: UInt32, dark: UInt32) -> Color {
        Color(nsColor: NSColor(name: nil) { appearance in
            let isDark = appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            return NSColor(hex: isDark ? dark : light)
        })
    }
}

extension NSColor {
    convenience init(hex: UInt32, alpha: CGFloat = 1) {
        self.init(
            red: CGFloat((hex >> 16) & 0xFF) / 255,
            green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255,
            alpha: alpha)
    }
}
