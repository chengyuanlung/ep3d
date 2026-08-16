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
#include <vector>

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

TEST(M7_REV_M12, ARefusedApplyReportsNothingReconstructed) {
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

// --- Round 2 regression tests -----------------------------------------------
// One per finding. Round 2 proved round 1's two Critical fixes did not close
// their findings: ObjectId is a per-process counter, so two documents were
// identity-indistinguishable and a 100x50 plate's plan applied cleanly to a
// 103x80 bracket; and the geometric check looked only at DIMENSIONAL
// magnitudes, so a length-preserving edit validated and the solver then
// silently rewrote the user's geometry back to the stale plan's shape.

namespace {

ObjectId BuildDimensionedRectangle(PartDocument& document, double width, double height,
                                   const char* sketchName = "s") {
    Sketch& sketch = document.addSketch(sketchName);
    sketch.addLine(Vec2{0, 0}, Vec2{width, 0});
    sketch.addLine(Vec2{width, 0}, Vec2{width, height});
    sketch.addLine(Vec2{width, height}, Vec2{0, height});
    sketch.addLine(Vec2{0, height}, Vec2{0, 0});
    return sketch.id();
}

std::vector<ImportedDimension2D> WidthDimension(double width) {
    ImportedDimension2D dimension;
    dimension.kind = ImportedDimensionKind::Linear;
    dimension.measureFrom = Vec2{0, 0};
    dimension.measureTo = Vec2{width, 0};
    dimension.statedValueMm = width;
    dimension.directionRad = 0.0;
    dimension.sourceHandle = "W1";
    return {dimension};
}

} // namespace

TEST(M7_REV2_C1, APlanFromADifferentDocumentWithTheSameIdsIsRefused) {
    // Demonstrated in review as ok=1 with NO diagnostic. The id collision is
    // forged the way two sessions produce it -- one file, loaded twice, so
    // both documents carry identical ids -- and then one copy is edited to a
    // DIFFERENT part whose width sits inside the 5% agreement band. That band
    // answers "does this dimension describe this line", never "is this the
    // same document"; round 1's test diverged by 150% and so was satisfied by
    // the band alone. Only the content fingerprint separates these two.
    std::string saved;
    ObjectId sketchId = kInvalidObjectId;
    {
        PartDocument source{"plate-A"};
        sketchId = BuildDimensionedRectangle(source, 100.0, 50.0);
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

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(*a.document, sketchId, WidthDimension(100.0));
    ASSERT_FALSE(plan.empty());

    // B becomes a different part: 103 x 80. 100 vs 103 is 3% -- inside the band.
    ASSERT_TRUE(b.document->editSketch(sketchId, [](Sketch& sketch) {
        const auto ids = sketch.entities();
        sketch.replaceGeometry(ids[0].id, SketchLine{Vec2{0, 0}, Vec2{103, 0}});
        sketch.replaceGeometry(ids[1].id, SketchLine{Vec2{103, 0}, Vec2{103, 80}});
        sketch.replaceGeometry(ids[2].id, SketchLine{Vec2{103, 80}, Vec2{0, 80}});
        sketch.replaceGeometry(ids[3].id, SketchLine{Vec2{0, 80}, Vec2{0, 0}});
    }));

    const PlanValidation describes = ValidatePlanAgainstDocument(*b.document, plan);
    EXPECT_FALSE(describes) << "a different part accepted a plan built elsewhere";
    EXPECT_NE(describes.message.find("different geometry"), std::string::npos)
        << describes.message;

    const ReconstructionResult applied = ApplyReconstruction(*b.document, plan);
    EXPECT_FALSE(applied);
    EXPECT_EQ(b.document->parameters().items().size(), 0u)
        << "the foreign plan created a Parameter in a document it does not describe";
}

TEST(M7_REV2_C1, ALengthPreservingEditIsRefused) {
    // Rotate the dimensioned edge 90 degrees about its own start point: same
    // entity id, same 100 mm length, every magnitude the old check looked at
    // unchanged. The Horizontal the plan carries is re-asserted now.
    PartDocument document{"rev2c1b"};
    const ObjectId sketchId = BuildDimensionedRectangle(document, 100.0, 50.0);
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketchId, WidthDimension(100.0));
    ASSERT_FALSE(plan.empty());

    SketchEntityId bottom = kInvalidSketchEntityId;
    ASSERT_TRUE(document.editSketch(sketchId, [&bottom](Sketch& sketch) {
        bottom = sketch.entities().front().id;
        sketch.replaceGeometry(bottom, SketchLine{Vec2{0, 0}, Vec2{0, -100}});
    }));

    const PlanValidation describes = ValidatePlanAgainstDocument(document, plan);
    EXPECT_FALSE(describes) << "a plan that no longer describes this geometry was accepted";

    const ReconstructionResult applied = ApplyReconstruction(document, plan);
    EXPECT_FALSE(applied);

    // And the user's edit survives untouched -- the whole point of refusing.
    const SketchEntity* edited = document.findSketch(sketchId)->findEntity(bottom);
    ASSERT_NE(edited, nullptr);
    const auto* line = std::get_if<SketchLine>(&edited->geometry);
    ASSERT_NE(line, nullptr);
    EXPECT_DOUBLE_EQ(line->end.x, 0.0) << "the refused plan still moved the user's line";
    EXPECT_DOUBLE_EQ(line->end.y, -100.0);
}

TEST(M7_REV2_C1, AHandBuiltPlanWithNoFingerprintIsStillCheckedAgainstGeometry) {
    // The fingerprint catches every edit for plans that CARRY one, which is
    // every plan analysis produces. A hand-built plan -- legal, because the
    // staged analyze/validate/apply separation is this milestone's headline
    // design (ADR-M7-005) -- carries no fingerprint, and for it the inferred
    // re-assertion is the only thing left. Verified by mutation: neutering the
    // Horizontal/Vertical re-check fails exactly this test and nothing else,
    // so it is guarded rather than merely present.
    PartDocument document{"rev2c1d"};
    const ObjectId sketchId = BuildDimensionedRectangle(document, 100.0, 50.0);
    ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketchId, WidthDimension(100.0));
    ASSERT_FALSE(plan.empty());
    plan.fingerprint = 0; // as a caller assembling a plan by hand would leave it

    SketchEntityId bottom = kInvalidSketchEntityId;
    ASSERT_TRUE(document.editSketch(sketchId, [&bottom](Sketch& sketch) {
        bottom = sketch.entities().front().id;
        sketch.replaceGeometry(bottom, SketchLine{Vec2{0, 0}, Vec2{0, -100}});
    }));

    const PlanValidation describes = ValidatePlanAgainstDocument(document, plan);
    EXPECT_FALSE(describes) << "an unfingerprinted plan skipped the geometry re-check";
    // Whichever inferred rule notices first -- here the Coincident, because
    // the rotated edge's endpoint no longer meets its neighbour -- the refusal
    // must name the plan, not blame the document for a magnitude change that
    // did not happen.
    EXPECT_NE(describes.message.find("no longer describes this document"), std::string::npos)
        << describes.message;
    EXPECT_FALSE(ApplyReconstruction(document, plan));
}

TEST(M7_REV2_C1, AnUnchangedDocumentStillApplies) {
    // The straddling half, without which both tests above would be satisfied
    // by a validator that simply refuses everything.
    PartDocument document{"rev2c1c"};
    const ObjectId sketchId = BuildDimensionedRectangle(document, 100.0, 50.0);
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketchId, WidthDimension(100.0));
    ASSERT_FALSE(plan.empty());

    EXPECT_TRUE(ValidatePlanAgainstDocument(document, plan));
    const ReconstructionResult applied = ApplyReconstruction(document, plan);
    EXPECT_TRUE(applied) << applied.message;
}

TEST(M7_REV2_M6, ANameTakenBetweenAnalyzeAndApplyIsRefused) {
    // The plan promises its names are free "checked at build time" -- and
    // nothing rechecked at apply, so a name taken in between produced TWO
    // Parameters of one name driving different geometry.
    PartDocument document{"rev2m6"};
    const ObjectId sketchId = BuildDimensionedRectangle(document, 100.0, 50.0);
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketchId, WidthDimension(100.0));
    ASSERT_EQ(plan.parameters.size(), 1u);
    const std::string planned = plan.parameters.front().name;

    document.addParameter(planned, 7.0, UnitType::Millimeter);
    const ReconstructionResult applied = ApplyReconstruction(document, plan);
    EXPECT_FALSE(applied) << "the plan created a second Parameter of the same name";

    int named = 0;
    for (const auto& parameter : document.parameters().items())
        if (parameter->name() == planned) ++named;
    EXPECT_EQ(named, 1);
}

TEST(M7_REV2_M4, AWidenedAgreementBandIsHonouredByValidationToo) {
    // Analysis used options.valueAgreementFraction; validation hard-coded the
    // global. A caller who widened the band had reconstruction reject the plan
    // it had just produced, blaming an edit that never happened.
    ReconstructionOptions options;
    options.valueAgreementFraction = 0.20;

    PartDocument document{"rev2m4"};
    const ObjectId sketchId = BuildDimensionedRectangle(document, 100.0, 50.0);
    // Stated 115 against 100 mm of geometry: outside the default 5%, inside 20%.
    // The dimension measures the REAL 100 mm edge but STATES 115: a 15%
    // disagreement, outside the default 5% band and inside the caller's 20%.
    std::vector<ImportedDimension2D> dimensions = WidthDimension(100.0);
    dimensions.front().statedValueMm = 115.0;
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketchId, dimensions, options);
    ASSERT_EQ(plan.parameters.size(), 1u) << "analysis refused a value its own band accepts";

    EXPECT_TRUE(ValidatePlanAgainstDocument(document, plan, options))
        << "validation used a different band than analysis";
    EXPECT_TRUE(ApplyReconstruction(document, plan, options));
}

TEST(M7_REV2_M5, TheAxisToleranceItselfIsPinnedAtItsBoundary) {
    // Round 2 replaced axisAngleToleranceRad with 0.6 rad -- a 600x widening
    // that would accept a 30-degree line as a Length -- and NO test failed:
    // all three C1 tests sit at 67 degrees, far outside any plausible band.
    // This pins the constant itself, from both sides.
    const double tolerance = kReconstructionAxisAngleToleranceRad;
    const auto refusedAt = [](double angleRad) {
        PartDocument document{"rev2m5"};
        Sketch& sketch = document.addSketch("s");
        const double length = 100.0;
        sketch.addLine(Vec2{0, 0}, Vec2{length * std::cos(angleRad), length * std::sin(angleRad)});
        ImportedDimension2D dimension;
        dimension.kind = ImportedDimensionKind::Linear;
        dimension.measureFrom = Vec2{0, 0};
        dimension.measureTo =
            Vec2{length * std::cos(angleRad), length * std::sin(angleRad)};
        dimension.statedValueMm = length;
        dimension.directionRad = 0.0; // stated along +u
        dimension.sourceHandle = "W1";
        const ReconstructionPlan plan =
            AnalyzeForReconstruction(document, sketch.id(), {dimension});
        return plan.parameters.empty();
    };
    EXPECT_FALSE(refusedAt(0.9 * tolerance)) << "a line inside the band was refused";
    EXPECT_TRUE(refusedAt(1.1 * tolerance)) << "a line outside the band was accepted as a Length";
}

} // namespace

// C2's regression test lives in tests/SerializationTests.cpp, inside
// GeneratorLimitTest. It has to advance the shared ObjectId generator past the
// cap to reproduce the defect, which poisons every test registered after it --
// putting it here failed 14 of them, reintroducing the exact bug it pins.
