// M7.5 / M7.6 — the adversarial matrix (spec 24) and the transaction contract
// (spec 16).
//
// Each test below is one row of spec 24's list. The ones already covered
// elsewhere are named in the M7 self-validation report rather than duplicated
// here; what follows is everything the earlier slices did not reach.

#include "Core/Document/PartDocument.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

std::size_t CountKind(const ReconstructionPlan& plan, const char* kindName) {
    std::size_t count = 0;
    for (const PlannedConstraint& constraint : plan.constraints)
        if (std::string(ConstraintKindName(constraint.data)) == kindName) ++count;
    return count;
}

bool HasSkip(const ReconstructionPlan& plan, ReconstructionSkipReason reason) {
    for (const ReconstructionSkip& skip : plan.skipped)
        if (skip.reason == reason) return true;
    return false;
}

struct RectangleFixture {
    PartDocument document{"M7Adv"};
    Sketch* sketch = nullptr;
    SketchEntityId bottom{};

    RectangleFixture() {
        Sketch& s = document.addSketch("Imported");
        sketch = &s;
        bottom = s.addLine(Vec2{0, 0}, Vec2{100, 0});
        s.addLine(Vec2{100, 0}, Vec2{100, 50});
        s.addLine(Vec2{100, 50}, Vec2{0, 50});
        s.addLine(Vec2{0, 50}, Vec2{0, 0});
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

ImportedDimension2D Radial(Vec2 center, double radius) {
    ImportedDimension2D dimension;
    dimension.kind = ImportedDimensionKind::Radius;
    dimension.measureFrom = center;
    dimension.measureTo = Vec2{center.x + radius, center.y};
    return dimension;
}

// --- Value extremes (spec 24) ----------------------------------------------

TEST(M7Adversarial, DimensionsExactlyAtAndJustBelowTheReconstructionFloor) {
    // The pair either side of the threshold, which is the only form of
    // tolerance test that proves anything (spec 19).
    {
        PartDocument document{"M7AtFloor"};
        Sketch& sketch = document.addSketch("s");
        const double at = kMinReconstructedDimensionMm;
        sketch.addLine(Vec2{0, 0}, Vec2{at, 0});
        const ReconstructionPlan plan =
            AnalyzeForReconstruction(document, sketch.id(), {Linear(Vec2{0, 0}, Vec2{at, 0})});
        EXPECT_EQ(CountKind(plan, "Length"), 1u) << "a dimension AT the floor must be accepted";
    }
    {
        PartDocument document{"M7BelowFloor"};
        Sketch& sketch = document.addSketch("s");
        const double below = std::nextafter(kMinReconstructedDimensionMm, 0.0);
        sketch.addLine(Vec2{0, 0}, Vec2{below, 0});
        const ReconstructionPlan plan =
            AnalyzeForReconstruction(document, sketch.id(), {Linear(Vec2{0, 0}, Vec2{below, 0})});
        EXPECT_EQ(CountKind(plan, "Length"), 0u);
        EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::InvalidValue));
    }
}

TEST(M7Adversarial, AVeryLargeDimensionIsStillReconstructed) {
    PartDocument document{"M7Huge"};
    Sketch& sketch = document.addSketch("s");
    // A kilometre, in millimetres. Large but entirely legitimate -- a survey or
    // site drawing -- and refusing it would be a silent scale limit.
    sketch.addLine(Vec2{0, 0}, Vec2{1.0e6, 0});
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketch.id(), {Linear(Vec2{0, 0}, Vec2{1.0e6, 0})});

    EXPECT_EQ(CountKind(plan, "Length"), 1u);
    ASSERT_EQ(plan.parameters.size(), 1u);
    EXPECT_DOUBLE_EQ(plan.parameters.front().value, 1.0e6);
}

TEST(M7Adversarial, ZeroAndNegativeRadiiAreRefused) {
    PartDocument document{"M7BadRadius"};
    Sketch& sketch = document.addSketch("s");
    sketch.addCircle(Vec2{25, 30}, 10.0);

    for (double bad : {0.0, -10.0}) {
        ImportedDimension2D dimension = Radial(Vec2{25, 30}, 10.0);
        dimension.statedValueMm = bad;
        const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {dimension});
        // A zero or negative stated value reads as "no stated measurement", so
        // the definition points are used instead -- and those are sound. What
        // must never happen is a Parameter carrying 0 or -10.
        for (const PlannedParameter& parameter : plan.parameters) {
            EXPECT_GT(parameter.value, 0.0);
            EXPECT_TRUE(std::isfinite(parameter.value));
        }
    }
}

TEST(M7Adversarial, NonFiniteDefinitionPointsProduceNoParameter) {
    RectangleFixture fx;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    for (double bad : {nan, inf, -inf}) {
        const ReconstructionPlan plan = AnalyzeForReconstruction(
            fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{bad, 0})});
        for (const PlannedParameter& parameter : plan.parameters)
            EXPECT_TRUE(std::isfinite(parameter.value));
        EXPECT_EQ(CountKind(plan, "Length"), 0u);
    }
}

// --- Missing and deleted targets (spec 24) ----------------------------------

TEST(M7Adversarial, ATargetDeletedBeforeReconstructionIsSimplyNotFound) {
    RectangleFixture fx;
    const ImportedDimension2D width = Linear(Vec2{0, 0}, Vec2{100, 0});

    // The drawing said 100 across the bottom; the bottom is gone.
    ASSERT_TRUE(fx.document.removeSketchEntity(fx.sketch->id(), fx.bottom));

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {width});

    EXPECT_EQ(CountKind(plan, "Length"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::NoTargetGeometry));
    // And the rest of the drawing still reconstructs -- three lines, two
    // joints. One missing target must not cost the user everything else.
    EXPECT_EQ(CountKind(plan, "Coincident"), 2u);
}

TEST(M7Adversarial, AnEntityDeletedBETWEENAnalysisAndApplyIsRefusedNotIgnored) {
    RectangleFixture fx;
    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0})});
    ASSERT_GT(plan.constraints.size(), 0u);

    // The plan is now stale: it names an entity that no longer exists. This is
    // the window a staged design opens, so it has to be closed at the far end.
    ASSERT_TRUE(fx.document.removeSketchEntity(fx.sketch->id(), fx.bottom));

    const ReconstructionResult result = ApplyReconstruction(fx.document, plan);

    EXPECT_FALSE(result) << "a stale plan was applied against changed geometry";
    // And nothing half-built survives the refusal.
    EXPECT_EQ(fx.document.parameters().findByName("Width"), nullptr);
}

// --- Redundancy and conflict (spec 17, 24) ----------------------------------

TEST(M7Adversarial, TwoDimensionsOnOneLineBothReconstructAndAreNamedApart) {
    RectangleFixture fx;
    // The same edge dimensioned twice, agreeing. Redundant but consistent --
    // legal, and the two must not collide on one name.
    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(),
        {Linear(Vec2{0, 0}, Vec2{100, 0}), Linear(Vec2{100, 0}, Vec2{0, 0})});
    ASSERT_TRUE(result) << result.message;

    ASSERT_EQ(result.createdParameters.size(), 2u);
    // Width, then Length1 -- NOT Width_2. Only the FIRST horizontal dimension
    // earns the name Width (ADR-M7-002); the `_2` suffix is for collisions with
    // a name already taken, which is a different situation and has its own
    // test. Two mechanisms that both produce "a second name" are easy to
    // confuse, so this asserts which one fired.
    EXPECT_NE(fx.document.parameters().findByName("Width"), nullptr);
    EXPECT_NE(fx.document.parameters().findByName("Length1"), nullptr);
    EXPECT_EQ(fx.document.parameters().findByName("Width_2"), nullptr);
}

// Conflict and recovery live in the INTEGRATION suite (Gate H), not here.
// This binary does not link a solver, so every sketch in it reports
// InvalidInput -- which is what a first draft of that test measured while
// appearing to be about conflicting dimensions.

// --- Order independence (spec 24) -------------------------------------------

TEST(M7Adversarial, ShuffledEntityOrderProducesTheSameConstraintCounts) {
    const auto build = [](PartDocument& document, int order) {
        Sketch& s = document.addSketch("s");
        // The same four sides, created in two different orders.
        if (order == 0) {
            s.addLine(Vec2{0, 0}, Vec2{100, 0});
            s.addLine(Vec2{100, 0}, Vec2{100, 50});
            s.addLine(Vec2{100, 50}, Vec2{0, 50});
            s.addLine(Vec2{0, 50}, Vec2{0, 0});
        } else {
            s.addLine(Vec2{0, 50}, Vec2{0, 0});
            s.addLine(Vec2{100, 50}, Vec2{0, 50});
            s.addLine(Vec2{0, 0}, Vec2{100, 0});
            s.addLine(Vec2{100, 0}, Vec2{100, 50});
        }
        return s.id();
    };

    PartDocument a{"A"};
    PartDocument b{"B"};
    const ObjectId sa = build(a, 0);
    const ObjectId sb = build(b, 1);
    const std::vector<ImportedDimension2D> dimensions{
        Linear(Vec2{0, 0}, Vec2{100, 0}), Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2)};

    const ReconstructionPlan pa = AnalyzeForReconstruction(a, sa, dimensions);
    const ReconstructionPlan pb = AnalyzeForReconstruction(b, sb, dimensions);

    // File order is not identity (ADR-M6-004), and reconstruction is the place
    // it would be easiest to accidentally make it so.
    EXPECT_EQ(CountKind(pa, "Coincident"), CountKind(pb, "Coincident"));
    EXPECT_EQ(CountKind(pa, "Horizontal"), CountKind(pb, "Horizontal"));
    EXPECT_EQ(CountKind(pa, "Vertical"), CountKind(pb, "Vertical"));
    EXPECT_EQ(CountKind(pa, "Length"), CountKind(pb, "Length"));
    ASSERT_EQ(pa.parameters.size(), pb.parameters.size());
    for (std::size_t i = 0; i < pa.parameters.size(); ++i) {
        EXPECT_EQ(pa.parameters[i].name, pb.parameters[i].name);
        EXPECT_DOUBLE_EQ(pa.parameters[i].value, pb.parameters[i].value);
    }
}

// --- Reconstruction after save/load (spec 24) -------------------------------

TEST(M7Adversarial, ASketchLoadedFromDiskCanStillBeReconstructed) {
    std::string saved;
    ObjectId sketchId = kInvalidObjectId;
    {
        PartDocument document{"M7Saved"};
        Sketch& sketch = document.addSketch("Imported");
        sketchId = sketch.id();
        sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
        sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
        sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
        sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(document, out));
        saved = out.str();
    }

    std::istringstream in(saved);
    LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    // Reconstruction takes native geometry, not import output, so a sketch that
    // has been round-tripped through the schema is as reconstructible as one
    // that just arrived. Nothing about it should remember where it came from.
    const ReconstructionResult result =
        ReconstructSketch(*loaded.document, sketchId, {Linear(Vec2{0, 0}, Vec2{100, 0})});
    ASSERT_TRUE(result) << result.message;
    EXPECT_NE(loaded.document->parameters().findByName("Width"), nullptr);
}

TEST(M7Adversarial, AnAlreadyReconstructedSketchIsStillRefusedAfterSaveAndLoad) {
    std::string saved;
    ObjectId sketchId = kInvalidObjectId;
    {
        PartDocument document{"M7Idem"};
        Sketch& sketch = document.addSketch("Imported");
        sketchId = sketch.id();
        sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
        sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
        sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
        sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
        ASSERT_TRUE(ReconstructSketch(document, sketchId, {Linear(Vec2{0, 0}, Vec2{100, 0})}));
        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(document, out));
        saved = out.str();
    }

    std::istringstream in(saved);
    LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    // The idempotence test is NATIVE STATE, not a provenance flag -- which is
    // exactly why it still works here, on a document whose provenance was never
    // persisted (ADR-M7-007, ADR-M7-017). A flag-based check would have
    // forgotten and produced Width_2.
    const ReconstructionResult second =
        ReconstructSketch(*loaded.document, sketchId, {Linear(Vec2{0, 0}, Vec2{100, 0})});
    EXPECT_FALSE(second);
    EXPECT_EQ(loaded.document->parameters().findByName("Width_2"), nullptr);
}

// --- Unknown modes (spec 24) ------------------------------------------------

TEST(M7Adversarial, EveryRecognizerDisabledProducesAnEmptyPlanAndNoDocumentChange) {
    RectangleFixture fx;
    ReconstructionOptions nothing;
    nothing.reconstructExplicitDimensions = false;
    nothing.recognizeHorizontal = false;
    nothing.recognizeVertical = false;
    nothing.recognizeCoincident = false;
    nothing.placeFix = false;

    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0})}, nothing);
    EXPECT_TRUE(plan.empty());

    // An empty plan applies cleanly and changes nothing -- "no edit performs no
    // work" (spec 23 Gate K), rather than failing or half-succeeding.
    const ReconstructionResult result = ApplyReconstruction(fx.document, plan);
    EXPECT_TRUE(result) << result.message;
    EXPECT_TRUE(fx.document.parameters().items().empty());
    EXPECT_TRUE(fx.document.findSketch(fx.sketch->id())->constraints().empty());
}

TEST(M7Adversarial, ANonsenseToleranceDoesNotCrashOrInventGeometry) {
    RectangleFixture fx;
    ReconstructionOptions absurd;
    absurd.coincidenceToleranceMm = 0.0;      // nothing is ever coincident
    absurd.axisAngleToleranceRad = 0.0;       // nothing is ever axis-aligned

    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0})}, absurd);

    // A zero coincidence tolerance is degenerate, not undefined: exact
    // endpoints still match exactly, so this rectangle still closes. What must
    // not happen is a crash, or geometry appearing that the rules did not
    // license.
    EXPECT_LE(CountKind(plan, "Coincident"), 4u);
    EXPECT_TRUE(ValidatePlan(plan)) << ValidatePlan(plan).message;
}

} // namespace
