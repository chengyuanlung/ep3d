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

// --- Annotation (M7, spec 6) ------------------------------------------------
//
// What the source said it MEASURED, as opposed to what it drew. M6 read only
// geometry; M7 reconstructs design intent, and intent is what a dimension
// carries.
//
// The same boundary rules apply as to the geometry above and for the same
// reason: no parser pointer, no `DRW_*` type, no file offset, no array position
// standing in for identity (spec 6). A dimension arrives as the two points it
// measures between and the direction it measures along -- which is enough to
// resolve it against native geometry, and is the whole of what M7.1 needs.

enum class ImportedDimensionKind {
    Linear,   // measured along a fixed direction (DXF rotated/horizontal/vertical)
    Aligned,  // measured along its own definition points
    Radius,
    Diameter,
    Angular
};

const char* ImportedDimensionKindName(ImportedDimensionKind kind) noexcept;

struct ImportedDimension2D {
    ImportedDimensionKind kind{ImportedDimensionKind::Linear};

    // The two points the dimension measures BETWEEN, already in millimetres
    // (DXF group codes 13/23 and 14/24 for a linear dimension). These are the
    // extension-line origins -- the points on the GEOMETRY, not on the
    // annotation -- which is precisely why they can be matched against native
    // entities without ever consulting the annotation lines.
    Vec2 measureFrom{};
    Vec2 measureTo{};

    // Direction the measurement is taken along, radians counter-clockwise from
    // +u (DXF group code 50). Meaningful for Linear; for Aligned the direction
    // IS measureFrom -> measureTo and this is set to match.
    //
    // This is why Linear and Aligned are different kinds rather than one:
    // a rotated linear dimension measures the PROJECTION onto this direction,
    // so for a sloped pair of points it legitimately states a smaller number
    // than their separation. Collapsing the two kinds would silently turn every
    // such dimension into a wrong one.
    double directionRad{0.0};

    // The value the source stated outright (DXF code 42, "actual measurement"),
    // when it stated one at all. Optional in the format and absent from most
    // files, so it is a CROSS-CHECK and never the authority -- see
    // MeasuredValueMm below and ADR-M7-009.
    std::optional<double> statedValueMm{};

    // A text override (DXF code 1). Empty, or the literal "<>", both mean "show
    // the measurement". ANYTHING ELSE means the drawing displays a number that
    // is not the one the geometry encodes, and M7 refuses to guess which the
    // user meant (spec 18).
    std::string textOverride;

    // Optional source metadata, for diagnostics and provenance only. Spec 7:
    // a DXF handle may be retained, but correctness must not depend on it, and
    // nothing here is ever native identity.
    std::string sourceHandle;
};

// What this dimension measures, in millimetres.
//
// Derived from the definition points rather than read from code 42, because
// code 42 is optional, is documented read-only, defaults to zero when absent,
// and cannot be distinguished from a genuine zero. The definition points are
// mandatory for the kinds M7 supports, so they are the authority and code 42 is
// the cross-check (ADR-M7-009).
//
// Linear projects onto `directionRad`; Aligned takes the full separation;
// Radius and Diameter take the separation of their own two points, which mean
// centre-to-rim and rim-to-rim respectively. Returns 0 for Angular, which has
// no linear measurement and is not reconstructed.
double MeasuredValueMm(const ImportedDimension2D& dimension) noexcept;

// The centre of the curve a Radius or Diameter dimension measures.
//
// A radial dimension states the centre outright; a DIAMETRIC one never does,
// so it is derived as the midpoint of the two opposite rim points. Using
// `measureFrom` for both would put a diametric dimension's centre a full radius
// off, where it matches the wrong curve or nothing at all.
//
// Returns the origin for the kinds that have no centre; check the kind first.
Vec2 DimensionCenter(const ImportedDimension2D& dimension) noexcept;

// Everything one file yielded. Ordering within each vector is the order the
// reader encountered the entities, which is convenient for diagnostics and is
// explicitly NOT identity (ADR-M6-004) -- the shuffled-order test exists to
// prove nothing downstream depends on it.
struct ImportedSketchGeometry {
    std::vector<ImportedLine2D> lines;
    std::vector<ImportedCircle2D> circles;
    std::vector<ImportedArc2D> arcs;
    std::vector<ImportedDimension2D> dimensions;
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
