#include "Core/Import/ImportedGeometry.h"

#include <cmath>

namespace paramcad {

const char* ImportedLengthUnitName(ImportedLengthUnit unit) noexcept {
    switch (unit) {
        case ImportedLengthUnit::Unitless: return "unitless";
        case ImportedLengthUnit::Micrometre: return "micrometres";
        case ImportedLengthUnit::Millimeter: return "millimetres";
        case ImportedLengthUnit::Centimeter: return "centimetres";
        case ImportedLengthUnit::Decimetre: return "decimetres";
        case ImportedLengthUnit::Meter: return "metres";
        case ImportedLengthUnit::Decametre: return "decametres";
        case ImportedLengthUnit::Hectometre: return "hectometres";
        case ImportedLengthUnit::Kilometre: return "kilometres";
        case ImportedLengthUnit::Microinch: return "microinches";
        case ImportedLengthUnit::Mil: return "mils";
        case ImportedLengthUnit::Inch: return "inches";
        case ImportedLengthUnit::Foot: return "feet";
        case ImportedLengthUnit::Yard: return "yards";
        case ImportedLengthUnit::Mile: return "miles";
        case ImportedLengthUnit::Unrecognized: return "an unrecognised unit";
    }
    return "an unrecognised unit";
}

std::optional<double> MillimetresPerUnit(ImportedLengthUnit unit) noexcept {
    // Every unit the DXF format defines for $INSUNITS, not just the handful a
    // sample file happened to use. Leaving mils, microns and kilometres
    // unmapped meant a PCB drawing imported 39.4x too large and a survey
    // drawing 1e6x too small, each with a message claiming the file had not
    // stated a usable unit -- which it had.
    switch (unit) {
        case ImportedLengthUnit::Micrometre: return 0.001;
        case ImportedLengthUnit::Millimeter: return 1.0;
        case ImportedLengthUnit::Centimeter: return 10.0;
        case ImportedLengthUnit::Decimetre: return 100.0;
        case ImportedLengthUnit::Meter: return 1000.0;
        case ImportedLengthUnit::Decametre: return 10000.0;
        case ImportedLengthUnit::Hectometre: return 100000.0;
        case ImportedLengthUnit::Kilometre: return 1000000.0;
        // Imperial lengths are written as multiples of the exact 1959
        // definition -- an inch is 25.4 mm exactly -- rather than as rounded
        // decimals, so nothing drifts.
        case ImportedLengthUnit::Microinch: return 25.4e-6;
        case ImportedLengthUnit::Mil: return 25.4e-3;
        case ImportedLengthUnit::Inch: return 25.4;
        case ImportedLengthUnit::Foot: return 12.0 * 25.4;
        case ImportedLengthUnit::Yard: return 36.0 * 25.4;
        case ImportedLengthUnit::Mile: return 63360.0 * 25.4;
        case ImportedLengthUnit::Unitless:
        case ImportedLengthUnit::Unrecognized:
            // No defensible factor. The caller applies the documented default
            // (ADR-M6-002) and records that it did, rather than silently
            // picking 1.0 here and losing the fact that it was a guess.
            return std::nullopt;
    }
    return std::nullopt;
}

const char* ImportedDimensionKindName(ImportedDimensionKind kind) noexcept {
    switch (kind) {
        case ImportedDimensionKind::Linear: return "linear";
        case ImportedDimensionKind::Aligned: return "aligned";
        case ImportedDimensionKind::Radius: return "radius";
        case ImportedDimensionKind::Diameter: return "diameter";
        case ImportedDimensionKind::Angular: return "angular";
    }
    return "unknown";
}

double MeasuredValueMm(const ImportedDimension2D& dimension) noexcept {
    const double du = dimension.measureTo.x - dimension.measureFrom.x;
    const double dv = dimension.measureTo.y - dimension.measureFrom.y;
    switch (dimension.kind) {
        case ImportedDimensionKind::Aligned:
            return std::sqrt(du * du + dv * dv);
        case ImportedDimensionKind::Linear: {
            // The PROJECTION onto the dimension's own direction, not the
            // separation of the points. For a horizontal dimension across a
            // sloped pair of corners those differ, and the projection is the
            // number the drawing displays.
            //
            // std::abs because a dimension measures a magnitude: which
            // definition point was written first is an artefact of how the
            // drawing was authored, and a negative length is not a length.
            const double c = std::cos(dimension.directionRad);
            const double s = std::sin(dimension.directionRad);
            return std::abs(du * c + dv * s);
        }
        case ImportedDimensionKind::Radius:
            // Centre to a point on the curve (DXF codes 10 and 15).
        case ImportedDimensionKind::Diameter:
            // Two opposite points on the curve (DXF codes 15 and 10). Both are
            // plain separations; what differs is what the two points MEAN, and
            // that is carried by the kind so no caller has to infer it.
            return std::sqrt(du * du + dv * dv);
        case ImportedDimensionKind::Angular:
            // No linear measurement to derive. Deliberately 0 rather than the
            // point separation, which would be a plausible wrong answer: an
            // angular dimension is not reconstructed and a caller that reaches
            // here has not checked the kind.
            return 0.0;
    }
    return 0.0;
}

Vec2 DimensionCenter(const ImportedDimension2D& dimension) noexcept {
    switch (dimension.kind) {
        case ImportedDimensionKind::Radius:
            // The centre IS the first point for a radial dimension.
            return dimension.measureFrom;
        case ImportedDimensionKind::Diameter:
            // Halfway between two opposite points on the curve. A diametric
            // dimension never states the centre, so it has to be derived --
            // and taking measureFrom instead, which is a point ON the circle,
            // would put the centre a full radius out and match either the
            // wrong curve or none.
            return Vec2{0.5 * (dimension.measureFrom.x + dimension.measureTo.x),
                        0.5 * (dimension.measureFrom.y + dimension.measureTo.y)};
        case ImportedDimensionKind::Linear:
        case ImportedDimensionKind::Aligned:
        case ImportedDimensionKind::Angular:
            return Vec2{};
    }
    return Vec2{};
}

const char* ImportSkipReasonName(ImportSkipReason reason) noexcept {
    switch (reason) {
        case ImportSkipReason::UnsupportedEntity: return "unsupported entity";
        case ImportSkipReason::InvalidGeometry: return "invalid geometry";
        case ImportSkipReason::NonFiniteValue: return "non-finite value";
    }
    return "unknown";
}

} // namespace paramcad
