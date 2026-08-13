#include "Core/Import/ImportedGeometry.h"

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

const char* ImportSkipReasonName(ImportSkipReason reason) noexcept {
    switch (reason) {
        case ImportSkipReason::UnsupportedEntity: return "unsupported entity";
        case ImportSkipReason::InvalidGeometry: return "invalid geometry";
        case ImportSkipReason::NonFiniteValue: return "non-finite value";
    }
    return "unknown";
}

} // namespace paramcad
