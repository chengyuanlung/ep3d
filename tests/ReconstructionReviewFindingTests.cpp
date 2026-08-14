// One regression test per finding of M7 independent review round 1.
//
// Named M7_REV_* so a reviewer can map each back to the finding it pins, and so
// a later round can tell "this was always covered" from "this was covered
// because of round 1".
//
// Several of these exist because the reviewers removed a line and NOTHING
// failed. Those are marked; they are the ones that matter most, because a rule
// with no test is a rule that will be deleted by someone eventually.

#include "Core/Document/PartDocument.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include <gtest/gtest.h>
#include <cmath>
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

const PlannedParameter* FindParameter(const ReconstructionPlan& plan, const std::string& name) {
    for (const PlannedParameter& parameter : plan.parameters)
        if (parameter.name == name) return &parameter;
    return nullptr;
}

bool HasSkip(const ReconstructionPlan& plan, ReconstructionSkipReason reason) {
    for (const ReconstructionSkip& skip : plan.skipped)
        if (skip.reason == reason) return true;
    return false;
}

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

// --- C1: a Linear dimension states a PROJECTION, not a length ---------------

TEST(M7_REV_C1, ALinearDimensionAcrossASlopedLineIsRefused) {
    PartDocument document{"revC1"};
    Sketch& sketch = document.addSketch("s");
    // 13 mm long: 5 across, 12 up.
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{5, 12});
    ASSERT_EQ(sketch.entities().size(), 1u);

    // An ordinary HORIZONTAL dimension across its two ends. This is what a real
    // DXF writes, and its measurement is 5 -- the horizontal run, not the line.
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketch.id(), {Linear(Vec2{0, 0}, Vec2{5, 12}, 0.0)});

    // Before the fix this produced Length=5 on a 13 mm line, with zero skips and
    // no diagnostic, and the solver then shortened the line to 5 mm.
    EXPECT_EQ(CountKind(plan, "Length"), 0u);
    EXPECT_TRUE(plan.parameters.empty());
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::ValueDisagreesWithGeometry));
    (void)line;
}

TEST(M7_REV_C1, TheSameTwoPointsAsAnALIGNEDDimensionAreAccepted) {
    PartDocument document{"revC1b"};
    Sketch& sketch = document.addSketch("s");
    sketch.addLine(Vec2{0, 0}, Vec2{5, 12});

    ImportedDimension2D aligned = Linear(Vec2{0, 0}, Vec2{5, 12});
    aligned.kind = ImportedDimensionKind::Aligned;
    aligned.directionRad = std::atan2(12.0, 5.0);

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {aligned});

    // The straddling half of the pair. Aligned measures along its own points, so
    // its value IS the line's length -- refusing this too would mean the fix had
    // simply stopped reconstructing sloped geometry.
    ASSERT_EQ(plan.parameters.size(), 1u);
    EXPECT_NEAR(plan.parameters.front().value, 13.0, 1e-9);
}

TEST(M7_REV_C1, ALinearDimensionAlongTheLineItNamesIsStillAccepted) {
    PartDocument document{"revC1c"};
    Sketch& sketch = document.addSketch("s");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketch.id(), {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0)});

    ASSERT_EQ(plan.parameters.size(), 1u);
    EXPECT_DOUBLE_EQ(plan.parameters.front().value, 100.0);
}

// --- C3: a plan belongs to the document it was built from -------------------

TEST(M7_REV_C3, APlanBuiltForAnotherDocumentIsRefused) {
    // Two documents from the SAME saved bytes, so their ids overlap exactly --
    // which is not a contrivance: ObjectId is a process-local counter, so this
    // is what two loads in one session always look like.
    std::string saved;
    ObjectId sketchId = kInvalidObjectId;
    {
        PartDocument source{"revC3"};
        Sketch& sketch = source.addSketch("s");
        sketchId = sketch.id();
        sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
        sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
        sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
        sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(source, out));
        saved = out.str();
    }

    std::istringstream inA(saved);
    LoadResult a = loadPartDocument(inA);
    ASSERT_TRUE(a) << a.message;
    std::istringstream inB(saved);
    LoadResult b = loadPartDocument(inB);
    ASSERT_TRUE(b) << b.message;

    const ReconstructionPlan planForA =
        AnalyzeForReconstruction(*a.document, sketchId, {Linear(Vec2{0, 0}, Vec2{100, 0})});
    ASSERT_FALSE(planForA.parameters.empty());

    // B DIVERGES: its bottom edge is 250 mm, not 100. This is the situation
    // that makes the two documents different things rather than two copies --
    // and note that both still carry the SAME sketch id AND the same document
    // id, because they came from one file. Identity cannot separate them.
    SketchEntityId bottom = kInvalidSketchEntityId;
    for (const SketchEntity& entity : b.document->findSketch(sketchId)->entities()) {
        const auto* line = std::get_if<SketchLine>(&entity.geometry);
        if (line != nullptr && line->start.y == 0.0 && line->end.y == 0.0) bottom = entity.id;
    }
    ASSERT_NE(bottom, kInvalidSketchEntityId);
    ASSERT_TRUE(b.document->editSketch(sketchId, [bottom](Sketch& sketch) {
        sketch.replaceGeometry(bottom, SketchLine{Vec2{0, 0}, Vec2{250, 0}});
    }));

    const ReconstructionResult applied = ApplyReconstruction(*b.document, planForA);

    // Before the fix this succeeded, and B's 250 mm edge was driven by a
    // Width=100 measured from a different document.
    EXPECT_FALSE(applied) << applied.message;
    EXPECT_EQ(b.document->parameters().findByName("Width"), nullptr);
}

TEST(M7_REV_C3, APlanStillDescribingItsOwnDocumentIsStillApplied) {
    // The straddling half: the geometric check must not refuse the ordinary
    // case, or it would simply have disabled reconstruction. The release
    // fixture is drawn 0.5% off its dimension and must still apply.
    PartDocument document{"revC3b"};
    Sketch& sketch = document.addSketch("s");
    sketch.addLine(Vec2{0, 0}, Vec2{99.5, 0});
    sketch.addLine(Vec2{99.5, 0}, Vec2{99.5, 50});
    sketch.addLine(Vec2{99.5, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    ImportedDimension2D width = Linear(Vec2{0, 0}, Vec2{99.5, 0});
    width.statedValueMm = 100.0;

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {width});
    ASSERT_EQ(plan.parameters.size(), 1u);
    EXPECT_DOUBLE_EQ(plan.parameters.front().value, 100.0);

    const ReconstructionResult applied = ApplyReconstruction(document, plan);
    EXPECT_TRUE(applied) << applied.message;
    EXPECT_NE(document.parameters().findByName("Width"), nullptr);
}

// --- C4: the slot-containment branch, whose removal was memory-unsafe -------

TEST(M7_REV_C4, AnOutOfRangeParameterSlotIsRejectedByValidation) {
    PartDocument document{"revC4"};
    Sketch& sketch = document.addSketch("s");
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});

    ReconstructionPlan plan;
    plan.documentId = document.id();
    plan.sketchId = sketch.id();
    plan.parameters.push_back(PlannedParameter{"Width", 100.0, UnitType::Millimeter,
                                               ReconstructionOrigin::ExplicitSource, {}});
    // Slot 5 against one parameter. `ApplyReconstruction` indexes the slot map
    // directly, so with this branch removed the read is out of bounds -- a
    // reviewer reproduced an access violation, not merely a wrong answer.
    plan.constraints.push_back(PlannedConstraint{LengthConstraint{line, kInvalidObjectId},
                                                 static_cast<PlanParameterSlot>(5),
                                                 ReconstructionOrigin::ExplicitSource, {}});

    EXPECT_FALSE(ValidatePlan(plan));
    EXPECT_FALSE(ApplyReconstruction(document, plan));
    EXPECT_TRUE(document.parameters().items().empty());
}

// --- M1: idempotence when the drawing carries no dimensions -----------------

TEST(M7_REV_M1, AnUndimensionedSketchIsNotReconstructedTwice) {
    PartDocument document{"revM1"};
    Sketch& sketch = document.addSketch("s");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    ASSERT_TRUE(ReconstructSketch(document, sketch.id(), {}));
    const std::size_t afterFirst = document.findSketch(sketch.id())->constraints().size();
    ASSERT_EQ(afterFirst, 9u); // 4 Coincident + 2 H + 2 V + 1 Fix

    const ReconstructionResult second = ReconstructSketch(document, sketch.id(), {});
    const ReconstructionResult third = ReconstructSketch(document, sketch.id(), {});

    // Before the fix: 9 -> 18 -> 27, every run reporting success, with a
    // duplicate Fix each time. The guard only tested for DIMENSIONAL
    // constraints, and a drawing with no DIMENSION entities creates none.
    EXPECT_FALSE(second);
    EXPECT_FALSE(third);
    EXPECT_EQ(document.findSketch(sketch.id())->constraints().size(), afterFirst);
}

// --- M7: curve naming must not depend on file order -------------------------

TEST(M7_REV_M7, RadiusNamesDoNotDependOnTheOrderDimensionsArrive) {
    const auto build = [](PartDocument& document) {
        Sketch& sketch = document.addSketch("s");
        sketch.addCircle(Vec2{0, 0}, 10.0);
        sketch.addCircle(Vec2{200, 0}, 4.0);
        return sketch.id();
    };

    PartDocument a{"revM7a"};
    PartDocument b{"revM7b"};
    const ObjectId sa = build(a);
    const ObjectId sb = build(b);

    const ImportedDimension2D big = Radial(Vec2{0, 0}, 10.0);
    const ImportedDimension2D small = Radial(Vec2{200, 0}, 4.0);

    const ReconstructionPlan forward = AnalyzeForReconstruction(a, sa, {big, small});
    const ReconstructionPlan reversed = AnalyzeForReconstruction(b, sb, {small, big});

    // Before the fix: forward gave Radius=10/Radius_2=4 and reversed gave
    // Radius=4/Radius_2=10 -- the same drawing exported the other way round
    // produced different Parameter names on the same holes.
    ASSERT_NE(FindParameter(forward, "Radius"), nullptr);
    ASSERT_NE(FindParameter(reversed, "Radius"), nullptr);
    EXPECT_DOUBLE_EQ(FindParameter(forward, "Radius")->value,
                     FindParameter(reversed, "Radius")->value);
    ASSERT_NE(FindParameter(forward, "Radius_2"), nullptr);
    ASSERT_NE(FindParameter(reversed, "Radius_2"), nullptr);
    EXPECT_DOUBLE_EQ(FindParameter(forward, "Radius_2")->value,
                     FindParameter(reversed, "Radius_2")->value);
}

// --- M8: the linear geometric sort, which had no test at all ----------------

TEST(M7_REV_M8, WidthLandsOnTheSameLineWhicheverOrderTwoHorizontalsArrive) {
    const auto build = [](PartDocument& document) {
        Sketch& sketch = document.addSketch("s");
        sketch.addLine(Vec2{0, 0}, Vec2{100, 0});   // at the origin
        sketch.addLine(Vec2{0, 500}, Vec2{60, 500}); // far away
        return sketch.id();
    };

    PartDocument a{"revM8a"};
    PartDocument b{"revM8b"};
    const ObjectId sa = build(a);
    const ObjectId sb = build(b);

    const ImportedDimension2D near = Linear(Vec2{0, 0}, Vec2{100, 0});
    const ImportedDimension2D far = Linear(Vec2{0, 500}, Vec2{60, 500});

    const ReconstructionPlan forward = AnalyzeForReconstruction(a, sa, {near, far});
    const ReconstructionPlan reversed = AnalyzeForReconstruction(b, sb, {far, near});

    // Deleting the sort failed NO test before this one existed. It only matters
    // from the SECOND dimension of a given orientation onward, which is why the
    // existing order test could not see it: Width/Height are chosen by
    // orientation, not by position.
    ASSERT_NE(FindParameter(forward, "Width"), nullptr);
    ASSERT_NE(FindParameter(reversed, "Width"), nullptr);
    EXPECT_DOUBLE_EQ(FindParameter(forward, "Width")->value,
                     FindParameter(reversed, "Width")->value);
    EXPECT_DOUBLE_EQ(FindParameter(forward, "Length1")->value,
                     FindParameter(reversed, "Length1")->value);
}

// --- M9: the circle-centre Fix min-search, which ADR-M7-016 wrongly claimed
//         already had a test ------------------------------------------------

TEST(M7_REV_M9, TheFixAnchorsTheSameCircleWhicheverOrderCirclesArrive) {
    const auto fixedCenter = [](PartDocument& document, ObjectId sketchId) {
        const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketchId, {});
        for (const PlannedConstraint& constraint : plan.constraints) {
            const auto* fix = std::get_if<FixConstraint>(&constraint.data);
            if (fix == nullptr) continue;
            const SketchEntity* entity =
                document.findSketch(sketchId)->findEntity(fix->target.entityId);
            EXPECT_NE(entity, nullptr);
            return std::get<SketchCircle>(entity->geometry).center;
        }
        return Vec2{-1, -1};
    };

    PartDocument a{"revM9a"};
    Sketch& sa = a.addSketch("s");
    sa.addCircle(Vec2{0, 0}, 5.0);
    sa.addCircle(Vec2{300, 200}, 5.0);

    PartDocument b{"revM9b"};
    Sketch& sb = b.addSketch("s");
    sb.addCircle(Vec2{300, 200}, 5.0); // the far one FIRST
    sb.addCircle(Vec2{0, 0}, 5.0);

    const Vec2 pa = fixedCenter(a, sa.id());
    const Vec2 pb = fixedCenter(b, sb.id());

    // Without the min-search the Fix followed entity storage position, which is
    // a persistent placement decision made by a vector index (ADR-M4-004).
    EXPECT_DOUBLE_EQ(pa.x, pb.x);
    EXPECT_DOUBLE_EQ(pa.y, pb.y);
    EXPECT_DOUBLE_EQ(pa.x, 0.0);
    EXPECT_DOUBLE_EQ(pa.y, 0.0);
}

// --- M10: the same-entity coincidence guard, on a sketch that is not empty --

TEST(M7_REV_M10, ANearlyClosedArcIsNotMadeCoincidentWithItself) {
    PartDocument document{"revM10"};
    Sketch& sketch = document.addSketch("s");
    // A sweep the MODEL ACCEPTS. The original test used 2pi - 1e-9, which
    // IsValidSketchGeometry refuses, so the sketch was empty and the test
    // asserted "no coincidences" about nothing -- it passed with the whole
    // engine deleted.
    sketch.addArc(Vec2{0, 0}, 10.0, 0.0, 2.0 * kPi - 1e-5, true);
    ASSERT_EQ(sketch.entities().size(), 1u) << "the fixture built no geometry";

    // The two endpoints are ~1e-4 mm apart, inside the coincidence tolerance,
    // so they DO cluster -- and the guard is what stops the arc being tied to
    // itself with a residual that is identically zero.
    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});
    EXPECT_EQ(CountKind(plan, "Coincident"), 0u);
    EXPECT_EQ(CountKind(plan, "Fix"), 1u);
}

// --- M12: a rolled-back apply must not claim it did the work ----------------

TEST(M7_REV_M12, ARolledBackApplyReportsNothingReconstructed) {
    PartDocument document{"revM12"};
    Sketch& sketch = document.addSketch("s");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketch.id(), {Linear(Vec2{0, 0}, Vec2{100, 0})});
    ASSERT_FALSE(plan.constraints.empty());
    plan.constraints.push_back(PlannedConstraint{
        HorizontalConstraint{static_cast<SketchEntityId>(987654)}, kInvalidPlanParameterSlot,
        ReconstructionOrigin::InferredGeometric, {}});

    const ReconstructionResult result = ApplyReconstruction(document, plan);
    ASSERT_FALSE(result);

    // Before the fix the report still said "reconstructed: 10 constraint(s)",
    // naming ids the rollback had destroyed -- and that string is what the UI's
    // diagnostic panel shows.
    EXPECT_TRUE(result.report.entries.empty());
    const std::string text = FormatReconstructionReport(result.report);
    EXPECT_NE(text.find("reconstructed: 0 constraint(s)"), std::string::npos) << text;

    // The skipped list is deliberately kept: on a failure path that is the half
    // worth having (ADR-M7-017).
    EXPECT_EQ(result.report.sketchId, sketch.id());
}

// --- M13: the vertical half of spec 24's tolerance-boundary pair ------------

TEST(M7_REV_M13, ALineJustInsideTheVerticalToleranceIsStillVertical) {
    PartDocument document{"revM13a"};
    Sketch& sketch = document.addSketch("s");
    const double run = 100.0 * std::sin(0.9 * kReconstructionAxisAngleToleranceRad);
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100 + run, 100});
    sketch.addLine(Vec2{100 + run, 100}, Vec2{0, 100});
    sketch.addLine(Vec2{0, 100}, Vec2{0, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});
    EXPECT_EQ(CountKind(plan, "Vertical"), 2u);
}

TEST(M7_REV_M13, ALineJustOutsideTheVerticalToleranceIsNot) {
    PartDocument document{"revM13b"};
    Sketch& sketch = document.addSketch("s");
    const double run = 100.0 * std::sin(1.1 * kReconstructionAxisAngleToleranceRad);
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100 + run, 100});
    sketch.addLine(Vec2{100 + run, 100}, Vec2{0, 100});
    sketch.addLine(Vec2{0, 100}, Vec2{0, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});
    // ONE, not zero: the left side is still exactly vertical and only the
    // skewed right side is refused. Spec 24 asks for this pair and only the
    // horizontal one existed.
    EXPECT_EQ(CountKind(plan, "Vertical"), 1u);
}

} // namespace
