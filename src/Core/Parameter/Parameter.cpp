#include "Core/Parameter/Parameter.h"
#include <utility>

namespace paramcad {

Parameter::Parameter(std::string name, double value, UnitType unit)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), value_(value), unit_(unit) {}

Parameter::Parameter(ObjectId id, std::string name, double value, UnitType unit,
                     std::string expression, ParameterState state)
    : id_(RestoreObjectId(id)), name_(std::move(name)), value_(value), unit_(unit),
      expression_(std::move(expression)), state_(state) {}

void Parameter::setValue(double value) noexcept {
    value_ = value;
    state_ = ParameterState::Dirty;
}

void Parameter::setExpression(std::string expression) {
    expression_ = std::move(expression);
    state_ = ParameterState::Dirty;
}

} // namespace paramcad
