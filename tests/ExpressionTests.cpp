#include "Core/Expression/ExpressionEvaluator.h"
#include "Core/Expression/ExpressionParser.h"
#include "Core/Expression/ExpressionTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kTol = 1e-9;

// A resolver backed by a fixed table. Deliberately NOT a document: these tests
// are about the evaluator, and pulling PartDocument in would make every failure
// ambiguous between the two.
VariableResolver TableResolver(std::map<std::string, Quantity> table) {
    return [table = std::move(table)](std::string_view name) -> std::optional<Quantity> {
        const auto it = table.find(std::string(name));
        if (it == table.end()) return std::nullopt;
        return it->second;
    };
}

// Evaluate with no field constraint -- the raw dimension the expression
// produces, which is what most of these tests are pinning.
ExpressionEvalResult Eval(std::string_view text, const VariableResolver& resolver = NoVariables()) {
    ExpressionParseResult parsed = ParseExpression(text);
    if (!parsed) {
        ExpressionEvalResult failed;
        failed.error = parsed.error;
        return failed;
    }
    return EvaluateExpression(parsed.expression, resolver);
}

ExpressionError ParseErrorOf(std::string_view text) {
    return ParseExpression(text).error;
}

} // namespace

// =============================================================================
// Arithmetic, precedence, associativity
// =============================================================================

TEST(ExpressionTest, M11_EXPR_001_PlainNumberEvaluatesUnitless) {
    const ExpressionEvalResult result = Eval("42");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 42.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Unitless);
}

TEST(ExpressionTest, M11_EXPR_002_MultiplicationBindsTighterThanAddition) {
    const ExpressionEvalResult result = Eval("2 + 3 * 4");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 14.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_003_ParenthesesOverridePrecedence) {
    const ExpressionEvalResult result = Eval("(2 + 3) * 4");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 20.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_004_SubtractionIsLeftAssociative) {
    // 10 - 3 - 2 is 5, not 9. A right-associative fold gives 9, so this
    // distinguishes the two.
    const ExpressionEvalResult result = Eval("10 - 3 - 2");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 5.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_005_DivisionIsLeftAssociative) {
    const ExpressionEvalResult result = Eval("100 / 5 / 2");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 10.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_006_PowerIsRightAssociative) {
    // 2^(3^2) = 512, not (2^3)^2 = 64.
    const ExpressionEvalResult result = Eval("2^3^2");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 512.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_007_UnaryMinusBindsLooserThanPower) {
    // -2^2 is -(2^2) = -4, the standard mathematical reading.
    const ExpressionEvalResult result = Eval("-2^2");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, -4.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_008_NegativeExponentIsAccepted) {
    const ExpressionEvalResult result = Eval("2^-1");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 0.5, kTol);
}

TEST(ExpressionTest, M11_EXPR_009_ScientificNotationIsRead) {
    const ExpressionEvalResult result = Eval("1.5e3");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 1500.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_010_ExponentMarkerWithoutDigitsIsNotSwallowed) {
    // "3e" must not be read as a number with an empty exponent. The 'e' falls
    // through to the identifier path and is reported as an unknown unit, which
    // is the useful message -- not a number-format failure.
    const ExpressionError error = ParseErrorOf("3e");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnknownUnit);
    EXPECT_EQ(error.position, 1u);
}

// =============================================================================
// Units
// =============================================================================

TEST(ExpressionTest, M11_EXPR_020_MillimetreIsCanonicalAndUnchanged) {
    const ExpressionEvalResult result = Eval("3mm");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 3.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
}

TEST(ExpressionTest, M11_EXPR_021_LengthUnitsConvertToMillimetres) {
    struct Row { const char* text; double expectedMm; };
    // Exact by definition; a wrong factor in any single row fails only that row.
    const Row rows[] = {
        {"1 mm", 1.0}, {"1 cm", 10.0}, {"1 m", 1000.0},
        {"1 inch", 25.4}, {"2 inches", 50.8},
        {"1 foot", 304.8}, {"2 feet", 609.6},
        {"1 yard", 914.4}, {"2 yards", 1828.8},
    };
    for (const Row& row : rows) {
        const ExpressionEvalResult result = Eval(row.text);
        ASSERT_TRUE(result) << row.text << ": " << DescribeExpressionError(result.error);
        EXPECT_NEAR(result.value.magnitude, row.expectedMm, 1e-9) << row.text;
        EXPECT_EQ(result.value.dimension, Dimension::Length) << row.text;
    }
}

TEST(ExpressionTest, M11_EXPR_022_AngleUnitsConvertToRadians) {
    struct Row { const char* text; double expectedRad; };
    const double pi = std::acos(-1.0);
    const Row rows[] = {
        {"180 degrees", pi}, {"180 degree", pi}, {"180 deg", pi},
        {"1 radian", 1.0}, {"1 radians", 1.0}, {"1 rad", 1.0},
    };
    for (const Row& row : rows) {
        const ExpressionEvalResult result = Eval(row.text);
        ASSERT_TRUE(result) << row.text << ": " << DescribeExpressionError(result.error);
        EXPECT_NEAR(result.value.magnitude, row.expectedRad, 1e-12) << row.text;
        EXPECT_EQ(result.value.dimension, Dimension::Angle) << row.text;
    }
}

TEST(ExpressionTest, M11_EXPR_023_UnitSuffixBindsToItsOwnLiteralOnly) {
    // 2 * 3mm is 6 mm -- the unit attaches to the 3, not to the product.
    const ExpressionEvalResult result = Eval("2 * 3mm");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 6.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
}

TEST(ExpressionTest, M11_EXPR_024_UnknownUnitIsReportedAtItsOwnPosition) {
    const ExpressionError error = ParseErrorOf("5 furlongs");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnknownUnit);
    EXPECT_EQ(error.position, 2u);
    EXPECT_EQ(error.length, 8u);
}

TEST(ExpressionTest, M11_EXPR_025_UnitMayNotFollowAParenthesisedValue) {
    const ExpressionError error = ParseErrorOf("(2+3)mm");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedToken);
    EXPECT_EQ(error.position, 5u);
}

TEST(ExpressionTest, M11_EXPR_026_UnitMayNotFollowAVariable) {
    const ExpressionError error = ParseErrorOf("#w mm");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedToken);
    EXPECT_EQ(error.position, 3u);
}

TEST(ExpressionTest, M11_EXPR_027_BareUnitNameSaysItNeedsANumber) {
    const ExpressionError error = ParseErrorOf("mm");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnknownFunction);
    EXPECT_NE(error.message.find("needs a number"), std::string::npos) << error.message;
}

TEST(ExpressionTest, M11_EXPR_028_UnitLookupIsCaseSensitive) {
    // "MM" is not "mm". Accepting it would mean deciding what "M" means, and
    // metre-versus-milli is exactly the confusion a CAD field must not invent.
    EXPECT_EQ(ParseErrorOf("5 MM").code, ExpressionErrorCode::UnknownUnit);
}

// =============================================================================
// Unit algebra -- the rules roadmap 42.2 states
// =============================================================================

TEST(ExpressionTest, M11_EXPR_040_SameUnitsAddAndSubtract) {
    const ExpressionEvalResult result = Eval("3mm + 2.5mm");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 5.5, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
}

TEST(ExpressionTest, M11_EXPR_041_MixedUnitsOfTheSameDimensionAdd) {
    const ExpressionEvalResult result = Eval("1 cm + 5 mm");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 15.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_042_LengthPlusAngleIsRejected) {
    // The reference model's own worked example of an invalid expression.
    const ExpressionEvalResult result = Eval("3mm + 2deg");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
    EXPECT_EQ(result.error.position, 4u); // the '+'
}

TEST(ExpressionTest, M11_EXPR_043_LengthTimesLengthIsRejected) {
    // The reference model's second worked example: no compound units.
    const ExpressionEvalResult result = Eval("3mm * 3mm");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::CompoundUnit);
}

TEST(ExpressionTest, M11_EXPR_044_LengthDividedByPlainNumberStaysALength) {
    const ExpressionEvalResult result = Eval("100mm / 2");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 50.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
}

TEST(ExpressionTest, M11_EXPR_045_LengthDividedByLengthIsAPlainRatio) {
    const ExpressionEvalResult result = Eval("100mm / 50mm");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 2.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Unitless);
}

TEST(ExpressionTest, M11_EXPR_046_PlainNumberDividedByLengthIsRejected) {
    const ExpressionEvalResult result = Eval("1 / 2mm");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::InverseUnit);
}

TEST(ExpressionTest, M11_EXPR_047_LengthDividedByAngleIsRejected) {
    const ExpressionEvalResult result = Eval("10mm / 2deg");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionTest, M11_EXPR_048_DimensionedBaseCannotBeRaisedToAPower) {
    const ExpressionEvalResult result = Eval("2mm ^ 2");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::CompoundUnit);
}

TEST(ExpressionTest, M11_EXPR_049_DimensionedExponentIsRejected) {
    const ExpressionEvalResult result = Eval("2 ^ 2mm");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionTest, M11_EXPR_050_DivisionByZeroIsAnError) {
    const ExpressionEvalResult result = Eval("1 / 0");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DivisionByZero);
}

TEST(ExpressionTest, M11_EXPR_051_DivisionByAVanishinglySmallValueIsAlsoRefused) {
    // Below kExpressionZeroTolerance. Producing an infinity here and failing
    // later, elsewhere, is the outcome this guard exists to prevent.
    const ExpressionEvalResult result = Eval("1 / 1e-15");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DivisionByZero);
}

// =============================================================================
// Variables
// =============================================================================

TEST(ExpressionTest, M11_EXPR_060_VariableResolvesThroughTheCallback) {
    const ExpressionEvalResult result =
        Eval("#width", TableResolver({{"width", MakeLengthMm(120.0)}}));
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 120.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
}

TEST(ExpressionTest, M11_EXPR_061_VariableCarriesItsOwnDimensionIntoArithmetic) {
    const ExpressionEvalResult result =
        Eval("#width / 2", TableResolver({{"width", MakeLengthMm(100.0)}}));
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 50.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
}

TEST(ExpressionTest, M11_EXPR_062_UnknownVariableNamesItselfAtItsPosition) {
    const ExpressionEvalResult result = Eval("10 + #nope", TableResolver({}));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::UnknownVariable);
    EXPECT_EQ(result.error.position, 5u);
    EXPECT_NE(result.error.message.find("nope"), std::string::npos) << result.error.message;
}

TEST(ExpressionTest, M11_EXPR_063_ReferencedVariablesAreSortedAndDeduplicated) {
    // This list IS the dependency-edge set (roadmap 42.3.4). Order must be
    // deterministic or two identical documents wire their graph differently.
    ExpressionParseResult parsed = ParseExpression("#b + #a * #b - #c");
    ASSERT_TRUE(parsed) << DescribeExpressionError(parsed.error);
    const std::vector<std::string> expected{"a", "b", "c"};
    EXPECT_EQ(parsed.expression.referencedVariables(), expected);
}

TEST(ExpressionTest, M11_EXPR_064_AnExpressionWithoutVariablesReferencesNone) {
    ExpressionParseResult parsed = ParseExpression("2 * 3mm");
    ASSERT_TRUE(parsed) << DescribeExpressionError(parsed.error);
    EXPECT_TRUE(parsed.expression.referencedVariables().empty());
}

TEST(ExpressionTest, M11_EXPR_065_HashWithoutANameIsRejected) {
    const ExpressionError error = ParseErrorOf("#");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedCharacter);
    EXPECT_EQ(error.position, 0u);
}

TEST(ExpressionTest, M11_EXPR_066_HashFollowedByADigitIsRejected) {
    // Variable names cannot start with a digit; the reader must say so rather
    // than parse "#1" as an anonymous reference.
    EXPECT_EQ(ParseErrorOf("#1abc").code, ExpressionErrorCode::UnexpectedCharacter);
}

TEST(ExpressionTest, M11_EXPR_067_NoResolverMeansVariablesAreRefusedNotCrashed) {
    const ExpressionEvalResult result = Eval("#w", VariableResolver{});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::UnknownVariable);
}

// =============================================================================
// Functions
// =============================================================================

TEST(ExpressionTest, M11_EXPR_080_UnitlessFunctionsComputeTheirValues) {
    struct Row { const char* text; double expected; };
    const Row rows[] = {
        {"ceil(2.1)", 3.0}, {"floor(2.9)", 2.0}, {"round(2.5)", 3.0},
        {"round(-2.5)", -3.0}, {"sqrt(9)", 3.0}, {"abs(-4)", 4.0},
        {"max(1, 7, 3)", 7.0}, {"min(1, 7, 3)", 1.0},
        {"log10(1000)", 3.0},
    };
    for (const Row& row : rows) {
        const ExpressionEvalResult result = Eval(row.text);
        ASSERT_TRUE(result) << row.text << ": " << DescribeExpressionError(result.error);
        EXPECT_NEAR(result.value.magnitude, row.expected, 1e-9) << row.text;
    }
}

TEST(ExpressionTest, M11_EXPR_081_LogIsTheNaturalLogarithm) {
    // Pinned explicitly because the base of `log` is the classic ambiguity and
    // the reference help does not name it. If this ever changes, it changes
    // HERE and the change is visible in review.
    const ExpressionEvalResult result = Eval("log(2.718281828459045)");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 1.0, 1e-12);
}

TEST(ExpressionTest, M11_EXPR_082_TrigAcceptsAnAngleAndReturnsAPlainNumber) {
    const ExpressionEvalResult result = Eval("sin(30 deg)");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 0.5, 1e-12);
    EXPECT_EQ(result.value.dimension, Dimension::Unitless);
}

TEST(ExpressionTest, M11_EXPR_083_TrigReadsAPlainNumberAsRadians) {
    const ExpressionEvalResult result = Eval("cos(0)");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 1.0, 1e-12);
}

TEST(ExpressionTest, M11_EXPR_084_TrigRefusesALength) {
    const ExpressionEvalResult result = Eval("sin(3mm)");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionTest, M11_EXPR_085_InverseTrigReturnsAnAngle) {
    const ExpressionEvalResult result = Eval("asin(0.5)");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_EQ(result.value.dimension, Dimension::Angle);
    EXPECT_NEAR(result.value.magnitude, std::acos(-1.0) / 6.0, 1e-12);
}

TEST(ExpressionTest, M11_EXPR_086_AbsPreservesDimension) {
    const ExpressionEvalResult result = Eval("abs(0mm - 7mm)");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 7.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
}

TEST(ExpressionTest, M11_EXPR_087_MaxPreservesDimensionAndRefusesAMix) {
    const ExpressionEvalResult good = Eval("max(3mm, 7mm)");
    ASSERT_TRUE(good) << DescribeExpressionError(good.error);
    EXPECT_NEAR(good.value.magnitude, 7.0, kTol);
    EXPECT_EQ(good.value.dimension, Dimension::Length);

    const ExpressionEvalResult bad = Eval("max(3mm, 7)");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionTest, M11_EXPR_088_RoundRefusesADimensionedValueAndSaysHow) {
    // Rounding in the canonical unit would round an angle to the nearest
    // RADIAN. The message must tell the user how to ask explicitly.
    const ExpressionEvalResult result = Eval("round(2.6mm)");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
    EXPECT_NE(result.error.message.find("1mm"), std::string::npos) << result.error.message;
}

TEST(ExpressionTest, M11_EXPR_089_DividingByAUnitMakesRoundingExplicit) {
    const ExpressionEvalResult result = Eval("round(2.6mm / 1mm) * 1mm");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 3.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
}

TEST(ExpressionTest, M11_EXPR_090_DomainErrorsAreNamedNotReportedAsNotFinite) {
    EXPECT_EQ(Eval("sqrt(-1)").error.code, ExpressionErrorCode::DomainError);
    EXPECT_EQ(Eval("log(0)").error.code, ExpressionErrorCode::DomainError);
    EXPECT_EQ(Eval("asin(2)").error.code, ExpressionErrorCode::DomainError);
    EXPECT_EQ(Eval("(0-4) ^ 0.5").error.code, ExpressionErrorCode::DomainError);
}

TEST(ExpressionTest, M11_EXPR_091_WrongArgumentCountIsReported) {
    const ExpressionError one = Eval("sqrt(1, 2)").error;
    EXPECT_EQ(one.code, ExpressionErrorCode::WrongArgumentCount);
    const ExpressionError two = Eval("max(1)").error;
    EXPECT_EQ(two.code, ExpressionErrorCode::WrongArgumentCount);
    const ExpressionError three = Eval("sqrt()").error;
    EXPECT_EQ(three.code, ExpressionErrorCode::WrongArgumentCount);
}

TEST(ExpressionTest, M11_EXPR_092_UnknownFunctionIsReportedAtItsPosition) {
    const ExpressionError error = ParseErrorOf("1 + frobnicate(2)");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnknownFunction);
    EXPECT_EQ(error.position, 4u);
}

// =============================================================================
// Comparisons and the conditional
// =============================================================================

TEST(ExpressionTest, M11_EXPR_100_ComparisonsYieldOneOrZero) {
    EXPECT_NEAR(Eval("3 > 2").value.magnitude, 1.0, kTol);
    EXPECT_NEAR(Eval("3 < 2").value.magnitude, 0.0, kTol);
    EXPECT_NEAR(Eval("2 <= 2").value.magnitude, 1.0, kTol);
    EXPECT_NEAR(Eval("2 >= 3").value.magnitude, 0.0, kTol);
    EXPECT_NEAR(Eval("2 == 2").value.magnitude, 1.0, kTol);
    EXPECT_NEAR(Eval("2 != 2").value.magnitude, 0.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_101_ComparisonsCannotBeChained) {
    const ExpressionError error = ParseErrorOf("1 < 2 < 3");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedToken);
    EXPECT_NE(error.message.find("chained"), std::string::npos) << error.message;
}

TEST(ExpressionTest, M11_EXPR_102_ComparingDifferentDimensionsIsRejected) {
    const ExpressionEvalResult result = Eval("3mm > 2deg");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionTest, M11_EXPR_103_ConditionalSelectsABranch) {
    const ExpressionEvalResult high = Eval("7 > 5 ? 7 : 4");
    ASSERT_TRUE(high) << DescribeExpressionError(high.error);
    EXPECT_NEAR(high.value.magnitude, 7.0, kTol);

    const ExpressionEvalResult low = Eval("3 > 5 ? 7 : 4");
    ASSERT_TRUE(low) << DescribeExpressionError(low.error);
    EXPECT_NEAR(low.value.magnitude, 4.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_104_ConditionalIsRightAssociative) {
    // a ? b : c ? d : e  ==  a ? b : (c ? d : e)
    const ExpressionEvalResult result = Eval("0 ? 1 : 0 ? 2 : 3");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 3.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_105_ConditionalBranchesMustAgreeOnDimension) {
    const ExpressionEvalResult result = Eval("1 ? 2mm : 3deg");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionTest, M11_EXPR_106_ConditionalRefusesADimensionedCondition) {
    const ExpressionEvalResult result = Eval("2mm ? 1 : 0");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionTest, M11_EXPR_107_BothBranchesAreEvaluatedSoDefectsSurfaceEarly) {
    // Documented trade (see evalTernary): the untaken branch is checked too, so
    // a malformed branch is reported now rather than on the day it is selected.
    const ExpressionEvalResult result = Eval("1 ? 5 : 1/0");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DivisionByZero);
}

TEST(ExpressionTest, M11_EXPR_108_MissingColonIsReported) {
    const ExpressionError error = ParseErrorOf("1 ? 2");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedToken);
    EXPECT_NE(error.message.find(':'), std::string::npos) << error.message;
}

// =============================================================================
// Parse-level failures and positions
// =============================================================================

TEST(ExpressionTest, M11_EXPR_120_EmptyAndWhitespaceAreEmptyExpressions) {
    EXPECT_EQ(ParseErrorOf("").code, ExpressionErrorCode::EmptyExpression);
    EXPECT_EQ(ParseErrorOf("   \t ").code, ExpressionErrorCode::EmptyExpression);
}

TEST(ExpressionTest, M11_EXPR_121_UnbalancedParenthesisIsReported) {
    EXPECT_EQ(ParseErrorOf("(1 + 2").code, ExpressionErrorCode::UnexpectedToken);
    EXPECT_EQ(ParseErrorOf("1 + 2)").code, ExpressionErrorCode::TrailingInput);
}

TEST(ExpressionTest, M11_EXPR_122_DanglingOperatorReportsTheEnd) {
    const ExpressionError error = ParseErrorOf("1 +");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedEnd);
}

TEST(ExpressionTest, M11_EXPR_123_StrayCharacterIsReportedAtItsPosition) {
    const ExpressionError error = ParseErrorOf("1 + 2 $ 3");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedCharacter);
    EXPECT_EQ(error.position, 6u);
}

TEST(ExpressionTest, M11_EXPR_124_SingleEqualsSuggestsTheComparisonOperator) {
    const ExpressionError error = ParseErrorOf("1 = 1");
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedCharacter);
    EXPECT_NE(error.message.find("=="), std::string::npos) << error.message;
}

TEST(ExpressionTest, M11_EXPR_125_CommaIsNotADecimalSeparator) {
    // An intentional difference from the reference model, recorded in the
    // lexer: accepting it would make max(1,2) ambiguous. "1,5" therefore parses
    // as a number followed by trailing input, never as 1.5.
    const ExpressionError error = ParseErrorOf("1,5");
    EXPECT_EQ(error.code, ExpressionErrorCode::TrailingInput);
    const ExpressionEvalResult twoArgs = Eval("max(1,5)");
    ASSERT_TRUE(twoArgs) << DescribeExpressionError(twoArgs.error);
    EXPECT_NEAR(twoArgs.value.magnitude, 5.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_126_ErrorDescriptionCarriesAOneBasedColumn) {
    const ExpressionError error = ParseErrorOf("1 + 2 $ 3");
    EXPECT_EQ(DescribeExpressionError(error).rfind("col 7:", 0), 0u)
        << DescribeExpressionError(error);
}

TEST(ExpressionTest, M11_EXPR_127_DeepNestingIsBoundedNotCrashed) {
    // A bound, not a preference: unbounded recursive descent answers this input
    // with a stack overflow, which reports nothing a user can act on.
    std::string deep;
    const std::size_t depth = kExpressionMaxDepth + 20;
    deep.reserve(depth * 2 + 1);
    for (std::size_t i = 0; i < depth; ++i) deep.push_back('(');
    deep.push_back('1');
    for (std::size_t i = 0; i < depth; ++i) deep.push_back(')');

    const ExpressionError error = ParseErrorOf(deep);
    EXPECT_EQ(error.code, ExpressionErrorCode::DepthLimitExceeded);
}

TEST(ExpressionTest, M11_EXPR_128_ModestNestingStillParses) {
    // The other side of the bound: the limit must not be so tight that ordinary
    // input trips it.
    const ExpressionEvalResult result = Eval("((((1 + 2))))");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 3.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_129_ALongFlatChainIsBoundedNotCrashed) {
    // The defect the depth guard does NOT catch, found in review: the parser
    // folds `1+1+1+...` in a loop, so its recursion stays shallow while the
    // tree gains a level per term -- and the EVALUATOR walks that tree
    // recursively. Without the node budget this input overflows the stack
    // during evaluation, which reports nothing a user can act on.
    std::string chain = "1";
    for (int i = 0; i < 400; ++i) chain += "+1";

    const ExpressionError error = ParseErrorOf(chain);
    EXPECT_EQ(error.code, ExpressionErrorCode::TooManyTerms);
}

TEST(ExpressionTest, M11_EXPR_130_AModestFlatChainStillEvaluates) {
    // The other side of the bound: 100 terms is well inside the budget, and
    // evaluating it must produce the right number rather than trip the cap.
    std::string chain = "1";
    for (int i = 0; i < 99; ++i) chain += "+1";

    const ExpressionEvalResult result = Eval(chain);
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 100.0, kTol);
}

TEST(ExpressionTest, M11_EXPR_131_AnOverflowingLiteralIsRejectedAtParseTime) {
    // 1e999 converts CLEANLY to infinity, so a "did strtod consume it all?"
    // check passes it. A literal is the one node the evaluator returns without
    // arithmetic, so it never meets the finiteness guard either -- the value
    // reached the model as an infinity.
    const ExpressionError error = ParseErrorOf("1e999");
    EXPECT_EQ(error.code, ExpressionErrorCode::NumberFormat);
    EXPECT_NE(error.message.find("too large"), std::string::npos) << error.message;
}

TEST(ExpressionTest, M11_EXPR_132_ALargeButRepresentableLiteralIsAccepted) {
    const ExpressionEvalResult result = Eval("1e300");
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 1e300, 1e288);
}

// =============================================================================
// The field boundary -- the ONE promotion site
// =============================================================================

TEST(ExpressionTest, M11_EXPR_140_PlainNumberIsAcceptedByALengthFieldAsMillimetres) {
    const ExpressionEvalResult result =
        EvaluateExpressionText("5", NoVariables(), Dimension::Length);
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, 5.0, kTol);
    EXPECT_EQ(result.value.dimension, Dimension::Length);
    EXPECT_TRUE(result.interpretedInFieldUnit);
}

TEST(ExpressionTest, M11_EXPR_141_MatchingDimensionIsNotFlaggedAsInterpreted) {
    const ExpressionEvalResult result =
        EvaluateExpressionText("5mm", NoVariables(), Dimension::Length);
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_FALSE(result.interpretedInFieldUnit);
}

TEST(ExpressionTest, M11_EXPR_142_WrongDimensionForTheFieldIsRejected) {
    const ExpressionEvalResult result =
        EvaluateExpressionText("30 deg", NoVariables(), Dimension::Length);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
    EXPECT_NE(result.error.message.find("length"), std::string::npos) << result.error.message;
}

TEST(ExpressionTest, M11_EXPR_143_AnAngleFieldAcceptsDegreesAndConverts) {
    const ExpressionEvalResult result =
        EvaluateExpressionText("90 deg", NoVariables(), Dimension::Angle);
    ASSERT_TRUE(result) << DescribeExpressionError(result.error);
    EXPECT_NEAR(result.value.magnitude, std::acos(-1.0) / 2.0, 1e-12);
}

TEST(ExpressionTest, M11_EXPR_144_NoPromotionHappensInsideTheExpression) {
    // The intentional difference from the reference model, pinned so it cannot
    // drift silently: promotion is a FIELD rule, not an arithmetic rule.
    const ExpressionEvalResult result =
        EvaluateExpressionText("3mm + 2", NoVariables(), Dimension::Length);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionTest, M11_EXPR_145_ParseFailureIsReturnedUnchangedByTheTextHelper) {
    const ExpressionEvalResult result =
        EvaluateExpressionText("5 furlongs", NoVariables(), Dimension::Length);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::UnknownUnit);
    EXPECT_EQ(result.error.position, 2u);
}

TEST(ExpressionTest, M11_EXPR_146_AnUnevaluatedExpressionObjectFailsCleanly) {
    const ParsedExpression empty;
    EXPECT_FALSE(empty.valid());
    const ExpressionEvalResult result = EvaluateExpression(empty, NoVariables());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, ExpressionErrorCode::EmptyExpression);
}

// =============================================================================
// Source text
// =============================================================================

TEST(ExpressionTest, M11_EXPR_160_SourceTextIsPreservedVerbatim) {
    // Roadmap 42.3.2: a user who typed `1 inch` must not reopen the field to
    // find `25.4`. The canonical value and the original text both exist.
    ExpressionParseResult parsed = ParseExpression(" 1 inch ");
    ASSERT_TRUE(parsed) << DescribeExpressionError(parsed.error);
    EXPECT_EQ(parsed.expression.sourceText(), " 1 inch ");

    const ExpressionEvalResult value =
        EvaluateExpression(parsed.expression, NoVariables());
    ASSERT_TRUE(value) << DescribeExpressionError(value.error);
    EXPECT_NEAR(value.value.magnitude, 25.4, 1e-12);
}

// =============================================================================
// A worked case from the roadmap
// =============================================================================

TEST(ExpressionTest, M11_EXPR_180_TheRoadmapWorkedExample) {
    // Roadmap section 7: `Width`, `Width/2`, `BaseWidth + 20 mm`.
    const VariableResolver resolver = TableResolver({
        {"Width", MakeLengthMm(100.0)},
        {"BaseWidth", MakeLengthMm(80.0)},
    });

    const ExpressionEvalResult direct =
        EvaluateExpressionText("#Width", resolver, Dimension::Length);
    ASSERT_TRUE(direct) << DescribeExpressionError(direct.error);
    EXPECT_NEAR(direct.value.magnitude, 100.0, kTol);

    const ExpressionEvalResult half =
        EvaluateExpressionText("#Width / 2", resolver, Dimension::Length);
    ASSERT_TRUE(half) << DescribeExpressionError(half.error);
    EXPECT_NEAR(half.value.magnitude, 50.0, kTol);

    const ExpressionEvalResult offset =
        EvaluateExpressionText("#BaseWidth + 20 mm", resolver, Dimension::Length);
    ASSERT_TRUE(offset) << DescribeExpressionError(offset.error);
    EXPECT_NEAR(offset.value.magnitude, 100.0, kTol);
}
