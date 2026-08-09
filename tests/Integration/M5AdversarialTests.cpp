// M5-N: the adversarial cases from spec 30 that the solver-level suites do not
// already cover, run END TO END through a real document, a real solver and real
// OCCT geometry.
//
// The solver suites already cover: duplicate ids, missing entity/parameter
// refs, wrong sub-element, non-finite and non-positive dimensions, contradictory
// dimensions, redundancy, constraint order, angles near 0 and pi, 100x drift,
// and four orders of magnitude of scale. What is added here is what only shows
// up once a whole document is involved: entity CREATION order, degenerate
// geometry reaching the solver, a poisoned Parameter, and repeated whole-
// document recomputes.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kLengthAbsTol = 1e-6;

struct Harness {
    PartDocument document{"Adversarial"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;

    Harness() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);
    }
};

// Builds the reference rectangle with its four lines created in a caller-chosen
// order. `order` lists which side (0=bottom, 1=right, 2=top, 3=left) is created
// at each step; the CONSTRAINTS are always the same, expressed against the same
// four sides. Solved geometry must therefore be identical whatever the order.
struct OrderedRectangle {
    Harness harness;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Sketch* sketch = nullptr;
    SketchEntityId sides[4]{}; // indexed by SIDE, not by creation position

    explicit OrderedRectangle(const std::vector<int>& order) {
        width = &harness.document.addParameter("Width", 100.0, UnitType::Millimeter);
        height = &harness.document.addParameter("Height", 50.0, UnitType::Millimeter);
        Sketch& s = harness.document.addSketch("Sketch001");
        sketch = &s;

        // Deliberately off-size and skewed, same start for every ordering.
        const Vec2 starts[4] = {{0, 0}, {112, 3}, {115, 58}, {2, 61}};
        const Vec2 ends[4] = {{112, 3}, {115, 58}, {2, 61}, {0, 0}};
        for (int position : order) sides[position] = s.addLine(starts[position], ends[position]);

        const auto sp = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::StartPoint};
        };
        const auto ep = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::EndPoint};
        };
        const auto add = [&](SketchConstraintData data) {
            harness.document.addSketchConstraint(s.id(), std::move(data));
        };
        add(CoincidentConstraint{ep(sides[0]), sp(sides[1])});
        add(CoincidentConstraint{ep(sides[1]), sp(sides[2])});
        add(CoincidentConstraint{ep(sides[2]), sp(sides[3])});
        add(CoincidentConstraint{ep(sides[3]), sp(sides[0])});
        add(HorizontalConstraint{sides[0]});
        add(HorizontalConstraint{sides[2]});
        add(VerticalConstraint{sides[1]});
        add(VerticalConstraint{sides[3]});
        add(FixConstraint{sp(sides[0])});
        add(LengthConstraint{sides[0], width->id()});
        add(LengthConstraint{sides[1], height->id()});
    }

    // The solved geometry keyed by SIDE, so two orderings are comparable even
    // though their entity ids differ.
    std::vector<SketchLine> solvedSides() const {
        const Sketch& solved = *harness.document.findSketch(sketch->id());
        std::vector<SketchLine> lines;
        for (SketchEntityId id : sides)
            lines.push_back(std::get<SketchLine>(solved.findEntity(id)->geometry));
        return lines;
    }
};

// --- Entity creation order (spec 19: order independence) ---------------------

TEST(M5Adversarial, M5_ADV_001_EntityCreationOrderDoesNotChangeTheSolvedGeometry) {
    OrderedRectangle baseline({0, 1, 2, 3});
    ASSERT_TRUE(baseline.harness.document.recompute().success);
    const std::vector<SketchLine> expected = baseline.solvedSides();

    // Every permutation of the four sides, not a couple of hand-picked ones:
    // an order dependence that only bites on one arrangement is exactly what a
    // sampled test misses.
    std::vector<int> order{0, 1, 2, 3};
    int permutations = 0;
    while (std::next_permutation(order.begin(), order.end())) {
        OrderedRectangle variant(order);
        ASSERT_TRUE(variant.harness.document.recompute().success)
            << "order " << order[0] << order[1] << order[2] << order[3];
        const std::vector<SketchLine> actual = variant.solvedSides();
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            EXPECT_NEAR(actual[i].start.x, expected[i].start.x, kLengthAbsTol) << i;
            EXPECT_NEAR(actual[i].start.y, expected[i].start.y, kLengthAbsTol) << i;
            EXPECT_NEAR(actual[i].end.x, expected[i].end.x, kLengthAbsTol) << i;
            EXPECT_NEAR(actual[i].end.y, expected[i].end.y, kLengthAbsTol) << i;
        }
        ++permutations;
    }
    EXPECT_EQ(permutations, 23); // 4! - 1
}

// --- Degenerate geometry reaching the solver ---------------------------------

TEST(M5Adversarial, M5_ADV_002_AngleOnAZeroLengthLineIsInvalidNotSolved) {
    Harness h;
    Parameter& angle = h.document.addParameter("Angle", 0.5, UnitType::Radian);
    Sketch& s = h.document.addSketch("Sketch001");
    // restoreEntity, not addLine: addLine rejects degenerate geometry, so this
    // is the path a hand-edited or corrupted file would take. The solver must
    // reject it too rather than trusting that nothing upstream let it through.
    const SketchEntityId degenerate = NextSketchEntityId();
    ASSERT_TRUE(s.restoreEntity(degenerate, SketchLine{Vec2{5, 5}, Vec2{5, 5}}));
    const SketchEntityId normal = s.addLine(Vec2{0, 0}, Vec2{10, 0});
    ASSERT_NE(h.document.addSketchConstraint(s.id(),
                                             AngleConstraint{normal, degenerate, angle.id()}),
              kInvalidSketchConstraintId);

    EXPECT_FALSE(h.document.recompute().success);
    const Sketch& solved = *h.document.findSketch(s.id());
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::InvalidInput);
    EXPECT_FALSE(solved.offendingConstraints().empty());
    // And the degenerate line was not "fixed" behind the user's back.
    const SketchLine& line = std::get<SketchLine>(solved.findEntity(degenerate)->geometry);
    EXPECT_DOUBLE_EQ(line.start.x, line.end.x);
    EXPECT_DOUBLE_EQ(line.start.y, line.end.y);
}

TEST(M5Adversarial, M5_ADV_003_StartPointOfACircleIsInvalidNotSilentlyTheCentre) {
    Harness h;
    Sketch& s = h.document.addSketch("Sketch001");
    const SketchEntityId circle = s.addCircle(Vec2{0, 0}, 10.0);
    // A circle has no start point. Quietly treating this as the centre would
    // solve to a plausible-looking wrong answer, which is worse than failing.
    ASSERT_NE(h.document.addSketchConstraint(
                  s.id(), FixConstraint{SketchElementRef{circle, SketchSubElement::StartPoint}}),
              kInvalidSketchConstraintId);

    EXPECT_FALSE(h.document.recompute().success);
    EXPECT_EQ(h.document.findSketch(s.id())->solveStatus(), SketchSolveStatus::InvalidInput);
}

// The FULL entity-kind x sub-element matrix, both directions.
//
// The pre-existing test asked only "CenterPoint on a line", which happened to be
// the guarded branch; "StartPoint on a circle" was never asked and silently
// resolved to the circle's centre. Enumerating every pair means a future entity
// kind or sub-element cannot be half-guarded without this failing.
struct SubElementCase {
    const char* entityKind;
    SketchSubElement subElement;
    bool valid;
};

class SubElementMatrixTest : public ::testing::TestWithParam<SubElementCase> {};

TEST_P(SubElementMatrixTest, M5_ADV_008_OnlyMeaningfulSubElementsResolve) {
    const SubElementCase& c = GetParam();
    Harness h;
    Sketch& s = h.document.addSketch("Sketch001");

    const std::string kind = c.entityKind;
    SketchEntityId entity{};
    if (kind == "Line") {
        entity = s.addLine(Vec2{0, 0}, Vec2{10, 0});
    } else if (kind == "Circle") {
        entity = s.addCircle(Vec2{0, 0}, 10.0);
    } else if (kind == "Arc") {
        entity = s.addArc(Vec2{0, 0}, 10.0, 0.0, 1.0);
    } else {
        entity = s.addPoint(Vec2{3, 4});
    }
    ASSERT_NE(entity, kInvalidSketchEntityId);
    ASSERT_NE(h.document.addSketchConstraint(
                  s.id(), FixConstraint{SketchElementRef{entity, c.subElement}}),
              kInvalidSketchConstraintId);

    const bool solved = h.document.recompute().success;
    EXPECT_EQ(solved, c.valid)
        << kind << " + sub-element " << static_cast<int>(c.subElement);
    if (!c.valid)
        EXPECT_EQ(h.document.findSketch(s.id())->solveStatus(), SketchSolveStatus::InvalidInput);
}

INSTANTIATE_TEST_SUITE_P(
    AllPairs, SubElementMatrixTest,
    ::testing::Values(
        // A line has two endpoints and nothing else.
        SubElementCase{"Line", SketchSubElement::StartPoint, true},
        SubElementCase{"Line", SketchSubElement::EndPoint, true},
        SubElementCase{"Line", SketchSubElement::CenterPoint, false},
        SubElementCase{"Line", SketchSubElement::Whole, false},
        // A curve's only referenceable point is its centre.
        SubElementCase{"Circle", SketchSubElement::CenterPoint, true},
        SubElementCase{"Circle", SketchSubElement::StartPoint, false},
        SubElementCase{"Circle", SketchSubElement::EndPoint, false},
        SubElementCase{"Circle", SketchSubElement::Whole, false},
        SubElementCase{"Arc", SketchSubElement::CenterPoint, true},
        SubElementCase{"Arc", SketchSubElement::StartPoint, false},
        SubElementCase{"Arc", SketchSubElement::EndPoint, false},
        SubElementCase{"Arc", SketchSubElement::Whole, false},
        // A point IS its position.
        SubElementCase{"Point", SketchSubElement::Whole, true},
        SubElementCase{"Point", SketchSubElement::StartPoint, false},
        SubElementCase{"Point", SketchSubElement::EndPoint, false},
        SubElementCase{"Point", SketchSubElement::CenterPoint, false}));

// --- A poisoned Parameter ----------------------------------------------------

class PoisonedParameterTest : public ::testing::TestWithParam<double> {};

TEST_P(PoisonedParameterTest, M5_ADV_004_NonFiniteOrNonPositiveDimensionIsRejected) {
    Harness h;
    Parameter& radius = h.document.addParameter("Radius", 10.0, UnitType::Millimeter);
    Parameter& padLength = h.document.addParameter("PadLength", 30.0, UnitType::Millimeter);
    Sketch& s = h.document.addSketch("Sketch001");
    const SketchEntityId circle = s.addCircle(Vec2{0, 0}, 7.0);
    h.document.addSketchConstraint(
        s.id(), FixConstraint{SketchElementRef{circle, SketchSubElement::CenterPoint}});
    h.document.addSketchConstraint(s.id(), RadiusConstraint{circle, radius.id()});
    Body& body = h.document.addBody("Body001");
    h.document.addPadFeature(body, "Pad001", s.id(), padLength.id());

    ASSERT_TRUE(h.document.recompute().success);
    const double goodVolume = h.document.massProperties().volumeMm3;
    const double goodRadius =
        std::get<SketchCircle>(h.document.findSketch(s.id())->findEntity(circle)->geometry)
            .radiusMm;
    ASSERT_NEAR(goodRadius, 10.0, kLengthAbsTol);

    ASSERT_TRUE(h.document.setParameterValue(radius.id(), GetParam()));
    EXPECT_FALSE(h.document.recompute().success);

    const Sketch& solved = *h.document.findSketch(s.id());
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::InvalidInput);
    // Nothing non-finite was committed, and the last good geometry survived.
    const SketchCircle& c = std::get<SketchCircle>(solved.findEntity(circle)->geometry);
    EXPECT_TRUE(std::isfinite(c.radiusMm));
    EXPECT_TRUE(std::isfinite(c.center.x) && std::isfinite(c.center.y));
    EXPECT_NEAR(c.radiusMm, goodRadius, kLengthAbsTol);
    EXPECT_TRUE(std::isfinite(h.document.massProperties().volumeMm3));
    EXPECT_NEAR(h.document.massProperties().volumeMm3, goodVolume, 1e-6 * goodVolume);

    // Recovery: a sane value brings the whole model back.
    ASSERT_TRUE(h.document.setParameterValue(radius.id(), 20.0));
    ASSERT_TRUE(h.document.recompute().success);
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_NEAR(
        std::get<SketchCircle>(solved.findEntity(circle)->geometry).radiusMm, 20.0,
        kLengthAbsTol);
}

INSTANTIATE_TEST_SUITE_P(PoisonValues, PoisonedParameterTest,
                         ::testing::Values(std::numeric_limits<double>::quiet_NaN(),
                                           std::numeric_limits<double>::infinity(),
                                           -std::numeric_limits<double>::infinity(), -5.0, 0.0));

// --- Scale, end to end -------------------------------------------------------

class ScaleTest : public ::testing::TestWithParam<double> {};

TEST_P(ScaleTest, M5_ADV_005_SmallAndLargeGeometrySolvesAndMeasuresCorrectly) {
    const double scale = GetParam();
    OrderedRectangle fx({0, 1, 2, 3});
    ASSERT_TRUE(fx.harness.document.setParameterValue(fx.width->id(), 100.0 * scale));
    ASSERT_TRUE(fx.harness.document.setParameterValue(fx.height->id(), 50.0 * scale));

    Parameter& padLength =
        fx.harness.document.addParameter("PadLength", 20.0 * scale, UnitType::Millimeter);
    Body& body = fx.harness.document.addBody("Body001");
    fx.harness.document.addPadFeature(body, "Pad001", fx.sketch->id(), padLength.id());

    ASSERT_TRUE(fx.harness.document.recompute().success);
    const Sketch& solved = *fx.harness.document.findSketch(fx.sketch->id());
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(solved.degreesOfFreedom(), 0);

    // Analytical: 100 x 50 x 20 mm^3, scaled in all three dimensions.
    const double expected = 100000.0 * scale * scale * scale;
    EXPECT_NEAR(fx.harness.document.massProperties().volumeMm3, expected, 1e-6 * expected);
}

// 0.01 -> a 1 x 0.5 mm rectangle; 100 -> 10 x 5 m. Both are within what a CAD
// user reasonably models, and both are far enough from 1 to expose a solver
// tolerance that is absolute where it should be relative.
INSTANTIATE_TEST_SUITE_P(Scales, ScaleTest, ::testing::Values(0.01, 0.1, 1.0, 10.0, 100.0));

// --- Repeated whole-document recompute ---------------------------------------

TEST(M5Adversarial, M5_ADV_006_HundredRecomputesProduceNoDrift) {
    OrderedRectangle fx({0, 1, 2, 3});
    Parameter& padLength =
        fx.harness.document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Body& body = fx.harness.document.addBody("Body001");
    fx.harness.document.addPadFeature(body, "Pad001", fx.sketch->id(), padLength.id());

    ASSERT_TRUE(fx.harness.document.recompute().success);
    const std::vector<SketchLine> first = fx.solvedSides();
    const double firstVolume = fx.harness.document.massProperties().volumeMm3;

    // Each pass re-solves from the PREVIOUS pass's output, which is where drift
    // would accumulate. Dirtying via the Width parameter (to the same value) is
    // what forces a genuine re-solve rather than a no-op.
    for (int pass = 0; pass < 100; ++pass) {
        ASSERT_TRUE(fx.harness.document.setParameterValue(fx.width->id(), 100.0));
        ASSERT_TRUE(fx.harness.document.recompute().success) << "pass " << pass;
    }

    const std::vector<SketchLine> last = fx.solvedSides();
    ASSERT_EQ(last.size(), first.size());
    for (std::size_t i = 0; i < last.size(); ++i) {
        EXPECT_NEAR(last[i].start.x, first[i].start.x, 1e-9) << i;
        EXPECT_NEAR(last[i].start.y, first[i].start.y, 1e-9) << i;
        EXPECT_NEAR(last[i].end.x, first[i].end.x, 1e-9) << i;
        EXPECT_NEAR(last[i].end.y, first[i].end.y, 1e-9) << i;
    }
    EXPECT_NEAR(fx.harness.document.massProperties().volumeMm3, firstVolume, 1e-6);
}

// --- Save / load / re-solve under adversarial editing ------------------------

TEST(M5Adversarial, M5_ADV_007_RoundTripAfterDeletionAndReAddition) {
    OrderedRectangle fx({0, 1, 2, 3});
    Parameter& padLength =
        fx.harness.document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Body& body = fx.harness.document.addBody("Body001");
    fx.harness.document.addPadFeature(body, "Pad001", fx.sketch->id(), padLength.id());
    ASSERT_TRUE(fx.harness.document.recompute().success);

    // Churn the constraint set: remove a dimension, re-add an equivalent one.
    const Sketch& solved = *fx.harness.document.findSketch(fx.sketch->id());
    SketchConstraintId widthLength = kInvalidSketchConstraintId;
    for (const SketchConstraint& c : solved.constraints())
        if (const auto* length = std::get_if<LengthConstraint>(&c.data))
            if (length->line == fx.sides[0]) widthLength = c.id;
    ASSERT_NE(widthLength, kInvalidSketchConstraintId);
    ASSERT_TRUE(fx.harness.document.removeSketchConstraint(fx.sketch->id(), widthLength));
    ASSERT_NE(fx.harness.document.addSketchConstraint(
                  fx.sketch->id(), LengthConstraint{fx.sides[0], fx.width->id()}),
              kInvalidSketchConstraintId);
    ASSERT_TRUE(fx.harness.document.recompute().success);

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.harness.document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);
    ASSERT_TRUE(loaded.document->recompute().success);

    const Sketch& after = *loaded.document->sketches().front();
    EXPECT_EQ(after.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(after.degreesOfFreedom(), 0);
    // The removed constraint stayed removed; nothing resurrected it.
    EXPECT_EQ(after.findConstraint(widthLength), nullptr);
    EXPECT_NEAR(loaded.document->massProperties().volumeMm3, 100000.0, 1e-6);
}

} // namespace
