#pragma once

#include "Core/Expression/ExpressionParser.h"
#include "Core/Expression/ExpressionTypes.h"

#include <functional>
#include <optional>
#include <string_view>

namespace paramcad {

// How a `#name` reference is turned into a value.
//
// A callback rather than a map, because the caller that owns the names is
// PartDocument and it resolves through the ObjectRegistry -- copying every
// parameter into a map on each evaluation would be both slower and a second
// place for the value to be stale. Returning nullopt means "no such variable",
// which the evaluator reports with the name and its position.
using VariableResolver = std::function<std::optional<Quantity>(std::string_view)>;

struct ExpressionEvalResult {
    Quantity value{};
    ExpressionError error{};

    // True when the expression produced a plain number and the FIELD supplied
    // the dimension (see EvaluateExpressionForField). Callers that want to warn
    // "interpreted as millimetres" have the flag; callers that do not can
    // ignore it.
    bool interpretedInFieldUnit{false};

    explicit operator bool() const noexcept { return !error.isError(); }
};

// Evaluate a parsed tree. Never throws. An invalid ParsedExpression yields an
// EmptyExpression error rather than undefined behaviour.
//
// DIMENSION RULES, all enforced at the operation that would break them, so no
// intermediate value ever holds a compound or inverse unit:
//
//   a + b, a - b     dimensions must MATCH exactly
//   a * b            at most one operand may be dimensioned  (mm * mm rejected)
//   a / b            b unitless -> a's dimension
//                    same dimension -> unitless (a ratio)
//                    unitless / dimensioned -> rejected (1/mm)
//   a ^ b            both must be unitless
//   comparisons      dimensions must MATCH exactly; result is unitless 0 or 1
//   c ? a : b        c unitless; a and b must MATCH
//   abs, max, min    dimension-preserving; max/min operands must MATCH
//   sin, cos, tan    angle or unitless (unitless read as radians) -> unitless
//   asin, acos, atan unitless -> angle
//   everything else  unitless -> unitless
//
// NOTE -- an intentional difference (roadmap section 32): there is NO implicit
// promotion of a plain number INSIDE an expression, so `3mm + 2` is an error
// rather than 5 mm. The reference model's documented ternary example
// (`#width>5?7:4`) therefore needs `5mm` in EP3D when `#width` is a length.
// The reason is angles: a promoting rule that reads `#angle > 90` as 90
// RADIANS is worse than an error message, and a rule that promotes lengths but
// not angles is two rules. Promotion happens in exactly one place -- the field
// boundary below -- where the target dimension is known.
ExpressionEvalResult EvaluateExpression(const ParsedExpression& expression,
                                        const VariableResolver& resolver);

// Evaluate for a field of a known dimension.
//
// Accepts a result whose dimension MATCHES the field, and a UNITLESS result,
// which is interpreted in the field's canonical unit (mm for a length, radians
// for an angle) and flagged via `interpretedInFieldUnit`. That is what makes
// typing `5` into a length field mean 5 mm.
//
// The radian default for a unitless angle is a placeholder for the display-unit
// layer EP3D does not have yet (roadmap section 42.3.2). When display units
// arrive, this is the ONE function that changes.
ExpressionEvalResult EvaluateExpressionForField(const ParsedExpression& expression,
                                                const VariableResolver& resolver,
                                                Dimension fieldDimension);

// Parse-and-evaluate in one call, for callers that do not need the variable
// list. Returns the parse error unchanged when parsing fails.
ExpressionEvalResult EvaluateExpressionText(std::string_view text,
                                            const VariableResolver& resolver,
                                            Dimension fieldDimension);

// A resolver that knows no names. Useful for constant-only fields and for
// tests that are not about variables.
VariableResolver NoVariables();

} // namespace paramcad
