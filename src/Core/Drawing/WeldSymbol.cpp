#include "Core/Drawing/WeldSymbol.h"

#include "Core/Text/NumberText.h"

#include <cmath>

namespace paramcad {

namespace {

// ONE LIST OF EVERY TYPE, so nothing walks a hand-copied subset. A parser that
// knew eleven of the twelve would turn the twelfth into a fillet -- and a butt
// weld read as a fillet is a joint with no penetration at all.
const WeldType kAllTypes[] = {
    WeldType::SquareButt, WeldType::SingleV,   WeldType::SingleBevel, WeldType::SingleU,
    WeldType::SingleJ,    WeldType::Fillet,    WeldType::Plug,        WeldType::Spot,
    WeldType::Seam,       WeldType::Backing,   WeldType::Surfacing,   WeldType::Edge};

const WeldContour kAllContours[] = {WeldContour::AsWelded, WeldContour::Flat,
                                    WeldContour::Convex, WeldContour::Concave,
                                    WeldContour::Blended};

const FilletSizeKind kAllSizeKinds[] = {FilletSizeKind::Unspecified, FilletSizeKind::Throat,
                                        FilletSizeKind::Leg};

// A SPOT AND A PLUG ARE COUNTED, NOT MEASURED ALONG. Their size is a diameter
// and their run has a number and a spacing but no length. Asked in one place
// so the refusal and the text agree about which types those are.
bool IsCounted(WeldType type) noexcept {
    return type == WeldType::Spot || type == WeldType::Plug;
}

// THE LETTER THAT GOES IN FRONT OF THE SIZE. It is part of the size's meaning,
// not decoration: a5 and z5 are different welds, and s10 is a depth of
// preparation rather than either.
std::string SizePrefix(const WeldBead& bead) {
    if (bead.type == WeldType::Fillet)
        return bead.sizeKind == FilletSizeKind::Leg ? "z" : "a";
    if (IsCounted(bead.type)) return {};
    return "s";
}

std::string BeadTextImpl(const WeldBead& bead) {
    std::string text;
    if (bead.sizeMm > 0.0) text = SizePrefix(bead) + ShortNumber(bead.sizeMm) + " ";
    text += SymbolOfWeldType(bead.type);
    const std::string contour = SymbolOfContour(bead.contour);
    if (!contour.empty()) text += contour;
    if (bead.run.has_value()) {
        // n x l (e) -- and for a spot, n (e), because there is no l to write.
        text += " " + std::to_string(bead.run->count);
        if (!IsCounted(bead.type))
            text += "\xC3\x97" + ShortNumber(bead.run->lengthMm);
        if (bead.run->gapMm > 0.0) text += "(" + ShortNumber(bead.run->gapMm) + ")";
    }
    return text;
}

// WHY THIS ONE SIDE'S WELD CANNOT BE MADE. Called for each side that has a
// bead, so neither side can carry a rule the other does not.
std::string WhyBeadRefused(const WeldBead& bead) {
    if (bead.sizeMm < 0.0) return "a weld size cannot be negative";
    if (bead.type == WeldType::Fillet) {
        if (!(bead.sizeMm > 0.0))
            return "a fillet weld has to say how big it is -- there is no default leg";
        // THE ONE THAT COSTS THIRTY PER CENT. See the header.
        if (bead.sizeKind == FilletSizeKind::Unspecified)
            return "a fillet size has to say whether it is the throat (a) or the leg (z) -- "
                   "they are different welds and both draw as one number";
    } else if (bead.sizeKind != FilletSizeKind::Unspecified) {
        // A THROAT ON A BUTT WELD IS A FIELD WITH NO MEANING, and it would
        // print a letter that tells the shop to measure something that is not
        // there.
        return "only a fillet is sized as a throat or a leg";
    }
    if (IsCounted(bead.type) && !(bead.sizeMm > 0.0))
        return "a spot or plug weld has to say how wide it is";
    if (!bead.run.has_value()) return {};
    const WeldRun& run = *bead.run;
    if (run.count < 1) return "an intermittent weld with no welds in it is not a weld";
    if (IsCounted(bead.type)) {
        // A SPOT HAS NO LENGTH. A number here means somebody filled in the
        // field a fillet uses, and it would print as a length nobody can weld.
        if (run.lengthMm != 0.0)
            return "a spot or plug weld is counted, not run along, so it has no length";
    } else if (!(run.lengthMm > 0.0)) {
        return "an intermittent weld has to say how long each weld is";
    }
    // GAP AND COUNT HAVE TO AGREE. One weld has nothing to be spaced from, and
    // several welds with no space between them are one continuous weld written
    // the hard way -- which draws as an intermittent one.
    if (run.count == 1 && run.gapMm != 0.0)
        return "a single weld has no gap to state";
    if (run.count > 1 && !(run.gapMm > 0.0))
        return "welds with no gap between them are one continuous weld, not an intermittent run";
    return {};
}

} // namespace

// --- types ------------------------------------------------------------------

std::string_view toString(WeldType type) noexcept {
    switch (type) {
        case WeldType::SquareButt: return "square-butt";
        case WeldType::SingleV: return "single-v";
        case WeldType::SingleBevel: return "single-bevel";
        case WeldType::SingleU: return "single-u";
        case WeldType::SingleJ: return "single-j";
        case WeldType::Fillet: return "fillet";
        case WeldType::Plug: return "plug";
        case WeldType::Spot: return "spot";
        case WeldType::Seam: return "seam";
        case WeldType::Backing: return "backing";
        case WeldType::Surfacing: return "surfacing";
        case WeldType::Edge: return "edge";
    }
    return "fillet";
}

bool ParseWeldType(std::string_view text, WeldType& into) noexcept {
    // READ FROM THE SAME LIST IT IS WRITTEN FROM.
    for (const WeldType type : kAllTypes)
        if (text == toString(type)) {
            into = type;
            return true;
        }
    return false;
}

std::string SymbolOfWeldType(WeldType type) {
    // The ISO 2553 glyphs. Where Unicode has the shape it is used; where it
    // does not, the nearest a reader recognises.
    switch (type) {
        case WeldType::SquareButt: return "\xE2\x80\x96";      // parallel bars
        case WeldType::SingleV: return "V";
        case WeldType::SingleBevel: return "\xE2\x95\xB2";     // one slanted face
        case WeldType::SingleU: return "U";
        case WeldType::SingleJ: return "J";
        case WeldType::Fillet: return "\xE2\x97\xBA";          // the right triangle
        case WeldType::Plug: return "\xE2\x96\xAD";            // a filled slot
        case WeldType::Spot: return "\xE2\x97\x8B";
        case WeldType::Seam: return "\xE2\x8A\x9C";
        case WeldType::Backing: return "\xE2\x8C\x92";
        case WeldType::Surfacing: return "\xE2\x8C\x93";
        case WeldType::Edge: return "\xE2\x8A\x93";
    }
    return "\xE2\x97\xBA";
}

// --- fillet sizing ----------------------------------------------------------

std::string_view toString(FilletSizeKind kind) noexcept {
    switch (kind) {
        case FilletSizeKind::Unspecified: return "unspecified";
        case FilletSizeKind::Throat: return "throat";
        case FilletSizeKind::Leg: return "leg";
    }
    return "unspecified";
}

bool ParseFilletSizeKind(std::string_view text, FilletSizeKind& into) noexcept {
    for (const FilletSizeKind kind : kAllSizeKinds)
        if (text == toString(kind)) {
            into = kind;
            return true;
        }
    return false;
}

double ThroatOfMm(double sizeMm, FilletSizeKind kind) noexcept {
    // ONE CONVERSION, ONE CONSTANT. A leg is the throat times the square root
    // of two; the throat of a leg is the leg divided by it.
    if (kind == FilletSizeKind::Leg) return sizeMm / std::sqrt(2.0);
    return sizeMm;
}

// --- contour ----------------------------------------------------------------

std::string_view toString(WeldContour contour) noexcept {
    switch (contour) {
        case WeldContour::AsWelded: return "as-welded";
        case WeldContour::Flat: return "flat";
        case WeldContour::Convex: return "convex";
        case WeldContour::Concave: return "concave";
        case WeldContour::Blended: return "blended";
    }
    return "as-welded";
}

bool ParseWeldContour(std::string_view text, WeldContour& into) noexcept {
    for (const WeldContour contour : kAllContours)
        if (text == toString(contour)) {
            into = contour;
            return true;
        }
    return false;
}

std::string SymbolOfContour(WeldContour contour) {
    switch (contour) {
        // AS WELDED IS NOT A SYMBOL. It is the absence of one, which is what
        // "no finishing is specified" looks like on paper.
        case WeldContour::AsWelded: return {};
        case WeldContour::Flat: return "\xE2\x80\x94";
        case WeldContour::Convex: return "\xE2\x8C\x92";
        case WeldContour::Concave: return "\xE2\x8C\xA3";
        // Unicode has no glyph for smoothly blended toes; this is the nearest
        // shape a reader recognises.
        case WeldContour::Blended: return "\xE2\x88\xBF";
    }
    return {};
}

// --- the run ----------------------------------------------------------------

double DepositedLengthMm(const WeldRun& run) noexcept {
    if (run.count < 1) return 0.0;
    return static_cast<double>(run.count) * run.lengthMm;
}

double RunExtentMm(const WeldRun& run) noexcept {
    if (run.count < 1) return 0.0;
    // The metal, plus one gap fewer than there are welds. Reading ISO's gap as
    // AWS's pitch gives count * pitch instead, which is shorter.
    return DepositedLengthMm(run) + static_cast<double>(run.count - 1) * run.gapMm;
}

double PitchMm(const WeldRun& run) noexcept { return run.lengthMm + run.gapMm; }

// --- the process reference --------------------------------------------------

std::string_view NameOfWeldProcess(std::string_view reference) noexcept {
    // A tail usually reads "ISO 4063-135". Take the number off the end of that
    // if it is there, and look up whatever is left.
    constexpr std::string_view kPrefix = "ISO 4063-";
    if (reference.size() > kPrefix.size() && reference.substr(0, kPrefix.size()) == kPrefix)
        reference = reference.substr(kPrefix.size());
    struct Process {
        std::string_view number;
        std::string_view name;
    };
    // ISO 4063's common numbers. NOT a gate -- see the header.
    static const Process kProcesses[] = {
        {"111", "manual metal arc"},        {"114", "self-shielded cored wire"},
        {"121", "submerged arc, wire"},     {"122", "submerged arc, strip"},
        {"131", "MIG, solid wire"},         {"135", "MAG, solid wire"},
        {"136", "MAG, flux-cored wire"},    {"138", "MAG, metal-cored wire"},
        {"141", "TIG, solid filler"},       {"15", "plasma arc"},
        {"21", "resistance spot"},          {"22", "resistance seam"},
        {"23", "projection"},               {"24", "flash"},
        {"31", "oxy-fuel gas"},             {"42", "friction"},
        {"51", "electron beam"},            {"52", "laser beam"},
        {"71", "thermit"},                  {"783", "drawn arc stud"}};
    for (const Process& process : kProcesses)
        if (process.number == reference) return process.name;
    return {};
}

// --- refusal and text -------------------------------------------------------

std::string WeldBeadText(const WeldBead& bead) { return BeadTextImpl(bead); }

std::string WhyWeldRefused(const WeldSymbolSpec& spec) {
    // NO SIDE IS NOT A WELD. This is the rule the whole type exists to make
    // unavoidable: there is no third place a bead could have been put.
    if (!spec.arrowSide.has_value() && !spec.otherSide.has_value())
        return "a weld symbol with no weld on either side says nothing -- a bead belongs to "
               "the arrow side or the other side";
    if (spec.arrowSide.has_value()) {
        const std::string why = WhyBeadRefused(*spec.arrowSide);
        if (!why.empty()) return "arrow side: " + why;
    }
    if (spec.otherSide.has_value()) {
        const std::string why = WhyBeadRefused(*spec.otherSide);
        if (!why.empty()) return "other side: " + why;
    }
    // STAGGERED AGAINST NOTHING. A staggered weld IS the relationship between
    // two intermittent runs; with one run, or none, the word describes nothing
    // and the symbol draws as an ordinary double fillet.
    if (spec.staggered) {
        const bool bothRun = spec.arrowSide.has_value() && spec.otherSide.has_value() &&
                             spec.arrowSide->run.has_value() && spec.otherSide->run.has_value();
        if (!bothRun)
            return "a staggered weld is two intermittent runs offset against each other, so "
                   "both sides need a run";
    }
    return {};
}

std::string WeldSymbolText(const WeldSymbolSpec& spec) {
    if (!WhyWeldRefused(spec).empty()) return {};
    std::string text;
    // THE SIDE IS SAID OUT LOUD. On paper the side is which line the symbol
    // sits on; in text there is no line, so leaving it out would be the same
    // ambiguity this whole type was built to remove.
    if (spec.arrowSide.has_value()) text = "arrow " + WeldBeadText(*spec.arrowSide);
    if (spec.otherSide.has_value()) {
        if (!text.empty()) text += " / ";
        text += "other " + WeldBeadText(*spec.otherSide);
    }
    if (spec.staggered) text += ", staggered";
    if (spec.allAround) text += ", all round";
    if (spec.fieldWeld) text += ", field weld";
    if (!spec.tail.empty()) text += " [" + spec.tail + "]";
    return text;
}

} // namespace paramcad
