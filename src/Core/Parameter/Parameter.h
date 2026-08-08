#pragma once

#include "Core/Document/ObjectId.h"
#include <string>

namespace paramcad {

enum class UnitType {
    Unitless,
    Millimeter,
    Radian,
    Kilogram,
    Second,
    KilogramPerCubicMeter
};

enum class ParameterState { Valid, Dirty, Failed };

class Parameter {
public:
    Parameter(std::string name, double value, UnitType unit);
    // Restore constructor (deserialization): keeps the persisted id and state
    // and advances the id generator past the id so future ids cannot collide.
    Parameter(ObjectId id, std::string name, double value, UnitType unit,
              std::string expression, ParameterState state);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    double value() const noexcept { return value_; }
    UnitType unit() const noexcept { return unit_; }
    const std::string& expression() const noexcept { return expression_; }
    ParameterState state() const noexcept { return state_; }

    void setValue(double value) noexcept;
    void setExpression(std::string expression);

private:
    ObjectId id_;
    std::string name_;
    double value_;
    UnitType unit_;
    std::string expression_;
    ParameterState state_{ParameterState::Valid};
};

} // namespace paramcad
