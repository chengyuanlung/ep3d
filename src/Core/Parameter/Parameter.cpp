#include "Core/Parameter/Parameter.h"
#include <cmath>
#include <utility>

namespace paramcad {

Parameter::Parameter(std::string name, double value, UnitType unit, ValueDomain domain)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), value_(value), unit_(unit),
      domain_(domain) {}

Parameter::Parameter(ObjectId id, std::string name, double value, UnitType unit,
                     std::string expression, ParameterState state, ValueDomain domain)
    : id_(RestoreObjectId(id)), name_(std::move(name)), value_(value), unit_(unit),
      domain_(domain), expression_(std::move(expression)), state_(state) {}

void Parameter::setValue(double value) noexcept {
    value_ = value;
    state_ = ParameterState::Dirty;
}

void Parameter::setExpression(std::string expression) {
    expression_ = std::move(expression);
    state_ = ParameterState::Dirty;
}

const char* ValueDomainName(ValueDomain domain) noexcept {
    switch (domain) {
    case ValueDomain::Continuous: return "Continuous";
    case ValueDomain::Integral: return "Integral";
    case ValueDomain::Boolean: return "Boolean";
    }
    return "Unknown";
}

bool ValueFitsDomain(double value, ValueDomain domain) noexcept {
    if (!std::isfinite(value)) return false;
    switch (domain) {
    case ValueDomain::Continuous: return true;
    case ValueDomain::Integral: return value == std::floor(value);
    case ValueDomain::Boolean: return value == 0.0 || value == 1.0;
    }
    return false;
}

const char* ParameterNameErrorText(ParameterNameError error) noexcept {
    switch (error) {
    case ParameterNameError::None: return "";
    case ParameterNameError::Empty: return "a parameter needs a name";
    case ParameterNameError::BadFirstCharacter:
        return "a name must start with a letter or an underscore";
    case ParameterNameError::BadCharacter:
        return "a name may contain only letters, digits and underscores";
    case ParameterNameError::AlreadyUsed:
        return "another parameter already has that name";
    }
    return "that name cannot be used";
}

ParameterNameError ValidateParameterName(std::string_view name) noexcept {
    // These two predicates are the expression lexer's, restated. Keeping them
    // in step is the whole point: a name this rejects is one `#name` could
    // never have referred to anyway.
    const auto isFirst = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto isRest = [&isFirst](char c) { return isFirst(c) || (c >= '0' && c <= '9'); };

    if (name.empty()) return ParameterNameError::Empty;
    if (!isFirst(name.front())) return ParameterNameError::BadFirstCharacter;
    for (char c : name)
        if (!isRest(c)) return ParameterNameError::BadCharacter;
    return ParameterNameError::None;
}

std::optional<Dimension> ExpressionDimensionOf(UnitType unit) noexcept {
    switch (unit) {
    case UnitType::Unitless: return Dimension::Unitless;
    case UnitType::Millimeter: return Dimension::Length;
    case UnitType::Radian: return Dimension::Angle;
    // No expression dimension. See the header: the expression model has three
    // dimensions by design, and density is compound.
    case UnitType::Kilogram:
    case UnitType::Second:
    case UnitType::KilogramPerCubicMeter: return std::nullopt;
    }
    return std::nullopt;
}

} // namespace paramcad
