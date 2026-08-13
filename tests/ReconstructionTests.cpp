// M7.1 reconstruction analysis, at the Core level: no DXF, no solver, no
// kernel. Everything here is about what the ANALYSIS decides and what APPLY
// creates, which is the layer where a wrong decision becomes a wrong document.
//
// Inputs are hand-built ImportedDimension2D values, so each test states the
// exact source situation it is about. Every expected number is computed by hand
// from the input; none is read back from the code under test.

#include "Core/Document/PartDocument.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Sketch/Sketch.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

// A 100 x 50 rectangle whose corners are EXACT, so association is not the thing
// under test unless a test makes it so.
struct RectangleFixture {
    PartDocument document{"M7Rect"};
    Sketch* sketch = nullptr;
    SketchEntityId bottom{}, right{}, top{}, left{};

    explicit RectangleFixture(double width = 100.0, double height = 50.0) {
        Sketch& s = document.addSketch("Imported");
        sketch = &s;
        bottom = s.addLine(Vec2{0, 0}, Vec2{width, 0});
        right = s.addLine(Vec2{width, 0}, Vec2{width, height});
        top = s.addLine(Vec2{width, height}, Vec2{0, height});
        left = s.addLine(Vec2{0, height}, Vec2{0, 0});
    }
};

ImportedDimension2D Linear(Vec2 from, Vec2 to, double directionRad = 0.0) {
    ImportedDimension2D dimension;
    dimension.kind = ImportedDimensionKind::Linear;
    dimension.measureFrom = from;
    dimension.measureTo = to;
    dimension.directionRad = directionRad;
    return dimension;
}

constexpr double kPi = 3.14159265358979323846;

const PlannedParameter* FindParameter(const ReconstructionPlan& plan, const std::string& name) {
    for (const PlannedParameter& parameter : plan.parameters)
        if (parameter.name == name) return &parameter;
    return nullptr;
}

std::size_t CountKind(const ReconstructionPlan& plan, const char* kindName) {
    std::size_t count = 0;
    for (const PlannedConstraint& constraint : plan.constraints)
        if (std::string(ConstraintKindName(constraint.data)) == kindName) ++count;
    return count;
}

bool HasSkip(const ReconstructionPlan& plan, ReconstructionSkipReason reason) {
    return std::any_of(plan.skipped.begin(), plan.skipped.end(),
                       [reason](const ReconstructionSkip& skip) { return skip.reason == reason; });
}

// --- The rectangle shape (ADR-M7-010) ---------------------------------------

TEST(M7Reconstruction, RectanglePlanHasTheExpectedDesignIntent) {
    RectangleFixture fx;
    const std::vector<ImportedDimension2D> dimensions{
        Linear(Vec2{0, 0}, Vec2{100, 0}),           // along the bottom
        Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2) // up the right side
    };

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), dimensions);

    // Spec 13's minimum design intent, counted by kind.
    EXPECT_EQ(CountKind(plan, "Coincident"), 4u);
    EXPECT_EQ(CountKind(plan, "Horizontal"), 2u);
    EXPECT_EQ(CountKind(plan, "Vertical"), 2u);
    EXPECT_EQ(CountKind(plan, "Fix"), 1u);
    EXPECT_EQ(CountKind(plan, "Length"), 2u);
    EXPECT_TRUE(plan.skipped.empty());

    // 16 variables (4 lines x 4 scalars) - 16 independent residuals
    // (4 Coincident x 2, 2 Horizontal, 2 Vertical, 1 Fix x 2, 2 Length) = 0.
    // Counted by hand here; the solver's own DOF is checked in the gate tests.
    ASSERT_NE(FindParameter(plan, "Width"), nullptr);
    ASSERT_NE(FindParameter(plan, "Height"), nullptr);
    EXPECT_DOUBLE_EQ(FindParameter(plan, "Width")->value, 100.0);
    EXPECT_DOUBLE_EQ(FindParameter(plan, "Height")->value, 50.0);
    EXPECT_EQ(FindParameter(plan, "Width")->unit, UnitType::Millimeter);
}

TEST(M7Reconstruction, WidthBindsTheHorizontalLineAndHeightTheVertical) {
    RectangleFixture fx;
    const std::vector<ImportedDimension2D> dimensions{
        Linear(Vec2{0, 0}, Vec2{100, 0}),
        Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2)};

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), dimensions);

    // The specific mis-wiring spec 32 names: swapping Width and Height source
    // mapping. Checked by entity, not by value, because 100 and 50 would still
    // look plausible on the wrong sides.
    for (const PlannedConstraint& constraint : plan.constraints) {
        const auto* length = std::get_if<LengthConstraint>(&constraint.data);
        if (length == nullptr) continue;
        const PlannedParameter* bound = plan.find(constraint.parameter);
        ASSERT_NE(bound, nullptr);
        if (bound->name == "Width") EXPECT_EQ(length->line, fx.bottom);
        if (bound->name == "Height") EXPECT_EQ(length->line, fx.right);
    }
}

TEST(M7Reconstruction, NamingDoesNotDependOnTheOrderDimensionsArrive) {
    RectangleFixture fx;
    const ImportedDimension2D widthDim = Linear(Vec2{0, 0}, Vec2{100, 0});
    const ImportedDimension2D heightDim = Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2);

    const ReconstructionPlan forward =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {widthDim, heightDim});
    const ReconstructionPlan reversed =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {heightDim, widthDim});

    // Same drawing, dimensions listed the other way round: the same names must
    // land on the same geometry. File order is not identity (ADR-M6-004), and
    // naming is the easiest place to accidentally make it so.
    ASSERT_NE(FindParameter(forward, "Width"), nullptr);
    ASSERT_NE(FindParameter(reversed, "Width"), nullptr);
    EXPECT_DOUBLE_EQ(FindParameter(forward, "Width")->value, 100.0);
    EXPECT_DOUBLE_EQ(FindParameter(reversed, "Width")->value, 100.0);
    EXPECT_DOUBLE_EQ(FindParameter(reversed, "Height")->value, 50.0);
}

// --- Ambiguity and refusal (spec 18) ----------------------------------------

TEST(M7Reconstruction, ADimensionMatchingTwoLinesIsSkippedNotGuessed) {
    RectangleFixture fx;
    // A duplicate of the bottom line, exactly on top of it. Nothing in the
    // source says which of the two the dimension meant.
    fx.sketch->addLine(Vec2{0, 0}, Vec2{100, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0})});

    EXPECT_EQ(CountKind(plan, "Length"), 0u);
    EXPECT_TRUE(plan.parameters.empty());
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::AmbiguousTarget));
    // And the diagnostic has to be worth reading.
    EXPECT_FALSE(plan.skipped.front().detail.empty());
}

TEST(M7Reconstruction, ADimensionMatchingNoLineIsSkipped) {
    RectangleFixture fx;
    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{5, 5}, Vec2{40, 5})});

    EXPECT_EQ(CountKind(plan, "Length"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::NoTargetGeometry));
}

TEST(M7Reconstruction, ATextOverrideIsNotTrusted) {
    RectangleFixture fx;
    ImportedDimension2D dimension = Linear(Vec2{0, 0}, Vec2{100, 0});
    dimension.textOverride = "APPROX 100";

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {dimension});

    EXPECT_EQ(CountKind(plan, "Length"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::TextOverride));
}

TEST(M7Reconstruction, TheFormatsOwnPlaceholderIsNotATextOverride) {
    RectangleFixture fx;
    ImportedDimension2D dimension = Linear(Vec2{0, 0}, Vec2{100, 0});
    dimension.textOverride = "<>"; // DXF for "show the measurement"

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {dimension});

    // The opposite mistake from the test above, and the one that would silently
    // stop reconstructing most real files: "<>" is in nearly every dimension a
    // CAD package writes.
    EXPECT_EQ(CountKind(plan, "Length"), 1u);
    EXPECT_TRUE(plan.skipped.empty());
}

TEST(M7Reconstruction, AStatedValueThatDisagreesMateriallyIsSkipped) {
    RectangleFixture fx;
    ImportedDimension2D dimension = Linear(Vec2{0, 0}, Vec2{100, 0});
    dimension.statedValueMm = 250.0; // the drawing disagrees with itself

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {dimension});

    EXPECT_EQ(CountKind(plan, "Length"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::ValueDisagreesWithGeometry));
}

TEST(M7Reconstruction, ASmallStatedDisagreementIsBelievedOverTheGeometry) {
    // Fixture B's whole point (spec 22): the drawing is slightly off and the
    // dimension is what the designer MEANT. 99.5 vs 100 is 0.5%, inside the
    // agreement band, so the Parameter takes the stated value and the solver is
    // what moves the geometry.
    RectangleFixture fx(99.5, 50.0);
    ImportedDimension2D dimension = Linear(Vec2{0, 0}, Vec2{99.5, 0});
    dimension.statedValueMm = 100.0;

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {dimension});

    ASSERT_NE(FindParameter(plan, "Width"), nullptr);
    EXPECT_DOUBLE_EQ(FindParameter(plan, "Width")->value, 100.0);
}

TEST(M7Reconstruction, UnsupportedDimensionKindsAreReportedNotReinterpreted) {
    RectangleFixture fx;
    ImportedDimension2D radius = Linear(Vec2{0, 0}, Vec2{100, 0});
    radius.kind = ImportedDimensionKind::Radius;

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {radius});

    // M7.1 does not do Radius (spec 37). It must say so, not quietly turn it
    // into the Length its definition points would suggest.
    EXPECT_EQ(CountKind(plan, "Length"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::UnsupportedKind));
}

TEST(M7Reconstruction, AValueBelowTheReconstructionFloorIsRefused) {
    RectangleFixture fx(1e-5, 50.0);
    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{1e-5, 0})});

    EXPECT_EQ(CountKind(plan, "Length"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::InvalidValue));
}

// --- Naming collisions (spec 9, Fixture G) ----------------------------------

TEST(M7Reconstruction, ANameAlreadyInTheDocumentIsResolvedDeterministically) {
    RectangleFixture fx;
    fx.document.addParameter("Width", 7.0, UnitType::Millimeter);

    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0})});

    ASSERT_EQ(plan.parameters.size(), 1u);
    EXPECT_EQ(plan.parameters.front().name, "Width_2");
    // And the existing Parameter is untouched -- a collision must not become an
    // overwrite of someone else's value.
    ASSERT_NE(fx.document.parameters().findByName("Width"), nullptr);
    EXPECT_DOUBLE_EQ(fx.document.parameters().findByName("Width")->value(), 7.0);
}

// --- Plan validation (spec 15) ----------------------------------------------

TEST(M7Reconstruction, APlanWithAnUnboundDimensionalConstraintIsRejected) {
    RectangleFixture fx;
    ReconstructionPlan plan;
    plan.sketchId = fx.sketch->id();
    plan.constraints.push_back(PlannedConstraint{LengthConstraint{fx.bottom, kInvalidObjectId},
                                                 kInvalidPlanParameterSlot,
                                                 ReconstructionOrigin::ExplicitSource, {}});

    // "Reports success and controls nothing" is a Critical in spec 34. It has
    // to be impossible to apply, not merely unlikely to occur.
    EXPECT_FALSE(ValidatePlan(plan));
    EXPECT_FALSE(ApplyReconstruction(fx.document, plan));
    EXPECT_TRUE(fx.document.parameters().items().empty());
}

TEST(M7Reconstruction, APlanBindingALengthToANonLengthParameterIsRejected) {
    RectangleFixture fx;
    ReconstructionPlan plan;
    plan.sketchId = fx.sketch->id();
    plan.parameters.push_back(PlannedParameter{"Angle", 1.0, UnitType::Radian,
                                               ReconstructionOrigin::ExplicitSource, {}});
    plan.constraints.push_back(PlannedConstraint{LengthConstraint{fx.bottom, kInvalidObjectId},
                                                 static_cast<PlanParameterSlot>(0),
                                                 ReconstructionOrigin::ExplicitSource, {}});

    EXPECT_FALSE(ValidatePlan(plan));
}

TEST(M7Reconstruction, TwoPlannedParametersMayNotShareAName) {
    RectangleFixture fx;
    ReconstructionPlan plan;
    plan.sketchId = fx.sketch->id();
    plan.parameters.push_back(PlannedParameter{"Width", 100.0, UnitType::Millimeter,
                                               ReconstructionOrigin::ExplicitSource, {}});
    plan.parameters.push_back(PlannedParameter{"Width", 50.0, UnitType::Millimeter,
                                               ReconstructionOrigin::ExplicitSource, {}});

    EXPECT_FALSE(ValidatePlan(plan));
}

// --- Transaction and rollback (spec 16) -------------------------------------

TEST(M7Reconstruction, AFailedApplyLeavesNoParameterBehind) {
    RectangleFixture fx;
    const std::size_t parametersBefore = fx.document.parameters().items().size();
    const std::size_t constraintsBefore = fx.sketch->constraints().size();

    // A plan whose LAST constraint cannot be created: it references an entity
    // this sketch does not own. Everything before it is perfectly valid, so a
    // non-transactional apply would leave two Parameters and several
    // constraints behind.
    ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(),
        {Linear(Vec2{0, 0}, Vec2{100, 0}), Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2)});
    ASSERT_FALSE(plan.parameters.empty());
    plan.constraints.push_back(PlannedConstraint{
        HorizontalConstraint{static_cast<SketchEntityId>(999999)}, kInvalidPlanParameterSlot,
        ReconstructionOrigin::InferredGeometric, {}});

    const ReconstructionResult result = ApplyReconstruction(fx.document, plan);

    EXPECT_FALSE(result);
    EXPECT_EQ(fx.document.parameters().items().size(), parametersBefore);
    EXPECT_EQ(fx.document.findSketch(fx.sketch->id())->constraints().size(), constraintsBefore);
    // Nothing half-created is reachable by name either.
    EXPECT_EQ(fx.document.parameters().findByName("Width"), nullptr);
}

TEST(M7Reconstruction, RollbackIsFollowedByASuccessfulReconstruction) {
    RectangleFixture fx;
    ReconstructionPlan doomed = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0})});
    doomed.constraints.push_back(PlannedConstraint{
        HorizontalConstraint{static_cast<SketchEntityId>(999999)}, kInvalidPlanParameterSlot,
        ReconstructionOrigin::InferredGeometric, {}});
    ASSERT_FALSE(ApplyReconstruction(fx.document, doomed));

    // The document must be usable afterwards, not merely unchanged. A rollback
    // that leaves the name "Width" taken would make this fail with Width_2.
    const ReconstructionResult second = ReconstructSketch(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0})});
    ASSERT_TRUE(second);
    ASSERT_EQ(second.createdParameters.size(), 1u);
    EXPECT_EQ(fx.document.parameters().findById(second.createdParameters.front())->name(), "Width");
}

// --- Idempotence (spec 25) --------------------------------------------------

TEST(M7Reconstruction, ReconstructingTwiceDoesNotDuplicateParameters) {
    RectangleFixture fx;
    const std::vector<ImportedDimension2D> dimensions{
        Linear(Vec2{0, 0}, Vec2{100, 0}), Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2)};

    ASSERT_TRUE(ReconstructSketch(fx.document, fx.sketch->id(), dimensions));
    const std::size_t afterFirst = fx.document.parameters().items().size();
    const std::size_t constraintsAfterFirst =
        fx.document.findSketch(fx.sketch->id())->constraints().size();

    const ReconstructionResult second =
        ReconstructSketch(fx.document, fx.sketch->id(), dimensions);

    // Spec 25's forbidden outcome by name: Width, Width_2, Width_3.
    EXPECT_FALSE(second);
    EXPECT_EQ(fx.document.parameters().items().size(), afterFirst);
    EXPECT_EQ(fx.document.findSketch(fx.sketch->id())->constraints().size(),
              constraintsAfterFirst);
    EXPECT_EQ(fx.document.parameters().findByName("Width_2"), nullptr);
    EXPECT_FALSE(second.message.empty());
}

// --- Applying creates NATIVE objects (spec 7) -------------------------------

TEST(M7Reconstruction, AppliedConstraintsBindRealParameterIdsAndWireTheGraph) {
    RectangleFixture fx;
    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(),
        {Linear(Vec2{0, 0}, Vec2{100, 0}), Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2)});
    ASSERT_TRUE(result);

    const Sketch& sketch = *fx.document.findSketch(fx.sketch->id());
    std::size_t dimensional = 0;
    for (const SketchConstraint& constraint : sketch.constraints()) {
        if (!IsDimensional(constraint.data)) continue;
        ++dimensional;
        const ObjectId bound = BoundParameterId(constraint.data);
        EXPECT_NE(bound, kInvalidObjectId);
        // The binding resolves to a Parameter that is actually IN the document,
        // not merely to a non-zero number.
        EXPECT_NE(fx.document.parameters().findById(bound), nullptr);
    }
    EXPECT_EQ(dimensional, 2u);

    // And the document knows the sketch reads those Parameters, which is what
    // makes a Width edit propagate at all.
    for (ObjectId parameterId : result.createdParameters)
        EXPECT_FALSE(fx.document.constraintsBindingParameter(parameterId).empty());
}

// --- Recogniser toggles, for Gate F's mutation controls (spec 23) -----------

TEST(M7Reconstruction, DisablingEachRecognizerRemovesExactlyItsConstraints) {
    RectangleFixture fx;
    const std::vector<ImportedDimension2D> dimensions{Linear(Vec2{0, 0}, Vec2{100, 0})};

    ReconstructionOptions noHorizontal;
    noHorizontal.recognizeHorizontal = false;
    EXPECT_EQ(CountKind(AnalyzeForReconstruction(fx.document, fx.sketch->id(), dimensions,
                                                 noHorizontal),
                        "Horizontal"),
              0u);

    ReconstructionOptions noVertical;
    noVertical.recognizeVertical = false;
    EXPECT_EQ(
        CountKind(AnalyzeForReconstruction(fx.document, fx.sketch->id(), dimensions, noVertical),
                  "Vertical"),
        0u);

    ReconstructionOptions noCoincident;
    noCoincident.recognizeCoincident = false;
    EXPECT_EQ(CountKind(AnalyzeForReconstruction(fx.document, fx.sketch->id(), dimensions,
                                                 noCoincident),
                        "Coincident"),
              0u);

    ReconstructionOptions noFix;
    noFix.placeFix = false;
    EXPECT_EQ(CountKind(AnalyzeForReconstruction(fx.document, fx.sketch->id(), dimensions, noFix),
                        "Fix"),
              0u);

    // Each toggle must remove ONLY its own kind. A flag that disabled two
    // recognisers would make Gate F's mutation controls point at the wrong one.
    EXPECT_EQ(CountKind(AnalyzeForReconstruction(fx.document, fx.sketch->id(), dimensions,
                                                 noHorizontal),
                        "Vertical"),
              2u);
}

// --- Shapes that are NOT rectangles -----------------------------------------

TEST(M7Reconstruction, ASlopedQuadrilateralGetsItsCornersButNoAxisConstraints) {
    PartDocument document{"M7Sloped"};
    Sketch& sketch = document.addSketch("Imported");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 10});
    sketch.addLine(Vec2{100, 10}, Vec2{110, 60});
    sketch.addLine(Vec2{110, 60}, Vec2{10, 50});
    sketch.addLine(Vec2{10, 50}, Vec2{0, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    // The corners are real and are reconstructed. The SIDES are visibly not
    // axis-aligned, and forcing them onto the axes would be the "confidently
    // wrong" spec 18 forbids -- so the two rules are asserted separately here,
    // because they are separate rules with separate tolerances.
    EXPECT_EQ(CountKind(plan, "Coincident"), 4u);
    EXPECT_EQ(CountKind(plan, "Horizontal"), 0u);
    EXPECT_EQ(CountKind(plan, "Vertical"), 0u);
}

TEST(M7Reconstruction, AnOpenChainIsNotSilentlyClosed) {
    PartDocument document{"M7Open"};
    Sketch& sketch = document.addSketch("Imported");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 10}); // stops short of closing

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    // THREE joints, not four. The fourth corner is 10 mm short -- four orders
    // of magnitude outside the coincidence tolerance -- and inventing it would
    // be reconstructing geometry the drawing does not contain.
    EXPECT_EQ(CountKind(plan, "Coincident"), 3u);
}

// --- Tolerance boundaries (spec 19, Fixture F) ------------------------------

TEST(M7Reconstruction, ACornerJustInsideTheCoincidenceToleranceStillCloses) {
    PartDocument document{"M7NearlyClosed"};
    Sketch& sketch = document.addSketch("Imported");
    const double gap = 0.9 * kReconstructionCoincidenceToleranceMm;
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{gap, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});
    EXPECT_EQ(CountKind(plan, "Coincident"), 4u);
}

TEST(M7Reconstruction, ACornerJustOutsideTheCoincidenceToleranceDoesNot) {
    PartDocument document{"M7NearlyClosedButNot"};
    Sketch& sketch = document.addSketch("Imported");
    const double gap = 1.1 * kReconstructionCoincidenceToleranceMm;
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{gap, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});
    // THREE, not four: the three corners inside the tolerance still join and
    // only the one just outside does not.
    //
    // The pair either side of the threshold is the point (spec 19): a test only
    // inside proves the tolerance is not zero, and one only outside proves it
    // is not infinite. Neither alone proves it is what it says. Since M7.2 this
    // pair is sharper than it was -- it names WHICH corner was refused, where
    // M7.1's named-shape rule could only fail the whole shape, and would have
    // read identically if the tolerance had been zero.
    EXPECT_EQ(CountKind(plan, "Coincident"), 3u);
}

TEST(M7Reconstruction, ALineJustInsideTheAxisToleranceIsStillHorizontal) {
    PartDocument document{"M7NearlyFlat"};
    Sketch& sketch = document.addSketch("Imported");
    // 0.9 of the angular tolerance, expressed as a rise over a 100 mm run.
    const double rise = 100.0 * std::sin(0.9 * kReconstructionAxisAngleToleranceRad);
    sketch.addLine(Vec2{0, 0}, Vec2{100, rise});
    sketch.addLine(Vec2{100, rise}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});
    EXPECT_EQ(CountKind(plan, "Horizontal"), 2u);
}

TEST(M7Reconstruction, ALineJustOutsideTheAxisToleranceIsNot) {
    PartDocument document{"M7NotFlatEnough"};
    Sketch& sketch = document.addSketch("Imported");
    const double rise = 100.0 * std::sin(1.1 * kReconstructionAxisAngleToleranceRad);
    sketch.addLine(Vec2{0, 0}, Vec2{100, rise});
    sketch.addLine(Vec2{100, rise}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});
    // ONE, not two: the top side is still exactly horizontal, and only the
    // skewed bottom side is refused. Expecting zero here would have passed just
    // as well if the recogniser had stopped working altogether.
    EXPECT_EQ(CountKind(plan, "Horizontal"), 1u);
}

// --- Direction handling -----------------------------------------------------

TEST(M7Reconstruction, ALineDrawnRightToLeftIsJustAsHorizontal) {
    PartDocument document{"M7Reversed"};
    Sketch& sketch = document.addSketch("Imported");
    // Every side drawn the opposite way round from the fixture above. A
    // recogniser that normalised direction wrongly would find two horizontals
    // and no verticals, or none at all.
    sketch.addLine(Vec2{100, 0}, Vec2{0, 0});
    sketch.addLine(Vec2{100, 50}, Vec2{100, 0});
    sketch.addLine(Vec2{0, 50}, Vec2{100, 50});
    sketch.addLine(Vec2{0, 0}, Vec2{0, 50});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});
    EXPECT_EQ(CountKind(plan, "Horizontal"), 2u);
    EXPECT_EQ(CountKind(plan, "Vertical"), 2u);
    EXPECT_EQ(CountKind(plan, "Coincident"), 4u);
}

TEST(M7Reconstruction, ADimensionMatchesItsLineWhicheverWayEachIsWritten) {
    RectangleFixture fx;
    // The dimension's definition points in the opposite order from the line's
    // endpoints. A dimension measures between two points; which was written
    // first is an artefact of authoring.
    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{100, 0}, Vec2{0, 0})});

    ASSERT_EQ(plan.parameters.size(), 1u);
    EXPECT_DOUBLE_EQ(plan.parameters.front().value, 100.0);
}

// --- The Linear / Aligned distinction ---------------------------------------

TEST(M7Reconstruction, ALinearDimensionMeasuresItsProjectionNotTheSeparation) {
    // Two points 5 apart in u and 12 apart in v: separated by 13, but a
    // horizontal linear dimension across them reads 5.
    ImportedDimension2D linear = Linear(Vec2{0, 0}, Vec2{5, 12});
    EXPECT_DOUBLE_EQ(MeasuredValueMm(linear), 5.0);

    ImportedDimension2D aligned = linear;
    aligned.kind = ImportedDimensionKind::Aligned;
    EXPECT_DOUBLE_EQ(MeasuredValueMm(aligned), 13.0);
}

// --- Adversarial values (spec 24) -------------------------------------------

TEST(M7Reconstruction, NonFiniteAndNegativeDimensionsAreRefused) {
    RectangleFixture fx;
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    for (double bad : {inf, -inf, nan, 0.0, -50.0}) {
        ImportedDimension2D dimension = Linear(Vec2{0, 0}, Vec2{100, 0});
        dimension.statedValueMm = bad;
        const ReconstructionPlan plan =
            AnalyzeForReconstruction(fx.document, fx.sketch->id(), {dimension});
        // Either refused outright, or -- for the values that read as "no stated
        // measurement" -- fall back to the definition points, which are sound.
        for (const PlannedParameter& parameter : plan.parameters) {
            EXPECT_TRUE(std::isfinite(parameter.value));
            EXPECT_GE(parameter.value, kMinReconstructedDimensionMm);
        }
    }
}

TEST(M7Reconstruction, AnUnknownSketchIdProducesAnEmptyPlanAndChangesNothing) {
    RectangleFixture fx;
    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, static_cast<ObjectId>(987654), {Linear(Vec2{0, 0}, Vec2{100, 0})});

    EXPECT_TRUE(plan.empty());
    EXPECT_FALSE(ApplyReconstruction(fx.document, plan));
    EXPECT_TRUE(fx.document.parameters().items().empty());
}

} // namespace
