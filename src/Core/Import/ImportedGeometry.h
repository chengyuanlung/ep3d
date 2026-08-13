#pragma once

#include "Core/Geometry/MathTypes.h"
#include <optional>
#include <string>
#include <vector>

namespace paramcad {

// The import boundary (ADR-M6-003, spec 6).
//
// Everything here is SEMANTIC GEOMETRY plus the metadata M6 needs to explain
// what it did. There is deliberately no parser pointer, no library handle, no
// file offset and no `DRW_*` type: those are the things that make a document
// depend on the tool that produced it, and spec 5 forbids them from reaching
// the model at all.
//
// This header lives in Core and names no DXF type, so Core can be compiled,
// linked and tested with no DXF library present -- the same arrangement
// IGeometryKernel and ISketchSolver have for OCCT and Eigen.
//
// Coordinates here are ALREADY IN MILLIMETRES. Unit conversion happens once, at
// the reader, before anything reaches this struct (ADR-M6-002), so no consumer
// can convert a second time or guess.

// How the file said, or failed to say, what its numbers mean.
enum class ImportedLengthUnit {
    Unitless,     // no $INSUNITS, or $INSUNITS = 0
    Micrometre,
    Millimeter,
    Centimeter,
    Decimetre,
    Meter,
    Decametre,
    Hectometre,
    Kilometre,
    Microinch,
    Mil,
    Inch,
    Foot,
    Yard,
    Mile,
    Unrecognized  // a value the DXF format does not define, or we cannot map
};

const char* ImportedLengthUnitName(ImportedLengthUnit unit) noexcept;

// Millimetres per one unit of `unit`, or nullopt when there is no defensible
// answer (Unitless / Unrecognized -- those take the documented default instead,
// and the result records that it did).
std::optional<double> MillimetresPerUnit(ImportedLengthUnit unit) noexcept;

struct ImportedLine2D {
    Vec2 start{};
    Vec2 end{};
};

struct ImportedCircle2D {
    Vec2 center{};
    double radiusMm{0.0};
};

struct ImportedArc2D {
    Vec2 center{};
    double radiusMm{0.0};
    // Counter-clockwise from +X, in radians, matching both DXF's own convention
    // and the project's (ADR-M5-006). The reader converts from DXF degrees.
    double startAngleRad{0.0};
    double endAngleRad{0.0};
};

// Why an entity in the file did not become geometry. Kept distinct from a
// parse failure: spec 11 requires the diagnostics to tell "unsupported" from
// "malformed" from "invalid", because they call for different user actions.
enum class ImportSkipReason {
    UnsupportedEntity, // a kind M6 does not import (TEXT, SPLINE, INSERT, ...)
    InvalidGeometry,   // a supported kind carrying values the model rejects
    NonFiniteValue     // NaN or infinity in a coordinate, radius or angle
};

const char* ImportSkipReasonName(ImportSkipReason reason) noexcept;

struct ImportedSkip {
    ImportSkipReason reason{ImportSkipReason::UnsupportedEntity};
    std::string entityKind; // as the file named it, e.g. "SPLINE"
    std::string detail;     // why, in words a user can act on
};

// Everything one file yielded. Ordering within each vector is the order the
// reader encountered the entities, which is convenient for diagnostics and is
// explicitly NOT identity (ADR-M6-004) -- the shuffled-order test exists to
// prove nothing downstream depends on it.
struct ImportedSketchGeometry {
    std::vector<ImportedLine2D> lines;
    std::vector<ImportedCircle2D> circles;
    std::vector<ImportedArc2D> arcs;
    std::vector<ImportedSkip> skipped;

    // What the numbers were interpreted as, and whether the file said so.
    ImportedLengthUnit unit{ImportedLengthUnit::Unitless};
    bool unitWasDefaulted{true};
    double millimetresPerUnit{1.0};

    std::size_t importedCount() const noexcept {
        return lines.size() + circles.size() + arcs.size();
    }
};

} // namespace paramcad
