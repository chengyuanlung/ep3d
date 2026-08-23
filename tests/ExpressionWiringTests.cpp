// M11.2 -- the expression evaluator wired into the document.
//
// M11.1's tests are about the grammar and the dimension rules; these are about
// the four seams it now crosses: the Parameter facade, the dependency graph,
// the recompute engine and persistence. Everything here goes through
// PartDocument, because a test that reaches past the facade cannot see the
// rules the facade exists to enforce.

#include "Core/Document/PartDocument.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kTol = 1e-9;

std::string SaveToString(const PartDocument& document, bool expectSuccess = true) {
    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    if (expectSuccess) EXPECT_TRUE(saved) << saved.message;
    return saved ? out.str() : std::string{};
}

LoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadPartDocument(in);
}

const Parameter* Find(const PartDocument& document, ObjectId id) {
    return document.parameters().findById(id);
}

double ValueOf(const PartDocument& document, ObjectId id) {
    const Parameter* parameter = Find(document, id);
    return parameter != nullptr ? parameter->value() : 0.0;
}

// A downstream node with no geometry, so propagation can be observed without a
// kernel. It counts its own executions, which is how "did the edge exist?" is
// answered by evidence rather than by inspecting the graph.
class CountingNode final : public IRecomputable {
public:
    ObjectId id() const noexcept override { return id_; }
    RecomputeResult recompute(const RecomputeContext&) override {
        ++runs;
        return {RecomputeStatus::Success, {}};
    }
    int runs = 0;

private:
    ObjectId id_ = ObjectIdGenerator::Next();
};

const RecomputeItemReport* ItemFor(const DocumentRecomputeReport& report, ObjectId id) {
    for (const auto& item : report.items)
        if (item.id == id) return &item;
    return nullptr;
}

} // namespace

// =============================================================================
// The core loop: an expression drives a value, and a change propagates
// =============================================================================

TEST(ExpressionWiringTest, M11_WIRE_001_AnExpressionDrivesTheValueOnRecompute) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();
    const ObjectId height = document.addParameter("Height", 0.0, UnitType::Millimeter).id();

    ExpressionError error;
    ASSERT_TRUE(document.setParameterExpression(height, "#Width / 2", &error))
        << DescribeExpressionError(error);

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_NEAR(ValueOf(document, height), 50.0, kTol);
    EXPECT_EQ(Find(document, height)->state(), ParameterState::Valid);
}

TEST(ExpressionWiringTest, M11_WIRE_002_ChangingThePrerequisiteMovesTheDependent) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();
    const ObjectId height = document.addParameter("Height", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(height, "#Width / 2"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, height), 50.0, kTol);

    // The whole point of M11.2: this is an ordinary Parameter edit, and the
    // dependent follows through the graph the expression built.
    ASSERT_TRUE(document.setParameterValue(width, 120.0));
    EXPECT_EQ(Find(document, height)->state(), ParameterState::Dirty)
        << "the edge did not carry the dirty mark";

    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, height), 60.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_003_TheEdgeReachesPastTheParameterToItsConsumers) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();
    const ObjectId height = document.addParameter("Height", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(height, "#Width / 2"));

    CountingNode consumer;
    ASSERT_TRUE(document.addRecomputableNode(consumer));
    ASSERT_TRUE(document.addDependency(consumer.id(), height));
    ASSERT_TRUE(document.recompute().success);
    const int before = consumer.runs;

    // Width -> Height -> consumer, entirely through existing M2 machinery.
    ASSERT_TRUE(document.setParameterValue(width, 140.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_GT(consumer.runs, before);
    EXPECT_NEAR(ValueOf(document, height), 70.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_004_ChainedExpressionsEvaluateInDependencyOrder) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    const ObjectId c = document.addParameter("C", 0.0, UnitType::Millimeter).id();
    // Declared out of order on purpose: C reads B before B has been given its
    // own expression. Order comes from the graph, never from creation order.
    ASSERT_TRUE(document.setParameterExpression(c, "#B + 1mm"));
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));

    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, b), 20.0, kTol);
    EXPECT_NEAR(ValueOf(document, c), 21.0, kTol);

    ASSERT_TRUE(document.setParameterValue(a, 5.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, b), 10.0, kTol);
    EXPECT_NEAR(ValueOf(document, c), 11.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_005_AnAngleParameterTakesAnAngleExpression) {
    PartDocument document{"Doc"};
    const ObjectId turns = document.addParameter("Turns", 2.0, UnitType::Unitless).id();
    const ObjectId angle = document.addParameter("Angle", 0.0, UnitType::Radian).id();
    ASSERT_TRUE(document.setParameterExpression(angle, "#Turns * 90 deg"));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, angle), 3.14159265358979323846, 1e-12);
}

// =============================================================================
// Refusals -- and the document is unchanged after every one
// =============================================================================

TEST(ExpressionWiringTest, M11_WIRE_020_AParseErrorIsRefusedWithAPosition) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(width, "1 + 2 $ 3", &error));
    EXPECT_EQ(error.code, ExpressionErrorCode::UnexpectedCharacter);
    EXPECT_EQ(error.position, 6u);
    EXPECT_TRUE(Find(document, width)->expression().empty())
        << "a refused edit must leave the document unchanged";
}

TEST(ExpressionWiringTest, M11_WIRE_021_AnUnknownNameIsRefusedWithAPosition) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(width, "10mm + #Nope", &error));
    EXPECT_EQ(error.code, ExpressionErrorCode::UnknownVariable);
    EXPECT_EQ(error.position, 7u);
    EXPECT_TRUE(Find(document, width)->expression().empty());
}

TEST(ExpressionWiringTest, M11_WIRE_022_AnAmbiguousNameIsRefusedAndSaysWhy) {
    // Parameter names are not unique, and `#name` cannot mean two things.
    PartDocument document{"Doc"};
    document.addParameter("Size", 1.0, UnitType::Millimeter);
    document.addParameter("Size", 2.0, UnitType::Millimeter);
    const ObjectId target = document.addParameter("Target", 0.0, UnitType::Millimeter).id();

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(target, "#Size * 2", &error));
    EXPECT_EQ(error.code, ExpressionErrorCode::UnknownVariable);
    EXPECT_NE(error.message.find("two parameters"), std::string::npos) << error.message;
}

TEST(ExpressionWiringTest, M11_WIRE_023_SelfReferenceIsRefused) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(width, "#Width + 1mm", &error));
    EXPECT_NE(error.message.find("itself"), std::string::npos) << error.message;
    EXPECT_TRUE(Find(document, width)->expression().empty());
}

TEST(ExpressionWiringTest, M11_WIRE_024_TheWrongDimensionForTheParameterIsRefused) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(width, "30 deg", &error));
    EXPECT_EQ(error.code, ExpressionErrorCode::DimensionMismatch);
    EXPECT_TRUE(Find(document, width)->expression().empty());
}

TEST(ExpressionWiringTest, M11_WIRE_025_APlainNumberIsAcceptedForALengthParameter) {
    // The field-boundary promotion, reaching the model: `5` means 5 mm.
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(width, "2 + 3"));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, width), 5.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_026_AUnitWithNoExpressionDimensionIsRefused) {
    // Density is compound, and the expression model has three dimensions by
    // design (ADR-M11-001). Refused with a message, never bent to fit.
    PartDocument document{"Doc"};
    const ObjectId mass = document.addParameter("Mass", 1.0, UnitType::Kilogram).id();

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(mass, "2 * 3", &error));
    EXPECT_EQ(error.code, ExpressionErrorCode::DimensionMismatch);
    EXPECT_NE(error.message.find("literal value only"), std::string::npos) << error.message;
}

TEST(ExpressionWiringTest, M11_WIRE_027_ReadingAUnitWithNoExpressionDimensionIsRefused) {
    PartDocument document{"Doc"};
    document.addParameter("Mass", 1.0, UnitType::Kilogram);
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(width, "#Mass * 1mm", &error));
    EXPECT_EQ(error.code, ExpressionErrorCode::DimensionMismatch);
}

TEST(ExpressionWiringTest, M11_WIRE_028_AnExpressionThatCannotBeEvaluatedNowIsRefused) {
    // The trial evaluation. Storing this would leave the parameter Failed on
    // the very next recompute, and the user would meet the failure somewhere
    // other than the edit that caused it.
    PartDocument document{"Doc"};
    document.addParameter("Zero", 0.0, UnitType::Unitless);
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(width, "10mm / #Zero", &error));
    EXPECT_EQ(error.code, ExpressionErrorCode::DivisionByZero);
    EXPECT_TRUE(Find(document, width)->expression().empty());
}

// =============================================================================
// Cycles
// =============================================================================

TEST(ExpressionWiringTest, M11_WIRE_040_ADirectCycleIsRefusedAndNamesThePath) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(a, "#B / 2", &error));
    EXPECT_NE(error.message.find("cycle"), std::string::npos) << error.message;
    // roadmap 42.3.5: the message must list the path, not merely say "cycle".
    EXPECT_NE(error.message.find("A"), std::string::npos) << error.message;
    EXPECT_NE(error.message.find("B"), std::string::npos) << error.message;
}

TEST(ExpressionWiringTest, M11_WIRE_041_AnIndirectCycleIsRefused) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    const ObjectId c = document.addParameter("C", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_TRUE(document.setParameterExpression(c, "#B + 1mm"));

    ExpressionError error;
    EXPECT_FALSE(document.setParameterExpression(a, "#C / 4", &error));
    EXPECT_NE(error.message.find("cycle"), std::string::npos) << error.message;
}

TEST(ExpressionWiringTest, M11_WIRE_042_ARefusedCycleDoesNotDisturbOtherParametersEdges) {
    // NOTE what this does and does not prove. The parameter being edited here
    // has NO previous expression, so the rollback path is not even reached --
    // a mutation that deleted the rollback survived this test. What it does
    // pin is that a refusal leaves everyone ELSE alone. The rollback itself is
    // M11_WIRE_043.
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    const ObjectId c = document.addParameter("C", 4.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_TRUE(document.setParameterExpression(c, "#A + 1mm"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, b), 20.0, kTol);

    // Refused: A cannot read B.
    ASSERT_FALSE(document.setParameterExpression(a, "#B / 2"));

    // B must still be driven by A.
    ASSERT_TRUE(document.setParameterValue(a, 50.0));
    EXPECT_EQ(Find(document, b)->state(), ParameterState::Dirty)
        << "the refusal dropped the surviving expression's edge";
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, b), 100.0, kTol);
    EXPECT_NEAR(ValueOf(document, c), 51.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_043_ARefusedCycleRestoresTheEditedParametersOwnEdges) {
    // THE rollback test. It needs a shape M11_WIRE_042 does not have: the
    // parameter being edited must ALREADY have a working expression, so that
    // detaching its edges before attaching the new ones has something to
    // detach. Without the restore, C keeps the text of its old expression and
    // loses the edge that made it work -- the worst of both, and invisible
    // until the next time A changes.
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId c = document.addParameter("C", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(c, "#A + 1mm"));
    const ObjectId d = document.addParameter("D", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(d, "#C * 2"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, c), 11.0, kTol);

    // C cannot read D: D already reads C.
    ExpressionError error;
    ASSERT_FALSE(document.setParameterExpression(c, "#D / 2", &error));
    EXPECT_NE(error.message.find("cycle"), std::string::npos) << error.message;
    EXPECT_EQ(Find(document, c)->expression(), std::string("#A + 1mm"))
        << "the refused edit must leave the old text in place";

    // And the old text must still be WIRED, not merely present.
    //
    // The recompute here is load-bearing, and a mutation proved it: detaching
    // an edge DIRTIES the dependent all by itself (DependencyGraph::
    // removeDependency invalidates it), so C is dirty after the refusal whether
    // or not the edge came back. Consuming that dirt first is what makes the
    // next line a question about the EDGE instead of about the detach.
    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(Find(document, c)->state(), ParameterState::Valid);
    ASSERT_TRUE(document.setParameterValue(a, 100.0));
    EXPECT_EQ(Find(document, c)->state(), ParameterState::Dirty)
        << "the refusal dropped the edge the surviving expression needs";
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, c), 101.0, kTol);
    EXPECT_NEAR(ValueOf(document, d), 202.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_044_ARefusedCycleAlsoUndoesTheEdgesItHadAlreadyAttached) {
    // The other half of the rollback: references are attached one at a time,
    // and the one that closes the cycle may not be the first. Everything
    // attached before it has to come back off, or the document keeps an edge
    // from an expression it refused to store.
    //
    // Observable because a leftover edge changes what is legal NEXT: with
    // E -> A still attached, giving A an expression that reads E would be
    // refused as a cycle, though nothing in the document says E depends on A.
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId e = document.addParameter("E", 4.0, UnitType::Millimeter).id();
    const ObjectId d = document.addParameter("D", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(d, "#E * 2"));

    // References are visited in sorted order, so A attaches first and D then
    // closes the cycle E -> D -> E.
    ASSERT_FALSE(document.setParameterExpression(e, "#A + #D"));

    // If E -> A survived the refusal, this is refused too.
    ExpressionError error;
    EXPECT_TRUE(document.setParameterExpression(a, "#E / 2", &error))
        << DescribeExpressionError(error);
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, a), 2.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_045_RepointingAnExpressionDetachesTheEdgeItStopsUsing) {
    // A stale edge is invisible to a value check -- evaluation resolves by
    // NAME, so C reads the right number either way. What a leftover edge
    // changes is what still dirties C, and what is still legal afterwards.
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 4.0, UnitType::Millimeter).id();
    const ObjectId c = document.addParameter("C", 0.0, UnitType::Millimeter).id();

    ASSERT_TRUE(document.setParameterExpression(c, "#A + 1mm"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, c), 11.0, kTol);

    ASSERT_TRUE(document.setParameterExpression(c, "#B * 2"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, c), 8.0, kTol);

    // A is no longer an input of C, so touching A must leave C current.
    ASSERT_TRUE(document.setParameterValue(a, 999.0));
    EXPECT_EQ(Find(document, c)->state(), ParameterState::Valid)
        << "the old edge outlived the expression that created it";

    // And with the C -> A edge gone, A may now read C.
    ExpressionError error;
    EXPECT_TRUE(document.setParameterExpression(a, "#C / 2", &error))
        << DescribeExpressionError(error);
}

TEST(ExpressionWiringTest, M11_WIRE_063_ClearingDetachesTheEdgeNotJustTheText) {
    // Same blindness as M11_WIRE_045: after clearing, B holds no expression, so
    // its state stops tracking the graph and a state probe cannot see a
    // leftover edge. What CAN see it is the direction of legality -- with
    // B -> A still attached, A reading B would close a cycle.
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_TRUE(document.recompute().success);

    ASSERT_TRUE(document.setParameterExpression(b, ""));

    ExpressionError error;
    EXPECT_TRUE(document.setParameterExpression(a, "#B / 2", &error))
        << "clearing left the edge behind: " << DescribeExpressionError(error);
}

// =============================================================================
// Clearing, and a typed number replacing a formula
// =============================================================================

TEST(ExpressionWiringTest, M11_WIRE_060_AnEmptyExpressionClearsAndDetaches) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, b), 20.0, kTol);

    ASSERT_TRUE(document.setParameterExpression(b, ""));
    EXPECT_TRUE(Find(document, b)->expression().empty());

    // The edge is gone: A no longer dirties B.
    ASSERT_TRUE(document.recompute().success);
    ASSERT_TRUE(document.setParameterValue(a, 999.0));
    EXPECT_EQ(Find(document, b)->state(), ParameterState::Valid)
        << "clearing an expression left its edge behind";
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, b), 20.0, kTol) << "the last computed value should remain";
}

TEST(ExpressionWiringTest, M11_WIRE_061_WhitespaceOnlyIsNormalisedToEmpty) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(a, "   \t "));
    EXPECT_EQ(Find(document, a)->expression(), std::string{})
        << "two blank states that round-trip differently is one state too many";
}

TEST(ExpressionWiringTest, M11_WIRE_062_ATypedNumberReplacesTheFormula) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_TRUE(document.recompute().success);

    ASSERT_TRUE(document.setParameterValue(b, 7.0));
    EXPECT_TRUE(Find(document, b)->expression().empty());
    EXPECT_NEAR(ValueOf(document, b), 7.0, kTol);

    // And the edge went with it: a recompute must not overwrite the 7.
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, b), 7.0, kTol);
    ASSERT_TRUE(document.setParameterValue(a, 100.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, b), 7.0, kTol);
}

// =============================================================================
// Undo
// =============================================================================

TEST(ExpressionWiringTest, M11_WIRE_080_UndoRestoresBothTheExpressionAndItsEdge) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 3.0, UnitType::Millimeter).id();

    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, b), 20.0, kTol);

    ASSERT_TRUE(document.undo());
    EXPECT_TRUE(Find(document, b)->expression().empty());

    ASSERT_TRUE(document.redo());
    EXPECT_EQ(Find(document, b)->expression(), std::string("#A * 2"));
    // The edge came back with it, not just the text.
    ASSERT_TRUE(document.recompute().success);
    ASSERT_TRUE(document.setParameterValue(a, 25.0));
    EXPECT_EQ(Find(document, b)->state(), ParameterState::Dirty);
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, b), 50.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_081_UndoOfATypedNumberBringsTheFormulaBack) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_TRUE(document.recompute().success);

    ASSERT_TRUE(document.setParameterValue(b, 7.0));
    ASSERT_TRUE(Find(document, b)->expression().empty());

    ASSERT_TRUE(document.undo());
    EXPECT_EQ(Find(document, b)->expression(), std::string("#A * 2"));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, b), 20.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_082_AnEditThatChangesNothingCostsNoUndoStep) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const std::size_t before = document.undoDepth();
    EXPECT_TRUE(document.setParameterExpression(a, ""));
    EXPECT_EQ(document.undoDepth(), before);
}

// =============================================================================
// Deletion
// =============================================================================

TEST(ExpressionWiringTest, M11_WIRE_100_DeletingAParameterAnExpressionReadsIsRefused) {
    // ADR-M5-009's rule, applied to the second kind of reference a named
    // parameter attracts.
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));

    EXPECT_EQ(document.parametersReferencingParameter(a), std::vector<ObjectId>{b});
    EXPECT_FALSE(document.removeObject(a));
    EXPECT_NE(Find(document, a), nullptr) << "a refusal must leave the document unchanged";
}

TEST(ExpressionWiringTest, M11_WIRE_101_ClearingTheExpressionMakesTheDeletionPossible) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_FALSE(document.removeObject(a));

    ASSERT_TRUE(document.setParameterExpression(b, ""));
    EXPECT_TRUE(document.parametersReferencingParameter(a).empty());
    EXPECT_TRUE(document.removeObject(a));
}

// =============================================================================
// Recompute failure
// =============================================================================

TEST(ExpressionWiringTest, M11_WIRE_120_AnExpressionThatFailsLaterFailsItsParameter) {
    // The trial evaluation passes at edit time and the world moves afterwards.
    // The parameter must FAIL rather than keep a stale value that looks current.
    PartDocument document{"Doc"};
    const ObjectId divisor = document.addParameter("Divisor", 2.0, UnitType::Unitless).id();
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(width, "10mm / #Divisor"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, width), 5.0, kTol);

    ASSERT_TRUE(document.setParameterValue(divisor, 0.0));
    const DocumentRecomputeReport report = document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(Find(document, width)->state(), ParameterState::Failed);

    const RecomputeItemReport* item = ItemFor(report, width);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->status, RecomputeStatus::Failed);
    EXPECT_NE(item->message.find("col "), std::string::npos)
        << "the diagnostic must carry the position: " << item->message;
}

TEST(ExpressionWiringTest, M11_WIRE_121_AFailedExpressionBlocksItsConsumers) {
    PartDocument document{"Doc"};
    const ObjectId divisor = document.addParameter("Divisor", 2.0, UnitType::Unitless).id();
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(width, "10mm / #Divisor"));

    CountingNode consumer;
    ASSERT_TRUE(document.addRecomputableNode(consumer));
    ASSERT_TRUE(document.addDependency(consumer.id(), width));
    ASSERT_TRUE(document.recompute().success);
    const int before = consumer.runs;

    ASSERT_TRUE(document.setParameterValue(divisor, 0.0));
    const DocumentRecomputeReport report = document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(consumer.runs, before) << "a consumer of a failed parameter must not run";

    const RecomputeItemReport* item = ItemFor(report, consumer.id());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->status, RecomputeStatus::BlockedByDependency);
}

// =============================================================================
// Persistence
// =============================================================================

TEST(ExpressionWiringTest, M11_WIRE_140_ExpressionsSurviveARoundTripAndStayWired) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));
    ASSERT_TRUE(document.recompute().success);

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_NE(Find(*loaded.document, b), nullptr);
    EXPECT_EQ(Find(*loaded.document, b)->expression(), std::string("#A * 2"));

    // Edges are NOT persisted, so this is the only evidence the load re-derived
    // them: a change to A must still reach B.
    ASSERT_TRUE(loaded.document->recompute().success);
    ASSERT_TRUE(loaded.document->setParameterValue(a, 30.0));
    EXPECT_EQ(Find(*loaded.document, b)->state(), ParameterState::Dirty);
    ASSERT_TRUE(loaded.document->recompute().success);
    EXPECT_NEAR(ValueOf(*loaded.document, b), 60.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_141_AForwardReferenceLoads) {
    // The parameter that is READ may be written after the one that reads it.
    // Wiring inside restoreParameter would refuse this; the post-pass does not.
    PartDocument document{"Doc"};
    const ObjectId reader = document.addParameter("Reader", 0.0, UnitType::Millimeter).id();
    document.addParameter("Later", 8.0, UnitType::Millimeter);
    ASSERT_TRUE(document.setParameterExpression(reader, "#Later * 3"));

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_TRUE(loaded.document->recompute().success);
    EXPECT_NEAR(ValueOf(*loaded.document, reader), 24.0, kTol);
}

TEST(ExpressionWiringTest, M11_WIRE_142_AFileWhoseExpressionNamesNothingIsRefused) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));

    std::string text = SaveToString(document);
    const std::size_t at = text.find("#A * 2");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, 6, "#Q * 2"); // same length: a name no parameter has

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("#Q"), std::string::npos) << loaded.message;
}

TEST(ExpressionWiringTest, M11_WIRE_143_AFileWhoseExpressionDoesNotParseIsRefused) {
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));

    std::string text = SaveToString(document);
    const std::size_t at = text.find("#A * 2");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, 6, "#A * $"); // same length, and unparseable

    EXPECT_FALSE(LoadFromString(text));
}

TEST(ExpressionWiringTest, M11_WIRE_144_AFileWithACyclicExpressionPairIsRefused) {
    // Unreachable through the facade, which refuses the second half of the
    // cycle -- so it is written by hand here, which is exactly the case the
    // loader-side check exists for.
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const ObjectId b = document.addParameter("B", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(b, "#A * 2"));

    std::string text = SaveToString(document);
    // Give A an expression reading B, by rewriting A's empty expression field.
    // A is written first, so the FIRST empty expression belongs to it.
    const std::size_t at = text.find("\"expression\": \"\"");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("\"expression\": \"\"").size(),
                 "\"expression\": \"#B / 2\"");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::InvalidDependency);
    EXPECT_NE(loaded.message.find("cycle"), std::string::npos) << loaded.message;
}

TEST(ExpressionWiringTest, M11_WIRE_145_SavingADocumentWithADanglingExpressionIsRefused) {
    // The save-side net. Built through the RESTORE path, which does not
    // validate -- that is the only route left once the facade refuses both the
    // bad expression and the deletion that would create one.
    PartDocument document{"Doc"};
    document.restoreParameter(ObjectIdGenerator::Next(), "B", 0.0, UnitType::Millimeter,
                              "#Missing * 2", ParameterState::Valid);

    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("#Missing"), std::string::npos) << saved.message;
}

TEST(ExpressionWiringTest, M11_WIRE_147_AParameterThatFailsToEvaluateStillRoundTrips) {
    // The shape that replaced SerializationTests.FailedStatesRoundTrip's old
    // fixture. "Failed" and "the file is loadable" are NOT in tension: an
    // expression that parses and resolves can still fail at EVALUATION time,
    // and such a document must save and load like any other.
    PartDocument document{"Doc"};
    const ObjectId divisor = document.addParameter("Divisor", 2.0, UnitType::Unitless).id();
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(width, "10mm / #Divisor"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_TRUE(document.setParameterValue(divisor, 0.0));
    ASSERT_FALSE(document.recompute().success);
    ASSERT_EQ(Find(document, width)->state(), ParameterState::Failed);

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(Find(*loaded.document, width)->state(), ParameterState::Failed);
    EXPECT_EQ(Find(*loaded.document, width)->expression(), std::string("10mm / #Divisor"));
    // And it fails the same way after the trip, rather than quietly recovering.
    EXPECT_FALSE(loaded.document->recompute().success);
}

TEST(ExpressionWiringTest, M11_WIRE_146_ADocumentWithNoExpressionsIsUnaffected) {
    // The regression guard for every pre-M11 document: nothing about the
    // save/load path may change when no expression is present.
    PartDocument document{"Doc"};
    const ObjectId a = document.addParameter("A", 10.0, UnitType::Millimeter).id();
    const std::string text = SaveToString(document);
    const LoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_NEAR(ValueOf(*loaded.document, a), 10.0, kTol);
    EXPECT_EQ(SaveToString(*loaded.document), text);
}
