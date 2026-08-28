#pragma once

#include "Core/Parameter/Parameter.h"
#include "Core/Sketch/SketchTypes.h"

#include <string>
#include <vector>

namespace paramcad {

class Sketch;

// MEASURING a sketch selection (M18, roadmap 50.2).
//
// An INSTANTANEOUS QUERY, not a document object. Nothing here has an ObjectId,
// nothing is stored, and nothing is undoable -- ask again after a solve and the
// answer is the new one. That is the whole difference between this and a driven
// dimension (roadmap 50.3.4): a driven dimension is a persistent annotation
// that happens to be read-only, and measuring is looking.
//
// Why it exists at all, and why now rather than at its place in the roadmap:
// every user-assisted validation in this project has so far checked volumes and
// lengths BY HAND (roadmap 31, 38). A number a test can read is the difference
// between a validation that is claimed and one that is measured, and that value
// is spent on every milestone after this one, not on the one where it lands.

// What a measured number IS, which is not always something UnitType can say.
//
// An area is square millimetres, and UnitType has no such member -- it exists
// to type a Parameter, and no parameter in this program is an area. Reusing
// Millimeter for it printed "2827.4 mm" for the area of a 30 mm circle: a
// plausible number with the wrong unit on it, which is worse than no number.
// M55 adds the two a SOLID needs. The same reason M18 gave for the square
// millimetre: a volume printed as "mm" is a plausible number with the wrong
// unit on it, and a mass printed as a length is worse -- somebody puts it in a
// lifting calculation.
enum class MeasureUnit {
    Millimetre,
    SquareMillimetre,
    CubicMillimetre,
    Kilogram,
    Radian,
    Count
};

const char* MeasureUnitSuffix(MeasureUnit unit) noexcept;

struct MeasureItem {
    std::string label;
    double value{0.0};
    MeasureUnit unit{MeasureUnit::Millimetre};
    // Whether the number is EXACT for the geometry it describes, or arrived at
    // by sampling. Reported rather than hidden: a spline's length has no closed
    // form, and a user who is told 128.4 mm without being told it is
    // approximate will use it as though it were not (roadmap 50.3.2).
    bool approximate{false};
};

struct MeasureResult {
    bool ok{false};
    // Why nothing could be measured. Empty when ok.
    std::string message;
    std::vector<MeasureItem> items;
};

// What can be said about ONE entity, or the relationship between TWO.
//
// An empty selection, or more than two, is refused rather than answered for the
// first one or two -- picking a subset would report a number about geometry the
// caller did not ask about, and it would be a plausible number.
MeasureResult MeasureSketch(const Sketch& sketch, const std::vector<SketchElementRef>& selection);

// The length of one piece of sketch geometry, in mm, and whether that length is
// exact. A spline's is sampled; everything else here has a closed form.
//
// Exposed on its own because a profile's perimeter is the sum of its pieces,
// and a second implementation of "how long is an elliptical arc" is a second
// chance to get the elliptic integral wrong.
double SketchGeometryLength(const SketchGeometry& geometry, bool* approximate);

} // namespace paramcad
