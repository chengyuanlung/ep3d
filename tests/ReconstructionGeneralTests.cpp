// M7.2 — general Horizontal / Vertical / Coincident recognition (spec 11, 38).
//
// M7.1 recognised ONE named shape. These tests are about the geometry that
// shape never saw: junctions where three curves meet, open chains, L-shapes,
// triangles, arcs, and the degenerate cases where a rule must decline rather
// than pick.
//
// Every expected constraint count and every degree-of-freedom figure is counted
// by hand from the fixture and written in the test that uses it.

#include "Core/Document/PartDocument.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Sketch/Sketch.h"
#include <gtest/gtest.h>
#include <cmath>
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

// --- Junctions: the case a pairwise matcher gets wrong ----------------------

TEST(M7General, ThreeCurvesMeetingAtAPointGetASpanningSetNotEveryPair) {
    PartDocument document{"M7Tee"};
    Sketch& sketch = document.addSketch("Imported");
    // Three lines radiating from (0,0). A T-junction, which is ordinary in any
    // real drawing and which M7.1's rectangle rule refused outright.
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addLine(Vec2{0, 0}, Vec2{0, 40});
    sketch.addLine(Vec2{0, 0}, Vec2{-30, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    // THREE endpoints in one cluster need TWO constraints, not three.
    // All-pairs would add a third saying what the first two already say, and
    // its residuals are redundant -- an ordinary junction reported as
    // OverConstrained.
    EXPECT_EQ(CountKind(plan, "Coincident"), 2u);
}

TEST(M7General, AFourWayJunctionScalesTheSameWay) {
    PartDocument document{"M7Cross"};
    Sketch& sketch = document.addSketch("Imported");
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addLine(Vec2{0, 0}, Vec2{0, 40});
    sketch.addLine(Vec2{0, 0}, Vec2{-30, 0});
    sketch.addLine(Vec2{0, 0}, Vec2{0, -20});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    // n-1, not n(n-1)/2. Four endpoints: 3, not 6. The parallel fixture to the
    // three-way case, because a rule that is right for one size of cluster and
    // wrong for the next is exactly the shape this project keeps finding.
    EXPECT_EQ(CountKind(plan, "Coincident"), 3u);
}

// --- Shapes that are not rectangles -----------------------------------------

TEST(M7General, AnLShapeGetsEveryAxisAndCornerConstraint) {
    PartDocument document{"M7L"};
    Sketch& sketch = document.addSketch("Imported");
    // Six axis-aligned sides, closed. M7.1 refused this because it was not
    // four lines; M7.2 must handle it, because an L bracket is as ordinary a
    // part as a plate.
    sketch.addLine(Vec2{0, 0}, Vec2{60, 0});
    sketch.addLine(Vec2{60, 0}, Vec2{60, 20});
    sketch.addLine(Vec2{60, 20}, Vec2{20, 20});
    sketch.addLine(Vec2{20, 20}, Vec2{20, 50});
    sketch.addLine(Vec2{20, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    EXPECT_EQ(CountKind(plan, "Coincident"), 6u);
    EXPECT_EQ(CountKind(plan, "Horizontal"), 3u);
    EXPECT_EQ(CountKind(plan, "Vertical"), 3u);
    EXPECT_EQ(CountKind(plan, "Fix"), 1u);

    // 6 lines x 4 scalars = 24 variables.
    // 6 Coincident x 2 + 3 Horizontal + 3 Vertical + 1 Fix x 2 = 20 residuals.
    // DOF = 4, which is the two remaining lengths of each leg -- counted here
    // by hand, and measured through the solver in the integration suite.
    EXPECT_EQ(plan.constraints.size(), 13u);
}

TEST(M7General, ATriangleGetsItsCornersAndOnlyTheAxisItActuallyHas) {
    PartDocument document{"M7Triangle"};
    Sketch& sketch = document.addSketch("Imported");
    sketch.addLine(Vec2{0, 0}, Vec2{60, 0});   // horizontal
    sketch.addLine(Vec2{60, 0}, Vec2{30, 40}); // sloped
    sketch.addLine(Vec2{30, 40}, Vec2{0, 0});  // sloped

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    EXPECT_EQ(CountKind(plan, "Coincident"), 3u);
    EXPECT_EQ(CountKind(plan, "Horizontal"), 1u);
    // The two sloped sides get NOTHING. Forcing them onto an axis is the
    // "confidently wrong" spec 18 forbids, and a rule that fired here would
    // flatten every triangle in every drawing.
    EXPECT_EQ(CountKind(plan, "Vertical"), 0u);
}

TEST(M7General, AnOpenChainStillGetsItsRealJoints) {
    PartDocument document{"M7Open"};
    Sketch& sketch = document.addSketch("Imported");
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addLine(Vec2{50, 0}, Vec2{50, 30});
    sketch.addLine(Vec2{50, 30}, Vec2{10, 30});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    // Two joints, not three: the chain is open, and inventing the closing one
    // would be reconstructing geometry the drawing does not contain. M7.1
    // returned nothing at all here.
    EXPECT_EQ(CountKind(plan, "Coincident"), 2u);
    EXPECT_EQ(CountKind(plan, "Horizontal"), 2u);
    EXPECT_EQ(CountKind(plan, "Vertical"), 1u);
}

// --- Arcs (spec 11: "two semantically relevant endpoints") ------------------

TEST(M7General, AnArcsEndpointsJoinLikeAnyOthers) {
    PartDocument document{"M7Rounded"};
    Sketch& sketch = document.addSketch("Imported");
    // A line into a quarter arc and out again -- a rounded corner. The arc runs
    // from (50,0) round to (60,10), centred at (50,10) with radius 10.
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addArc(Vec2{50, 10}, 10.0, -kPi / 2.0, 0.0, true);
    sketch.addLine(Vec2{60, 10}, Vec2{60, 40});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    // Both joints found. Leaving arcs out of the clustering would silently
    // refuse to close any profile with a rounded corner in it -- which is most
    // real sheet-metal and bracket geometry.
    EXPECT_EQ(CountKind(plan, "Coincident"), 2u);
    // ...but the arc gets no Horizontal or Vertical. Those are line-only in the
    // model, and the solver rejects them on anything else, so proposing one
    // would produce InvalidInput rather than a constraint.
    EXPECT_EQ(CountKind(plan, "Horizontal"), 1u);
    EXPECT_EQ(CountKind(plan, "Vertical"), 1u);
}

TEST(M7General, ACircleContributesNoCoincidence) {
    PartDocument document{"M7Circle"};
    Sketch& sketch = document.addSketch("Imported");
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addCircle(Vec2{0, 0}, 10.0); // centred exactly on the line's start

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    // A circle has no endpoints, so it joins nothing -- even sitting exactly on
    // one. Treating a centre as an endpoint would tie the circle to the line
    // and silently change what the drawing means.
    EXPECT_EQ(CountKind(plan, "Coincident"), 0u);
}

// --- Declining rather than picking ------------------------------------------

TEST(M7General, ALineInsideBothAxisTolerancesGetsNeither) {
    PartDocument document{"M7Tiny"};
    Sketch& sketch = document.addSketch("Imported");
    // Short enough that its direction is below the angular resolution: both
    // IsHorizontal and IsVertical are true. Asserting either would be a coin
    // toss; asserting both would be a contradiction the solver reports as
    // Conflicting on geometry whose only sin is being small.
    const double tiny = 2.0 * kMinSketchDimensionMm;
    sketch.addLine(Vec2{0, 0}, Vec2{tiny, tiny});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    EXPECT_EQ(CountKind(plan, "Horizontal"), 0u);
    EXPECT_EQ(CountKind(plan, "Vertical"), 0u);
}

TEST(M7General, AClosedCurveIsNotMadeCoincidentWithItself) {
    PartDocument document{"M7FullArc"};
    Sketch& sketch = document.addSketch("Imported");
    // An arc sweeping very nearly the whole turn: its two endpoints land in one
    // cluster, and they belong to the SAME entity. Constraining it to itself
    // says nothing and gives the solver a residual that is always zero.
    sketch.addArc(Vec2{0, 0}, 10.0, 0.0, 2.0 * kPi - 1e-9, true);

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    EXPECT_EQ(CountKind(plan, "Coincident"), 0u);
}

TEST(M7General, TwoSeparateShapesEachKeepTheirOwnJoints) {
    PartDocument document{"M7TwoShapes"};
    Sketch& sketch = document.addSketch("Imported");
    // Two triangles far apart. Nothing may join across them, and exactly one
    // Fix is placed for the whole sketch.
    sketch.addLine(Vec2{0, 0}, Vec2{10, 0});
    sketch.addLine(Vec2{10, 0}, Vec2{5, 8});
    sketch.addLine(Vec2{5, 8}, Vec2{0, 0});
    sketch.addLine(Vec2{500, 500}, Vec2{510, 500});
    sketch.addLine(Vec2{510, 500}, Vec2{505, 508});
    sketch.addLine(Vec2{505, 508}, Vec2{500, 500});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    EXPECT_EQ(CountKind(plan, "Coincident"), 6u);
    EXPECT_EQ(CountKind(plan, "Horizontal"), 2u);
    // ONE Fix, not one per shape. A second would be a second placement policy,
    // and the far shape stays free to be positioned by dimensions.
    EXPECT_EQ(CountKind(plan, "Fix"), 1u);
}

// --- Determinism ------------------------------------------------------------

TEST(M7General, TheSameShapeDrawnInADifferentOrderPlansTheSameFix) {
    // The Fix must land on the same POINT regardless of the order the entities
    // were created in, because entity ids are handed out in file order and a
    // Fix that moved would relocate the whole part between two imports of one
    // drawing.
    PartDocument a{"M7OrderA"};
    Sketch& sa = a.addSketch("A");
    sa.addLine(Vec2{0, 0}, Vec2{60, 0});
    sa.addLine(Vec2{60, 0}, Vec2{60, 40});
    sa.addLine(Vec2{60, 40}, Vec2{0, 40});
    sa.addLine(Vec2{0, 40}, Vec2{0, 0});

    PartDocument b{"M7OrderB"};
    Sketch& sb = b.addSketch("B");
    sb.addLine(Vec2{60, 40}, Vec2{0, 40});
    sb.addLine(Vec2{0, 40}, Vec2{0, 0});
    sb.addLine(Vec2{0, 0}, Vec2{60, 0});
    sb.addLine(Vec2{60, 0}, Vec2{60, 40});

    const ReconstructionPlan planA = AnalyzeForReconstruction(a, sa.id(), {});
    const ReconstructionPlan planB = AnalyzeForReconstruction(b, sb.id(), {});

    const auto fixedPoint = [](const ReconstructionPlan& plan, const Sketch& sketch) {
        for (const PlannedConstraint& constraint : plan.constraints) {
            const auto* fix = std::get_if<FixConstraint>(&constraint.data);
            if (fix == nullptr) continue;
            const SketchEntity* entity = sketch.findEntity(fix->target.entityId);
            EXPECT_NE(entity, nullptr);
            return fix->target.subElement == SketchSubElement::StartPoint
                       ? StartPointOf(entity->geometry)
                       : EndPointOf(entity->geometry);
        }
        return Vec2{-1, -1};
    };

    const Vec2 pa = fixedPoint(planA, sa);
    const Vec2 pb = fixedPoint(planB, sb);
    EXPECT_DOUBLE_EQ(pa.x, pb.x);
    EXPECT_DOUBLE_EQ(pa.y, pb.y);
    // And it is the corner the policy names: the smallest (u,v) (ADR-M7-008).
    EXPECT_DOUBLE_EQ(pa.x, 0.0);
    EXPECT_DOUBLE_EQ(pa.y, 0.0);
}

TEST(M7General, TheRectangleStillGetsExactlyWhatM7_1GaveIt) {
    // The named-shape rule is gone; the general rules replace it. The release
    // fixture's design intent must be unchanged, or M7.2 has quietly altered
    // what M7.1's gates were proving.
    PartDocument document{"M7StillRect"};
    Sketch& sketch = document.addSketch("Imported");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    EXPECT_EQ(CountKind(plan, "Coincident"), 4u);
    EXPECT_EQ(CountKind(plan, "Horizontal"), 2u);
    EXPECT_EQ(CountKind(plan, "Vertical"), 2u);
    EXPECT_EQ(CountKind(plan, "Fix"), 1u);
}

} // namespace
