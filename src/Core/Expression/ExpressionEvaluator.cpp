#include "Core/Expression/ExpressionEvaluator.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace paramcad {
namespace {

// Every evaluation step returns one of these: a value, or an error that already
// carries the source span of the node that produced it.
struct StepResult {
    Quantity value{};
    ExpressionError error{};
    bool ok() const noexcept { return !error.isError(); }
};

StepResult Fail(ExpressionErrorCode code, const ExpressionNode& node, std::string message) {
    StepResult result;
    result.error = {code, node.position, node.length, std::move(message)};
    return result;
}

StepResult Ok(Quantity value) {
    StepResult result;
    result.value = value;
    return result;
}

// A single guard applied after every arithmetic step. Without it an overflow
// or a 0/0 travels silently into the model and fails somewhere with no
// connection to the expression that produced it.
StepResult Finite(const ExpressionNode& node, Quantity value) {
    if (!std::isfinite(value.magnitude))
        return Fail(ExpressionErrorCode::NotFinite, node,
                    "this operation produced a value that is not a finite number");
    return Ok(value);
}

std::string DimensionPair(Dimension a, Dimension b) {
    return std::string(DimensionName(a)) + " and " + DimensionName(b);
}

class Evaluator {
public:
    explicit Evaluator(const VariableResolver& resolver) : resolver_(resolver) {}

    StepResult eval(const ExpressionNode& node) {
        switch (node.kind) {
        case NodeKind::Literal: return Ok(node.literal);
        case NodeKind::Variable: return evalVariable(node);
        case NodeKind::Unary: return evalUnary(node);
        case NodeKind::Binary: return evalBinary(node);
        case NodeKind::Ternary: return evalTernary(node);
        case NodeKind::Function: return evalFunction(node);
        }
        return Fail(ExpressionErrorCode::UnexpectedToken, node, "unrecognised expression node");
    }

private:
    StepResult evalVariable(const ExpressionNode& node) {
        if (!resolver_)
            return Fail(ExpressionErrorCode::UnknownVariable, node,
                        "'#" + node.name + "' cannot be resolved here: this field takes "
                                           "no variables");
        const std::optional<Quantity> value = resolver_(node.name);
        if (!value.has_value())
            return Fail(ExpressionErrorCode::UnknownVariable, node,
                        "there is no variable named '" + node.name + "'");
        return Finite(node, *value);
    }

    StepResult evalUnary(const ExpressionNode& node) {
        StepResult operand = eval(*node.children[0]);
        if (!operand.ok()) return operand;
        if (node.unaryOp == UnaryOp::Negate) operand.value.magnitude = -operand.value.magnitude;
        return Finite(node, operand.value);
    }

    StepResult evalBinary(const ExpressionNode& node) {
        StepResult left = eval(*node.children[0]);
        if (!left.ok()) return left;
        StepResult right = eval(*node.children[1]);
        if (!right.ok()) return right;

        const Quantity a = left.value;
        const Quantity b = right.value;

        switch (node.binaryOp) {
        case BinaryOp::Add:
        case BinaryOp::Subtract: {
            if (a.dimension != b.dimension)
                return Fail(ExpressionErrorCode::DimensionMismatch, node,
                            "cannot add or subtract " + DimensionPair(a.dimension, b.dimension) +
                                "; give both sides the same unit");
            const double sum = (node.binaryOp == BinaryOp::Add)
                                   ? a.magnitude + b.magnitude
                                   : a.magnitude - b.magnitude;
            return Finite(node, Quantity{sum, a.dimension});
        }
        case BinaryOp::Multiply: {
            if (a.dimension != Dimension::Unitless && b.dimension != Dimension::Unitless)
                return Fail(ExpressionErrorCode::CompoundUnit, node,
                            "multiplying " + DimensionPair(a.dimension, b.dimension) +
                                " would produce a compound unit, which a field cannot hold");
            const Dimension result =
                (a.dimension == Dimension::Unitless) ? b.dimension : a.dimension;
            return Finite(node, Quantity{a.magnitude * b.magnitude, result});
        }
        case BinaryOp::Divide: {
            if (std::abs(b.magnitude) < kExpressionZeroTolerance)
                return Fail(ExpressionErrorCode::DivisionByZero, node, "division by zero");
            if (b.dimension == Dimension::Unitless)
                return Finite(node, Quantity{a.magnitude / b.magnitude, a.dimension});
            if (a.dimension == b.dimension)
                return Finite(node,
                              Quantity{a.magnitude / b.magnitude, Dimension::Unitless});
            if (a.dimension == Dimension::Unitless)
                return Fail(ExpressionErrorCode::InverseUnit, node,
                            "dividing a plain number by " + std::string(DimensionName(b.dimension)) +
                                " would produce an inverse unit, which a field cannot hold");
            return Fail(ExpressionErrorCode::DimensionMismatch, node,
                        "cannot divide " + DimensionPair(a.dimension, b.dimension));
        }
        case BinaryOp::Power: {
            if (a.dimension != Dimension::Unitless)
                return Fail(ExpressionErrorCode::CompoundUnit, node,
                            "raising " + std::string(DimensionName(a.dimension)) +
                                " to a power would produce a compound unit; divide by the "
                                "unit first");
            if (b.dimension != Dimension::Unitless)
                return Fail(ExpressionErrorCode::DimensionMismatch, node,
                            "an exponent must be a plain number, not " +
                                std::string(DimensionName(b.dimension)));
            if (std::abs(a.magnitude) < kExpressionZeroTolerance && b.magnitude < 0.0)
                return Fail(ExpressionErrorCode::DivisionByZero, node,
                            "zero raised to a negative power is undefined");
            // A negative base with a fractional exponent has no real value.
            // std::pow answers NaN, which Finite would report as "not finite" --
            // true but unhelpful, so it is named here instead.
            if (a.magnitude < 0.0 && b.magnitude != std::floor(b.magnitude))
                return Fail(ExpressionErrorCode::DomainError, node,
                            "a negative number cannot be raised to a fractional power");
            return Finite(node, MakeUnitless(std::pow(a.magnitude, b.magnitude)));
        }
        default: break;
        }

        // Comparisons.
        if (a.dimension != b.dimension)
            return Fail(ExpressionErrorCode::DimensionMismatch, node,
                        "cannot compare " + DimensionPair(a.dimension, b.dimension) +
                            "; give both sides the same unit");
        bool truth = false;
        switch (node.binaryOp) {
        case BinaryOp::Less: truth = a.magnitude < b.magnitude; break;
        case BinaryOp::Greater: truth = a.magnitude > b.magnitude; break;
        case BinaryOp::LessEqual: truth = a.magnitude <= b.magnitude; break;
        case BinaryOp::GreaterEqual: truth = a.magnitude >= b.magnitude; break;
        // Exact comparison, deliberately. A tolerance here would be a THIRD
        // tolerance story in the project (kSketchToleranceMm, the solver's
        // residual), invisible in the expression text and impossible for a user
        // to reason about. Anyone who needs a tolerance can write one:
        // `abs(#a - #b) < 0.001mm`.
        case BinaryOp::Equal: truth = a.magnitude == b.magnitude; break;
        case BinaryOp::NotEqual: truth = a.magnitude != b.magnitude; break;
        default:
            return Fail(ExpressionErrorCode::UnexpectedToken, node, "unrecognised operator");
        }
        return Ok(MakeUnitless(truth ? 1.0 : 0.0));
    }

    StepResult evalTernary(const ExpressionNode& node) {
        StepResult condition = eval(*node.children[0]);
        if (!condition.ok()) return condition;
        if (condition.value.dimension != Dimension::Unitless)
            return Fail(ExpressionErrorCode::DimensionMismatch, node,
                        "a condition must be a plain number or a comparison, not " +
                            std::string(DimensionName(condition.value.dimension)));

        // BOTH branches are evaluated, not just the taken one.
        //
        // That is deliberate: an expression whose untaken branch is malformed
        // is a defect the user should see NOW, not on the day a parameter
        // changes and the other branch is selected. The cost is that
        // `#x==0 ? 0 : 1/#x` reports a division by zero -- which is why the
        // message names the branch. Recorded so the trade is visible rather
        // than looking like an oversight.
        StepResult thenBranch = eval(*node.children[1]);
        if (!thenBranch.ok()) return thenBranch;
        StepResult elseBranch = eval(*node.children[2]);
        if (!elseBranch.ok()) return elseBranch;

        if (thenBranch.value.dimension != elseBranch.value.dimension)
            return Fail(ExpressionErrorCode::DimensionMismatch, node,
                        "the two results of a conditional must have the same unit, but "
                        "they are " +
                            DimensionPair(thenBranch.value.dimension,
                                          elseBranch.value.dimension));

        const bool taken = condition.value.magnitude != 0.0;
        return Ok(taken ? thenBranch.value : elseBranch.value);
    }

    StepResult requireUnitless(const ExpressionNode& node, const Quantity& value,
                               const char* functionName) {
        if (value.dimension == Dimension::Unitless) return Ok(value);
        return Fail(ExpressionErrorCode::DimensionMismatch, node,
                    std::string(functionName) + "() needs a plain number, not " +
                        DimensionName(value.dimension) + "; divide by the unit first, "
                        "as in " + functionName + "(#x / 1mm)");
    }

    StepResult evalFunction(const ExpressionNode& node) {
        std::vector<Quantity> args;
        args.reserve(node.children.size());
        for (const ExpressionNodePtr& child : node.children) {
            StepResult argument = eval(*child);
            if (!argument.ok()) return argument;
            args.push_back(argument.value);
        }
        const char* name = FunctionKindName(node.function);

        switch (node.function) {
        case FunctionKind::Abs:
            return Finite(node, Quantity{std::abs(args[0].magnitude), args[0].dimension});

        case FunctionKind::Max:
        case FunctionKind::Min: {
            const Dimension dimension = args[0].dimension;
            double best = args[0].magnitude;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i].dimension != dimension)
                    return Fail(ExpressionErrorCode::DimensionMismatch, node,
                                std::string(name) + "() cannot mix " +
                                    DimensionPair(dimension, args[i].dimension));
                best = (node.function == FunctionKind::Max)
                           ? std::max(best, args[i].magnitude)
                           : std::min(best, args[i].magnitude);
            }
            return Finite(node, Quantity{best, dimension});
        }

        case FunctionKind::Sin:
        case FunctionKind::Cos:
        case FunctionKind::Tan: {
            // An angle, or a plain number read as radians -- the mathematical
            // convention, and unambiguous because radians ARE the canonical
            // angle unit here.
            if (args[0].dimension == Dimension::Length)
                return Fail(ExpressionErrorCode::DimensionMismatch, node,
                            std::string(name) + "() needs an angle, not a length");
            const double x = args[0].magnitude;
            double y = 0.0;
            if (node.function == FunctionKind::Sin) y = std::sin(x);
            else if (node.function == FunctionKind::Cos) y = std::cos(x);
            else y = std::tan(x); // tan is infinite at pi/2; Finite() catches it
            return Finite(node, MakeUnitless(y));
        }

        case FunctionKind::Asin:
        case FunctionKind::Acos: {
            StepResult checked = requireUnitless(node, args[0], name);
            if (!checked.ok()) return checked;
            if (args[0].magnitude < -1.0 || args[0].magnitude > 1.0)
                return Fail(ExpressionErrorCode::DomainError, node,
                            std::string(name) + "() is only defined between -1 and 1");
            const double y = (node.function == FunctionKind::Asin) ? std::asin(args[0].magnitude)
                                                                   : std::acos(args[0].magnitude);
            return Finite(node, MakeAngleRad(y));
        }

        case FunctionKind::Atan: {
            StepResult checked = requireUnitless(node, args[0], name);
            if (!checked.ok()) return checked;
            return Finite(node, MakeAngleRad(std::atan(args[0].magnitude)));
        }

        case FunctionKind::Sqrt: {
            StepResult checked = requireUnitless(node, args[0], name);
            if (!checked.ok()) return checked;
            if (args[0].magnitude < 0.0)
                return Fail(ExpressionErrorCode::DomainError, node,
                            "sqrt() is not defined for a negative number");
            return Finite(node, MakeUnitless(std::sqrt(args[0].magnitude)));
        }

        case FunctionKind::Log:
        case FunctionKind::Log10: {
            StepResult checked = requireUnitless(node, args[0], name);
            if (!checked.ok()) return checked;
            if (args[0].magnitude <= 0.0)
                return Fail(ExpressionErrorCode::DomainError, node,
                            std::string(name) + "() is only defined for a positive number");
            const double y = (node.function == FunctionKind::Log) ? std::log(args[0].magnitude)
                                                                  : std::log10(args[0].magnitude);
            return Finite(node, MakeUnitless(y));
        }

        case FunctionKind::Ceil:
        case FunctionKind::Floor:
        case FunctionKind::Round: {
            // Unitless only. Rounding a dimensioned value would silently round
            // in the CANONICAL unit -- to the nearest millimetre, or the
            // nearest RADIAN -- which is almost never what was meant. The
            // message says how to ask for it explicitly.
            StepResult checked = requireUnitless(node, args[0], name);
            if (!checked.ok()) return checked;
            const double x = args[0].magnitude;
            double y = 0.0;
            if (node.function == FunctionKind::Ceil) y = std::ceil(x);
            else if (node.function == FunctionKind::Floor) y = std::floor(x);
            else y = std::round(x);
            return Finite(node, MakeUnitless(y));
        }
        }
        return Fail(ExpressionErrorCode::UnknownFunction, node, "unrecognised function");
    }

    const VariableResolver& resolver_;
};

} // namespace

ExpressionEvalResult EvaluateExpression(const ParsedExpression& expression,
                                        const VariableResolver& resolver) {
    ExpressionEvalResult result;
    if (!expression.valid()) {
        result.error = {ExpressionErrorCode::EmptyExpression, 0, 0,
                        "there is no expression to evaluate"};
        return result;
    }
    Evaluator evaluator(resolver);
    const StepResult step = evaluator.eval(*expression.root());
    result.value = step.value;
    result.error = step.error;
    return result;
}

ExpressionEvalResult EvaluateExpressionForField(const ParsedExpression& expression,
                                                const VariableResolver& resolver,
                                                Dimension fieldDimension) {
    ExpressionEvalResult result = EvaluateExpression(expression, resolver);
    if (!result) return result;

    if (result.value.dimension == fieldDimension) return result;
    if (result.value.dimension == Dimension::Unitless) {
        // The one promotion site. The magnitude is already the number the user
        // typed, and the field's canonical unit is what it now means.
        result.value.dimension = fieldDimension;
        result.interpretedInFieldUnit = true;
        return result;
    }

    const ExpressionNode* root = expression.root();
    result.error = {ExpressionErrorCode::DimensionMismatch,
                    root != nullptr ? root->position : 0,
                    root != nullptr ? root->length : 0,
                    "this field takes " + std::string(DimensionName(fieldDimension)) +
                        ", but the expression produces " +
                        DimensionName(result.value.dimension)};
    return result;
}

ExpressionEvalResult EvaluateExpressionText(std::string_view text,
                                            const VariableResolver& resolver,
                                            Dimension fieldDimension) {
    ExpressionParseResult parsed = ParseExpression(text);
    if (!parsed) {
        ExpressionEvalResult result;
        result.error = parsed.error;
        return result;
    }
    return EvaluateExpressionForField(parsed.expression, resolver, fieldDimension);
}

VariableResolver NoVariables() {
    return [](std::string_view) { return std::optional<Quantity>{}; };
}

} // namespace paramcad
