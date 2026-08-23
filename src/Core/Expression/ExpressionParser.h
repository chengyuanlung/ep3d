#pragma once

#include "Core/Expression/ExpressionTypes.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// Parsing is separate from evaluation, and that separation is load-bearing
// rather than tidy: PartDocument must know which variables an expression READS
// in order to wire dependency-graph edges (roadmap 42.3.4), and it must know
// that BEFORE any value exists to evaluate with. A combined parse-and-evaluate
// entry point cannot answer that question.

enum class NodeKind {
    Literal,    // a number, already converted to its canonical unit
    Variable,   // #name
    Unary,      // -x, +x
    Binary,     // x + y, x < y, ...
    Ternary,    // c ? a : b
    Function    // sqrt(x), max(a, b)
};

enum class UnaryOp { Negate, Plus };

enum class BinaryOp {
    Add, Subtract, Multiply, Divide, Power,
    Less, Greater, LessEqual, GreaterEqual, Equal, NotEqual
};

// The documented function set (roadmap 42.1), with one addition.
//
// `Log` is the NATURAL logarithm, following the FeatureScript convention. The
// reference help lists only "log" without naming its base, which is the classic
// ambiguity in this area -- so `Log10` is provided explicitly rather than
// leaving a caller to discover the base by experiment. If the reference is
// later confirmed to mean base 10, only this mapping changes.
enum class FunctionKind {
    Ceil, Floor, Round, Sqrt, Abs, Max, Min, Log, Log10,
    Sin, Cos, Tan, Asin, Acos, Atan
};

const char* FunctionKindName(FunctionKind kind) noexcept;

// A tagged node with a child vector, rather than the variant-of-purpose-built-
// structs shape SketchConstraintData uses.
//
// That shape earns its keep where invalid CARDINALITY must be unconstructible
// and the alternatives are reached from many call sites. Neither holds here:
// arity is validated once, at parse time, and the only consumer is the
// evaluator's single switch. A variant would multiply the visitor code without
// closing a bug class -- so the simpler shape is chosen deliberately, not by
// default.
struct ExpressionNode;
using ExpressionNodePtr = std::unique_ptr<ExpressionNode>;

struct ExpressionNode {
    NodeKind kind{NodeKind::Literal};

    Quantity literal{};      // Literal
    std::string name;        // Variable (without the leading '#')
    UnaryOp unaryOp{UnaryOp::Plus};
    BinaryOp binaryOp{BinaryOp::Add};
    FunctionKind function{FunctionKind::Abs};
    std::vector<ExpressionNodePtr> children;

    // Source span, carried so an error raised during EVALUATION (a dimension
    // mismatch, a division by zero) can point at the same characters a parse
    // error would. Without it, half the diagnostics lose their position.
    std::size_t position{0};
    std::size_t length{0};
};

// A parsed expression. Move-only: it owns a tree.
//
// An INVALID ParsedExpression is a legal object -- `ParseExpression` always
// returns one, and `valid()` distinguishes. This avoids an optional wrapper
// around a type that already has an empty state.
class ParsedExpression {
public:
    ParsedExpression() = default;

    ParsedExpression(const ParsedExpression&) = delete;
    ParsedExpression& operator=(const ParsedExpression&) = delete;
    ParsedExpression(ParsedExpression&&) noexcept = default;
    ParsedExpression& operator=(ParsedExpression&&) noexcept = default;

    bool valid() const noexcept { return root_ != nullptr; }
    const ExpressionNode* root() const noexcept { return root_.get(); }

    // The variable names this expression reads, sorted and de-duplicated.
    //
    // This is the input to dependency wiring: one graph edge per name. Sorted
    // so the edge set is deterministic -- an unordered result would make the
    // graph's shape depend on parse order, and identical documents would then
    // serialize differently.
    const std::vector<std::string>& referencedVariables() const noexcept {
        return variables_;
    }

    // The text as the user wrote it. Kept because roadmap 42.3.2 requires it:
    // a user who types `1 inch` must not reopen the field to find `25.4`.
    const std::string& sourceText() const noexcept { return sourceText_; }

private:
    friend struct ExpressionParserAccess;
    ExpressionNodePtr root_;
    std::vector<std::string> variables_;
    std::string sourceText_;
};

struct ExpressionParseResult {
    ParsedExpression expression;
    ExpressionError error;

    explicit operator bool() const noexcept { return !error.isError(); }
};

// Parse `text` into a tree. Never throws; every failure is an ExpressionError
// with a position.
//
// An empty or all-whitespace input is an EmptyExpression error rather than a
// silently valid zero: a field holding nothing and a field holding `0` are
// different states, and only the caller knows which one is acceptable.
ExpressionParseResult ParseExpression(std::string_view text);

} // namespace paramcad
