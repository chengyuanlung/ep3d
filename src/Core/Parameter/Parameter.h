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

private:
    // ALL mutators are private with the document facade and the recompute
    // engine as the only callers (M8 round 3, R1R3-M2): `parameters().items()`
    // leaks mutable Parameter* through a const PartDocument, and a public
    // setValue let a "const" document be edited past the dirty-propagation
    // machinery -- recompute then reported success while mass read yesterday's
    // volume as current. Same accessor-hazard class as sketches() (M5) and
    // Body::addFeature (M8 round 2); mutation now enters through
    // PartDocument::setParameterValue / setParameterExpression only.
    friend class PartDocument;
    friend class DocumentRecomputeEngine;

    void setValue(double value) noexcept;
    void setExpression(std::string expression);

    // ADR-011 bridge: ParameterState is the EVALUATION validity of the
    // value/expression; the document recompute engine transitions it when the
    // parameter's graph node is visited (scalar evaluation is trivial in M2).
    void markEvaluationDirty() noexcept { state_ = ParameterState::Dirty; }
    void markEvaluated() noexcept { state_ = ParameterState::Valid; }

    ObjectId id_;
    std::string name_;
    double value_;
    UnitType unit_;
    std::string expression_;
    ParameterState state_{ParameterState::Valid};
};

} // namespace paramcad
