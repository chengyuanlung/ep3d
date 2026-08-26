#include "Core/Feature/HoleStandards.h"
#include "Core/Text/NumberText.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace paramcad {

namespace {

// ONE ROW PER SIZE, so a size cannot be in one table and missing from another.
//
// Split across four arrays -- one for pitches, one for drills, one for
// clearances -- they would have to be kept in step by hand, which is the shape
// of defect this project keeps closing. Here a size is present or it is not.
//
// A zero means NOT PUBLISHED HERE and is refused, not filled in: there is no
// standard counterbore for an M1.6 socket head in DIN 974-1, and inventing one
// would put a number on a drawing that no standard backs.
struct Row {
    double nominalMm;
    double pitchMm;    // ISO 261 coarse
    double tapDrillMm; // the standard drill, NOT nominal minus pitch
    double closeMm;    // ISO 273, three series
    double normalMm;
    double looseMm;
    double counterboreMm;      // DIN 974-1, socket head cap screw
    double counterboreDepthMm; // = the screw's head height, so it finishes flush
    double countersinkMm;      // ISO 7046, 90 degrees
};

constexpr std::array<Row, 19> kSizes{{
    {1.6, 0.35, 1.25, 1.7, 1.8, 2.0, 0.0, 0.0, 0.0},
    {2.0, 0.40, 1.60, 2.2, 2.4, 2.6, 4.4, 2.0, 4.0},
    {2.5, 0.45, 2.05, 2.7, 2.9, 3.1, 5.0, 2.5, 5.0},
    {3.0, 0.50, 2.50, 3.2, 3.4, 3.6, 6.5, 3.0, 6.0},
    {4.0, 0.70, 3.30, 4.3, 4.5, 4.8, 8.0, 4.0, 8.0},
    {5.0, 0.80, 4.20, 5.3, 5.5, 5.8, 10.0, 5.0, 10.0},
    {6.0, 1.00, 5.00, 6.4, 6.6, 7.0, 11.0, 6.0, 12.0},
    // M8 AND M12 ARE WHY THIS IS A TABLE. Nominal minus pitch gives 6.75 and
    // 10.25; the standard drills are 6.8 and 10.2.
    {8.0, 1.25, 6.80, 8.4, 9.0, 10.0, 15.0, 8.0, 16.0},
    {10.0, 1.50, 8.50, 10.5, 11.0, 12.0, 18.0, 10.0, 20.0},
    {12.0, 1.75, 10.20, 13.0, 13.5, 14.5, 20.0, 12.0, 0.0},
    {14.0, 2.00, 12.00, 15.0, 15.5, 16.5, 24.0, 14.0, 0.0},
    {16.0, 2.00, 14.00, 17.0, 17.5, 18.5, 26.0, 16.0, 0.0},
    {18.0, 2.50, 15.50, 19.0, 20.0, 21.0, 30.0, 18.0, 0.0},
    {20.0, 2.50, 17.50, 21.0, 22.0, 24.0, 33.0, 20.0, 0.0},
    {22.0, 2.50, 19.50, 23.0, 24.0, 26.0, 36.0, 22.0, 0.0},
    {24.0, 3.00, 21.00, 25.0, 26.0, 28.0, 40.0, 24.0, 0.0},
    {27.0, 3.00, 24.00, 28.0, 30.0, 32.0, 0.0, 0.0, 0.0},
    {30.0, 3.50, 26.50, 31.0, 33.0, 35.0, 0.0, 0.0, 0.0},
    {36.0, 4.00, 32.00, 37.0, 39.0, 42.0, 0.0, 0.0, 0.0},
}};

const Row* FindSize(double nominalMm) noexcept {
    for (const Row& row : kSizes)
        // A NAMED SIZE, not the nearest one. The tolerance is here only so
        // that a value that arrived through a double survives the trip.
        if (std::fabs(row.nominalMm - nominalMm) < 1e-9) return &row;
    return nullptr;
}

// "1.25" from a double, without a trailing zero parade: a drawing says
// M8x1.25 and M6x1, not M8x1.250000 and M6x1.000000.

// Reads a run of digits and at most one point. Returns false on anything else,
// INCLUDING an empty run -- "Mx1.25" is not a size.
bool ReadNumber(std::string_view text, std::size_t& at, double& out) {
    const std::size_t start = at;
    bool seenPoint = false;
    while (at < text.size()) {
        const char c = text[at];
        if (c >= '0' && c <= '9') {
            ++at;
        } else if (c == '.' && !seenPoint) {
            seenPoint = true;
            ++at;
        } else {
            break;
        }
    }
    if (at == start) return false;
    out = std::stod(std::string(text.substr(start, at - start)));
    return true;
}

} // namespace

std::string MetricThread::designation() const {
    return "M" + ShortNumber(nominalMm) + "x" + ShortNumber(pitchMm);
}

std::optional<MetricThread> MetricCoarseThreadOfSize(double nominalMm) {
    const Row* row = FindSize(nominalMm);
    if (row == nullptr) return std::nullopt;
    return MetricThread{row->nominalMm, row->pitchMm, row->tapDrillMm};
}

std::optional<MetricThread> MetricCoarseThread(std::string_view designation) {
    std::size_t at = 0;
    while (at < designation.size() &&
           std::isspace(static_cast<unsigned char>(designation[at])) != 0)
        ++at;
    if (at >= designation.size()) return std::nullopt;
    if (designation[at] != 'M' && designation[at] != 'm') return std::nullopt;
    ++at;

    double nominal = 0.0;
    if (!ReadNumber(designation, at, nominal)) return std::nullopt;
    const Row* row = FindSize(nominal);
    if (row == nullptr) return std::nullopt;

    // A PITCH, IF ONE IS WRITTEN, HAS TO BE THE COARSE ONE.
    //
    // "M8x1" is a real thread and this table does not hold it -- its drill is
    // 7.0, not 6.8. Reading the pitch and then ignoring it would answer a
    // question about a fine thread with a coarse thread's numbers, which is
    // the worst of the three available outcomes.
    while (at < designation.size() &&
           std::isspace(static_cast<unsigned char>(designation[at])) != 0)
        ++at;
    if (at < designation.size()) {
        const char separator = designation[at];
        if (separator != 'x' && separator != 'X') return std::nullopt;
        ++at;
        double pitch = 0.0;
        if (!ReadNumber(designation, at, pitch)) return std::nullopt;
        if (std::fabs(pitch - row->pitchMm) > 1e-9) return std::nullopt;
    }
    while (at < designation.size() &&
           std::isspace(static_cast<unsigned char>(designation[at])) != 0)
        ++at;
    if (at != designation.size()) return std::nullopt;

    return MetricThread{row->nominalMm, row->pitchMm, row->tapDrillMm};
}

const char* NameOfHoleKind(HoleKind kind) noexcept {
    switch (kind) {
        case HoleKind::Simple: return "simple";
        case HoleKind::Counterbore: return "counterbore";
        case HoleKind::Countersink: return "countersink";
    }
    return "simple";
}

std::optional<HoleKind> HoleKindNamed(std::string_view name) noexcept {
    // READ FROM THE SAME LIST IT IS WRITTEN FROM, so a name cannot be written
    // in one spelling and looked for in another.
    for (const HoleKind kind : {HoleKind::Simple, HoleKind::Counterbore, HoleKind::Countersink})
        if (name == NameOfHoleKind(kind)) return kind;
    return std::nullopt;
}

std::optional<ClearanceFit> ClearanceFitNamed(std::string_view name) noexcept {
    for (const ClearanceFit fit :
         {ClearanceFit::Close, ClearanceFit::Normal, ClearanceFit::Loose})
        if (name == NameOf(fit)) return fit;
    return std::nullopt;
}

const char* NameOf(ClearanceFit fit) noexcept {
    switch (fit) {
        case ClearanceFit::Close: return "close";
        case ClearanceFit::Normal: return "normal";
        case ClearanceFit::Loose: return "loose";
    }
    return "normal";
}

std::optional<double> ClearanceHoleMm(double nominalMm, ClearanceFit fit) {
    const Row* row = FindSize(nominalMm);
    if (row == nullptr) return std::nullopt;
    switch (fit) {
        case ClearanceFit::Close: return row->closeMm;
        case ClearanceFit::Normal: return row->normalMm;
        case ClearanceFit::Loose: return row->looseMm;
    }
    return std::nullopt;
}

std::optional<Counterbore> CounterboreForSocketHead(double nominalMm) {
    const Row* row = FindSize(nominalMm);
    if (row == nullptr || row->counterboreMm <= 0.0) return std::nullopt;
    return Counterbore{row->counterboreMm, row->counterboreDepthMm};
}

std::optional<Countersink> CountersinkForFlatHead(double nominalMm) {
    const Row* row = FindSize(nominalMm);
    if (row == nullptr || row->countersinkMm <= 0.0) return std::nullopt;
    return Countersink{row->countersinkMm, 90.0};
}

namespace {

// THE DRAWING SYMBOLS, as UTF-8 bytes, following the convention already in
// DrawingDocument.cpp. Kept as named constants so no literal ever has one of
// these escapes sitting next to a digit -- "\xB1" followed by a 2 is a single
// three-digit escape, which is a real afternoon this project has already
// spent once.
const char* const kDiameter = "\xE2\x8C\x80";     // U+2300 DIAMETER SIGN
const char* const kCounterbore = "\xE2\x8C\xB4";  // U+2334 COUNTERBORE
const char* const kCountersink = "\xE2\x8C\xB5";  // U+2335 COUNTERSINK
const char* const kDepth = "\xE2\x96\xBC";        // U+25BC DEPTH

// A measurement as a drawing writes it: 6.8, not 6.800000, and 12 rather
// than 12.0.
std::string Measure(double value) { return ShortNumber(value); }

} // namespace

HoleSizes SizeAHole(const HoleScrew& screw, HoleKind kind, double typedDiameterMm,
                    double depthMm) {
    HoleSizes out;

    // --- HOW BIG IS THE DRILL ------------------------------------------------
    //
    // From the designation when there is one, and NEVER from both. A hole that
    // names a screw and also carries a typed diameter is two answers to one
    // question; the designation wins because it is the thing the designer
    // decided, and the diameter is only ever a consequence of it.
    std::string threadForCallout;
    if (screw.named()) {
        const std::optional<MetricThread> thread = MetricCoarseThread(screw.designation);
        if (!thread) {
            // REFUSED, not fallen back on. Falling back to the typed diameter
            // would drill a hole that is the wrong size and looks right, and
            // print a callout naming a thread that hole cannot take.
            out.why = "'" + screw.designation +
                      "' is not a thread this build has numbers for, so there is no drill size "
                      "for it -- clear the thread to drill a plain hole instead";
            return out;
        }
        if (screw.tapped) {
            out.drillDiameterMm = thread->tapDrillMm;
            threadForCallout = thread->designation();
        } else {
            const std::optional<double> clearance =
                ClearanceHoleMm(thread->nominalMm, screw.fit);
            if (!clearance) {
                out.why = "there is no published " + std::string(NameOf(screw.fit)) +
                          " clearance hole for " + thread->designation();
                return out;
            }
            out.drillDiameterMm = *clearance;
        }
    } else {
        if (!(typedDiameterMm > 0.0)) {
            out.why = "a hole's diameter has to be positive";
            return out;
        }
        out.drillDiameterMm = typedDiameterMm;
    }

    // --- AND THE RECESS AT ITS MOUTH ----------------------------------------
    //
    // A counterbore or a countersink is sized for a SCREW HEAD, so it needs to
    // know which screw. Asking for one on a hole that names no screw is
    // refused rather than guessed: there is no such thing as the standard
    // counterbore for a 12.7 mm hole.
    if (kind != HoleKind::Simple && !screw.named()) {
        out.why = "a counterbore or countersink is sized for a screw head, so the hole has to "
                  "say which screw it is for";
        return out;
    }
    if (kind != HoleKind::Simple) {
        const std::optional<MetricThread> thread = MetricCoarseThread(screw.designation);
        if (kind == HoleKind::Counterbore) {
            const std::optional<Counterbore> bore =
                CounterboreForSocketHead(thread->nominalMm);
            if (!bore) {
                out.why = "no standard socket head counterbore is published here for " +
                          thread->designation();
                return out;
            }
            out.counterboreDiameterMm = bore->diameterMm;
            out.counterboreDepthMm = bore->depthMm;
        } else {
            const std::optional<Countersink> cone =
                CountersinkForFlatHead(thread->nominalMm);
            if (!cone) {
                out.why = "no standard countersink is published here for " +
                          thread->designation();
                return out;
            }
            out.countersinkDiameterMm = cone->diameterMm;
            out.countersinkAngleDeg = cone->includedAngleDeg;
        }
    }

    // A RECESS DEEPER THAN THE HOLE IS NOT A HOLE.
    //
    // It reads as a blind pocket with a dimple in the bottom, and the callout
    // still says THRU. Caught here rather than by the kernel, because what the
    // kernel would produce is a valid solid that is the wrong part.
    if (depthMm > 0.0 && out.counterboreDepthMm >= depthMm) {
        out.why = "the counterbore is as deep as the hole itself, which leaves no hole";
        return out;
    }

    // --- WHAT THE DRAWING SAYS ----------------------------------------------
    //
    // Composed HERE, from the same numbers the cut is made from. The callout
    // and the geometry are the classic pair that must agree, so they are not
    // allowed to have two sources.
    std::string text;
    if (!threadForCallout.empty())
        text = threadForCallout; // a tapped hole is called out by its THREAD
    else
        text = std::string(kDiameter) + Measure(out.drillDiameterMm);

    // THRU is a word, not a big number. A through hole whose callout says a
    // depth is a hole a machinist will drill to that depth and stop.
    if (depthMm > 0.0)
        text += std::string(" ") + kDepth + " " + Measure(depthMm);
    else
        text += " THRU";

    if (kind == HoleKind::Counterbore)
        text += std::string("  ") + kCounterbore + " " + kDiameter +
                Measure(out.counterboreDiameterMm) + " " + kDepth + " " +
                Measure(out.counterboreDepthMm);
    else if (kind == HoleKind::Countersink)
        text += std::string("  ") + kCountersink + " " + kDiameter +
                Measure(out.countersinkDiameterMm) + " X " +
                Measure(out.countersinkAngleDeg) + " deg";

    out.callout = std::move(text);
    out.ok = true;
    return out;
}

} // namespace paramcad
