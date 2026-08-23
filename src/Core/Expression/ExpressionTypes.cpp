#include "Core/Expression/ExpressionTypes.h"

#include <array>
#include <string>

namespace paramcad {
namespace {

struct UnitRow {
    std::string_view name;
    Dimension dimension;
    double toCanonical;
};

// Exact conversions, written as the defining ratios rather than as decimals a
// reader has to trust: 1 inch is 25.4 mm BY DEFINITION (international inch),
// and foot and yard are exact multiples of it.
constexpr double kMmPerInch = 25.4;
constexpr double kMmPerFoot = kMmPerInch * 12.0;
constexpr double kMmPerYard = kMmPerFoot * 3.0;

// pi to double precision. Written out rather than taken from <numbers> so the
// value is visible next to the conversion that uses it.
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadPerDegree = kPi / 180.0;

constexpr std::array<UnitRow, 15> kUnits{{
    // Length -- the reference set, plus the plural forms it accepts.
    {"mm", Dimension::Length, 1.0},
    {"cm", Dimension::Length, 10.0},
    {"m", Dimension::Length, 1000.0},
    {"inch", Dimension::Length, kMmPerInch},
    {"inches", Dimension::Length, kMmPerInch},
    {"foot", Dimension::Length, kMmPerFoot},
    {"feet", Dimension::Length, kMmPerFoot},
    {"yard", Dimension::Length, kMmPerYard},
    {"yards", Dimension::Length, kMmPerYard},
    // Angle -- degree/radian and their plurals are the reference set; deg and
    // rad are EP3D additions (see the header).
    {"degree", Dimension::Angle, kRadPerDegree},
    {"degrees", Dimension::Angle, kRadPerDegree},
    {"deg", Dimension::Angle, kRadPerDegree},
    {"radian", Dimension::Angle, 1.0},
    {"radians", Dimension::Angle, 1.0},
    {"rad", Dimension::Angle, 1.0},
}};

} // namespace

const char* DimensionName(Dimension dimension) noexcept {
    switch (dimension) {
    case Dimension::Unitless: return "unitless";
    case Dimension::Length: return "length";
    case Dimension::Angle: return "angle";
    }
    return "unknown";
}

std::optional<UnitInfo> LookupUnit(std::string_view name) noexcept {
    for (const UnitRow& row : kUnits)
        if (row.name == name) return UnitInfo{row.dimension, row.toCanonical};
    return std::nullopt;
}

const char* ExpressionErrorCodeName(ExpressionErrorCode code) noexcept {
    switch (code) {
    case ExpressionErrorCode::None: return "None";
    case ExpressionErrorCode::EmptyExpression: return "EmptyExpression";
    case ExpressionErrorCode::UnexpectedCharacter: return "UnexpectedCharacter";
    case ExpressionErrorCode::UnexpectedToken: return "UnexpectedToken";
    case ExpressionErrorCode::UnexpectedEnd: return "UnexpectedEnd";
    case ExpressionErrorCode::TrailingInput: return "TrailingInput";
    case ExpressionErrorCode::DepthLimitExceeded: return "DepthLimitExceeded";
    case ExpressionErrorCode::TooManyTerms: return "TooManyTerms";
    case ExpressionErrorCode::UnknownUnit: return "UnknownUnit";
    case ExpressionErrorCode::UnknownFunction: return "UnknownFunction";
    case ExpressionErrorCode::WrongArgumentCount: return "WrongArgumentCount";
    case ExpressionErrorCode::UnknownVariable: return "UnknownVariable";
    case ExpressionErrorCode::DimensionMismatch: return "DimensionMismatch";
    case ExpressionErrorCode::CompoundUnit: return "CompoundUnit";
    case ExpressionErrorCode::InverseUnit: return "InverseUnit";
    case ExpressionErrorCode::DivisionByZero: return "DivisionByZero";
    case ExpressionErrorCode::DomainError: return "DomainError";
    case ExpressionErrorCode::NotFinite: return "NotFinite";
    case ExpressionErrorCode::NumberFormat: return "NumberFormat";
    }
    return "Unknown";
}

std::string DescribeExpressionError(const ExpressionError& error) {
    if (!error.isError()) return "no error";
    // 1-based column, because that is how every editor numbers characters and
    // this string is read by humans. `position` stays 0-based for callers that
    // index into the text.
    return "col " + std::to_string(error.position + 1) + ": " + error.message;
}

} // namespace paramcad
