#include "Core/Drawing/Annotation.h"
#include "Core/Text/NumberText.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace paramcad {

// --- surface finish ---------------------------------------------------------

std::string_view toString(SurfaceSymbol symbol) noexcept {
    switch (symbol) {
        case SurfaceSymbol::Basic: return "basic";
        case SurfaceSymbol::Machined: return "machined";
        case SurfaceSymbol::AsCast: return "as-cast";
    }
    return "machined";
}

bool ParseSurfaceSymbol(std::string_view text, SurfaceSymbol& into) noexcept {
    // READ FROM THE SAME LIST IT IS WRITTEN FROM, so a name cannot be written
    // in one spelling and looked for in another.
    for (const SurfaceSymbol symbol :
         {SurfaceSymbol::Basic, SurfaceSymbol::Machined, SurfaceSymbol::AsCast})
        if (text == toString(symbol)) {
            into = symbol;
            return true;
        }
    return false;
}

std::string_view toString(SurfaceLay lay) noexcept {
    switch (lay) {
        case SurfaceLay::Unspecified: return "unspecified";
        case SurfaceLay::Parallel: return "parallel";
        case SurfaceLay::Perpendicular: return "perpendicular";
        case SurfaceLay::Crossed: return "crossed";
        case SurfaceLay::Multi: return "multi";
        case SurfaceLay::Circular: return "circular";
        case SurfaceLay::Radial: return "radial";
        case SurfaceLay::Particulate: return "particulate";
    }
    return "unspecified";
}

bool ParseSurfaceLay(std::string_view text, SurfaceLay& into) noexcept {
    for (const SurfaceLay lay :
         {SurfaceLay::Unspecified, SurfaceLay::Parallel, SurfaceLay::Perpendicular,
          SurfaceLay::Crossed, SurfaceLay::Multi, SurfaceLay::Circular, SurfaceLay::Radial,
          SurfaceLay::Particulate})
        if (text == toString(lay)) {
            into = lay;
            return true;
        }
    return false;
}

std::string SymbolOfLay(SurfaceLay lay) {
    switch (lay) {
        case SurfaceLay::Unspecified: return {};
        case SurfaceLay::Parallel: return "=";
        case SurfaceLay::Perpendicular: return "\xE2\x8A\xA5"; // U+22A5 UP TACK
        case SurfaceLay::Crossed: return "X";
        case SurfaceLay::Multi: return "M";
        case SurfaceLay::Circular: return "C";
        case SurfaceLay::Radial: return "R";
        case SurfaceLay::Particulate: return "P";
    }
    return {};
}

const std::vector<double>& PreferredRaSeries() {
    // ISO 4288's R10 series, in micrometres. A LIST TO OFFER, not a gate --
    // see the header. 1.2 is a perfectly good instruction and is not here.
    static const std::vector<double> series{0.012, 0.025, 0.05, 0.1,  0.2,  0.4, 0.8,
                                            1.6,   3.2,   6.3,  12.5, 25.0, 50.0};
    return series;
}

std::string WhySurfaceFinishRefused(const SurfaceFinishSpec& spec) {
    // A ROUGHNESS OF NOTHING IS NOT A SMOOTH SURFACE, it is a specification
    // nobody can meet or measure. Refused rather than drawn as "Ra 0".
    if (!(spec.raMicrometres > 0.0))
        return "a roughness has to be more than nothing -- Ra 0 is a surface nobody can make";
    if (spec.raLowerMicrometres < 0.0) return "a lower roughness limit cannot be negative";
    // A BAND WHOSE LOWER LIMIT IS ABOVE ITS UPPER ONE is empty: no surface
    // satisfies it, and it draws as two perfectly ordinary numbers.
    if (spec.raLowerMicrometres > 0.0 && spec.raLowerMicrometres >= spec.raMicrometres)
        return "the lower roughness limit is not below the upper one, so no surface can meet it";
    if (spec.machiningAllowanceMm < 0.0)
        return "a machining allowance is material left ON, so it cannot be negative";
    // MATERIAL MUST NOT BE REMOVED, AND HERE IS HOW MUCH TO LEAVE FOR
    // MACHINING. One of those two has to go.
    if (spec.symbol == SurfaceSymbol::AsCast && spec.machiningAllowanceMm > 0.0)
        return "this symbol says material must NOT be removed, so a machining allowance "
               "contradicts it";
    return {};
}

std::string SurfaceFinishText(const SurfaceFinishSpec& spec) {
    if (!WhySurfaceFinishRefused(spec).empty()) return {};
    std::string text = "Ra " + ShortNumber(spec.raMicrometres);
    // A BAND IS WRITTEN LOWER-UPPER, in that order, because that is the order
    // a reader expects a range in.
    if (spec.raLowerMicrometres > 0.0)
        text = "Ra " + ShortNumber(spec.raLowerMicrometres) + "-" +
               ShortNumber(spec.raMicrometres);
    if (!spec.process.empty()) text = spec.process + " " + text;
    if (spec.machiningAllowanceMm > 0.0) text += " +" + ShortNumber(spec.machiningAllowanceMm);
    const std::string lay = SymbolOfLay(spec.lay);
    if (!lay.empty()) text += " " + lay;
    return text;
}

// --- geometric tolerance ----------------------------------------------------

std::string_view toString(GeometricCharacteristic characteristic) noexcept {
    switch (characteristic) {
        case GeometricCharacteristic::Straightness: return "straightness";
        case GeometricCharacteristic::Flatness: return "flatness";
        case GeometricCharacteristic::Roundness: return "roundness";
        case GeometricCharacteristic::Cylindricity: return "cylindricity";
        case GeometricCharacteristic::LineProfile: return "line-profile";
        case GeometricCharacteristic::SurfaceProfile: return "surface-profile";
        case GeometricCharacteristic::Parallelism: return "parallelism";
        case GeometricCharacteristic::Perpendicularity: return "perpendicularity";
        case GeometricCharacteristic::Angularity: return "angularity";
        case GeometricCharacteristic::Position: return "position";
        case GeometricCharacteristic::Concentricity: return "concentricity";
        case GeometricCharacteristic::Symmetry: return "symmetry";
        case GeometricCharacteristic::CircularRunout: return "circular-runout";
        case GeometricCharacteristic::TotalRunout: return "total-runout";
    }
    return "position";
}

namespace {

// ONE LIST OF ALL FOURTEEN, so nothing walks a hand-copied subset. A parser
// that knew thirteen would silently turn the fourteenth into position.
const GeometricCharacteristic kAll[] = {
    GeometricCharacteristic::Straightness,     GeometricCharacteristic::Flatness,
    GeometricCharacteristic::Roundness,        GeometricCharacteristic::Cylindricity,
    GeometricCharacteristic::LineProfile,      GeometricCharacteristic::SurfaceProfile,
    GeometricCharacteristic::Parallelism,      GeometricCharacteristic::Perpendicularity,
    GeometricCharacteristic::Angularity,       GeometricCharacteristic::Position,
    GeometricCharacteristic::Concentricity,    GeometricCharacteristic::Symmetry,
    GeometricCharacteristic::CircularRunout,   GeometricCharacteristic::TotalRunout};

} // namespace

bool ParseGeometricCharacteristic(std::string_view text,
                                  GeometricCharacteristic& into) noexcept {
    for (const GeometricCharacteristic characteristic : kAll)
        if (text == toString(characteristic)) {
            into = characteristic;
            return true;
        }
    return false;
}

std::string SymbolOfCharacteristic(GeometricCharacteristic characteristic) {
    // The ISO 1101 glyphs. Where Unicode has the real one it is used; where it
    // does not, the nearest shape a reader recognises.
    switch (characteristic) {
        case GeometricCharacteristic::Straightness: return "\xE2\x80\x94";     // —
        case GeometricCharacteristic::Flatness: return "\xE2\x96\xB1";         // ▱
        case GeometricCharacteristic::Roundness: return "\xE2\x97\x8B";        // ○
        case GeometricCharacteristic::Cylindricity: return "\xE2\x8C\xAD";     // ⌭
        case GeometricCharacteristic::LineProfile: return "\xE2\x8C\x92";      // ⌒
        case GeometricCharacteristic::SurfaceProfile: return "\xE2\x8C\x93";   // ⌓
        case GeometricCharacteristic::Parallelism: return "\xE2\x88\xA5";      // ∥
        case GeometricCharacteristic::Perpendicularity: return "\xE2\x8A\xA5"; // ⊥
        case GeometricCharacteristic::Angularity: return "\xE2\x88\xA0";       // ∠
        case GeometricCharacteristic::Position: return "\xE2\x8C\x96";         // ⌖
        case GeometricCharacteristic::Concentricity: return "\xE2\x8C\x9A";    // ⌚-like
        case GeometricCharacteristic::Symmetry: return "\xE2\x8C\x96";         // ⌖
        case GeometricCharacteristic::CircularRunout: return "\xE2\x86\x97";   // ↗
        case GeometricCharacteristic::TotalRunout: return "\xE2\x86\x97\xE2\x86\x97";
    }
    return "?";
}

DatumNeed DatumNeedOf(GeometricCharacteristic characteristic) noexcept {
    switch (characteristic) {
        // FORM IS ABOUT ONE SURFACE BY ITSELF. "Flat with respect to A" is not
        // a tighter specification, it is a meaningless one -- and it draws as
        // an ordinary frame with an extra box on the end.
        case GeometricCharacteristic::Straightness:
        case GeometricCharacteristic::Flatness:
        case GeometricCharacteristic::Roundness:
        case GeometricCharacteristic::Cylindricity:
            return DatumNeed::Never;
        // PROFILE IS BOTH: a profile of a surface with no datum controls the
        // shape, and with one it controls the shape AND where it sits.
        case GeometricCharacteristic::LineProfile:
        case GeometricCharacteristic::SurfaceProfile:
            return DatumNeed::Either;
        // ORIENTATION, LOCATION AND RUNOUT ARE ALL RELATIONSHIPS. Without a
        // datum there is nothing to be parallel TO, and the frame is
        // unmeasurable while looking complete.
        default:
            return DatumNeed::Always;
    }
}

std::string_view toString(MaterialCondition condition) noexcept {
    switch (condition) {
        case MaterialCondition::RegardlessOfFeatureSize: return "rfs";
        case MaterialCondition::Maximum: return "mmc";
        case MaterialCondition::Least: return "lmc";
    }
    return "rfs";
}

bool ParseMaterialCondition(std::string_view text, MaterialCondition& into) noexcept {
    for (const MaterialCondition condition :
         {MaterialCondition::RegardlessOfFeatureSize, MaterialCondition::Maximum,
          MaterialCondition::Least})
        if (text == toString(condition)) {
            into = condition;
            return true;
        }
    return false;
}

std::string SymbolOfCondition(MaterialCondition condition) {
    switch (condition) {
        // RFS is the default and is written by writing NOTHING. A drawing that
        // spelled it out on every frame would be one a reader stops reading.
        case MaterialCondition::RegardlessOfFeatureSize: return {};
        case MaterialCondition::Maximum: return "\xE2\x93\x82";  // Ⓜ
        case MaterialCondition::Least: return "\xE2\x93\x81";    // Ⓛ
    }
    return {};
}

namespace {

// WHETHER A DIAMETRIC ZONE MEANS ANYTHING FOR THIS CHARACTERISTIC.
//
// A cylindrical zone is a zone about an AXIS or a centre plane. Flatness,
// roundness, cylindricity, the two profiles and the two runouts are all about
// a SURFACE, and a diameter symbol in front of their value is not tighter or
// looser -- it is nonsense that draws as an ordinary frame.
bool ZoneCanBeDiametric(GeometricCharacteristic characteristic) noexcept {
    switch (characteristic) {
        case GeometricCharacteristic::Flatness:
        case GeometricCharacteristic::Roundness:
        case GeometricCharacteristic::Cylindricity:
        case GeometricCharacteristic::LineProfile:
        case GeometricCharacteristic::SurfaceProfile:
        case GeometricCharacteristic::CircularRunout:
        case GeometricCharacteristic::TotalRunout:
            return false;
        default:
            // Straightness of an AXIS, the three orientations of an axis, and
            // the three location characteristics all take one.
            return true;
    }
}

} // namespace

std::string WhyFrameRefused(const FeatureControlFrameSpec& spec) {
    if (!(spec.toleranceMm > 0.0))
        return "a geometric tolerance of nothing is a zone no part can lie inside";

    const DatumNeed need = DatumNeedOf(spec.characteristic);
    // FORM IS ABOUT ONE SURFACE BY ITSELF. A flatness frame with A in it looks
    // like a stricter specification and is a meaningless one.
    if (need == DatumNeed::Never && !spec.datums.empty())
        return std::string(toString(spec.characteristic)) +
               " is a form tolerance -- it is about one surface by itself, so it cannot "
               "refer to a datum";
    // ...AND A RELATIONSHIP NEEDS SOMETHING TO BE RELATED TO. This one is
    // worse: the frame is complete-looking and unmeasurable.
    if (need == DatumNeed::Always && spec.datums.empty())
        return std::string(toString(spec.characteristic)) +
               " is measured against a datum, and this frame names none";

    // PRIMARY, SECONDARY, TERTIARY. A fourth has nowhere to go.
    if (spec.datums.size() > 3)
        return "a frame has room for a primary, a secondary and a tertiary datum, and no more";
    for (std::size_t i = 0; i < spec.datums.size(); ++i) {
        if (spec.datums[i].datumId == kInvalidObjectId)
            return "this frame refers to a datum that is not there";
        // THE SAME DATUM TWICE constrains nothing the first mention did not,
        // and reads as a three-datum reference frame that is really one.
        for (std::size_t j = 0; j < i; ++j)
            if (spec.datums[j].datumId == spec.datums[i].datumId)
                return "this frame names the same datum twice";
    }

    if (spec.diametricZone && !ZoneCanBeDiametric(spec.characteristic))
        return std::string(toString(spec.characteristic)) +
               " controls a surface, so its zone is not a cylinder and cannot carry a "
               "diameter symbol";
    return {};
}

std::string FrameText(const FeatureControlFrameSpec& spec,
                      const std::vector<std::string>& datumLetters) {
    if (!WhyFrameRefused(spec).empty()) return {};
    // THE LETTERS HAVE TO BE THE ONES THE DATUMS ACTUALLY CARRY. A caller that
    // handed in the wrong number of them is a caller that resolved something
    // else, and drawing "A" over a frame that refers to B is precisely the
    // failure this whole arrangement exists to prevent.
    if (datumLetters.size() != spec.datums.size()) return {};

    std::string text = SymbolOfCharacteristic(spec.characteristic);
    text += " ";
    if (spec.diametricZone) text += "\xE2\x8C\x80"; // U+2300 DIAMETER SIGN
    text += ShortNumber(spec.toleranceMm);
    const std::string condition = SymbolOfCondition(spec.condition);
    if (!condition.empty()) text += condition;
    for (std::size_t i = 0; i < spec.datums.size(); ++i) {
        text += " | " + datumLetters[i];
        const std::string each = SymbolOfCondition(spec.datums[i].condition);
        if (!each.empty()) text += each;
    }
    return text;
}

// --- the annotation ---------------------------------------------------------

Annotation::Annotation(AnnotationBody body, DimensionAnchor anchor, Vec2 positionMm,
                       ObjectId layerId)
    : id_(ObjectIdGenerator::Next()), body_(std::move(body)), anchor_(anchor),
      positionMm_(positionMm), layerId_(layerId) {}

Annotation::Annotation(ObjectId id, AnnotationBody body, DimensionAnchor anchor,
                       Vec2 positionMm, ObjectId layerId)
    : id_(RestoreObjectId(id)), body_(std::move(body)), anchor_(anchor),
      positionMm_(positionMm), layerId_(layerId) {}

} // namespace paramcad
