#include "Core/Import/ImportedGeometry.h"

namespace paramcad {

const char* ImportedLengthUnitName(ImportedLengthUnit unit) noexcept {
    switch (unit) {
        case ImportedLengthUnit::Unitless: return "unitless";
        case ImportedLengthUnit::Millimeter: return "millimetres";
        case ImportedLengthUnit::Centimeter: return "centimetres";
        case ImportedLengthUnit::Meter: return "metres";
        case ImportedLengthUnit::Inch: return "inches";
        case ImportedLengthUnit::Foot: return "feet";
        case ImportedLengthUnit::Unrecognized: return "an unrecognised unit";
    }
    return "an unrecognised unit";
}

std::optional<double> MillimetresPerUnit(ImportedLengthUnit unit) noexcept {
    switch (unit) {
        case ImportedLengthUnit::Millimeter: return 1.0;
        case ImportedLengthUnit::Centimeter: return 10.0;
        case ImportedLengthUnit::Meter: return 1000.0;
        // Exact by definition since 1959, so it is written as the definition
        // rather than as a rounded decimal: an inch is 25.4 mm exactly.
        case ImportedLengthUnit::Inch: return 25.4;
        case ImportedLengthUnit::Foot: return 12.0 * 25.4;
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
