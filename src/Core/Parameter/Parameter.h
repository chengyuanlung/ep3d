#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Expression/ExpressionTypes.h"
#include <optional>
#include <string_view>
#include <string>
#include <utility>

namespace paramcad {

enum class UnitType {
    Unitless,
    Millimeter,
    Radian,
    Kilogram,
    Second,
    KilogramPerCubicMeter
};

// Which expression Dimension a UnitType corresponds to, or nullopt when the
// unit has none (M11.2).
//
// Only three UnitTypes map: Unitless, Millimeter -> Length, Radian -> Angle.
// Kilogram, Second and KilogramPerCubicMeter deliberately do NOT, and that is a
// consequence of ADR-M11-001: the expression model has three dimensions because
// every compound unit is refused at the operation that would create one, and
// kg/m3 IS compound. Rather than bend the model to fit a unit no field currently
// accepts an expression for, a parameter in one of those units takes a literal
// value only, and an expression on it is refused with a message that says so.
std::optional<Dimension> ExpressionDimensionOf(UnitType unit) noexcept;

// What VALUES a parameter may hold, independently of what UNIT it carries
// (M11.4).
//
// Deliberately NOT folded into UnitType. Integer and Boolean are not units, and
// putting them beside Millimeter would be a category error that also loses a
// real combination: `Millimeter + Integral` is a length in whole millimetres,
// which is a thing a machinist asks for. The two axes are orthogonal, so they
// are two enums.
//
// The roadmap (section 43) lists Length / Angle / Integer / Real / Text /
// Boolean as one flat set of "types". Three of those are already UnitType
// (Millimeter / Radian / Unitless); the remaining useful two are value
// constraints, which is what this is. Text is the one genuinely absent kind and
// it is deferred with a reason -- see ADR-M11-016.
enum class ValueDomain {
    Continuous, // any finite double
    Integral,   // whole numbers only -- a count, a number of instances
    Boolean     // 0 or 1 only -- a flag, readable in a ternary: #Flag ? 10mm : 20mm
};

const char* ValueDomainName(ValueDomain domain) noexcept;

// True when `value` is one this domain admits. Non-finite is never admitted, by
// any domain.
bool ValueFitsDomain(double value, ValueDomain domain) noexcept;

// Why a name cannot be used (M11.4).
//
// The rules are NOT invented here: they are exactly what the expression lexer
// can read after a `#`. A name that cannot be lexed is a name no expression can
// ever refer to, so accepting one creates a parameter that is unreachable by
// the only mechanism that reaches parameters by name.
enum class ParameterNameError {
    None,
    Empty,
    BadFirstCharacter, // a digit, or anything that is not a letter or underscore
    BadCharacter,      // a space, a hyphen, punctuation, a non-ASCII character
    AlreadyUsed        // another parameter in this document has it (case-sensitive)
};

const char* ParameterNameErrorText(ParameterNameError error) noexcept;

// Shape only -- knows nothing about which names are taken. PartDocument adds
// the uniqueness half, because only a document can answer it.
ParameterNameError ValidateParameterName(std::string_view name) noexcept;

enum class ParameterState { Valid, Dirty, Failed };

class Parameter {
public:
    Parameter(std::string name, double value, UnitType unit,
              ValueDomain domain = ValueDomain::Continuous);
    // Restore constructor (deserialization): keeps the persisted id and state
    // and advances the id generator past the id so future ids cannot collide.
    Parameter(ObjectId id, std::string name, double value, UnitType unit,
              std::string expression, ParameterState state,
              ValueDomain domain = ValueDomain::Continuous);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    double value() const noexcept { return value_; }
    UnitType unit() const noexcept { return unit_; }
    ValueDomain domain() const noexcept { return domain_; }
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
    // M11.2: an expression that cannot be evaluated leaves its parameter
    // FAILED, so the recompute engine can block the dependents rather than
    // letting them read a stale value that looks current.
    void markEvaluationFailed() noexcept { state_ = ParameterState::Failed; }

    ObjectId id_;
    // PRIVATE with PartDocument as the only caller (M17.16, ADR-M17-039).
    //
    // A rename is ONE undo step and must refuse a duplicate; both decisions
    // live in PartDocument::renameObject, and a public setter here would be a
    // way around both. Every other name-writing rule in this file is enforced
    // the same way rather than described in a comment.
    friend class PartDocument;
    void setName(std::string name) { name_ = std::move(name); }

    std::string name_;
    double value_;
    UnitType unit_;
    ValueDomain domain_{ValueDomain::Continuous};
    std::string expression_;
    ParameterState state_{ParameterState::Valid};
};

} // namespace paramcad
