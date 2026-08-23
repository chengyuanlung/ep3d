#pragma once

#include "Viewer/DocumentOutline.h"
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

namespace paramcad::ui {

// Centralized design tokens (UI spec 3). Every spacing, size and state colour
// used by the shell is defined HERE and nowhere else -- scattering these values
// is what makes a UI drift out of alignment one widget at a time.
//
// All sizes are LOGICAL pixels. Qt scales them by the device pixel ratio, so
// nothing here may assume 100% DPI (UI spec 3/15).
namespace spacing {
inline constexpr int kMicro = 4;
inline constexpr int kStandard = 8;
inline constexpr int kGrouped = 12;
inline constexpr int kSection = 16;
inline constexpr int kMajor = 24;
} // namespace spacing

namespace size {
inline constexpr int kToolbarIcon = 20;
// The SKETCH toolbar runs icon-only, so its icons carry the whole meaning and
// are given more room than the main bar's, where a label sits beside them.
inline constexpr int kSketchToolbarIcon = 24;
inline constexpr int kPropertyRowHeight = 30;
inline constexpr int kTreeRowHeight = 26;
inline constexpr int kStatusBarHeight = 24;
inline constexpr int kModelTreeMinWidth = 220;
inline constexpr int kPropertyPanelWidth = 300;
// Narrowest the VALUE column may become before the panel stops being readable.
//
// The M6.14 fix stopped values being pushed off the right edge; independent
// review then showed it had turned that failure into a different one -- the
// value column being squeezed to nothing by a long LABEL. A 133-character skip
// diagnostic was being delivered as nine characters, and three different skip
// rows rendered identically. Both failures are "the user cannot read the
// value"; only the first was ever measured.
//
// 120 px is roughly 18 characters at the panel's font -- enough to tell two
// diagnostics apart and to show a dimension with its sign and decimals.
inline constexpr int kMinValueColumnWidth = 110;
// Widest the LABEL column may become. Labels are bounded by design; this is
// what makes that assumption enforceable rather than a comment.
// The smallest window the shell must stay usable in (UI spec 5). Enforced as a
// minimum size so the layout cannot be dragged into an unusable state.
inline constexpr int kMinWindowWidth = 1000;
inline constexpr int kMinWindowHeight = 640;
} // namespace size

// State presentation. Colour is ALWAYS paired with a text marker, because
// UI spec 11/19 forbid conveying state by colour alone -- these values are the
// secondary channel, never the only one.
struct StatePresentation {
    QString marker;   // "", "*", "!", "-", "x"
    QString label;    // human-facing, used in tooltips and the status bar
    QColor color;     // text colour hint; never the sole carrier of meaning
    bool bold;
};

// Presentation derived from the ACTIVE PALETTE, never from hard-coded RGB.
//
// UI spec 14 forbids encoding semantic state as a fixed colour value, and the
// first run proved why: tokens picked for a dark theme rendered near-white text
// on this machine's light theme and the Model Tree was effectively unreadable.
// The tint is chosen per theme and the text marker carries the meaning either
// way, so state survives a theme switch, greyscale and colour-blindness.
inline StatePresentation presentationFor(OutlineState state, const QPalette& palette) {
    // Perceived luminance of the window background decides which tints read.
    const QColor background = palette.color(QPalette::Base);
    const double luminance = (0.299 * background.red() + 0.587 * background.green() +
                              0.114 * background.blue()) /
                             255.0;
    const bool dark = luminance < 0.5;

    const QColor normalText = palette.color(QPalette::Text);
    const QColor mutedText = palette.color(QPalette::Disabled, QPalette::Text);
    const QColor warn = dark ? QColor(0xE8, 0xC8, 0x6A) : QColor(0x8A, 0x62, 0x00);
    const QColor error = dark ? QColor(0xE8, 0x7D, 0x7D) : QColor(0xA3, 0x1D, 0x1D);

    switch (state) {
        case OutlineState::Valid:
            return {QString(), QStringLiteral("Up to date"), normalText, false};
        case OutlineState::Dirty:
            return {QStringLiteral("*"), QStringLiteral("Needs recompute"), warn, false};
        case OutlineState::Failed:
            return {QStringLiteral("!"), QStringLiteral("Failed"), error, true};
        case OutlineState::Blocked:
            return {QStringLiteral("-"), QStringLiteral("Blocked by a failed input"), error,
                    false};
        case OutlineState::Suppressed:
            return {QStringLiteral("x"), QStringLiteral("Suppressed"), mutedText, false};
        case OutlineState::Hidden:
            // Hidden is a VIEW state and must not look like Suppressed, which is
            // a document state (UI spec 11).
            return {QStringLiteral("h"), QStringLiteral("Hidden"), mutedText, false};
        default:
            return {QString(), QStringLiteral("Not computed"), mutedText, false};
    }
}

// A short type tag shown before an object's name, so kind is readable without
// relying on an icon set this milestone does not ship.
inline QString kindTag(OutlineKind kind) {
    switch (kind) {
        case OutlineKind::Document: return QStringLiteral("[Part]");
        case OutlineKind::Parameter: return QStringLiteral("[Par]");
        case OutlineKind::Sketch: return QStringLiteral("[Skt]");
        case OutlineKind::Constraint: return QStringLiteral("[Cst]");
        case OutlineKind::Solid: return QStringLiteral("[Sld]");
        case OutlineKind::MassProperties: return QStringLiteral("[Phy]");
        case OutlineKind::Material: return QStringLiteral("[Mat]");
        default: return QStringLiteral("[   ]");
    }
}

// Monospaced digits for numeric columns where the platform offers them
// (UI spec 4), falling back to the UI font rather than to a font that may not
// exist on the machine.
inline QFont numericFont(const QFont& base) {
    QFont font = base;
    font.setStyleHint(QFont::Monospace);
    return font;
}

} // namespace paramcad::ui
