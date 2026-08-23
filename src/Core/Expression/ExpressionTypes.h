#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace paramcad {

// Expression evaluation for numeric fields (roadmap section 42).
//
// ONE parser and ONE evaluator for every field that accepts a number: a
// dimension, a feature parameter, a mate offset, a limit. Core owns the
// grammar; the UI owns only the text box and the error presentation
// (roadmap 42.3.1). A second parser living in a widget is the defect this
// header exists to prevent.

// --- Dimension --------------------------------------------------------------
//
// Three states, NOT an exponent vector, and that is a deliberate consequence of
// the rule the reference model states plainly: an expression must reduce to a
// unit value "to the first power" (roadmap 42.2). Every operation that WOULD
// produce a compound unit (mm * mm) or an inverse unit (1 / mm) is rejected at
// the operation itself, so no intermediate value can ever hold an exponent
// outside {0, 1}. With that guaranteed, an exponent vector would be three
// states wearing a heavier coat -- and it would silently ACCEPT mm^2, which is
// exactly what the rule forbids.
//
// Angle is tracked separately even though it is mathematically dimensionless.
// Without that separation `3mm + 2deg` is arithmetic rather than an error, and
// catching that mistake is one of the two worked examples the rule gives.
enum class Dimension {
    Unitless, // a pure number, or a ratio of two same-dimension quantities
    Length,   // canonical unit: millimetre
    Angle     // canonical unit: radian
};

const char* DimensionName(Dimension dimension) noexcept;

// A number with its dimension. `magnitude` is ALWAYS in the canonical unit of
// its dimension -- mm for Length, radians for Angle (CodingRules 11), the raw
// value for Unitless. Input units are converted at the point the literal is
// read; nothing downstream needs to know that the user typed inches.
struct Quantity {
    double magnitude{0.0};
    Dimension dimension{Dimension::Unitless};
};

constexpr Quantity MakeUnitless(double v) noexcept { return {v, Dimension::Unitless}; }
constexpr Quantity MakeLengthMm(double v) noexcept { return {v, Dimension::Length}; }
constexpr Quantity MakeAngleRad(double v) noexcept { return {v, Dimension::Angle}; }

// --- Units ------------------------------------------------------------------
//
// The set the reference model documents (mm, cm, m, inch, foot, yard for
// length; degree and radian for angle), plus the plural forms it says are
// accepted. `deg` and `rad` are EP3D additions: they are near-universal in
// engineering input and cost nothing, but they are NOT in the reference set --
// recorded here so the difference is visible rather than assumed (A05).
//
// Deliberately ABSENT: `in` and `ft`. Both are common, and both are also
// plausible identifiers; admitting them now would make it harder to add bare
// identifiers to the grammar later. They can be added when that question is
// settled.
struct UnitInfo {
    Dimension dimension{Dimension::Unitless};
    double toCanonical{1.0}; // multiply a magnitude in this unit to get canonical
};

// Case-sensitive, as the reference model's variable names are. Returns nullopt
// for anything not in the table -- the caller reports it with a position rather
// than guessing a dimension.
std::optional<UnitInfo> LookupUnit(std::string_view name) noexcept;

// --- Errors -----------------------------------------------------------------
//
// Every failure carries a POSITION, because "invalid expression" is the message
// roadmap 42.3.3 exists to forbid. `position` is a 0-based offset into the
// source text and `length` is the span to underline; together they let a UI
// point at the offending characters without re-parsing.
enum class ExpressionErrorCode {
    None,
    EmptyExpression,
    UnexpectedCharacter,
    UnexpectedToken,
    UnexpectedEnd,
    TrailingInput,
    DepthLimitExceeded,
    TooManyTerms,
    UnknownUnit,
    UnknownFunction,
    WrongArgumentCount,
    UnknownVariable,
    DimensionMismatch, // adding, comparing or selecting between different dimensions
    CompoundUnit,      // mm * mm, or a power of a dimensioned value
    InverseUnit,       // 1 / mm
    DivisionByZero,
    DomainError,   // sqrt(-1), log(0), asin(2)
    NotFinite,     // an operation produced NaN or infinity
    NumberFormat   // a numeric literal the reader cannot convert
};

const char* ExpressionErrorCodeName(ExpressionErrorCode code) noexcept;

struct ExpressionError {
    ExpressionErrorCode code{ExpressionErrorCode::None};
    std::size_t position{0};
    std::size_t length{0};
    std::string message;

    bool isError() const noexcept { return code != ExpressionErrorCode::None; }
};

// A one-line rendering for logs and tests: "col 7: an angle cannot be added to
// a length". Not a UI string -- a UI has the position and can underline.
std::string DescribeExpressionError(const ExpressionError& error);

// Smallest magnitude treated as a zero divisor. Matched to the project's
// existing length tolerance story (kSketchToleranceMm is 1e-6) one order
// tighter, so a divisor that rounds to nothing is refused rather than producing
// an infinity that only fails later, somewhere else.
inline constexpr double kExpressionZeroTolerance = 1e-12;

// Maximum nesting depth the parser will accept. A bound, not a preference:
// `((((((...` is user input, and an unbounded recursive-descent parser answers
// it with a stack overflow rather than an error message. AGENTS.md's rule about
// bounding loops applies to recursion for the same reason -- a crash reports
// nothing a user can act on.
inline constexpr std::size_t kExpressionMaxDepth = 64;

// Maximum number of nodes one expression may contain.
//
// This bounds the EVALUATOR's recursion, which kExpressionMaxDepth does not.
// The parser folds `1 + 1 + 1 + ...` in a LOOP, so its own recursion depth
// stays constant while the tree it builds leans one node deeper per term -- and
// the evaluator then walks that tree recursively. A thousand-term sum therefore
// parses happily and overflows the stack during evaluation, which reports
// nothing a user can act on.
//
// Capping node count caps tree depth, and 512 nodes is a ~250-term expression:
// far past anything a dimension field will legitimately hold, and far short of
// the stack.
inline constexpr std::size_t kExpressionMaxNodes = 512;

} // namespace paramcad
