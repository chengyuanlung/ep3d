// M17.5 -- sketch on a face, end to end, against real geometry.
//
// Every other test of this feature checks one link: the frame maths, the plane
// reader, the refusal messages. This one checks that the links form a chain.
// It pads a solid, reads a face off the RESULT, plans a sketch on it, draws,
// pads again -- and then asks the kernel where the material actually went.
//
// That last question is the whole point. A frame that is subtly wrong -- an
// origin at the face's centre instead of the part origin, a normal flipped by
// the orientation bug, axes swapped -- produces a sketch that looks perfectly
// correct on the 2D canvas and a solid somewhere nobody asked for. The centre
// of mass is the only witness that cannot be fooled by a plausible-looking
// drawing.

#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Kernel/EdgeQuery.h"
#include "Core/Kernel/FaceQuery.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Sketch/Sketch.h"
#include "Kernel/Occt/OcctFaceQuery.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Viewer/FaceSketch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>

namespace {

using namespace paramcad;

constexpr double kW = 40.0; // the base pad's rectangle, +u
constexpr double kH = 30.0; // +v
constexpr double kBase = 20.0; // how tall the base pad is

// A document with one 40 x 30 x 20 pad standing on the world XY plane.
struct BasePad {
    PartDocument document{"FaceSketchDoc"};
    OcctGeometryKernel kernel;
    PadFeature* pad = nullptr;

    BasePad() {
        document.setGeometryKernel(&kernel);
        Parameter& length = document.addParameter("PadLength", kBase, UnitType::Millimeter);
        Sketch& sketch = document.addSketch("Sketch001");
        // Unconstrained on purpose: this is a test about WHERE geometry lands,
        // and a constraint system between the frame and the answer would be a
        // second mechanism able to explain a failure.
        sketch.addLine(Vec2{0, 0}, Vec2{kW, 0});
        sketch.addLine(Vec2{kW, 0}, Vec2{kW, kH});
        sketch.addLine(Vec2{kW, kH}, Vec2{0, kH});
        sketch.addLine(Vec2{0, kH}, Vec2{0, 0});
        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    }
};

// The face of `shape` whose outward normal is `normal`, exactly as the viewer
// hands it on after a click.
//
// It RETURNS THE KERNEL'S OWN STRUCT. An earlier version copied it field by
// field into a separate PickedFace -- which is precisely the copy the viewer
// was making, and precisely where the boundary was being dropped. A test that
// reproduces the production copy cannot catch a bug in it, so there is one
// struct now and this simply passes it along.
std::optional<PickedFace> FaceFacing(const KernelShape& shape, Vec3 normal) {
    for (const FacePlane& face : FacesOf(shape)) {
        if (!face.planar) continue;
        if (std::fabs(face.normal.x - normal.x) > 1e-9 ||
            std::fabs(face.normal.y - normal.y) > 1e-9 ||
            std::fabs(face.normal.z - normal.z) > 1e-9)
            continue;
        return face;
    }
    return std::nullopt;
}

} // namespace

TEST(M17FaceSketch, M17_E2E_001_APadOnTheTopFaceStandsOnTopOfTheSolid) {
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(fx.pad->state(), ComputeState::Valid);

    // Click the top face -- read off the real solid, not asserted about.
    const std::optional<PickedFace> top = FaceFacing(fx.pad->currentShape(), Vec3{0, 0, 1});
    ASSERT_TRUE(top.has_value()) << "the pad has no upward face";

    const FaceSketchPlan plan = PlanSketchOnFace(*top);
    ASSERT_TRUE(plan.ok) << plan.message;

    // A 10 x 10 square in the corner of that face, padded 5 mm.
    Parameter& length = fx.document.addParameter("BossLength", 5.0, UnitType::Millimeter);
    Sketch& boss = fx.document.addSketch("Sketch002", plan.frame);
    boss.addLine(Vec2{0, 0}, Vec2{10, 0});
    boss.addLine(Vec2{10, 0}, Vec2{10, 10});
    boss.addLine(Vec2{10, 10}, Vec2{0, 10});
    boss.addLine(Vec2{0, 10}, Vec2{0, 0});
    PadFeature& second = fx.document.addPadFeature(*fx.document.bodies().front(), "Pad002",
                                                  boss.id(), length.id());
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(second.state(), ComputeState::Valid);

    const KernelMassPropertiesResult mass =
        fx.kernel.calculateMassProperties(second.currentShape());
    ASSERT_TRUE(mass) << mass.message;
    EXPECT_NEAR(mass.properties.volumeMm3, 10.0 * 10.0 * 5.0, 1e-6);

    // WHERE it is, which is the claim worth making. The square occupies
    // x in [0,10], y in [0,10], and z from the top of the base pad UPWARD --
    // a flipped normal would put the centre at z = 17.5 and the boss inside
    // the material, where it is invisible and looks like nothing happened.
    EXPECT_NEAR(mass.properties.centerOfMassMm.x, 5.0, 1e-6);
    EXPECT_NEAR(mass.properties.centerOfMassMm.y, 5.0, 1e-6);
    EXPECT_NEAR(mass.properties.centerOfMassMm.z, kBase + 2.5, 1e-6);
}

TEST(M17FaceSketch, M17_E2E_002_APadOnASideFaceGrowsSIDEWAYSOutOfTheSolid) {
    // The vertical case, where u and v are not world X and Y and every
    // convention in PlanSketchOnFace is load-bearing at once.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);

    const std::optional<PickedFace> right = FaceFacing(fx.pad->currentShape(), Vec3{1, 0, 0});
    ASSERT_TRUE(right.has_value()) << "the pad has no +X face";

    const FaceSketchPlan plan = PlanSketchOnFace(*right);
    ASSERT_TRUE(plan.ok) << plan.message;
    // On this face u is world +Y and v is world +Z (v points up, by the rule
    // in FaceSketch.h), and the origin is the part origin dropped onto the
    // plane -- so sketch (0,0) is world (40, 0, 0).
    Parameter& length = fx.document.addParameter("LugLength", 6.0, UnitType::Millimeter);
    Sketch& lug = fx.document.addSketch("Sketch002", plan.frame);
    lug.addLine(Vec2{0, 0}, Vec2{8, 0});
    lug.addLine(Vec2{8, 0}, Vec2{8, 4});
    lug.addLine(Vec2{8, 4}, Vec2{0, 4});
    lug.addLine(Vec2{0, 4}, Vec2{0, 0});
    PadFeature& second = fx.document.addPadFeature(*fx.document.bodies().front(), "Pad002",
                                                   lug.id(), length.id());
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(second.state(), ComputeState::Valid);

    const KernelMassPropertiesResult mass =
        fx.kernel.calculateMassProperties(second.currentShape());
    ASSERT_TRUE(mass) << mass.message;
    EXPECT_NEAR(mass.properties.volumeMm3, 8.0 * 4.0 * 6.0, 1e-6);
    // Out along +X from the face at x = 40, centred over the 8 x 4 rectangle
    // that starts at the corner of the plane.
    EXPECT_NEAR(mass.properties.centerOfMassMm.x, kW + 3.0, 1e-6);
    EXPECT_NEAR(mass.properties.centerOfMassMm.y, 4.0, 1e-6);
    EXPECT_NEAR(mass.properties.centerOfMassMm.z, 2.0, 1e-6);
}

TEST(M17FaceSketch, M17_E2E_003_TwoClicksOnTheSameFaceGiveTheSameSketchPlane) {
    // The stability claim from FaceSketchTest, made against a real face: the
    // frame must not depend on where the click landed. Here both "clicks" are
    // the same face, but the plane point OCCT reports is whatever the surface
    // carries -- so this also pins that the projection, not the reported
    // point, is what reaches the frame.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);

    const std::optional<PickedFace> top = FaceFacing(fx.pad->currentShape(), Vec3{0, 0, 1});
    ASSERT_TRUE(top.has_value());

    PickedFace elsewhere = *top;
    elsewhere.point = Vec3{top->point.x + 123.0, top->point.y - 45.0,
                           top->point.z}; // same plane, different point on it

    const FaceSketchPlan a = PlanSketchOnFace(*top);
    const FaceSketchPlan b = PlanSketchOnFace(elsewhere);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    const Vec3 pa = a.frame.toWorld(Vec2{3, 4});
    const Vec3 pb = b.frame.toWorld(Vec2{3, 4});
    EXPECT_NEAR(pa.x, pb.x, 1e-9);
    EXPECT_NEAR(pa.y, pb.y, 1e-9);
    EXPECT_NEAR(pa.z, pb.z, 1e-9);
}

TEST(M17FaceSketch, M17_E2E_004_ARealFacesBOUNDARYReachesTheSketchAsReferenceGeometry) {
    // THE test this feature was missing, and the one the owner found first by
    // running the program: sketching on a face projected nothing.
    //
    // Everything was individually correct. The kernel read all four edges of
    // the face. The projection turned a boundary into sketch geometry. The
    // canvas painted references. But the viewer copied the kernel's answer into
    // a SECOND struct field by field and left `boundary` behind, so what
    // reached the projection was always empty -- and no test crossed that seam,
    // because each side was tested with data the test itself built.
    //
    // This one starts from a REAL face of a REAL solid and follows it all the
    // way to the geometry a user would trace.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);

    const std::optional<PickedFace> top = FaceFacing(fx.pad->currentShape(), Vec3{0, 0, 1});
    ASSERT_TRUE(top.has_value());
    // The kernel found the edges...
    ASSERT_EQ(top->boundary.size(), 4u) << "the top face of a box has four edges";

    // ...and they survived the trip to the sketch.
    const FaceSketchPlan plan = PlanSketchOnFace(*top);
    ASSERT_TRUE(plan.ok) << plan.message;
    EXPECT_EQ(plan.reference.skipped, 0) << plan.reference.skippedReason;

    int lines = 0;
    int points = 0;
    for (const SketchGeometry& geometry : plan.reference.geometry) {
        if (std::holds_alternative<SketchLine>(geometry)) ++lines;
        if (std::holds_alternative<SketchPoint>(geometry)) ++points;
    }
    EXPECT_EQ(lines, 4);
    // Four corners, not eight: neighbouring edges share them.
    EXPECT_EQ(points, 4);

    // And they are in the right PLACE -- the face is 40 x 30, and the sketch
    // origin is the part origin dropped onto it, so the projected rectangle
    // runs (0,0) to (40,30) in sketch coordinates. A projection that forgot to
    // subtract the frame origin would put all four lines 20 mm out and still
    // draw a perfectly convincing rectangle.
    double maxU = 0.0;
    double maxV = 0.0;
    double minU = 0.0;
    double minV = 0.0;
    for (const SketchGeometry& geometry : plan.reference.geometry) {
        const auto* point = std::get_if<SketchPoint>(&geometry);
        if (point == nullptr) continue;
        maxU = std::max(maxU, point->position.x);
        maxV = std::max(maxV, point->position.y);
        minU = std::min(minU, point->position.x);
        minV = std::min(minV, point->position.y);
    }
    EXPECT_NEAR(minU, 0.0, 1e-6);
    EXPECT_NEAR(minV, 0.0, 1e-6);
    EXPECT_NEAR(maxU, kW, 1e-6);
    EXPECT_NEAR(maxV, kH, 1e-6);
}

TEST(M17FaceSketch, M17_E2E_005_AFaceWithAHoleProjectsTheHoleToo) {
    // A pad with a circular island cut out of nothing -- here, a face that has
    // an inner loop. The hole is what a user most wants to trace, because it is
    // what they line the next feature up with.
    PartDocument document{"HoleDoc"};
    OcctGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Parameter& length = document.addParameter("PadLength", 10.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    sketch.addLine(Vec2{40, 0}, Vec2{40, 30});
    sketch.addLine(Vec2{40, 30}, Vec2{0, 30});
    sketch.addLine(Vec2{0, 30}, Vec2{0, 0});
    sketch.addCircle(Vec2{20, 15}, 6.0); // the inner loop
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(pad.state(), ComputeState::Valid);

    const std::optional<PickedFace> top = FaceFacing(pad.currentShape(), Vec3{0, 0, 1});
    ASSERT_TRUE(top.has_value());
    const FaceSketchPlan plan = PlanSketchOnFace(*top);
    ASSERT_TRUE(plan.ok) << plan.message;

    int circles = 0;
    for (const SketchGeometry& geometry : plan.reference.geometry) {
        const auto* circle = std::get_if<SketchCircle>(&geometry);
        if (circle == nullptr) continue;
        ++circles;
        EXPECT_NEAR(circle->radiusMm, 6.0, 1e-6);
        EXPECT_NEAR(circle->center.x, 20.0, 1e-6);
        EXPECT_NEAR(circle->center.y, 15.0, 1e-6);
    }
    EXPECT_EQ(circles, 1) << plan.reference.skippedReason;
}

TEST(M17FaceSketch, M17_E2E_006_APocketFromAFaceSketchCutsInwardWithANegativeDepth) {
    // The owner's report, reproduced: a circle sketched on a face pads fine and
    // pockets nothing.
    //
    // Both halves were individually right. A sketch on a face has its normal
    // pointing OUT of the solid, so a pad grows away from the part
    // (ADR-M17-028). A pocket extrudes its tool along the SAME +normal
    // (ADR-M8-002), which from that plane puts the tool entirely outside the
    // material -- so the boolean succeeds, removes nothing, and reports Valid.
    //
    // A negative depth is the direct way to say "the other way" (ADR-M17-031),
    // and this test pins both outcomes: nothing removed one way, a real hole
    // the other.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const double solidVolume = kW * kH * kBase;

    const std::optional<PickedFace> top = FaceFacing(fx.pad->currentShape(), Vec3{0, 0, 1});
    ASSERT_TRUE(top.has_value());
    const FaceSketchPlan plan = PlanSketchOnFace(*top);
    ASSERT_TRUE(plan.ok) << plan.message;

    Sketch& hole = fx.document.addSketch("Sketch002", plan.frame);
    hole.addCircle(Vec2{20, 15}, 6.0);
    Parameter& depth = fx.document.addParameter("PocketDepth", 5.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    PocketFeature& pocket =
        fx.document.addPocketFeature(body, "Pocket001", fx.pad->id(), hole.id(), depth.id());

    // POSITIVE depth: legal, computes, and removes nothing -- the tool is above
    // the face. This is what the owner saw.
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(pocket.state(), ComputeState::Valid);
    KernelMassPropertiesResult mass = fx.kernel.calculateMassProperties(pocket.currentShape());
    ASSERT_TRUE(mass) << mass.message;
    EXPECT_NEAR(mass.properties.volumeMm3, solidVolume, solidVolume * 1e-9)
        << "a pocket built outside the solid removed material it could not reach";

    // NEGATIVE depth: cuts into the face, and the hole is exactly the size the
    // circle describes.
    ASSERT_TRUE(fx.document.setParameterValue(depth.id(), -5.0));
    ASSERT_TRUE(fx.document.recompute().success) << "a negative pocket depth was refused";
    EXPECT_EQ(pocket.state(), ComputeState::Valid);
    mass = fx.kernel.calculateMassProperties(pocket.currentShape());
    ASSERT_TRUE(mass) << mass.message;

    const double removed = 3.14159265358979323846 * 6.0 * 6.0 * 5.0;
    EXPECT_NEAR(mass.properties.volumeMm3, solidVolume - removed, 1e-6)
        << "the inward pocket did not remove a cylinder of the sketched circle";
}

// --- M17.12: a query survives the rebuild an index would not -----------------

TEST(M17EdgeSelection, M17_QUERY_001_TheTopFacesFilletIsSTILLOnTopAfterThePartGrows) {
    // THE reason the selection is a query and not an edge.
    //
    // Round the top face's edges on a 20 mm-tall pad, then make the pad 40 mm
    // tall. An index would now point at whatever happens to be seventh in the
    // taller solid's edge list -- a different edge, or none. A sentence about
    // the top face still means the top face.
    //
    // The check is the CENTRE OF MASS, not a count: top and bottom both have
    // four edges, so counting cannot tell a fillet that stayed on top from one
    // that slid to the bottom. Material missing from the top pulls the centre
    // of mass below the middle, and nothing else does.
    BasePad fx;
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
    fx.document.setFeatureEdgeSelection(fillet.id(), EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, 1}}});

    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(fillet.state(), ComputeState::Valid);
    KernelMassPropertiesResult mass = fx.kernel.calculateMassProperties(fillet.currentShape());
    ASSERT_TRUE(mass) << mass.message;
    EXPECT_LT(mass.properties.centerOfMassMm.z, kBase / 2.0)
        << "the fillet is not on the top face to begin with";

    // Twice as tall. Every edge in the solid is rebuilt.
    const Parameter* length = fx.document.parameters().findByName("PadLength");
    ASSERT_NE(length, nullptr);
    ASSERT_TRUE(fx.document.setParameterValue(length->id(), kBase * 2.0));
    ASSERT_TRUE(fx.document.recompute().success) << "the fillet did not survive the rebuild";
    EXPECT_EQ(fillet.state(), ComputeState::Valid);

    mass = fx.kernel.calculateMassProperties(fillet.currentShape());
    ASSERT_TRUE(mass) << mass.message;
    // Still lighter at the top, in the TALLER part.
    EXPECT_LT(mass.properties.centerOfMassMm.z, kBase)
        << "after the rebuild the fillet is no longer on the top face";
    // And the part really did grow -- otherwise the check above would pass on a
    // rebuild that never happened.
    EXPECT_NEAR(mass.properties.volumeMm3, kW * kH * kBase * 2.0, kW * kH * kBase * 0.02);
}

TEST(M17EdgeSelection, M17_QUERY_002_ADressWithNoMatchingEdgeFAILSAndNamesTheSelection) {
    // A fillet whose selection matches nothing returns the solid unchanged.
    // Reporting Valid would leave the user with a feature in the tree, a
    // radius in the panel, and no fillet -- and nothing to read.
    BasePad fx;
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
    // No face of a box faces the (1,1,1) diagonal.
    const double s = 1.0 / std::sqrt(3.0);
    fx.document.setFeatureEdgeSelection(fillet.id(), EdgeSelection{EdgesOfExtremeFace{Vec3{s, s, s}}});

    EXPECT_FALSE(fx.document.recompute().success);
    EXPECT_EQ(fillet.state(), ComputeState::Failed);
}

TEST(M17EdgeSelection, M17_QUERY_003_TheDefaultIsEveryEdgeSoNothingOldChangesShape) {
    // Every fillet in every file written before selections existed says
    // nothing about edges, and must go on rounding all of them.
    BasePad fx;
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
    EXPECT_TRUE(IsAllEdges(fillet.edgeSelection()));

    ASSERT_TRUE(fx.document.recompute().success);
    const KernelMassPropertiesResult mass =
        fx.kernel.calculateMassProperties(fillet.currentShape());
    ASSERT_TRUE(mass) << mass.message;
    // Symmetric in z, because every edge was rounded -- which a top-only
    // selection is not.
    EXPECT_NEAR(mass.properties.centerOfMassMm.z, kBase / 2.0, 1e-6);
}

// --- M17.13: CreatedBy, the query that names an inner face -------------------

TEST(M17EdgeSelection, M17_QUERY_010_APocketsOwnEdgesCanBeSelectedAtAll) {
    // Before provenance there was no sentence for this. "The outermost face
    // towards +Z" is the TOP face; a pocket floor also faces +Z and is not
    // outermost, so a pocket's own edges could not be named -- and rounding
    // the inside of a pocket is one of the most ordinary things anybody does.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // A pocket cut into the top face.
    const std::optional<PickedFace> top = FaceFacing(fx.pad->currentShape(), Vec3{0, 0, 1});
    ASSERT_TRUE(top.has_value());
    const FaceSketchPlan plan = PlanSketchOnFace(*top);
    ASSERT_TRUE(plan.ok) << plan.message;
    Sketch& hole = fx.document.addSketch("Sketch002", plan.frame);
    hole.addLine(Vec2{10, 8}, Vec2{30, 8});
    hole.addLine(Vec2{30, 8}, Vec2{30, 22});
    hole.addLine(Vec2{30, 22}, Vec2{10, 22});
    hole.addLine(Vec2{10, 22}, Vec2{10, 8});
    Parameter& depth = fx.document.addParameter("PocketDepth", -6.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    PocketFeature& pocket =
        fx.document.addPocketFeature(body, "Pocket001", fx.pad->id(), hole.id(), depth.id());
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(pocket.state(), ComputeState::Valid);
    const double pocketed =
        fx.kernel.calculateMassProperties(pocket.currentShape()).properties.volumeMm3;

    // Round what the POCKET made -- named by provenance, which is the only
    // vocabulary that can reach an inner face.
    Parameter& radius = fx.document.addParameter("FilletRadius", 1.5, UnitType::Millimeter);
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", pocket.id(), radius.id());
    fx.document.setFeatureEdgeSelection(fillet.id(), EdgeSelection{EdgesCreatedBy{pocket.id()}});

    ASSERT_TRUE(fx.document.recompute().success) << "the pocket's own edges could not be rounded";
    EXPECT_EQ(fillet.state(), ComputeState::Valid);

    const auto volumeNow = [&]() {
        const KernelMassPropertiesResult mass =
            fx.kernel.calculateMassProperties(fillet.currentShape());
        EXPECT_TRUE(mass) << mass.message;
        return mass ? mass.properties.volumeMm3 : 0.0;
    };
    const double roundedPocket = volumeNow();
    EXPECT_NE(roundedPocket, pocketed) << "the fillet did nothing at all";

    // THE discriminating check, and it needs three answers rather than a sign.
    //
    // A pocket's rim is convex and its inside corners are concave, so rounding
    // its edges removes material at the rim and adds it in the corners -- the
    // NET can land either way, and asserting a direction would be asserting
    // this particular geometry rather than the query.
    //
    // What no wrong answer can produce is three DIFFERENT solids. If provenance
    // were ignored and every query fell through to "everything", all three of
    // these would be identical.
    fx.document.setFeatureEdgeSelection(fillet.id(), EdgeSelection{EdgesCreatedBy{fx.pad->id()}});
    ASSERT_TRUE(fx.document.recompute().success);
    const double roundedPad = volumeNow();

    fx.document.setFeatureEdgeSelection(fillet.id(), AllEdgesSelection());
    ASSERT_TRUE(fx.document.recompute().success);
    const double roundedEverything = volumeNow();

    EXPECT_NE(roundedPocket, roundedPad) << "the pocket's edges and the pad's are the same set";
    EXPECT_NE(roundedPocket, roundedEverything) << "the pocket's edges are every edge";
    EXPECT_NE(roundedPad, roundedEverything) << "the pad's edges are every edge";
    // Rounding the pad's own edges is pure convex removal, so THAT one has a
    // direction worth pinning.
    EXPECT_LT(roundedPad, pocketed) << "rounding the block's outer edges added material";
}

TEST(M17EdgeSelection, M17_QUERY_011_CreatedByFollowsTheFeatureWhenItsGeometryMOVES) {
    // The property no other query has. "The outermost face towards +Z" is a
    // shape; "what Pad001 created" is a HISTORY, and a history does not stop
    // being true when the geometry moves. Here the pad doubles in height and
    // the selection still names the same feature's work.
    BasePad fx;
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
    fx.document.setFeatureEdgeSelection(fillet.id(), EdgeSelection{EdgesCreatedBy{fx.pad->id()}});

    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(fillet.state(), ComputeState::Valid);

    const Parameter* length = fx.document.parameters().findByName("PadLength");
    ASSERT_NE(length, nullptr);
    ASSERT_TRUE(fx.document.setParameterValue(length->id(), kBase * 2.0));
    ASSERT_TRUE(fx.document.recompute().success)
        << "the CreatedBy selection did not survive the rebuild";
    EXPECT_EQ(fillet.state(), ComputeState::Valid);

    const KernelMassPropertiesResult mass =
        fx.kernel.calculateMassProperties(fillet.currentShape());
    ASSERT_TRUE(mass) << mass.message;
    // Every edge of the pad is the pad's, so this rounds all twelve -- the part
    // is symmetric in z and the volume is below the raw block.
    EXPECT_NEAR(mass.properties.centerOfMassMm.z, kBase, 1e-6);
    EXPECT_LT(mass.properties.volumeMm3, kW * kH * kBase * 2.0);
}

TEST(M17EdgeSelection, M17_QUERY_012_NamingAFeatureThatMadeNothingHereFAILSLoudly) {
    // A selection that matches nothing returns the solid unchanged, and
    // reporting success would leave a fillet in the tree that did not happen.
    BasePad fx;
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
    // An id that made nothing in this shape.
    fx.document.setFeatureEdgeSelection(fillet.id(), EdgeSelection{EdgesCreatedBy{999999}});

    EXPECT_FALSE(fx.document.recompute().success);
    EXPECT_EQ(fillet.state(), ComputeState::Failed);
}

// --- M17.14: a sketch that FOLLOWS the face it was drawn on ------------------

TEST(M17TrackedFace, M17_TRACK_001_TheSketchRidesUpWhenThePadGetsTaller) {
    // THE point of the whole query architecture, felt by a user for the first
    // time here. Until now a face sketch was frozen on the plane the face
    // happened to occupy (ADR-M17-028): make the pad taller and the sketch
    // stayed behind, buried inside the solid it was drawn on top of.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);

    Sketch& boss = fx.document.addSketch("Sketch002");
    ASSERT_TRUE(fx.document.setSketchTrackedFace(
        boss.id(), FaceQuery{fx.pad->id(), Vec3{0, 0, 1}, std::nullopt}));
    ASSERT_TRUE(fx.document.recompute().success) << "the tracked face could not be resolved";

    // On the pad's top face, at z = 20.
    EXPECT_NEAR(boss.frame().toWorld(Vec2{0, 0}).z, kBase, 1e-6);

    // Twice as tall...
    const Parameter* length = fx.document.parameters().findByName("PadLength");
    ASSERT_NE(length, nullptr);
    ASSERT_TRUE(fx.document.setParameterValue(length->id(), kBase * 2.0));
    ASSERT_TRUE(fx.document.recompute().success);

    // ...and the sketch is on the NEW top face. A frozen plane would still say
    // 20, and the sketch would now be inside the material.
    EXPECT_NEAR(boss.frame().toWorld(Vec2{0, 0}).z, kBase * 2.0, 1e-6)
        << "the sketch did not follow the face it was drawn on";
    // Still the same plane orientation -- following a face must not spin it.
    EXPECT_NEAR(boss.frame().normal().z, 1.0, 1e-9);
    EXPECT_NEAR(boss.frame().uAxis().x, 1.0, 1e-9);
}

TEST(M17TrackedFace, M17_TRACK_002_AFeatureBuiltOnTheTrackedSketchMovesWithIt) {
    // The half that proves the ORDER is right. The sketch is re-resolved
    // before it solves and before anything downstream reads it, so a pad built
    // on the tracked sketch lands on the new face -- not on last recompute's
    // plane, which nothing downstream could have detected.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);

    Sketch& boss = fx.document.addSketch("Sketch002");
    ASSERT_TRUE(fx.document.setSketchTrackedFace(
        boss.id(), FaceQuery{fx.pad->id(), Vec3{0, 0, 1}, std::nullopt}));
    boss.addLine(Vec2{0, 0}, Vec2{10, 0});
    boss.addLine(Vec2{10, 0}, Vec2{10, 10});
    boss.addLine(Vec2{10, 10}, Vec2{0, 10});
    boss.addLine(Vec2{0, 10}, Vec2{0, 0});
    Parameter& bossLength = fx.document.addParameter("BossLength", 5.0, UnitType::Millimeter);
    PadFeature& second = fx.document.addPadFeature(*fx.document.bodies().front(), "Pad002",
                                                   boss.id(), bossLength.id());
    ASSERT_TRUE(fx.document.recompute().success);

    const auto bossCentreZ = [&]() {
        const KernelMassPropertiesResult mass =
            fx.kernel.calculateMassProperties(second.currentShape());
        EXPECT_TRUE(mass) << mass.message;
        return mass ? mass.properties.centerOfMassMm.z : 0.0;
    };
    EXPECT_NEAR(bossCentreZ(), kBase + 2.5, 1e-6);

    const Parameter* length = fx.document.parameters().findByName("PadLength");
    ASSERT_NE(length, nullptr);
    ASSERT_TRUE(fx.document.setParameterValue(length->id(), kBase * 2.0));
    ASSERT_TRUE(fx.document.recompute().success);

    // The boss sits on the taller pad's top. A stale plane would leave it at
    // 22.5, buried 17.5 mm inside the part.
    EXPECT_NEAR(bossCentreZ(), kBase * 2.0 + 2.5, 1e-6)
        << "the feature built on the tracked sketch stayed on the old plane";
}

TEST(M17TrackedFace, M17_TRACK_003_ALostFaceFAILSLoudlyInsteadOfKeepingTheOldPlane) {
    // The topological-naming failure, made visible. Keeping the last known
    // plane and carrying on is geometry sitting somewhere the model no longer
    // claims it belongs -- and nothing anywhere would say so.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);

    Sketch& boss = fx.document.addSketch("Sketch002");
    // A direction no face of the block faces.
    const double s = 1.0 / std::sqrt(3.0);
    ASSERT_TRUE(fx.document.setSketchTrackedFace(
        boss.id(), FaceQuery{fx.pad->id(), Vec3{s, s, s}, std::nullopt}));

    EXPECT_FALSE(fx.document.recompute().success);
    EXPECT_EQ(boss.solveStatus(), SketchSolveStatus::InvalidInput);
    EXPECT_FALSE(boss.trackedFaceMessage().empty());
}

TEST(M17TrackedFace, M17_TRACK_004_TrackingIsREFUSEDWhenItWouldCloseACycle) {
    // A sketch tracking a face of the very feature built from it. Accepting it
    // would make the recompute order undefined -- and the graph is the only
    // thing that can say so, which is why the edge is added BEFORE the query.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);
    // fx.pad is built FROM the base sketch, so the base sketch may not track
    // one of the pad's own faces.
    const Sketch* base = fx.document.sketches().front();
    ASSERT_NE(base, nullptr);
    EXPECT_FALSE(fx.document.setSketchTrackedFace(
        base->id(), FaceQuery{fx.pad->id(), Vec3{0, 0, 1}, std::nullopt}));
    EXPECT_FALSE(base->trackedFace().has_value())
        << "a refused track was stored anyway";
}

TEST(M17TrackedFace, M17_TRACK_005_AnUntrackedSketchIsUNCHANGEDByAllOfThis) {
    // Every sketch on a world plane, and every sketch made before M17.14. The
    // frozen frame is still the whole story for them, and a rebuild must not
    // move one.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const Sketch* base = fx.document.sketches().front();
    ASSERT_NE(base, nullptr);
    EXPECT_FALSE(base->trackedFace().has_value());

    const Vec3 before = base->frame().toWorld(Vec2{3, 4});
    const Parameter* length = fx.document.parameters().findByName("PadLength");
    ASSERT_NE(length, nullptr);
    ASSERT_TRUE(fx.document.setParameterValue(length->id(), kBase * 3.0));
    ASSERT_TRUE(fx.document.recompute().success);

    const Vec3 after = base->frame().toWorld(Vec2{3, 4});
    EXPECT_NEAR(after.x, before.x, 1e-9);
    EXPECT_NEAR(after.y, before.y, 1e-9);
    EXPECT_NEAR(after.z, before.z, 1e-9);
}

// --- M17.15: what survives a delete, and what the user is told ---------------

TEST(M17EdgeSelection, M17_UNDO_001_UndoingAFilletsDeletionBringsBackITSEdges) {
    // FeatureSnapshot has two producers -- SnapshotFeature here, and the JSON
    // loader -- and `edgeSelection` reached only one of them. Undo brought the
    // fillet back dressing EVERY edge instead of the face the user had chosen:
    // the shape changed, and nothing anywhere said so.
    BasePad fx;
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
    ASSERT_TRUE(fx.document.setFeatureEdgeSelection(
        fillet.id(), EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, 1}}}));
    ASSERT_TRUE(fx.document.recompute().success);
    const double chosen =
        fx.kernel.calculateMassProperties(fillet.currentShape()).properties.volumeMm3;

    const ObjectId filletId = fillet.id();
    ASSERT_TRUE(fx.document.removeObject(filletId));
    ASSERT_TRUE(fx.document.undo()) << "deleting an unconsumed fillet was not undoable";
    ASSERT_TRUE(fx.document.recompute().success);

    const FilletFeature* restored = nullptr;
    for (const auto& feature : fx.document.bodies().front()->features())
        if (const auto* f = dynamic_cast<const FilletFeature*>(feature.get())) restored = f;
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->edgeSelection().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<EdgesOfExtremeFace>(restored->edgeSelection().front()))
        << "undo brought the fillet back with a different selection";
    // And the SOLID is the one the user had, which is the claim that matters:
    // every-edge and top-face-only are different volumes.
    EXPECT_NEAR(fx.kernel.calculateMassProperties(restored->currentShape()).properties.volumeMm3,
                chosen, 1e-6);
}

TEST(M17EdgeSelection, M17_UNDO_002_DeletingAConsumedFeatureCLEARSTheHistory) {
    // The behaviour the shell was not telling anyone about. Core is explicit
    // that it clears rather than offer an undo that would do the wrong thing --
    // and that the clearing is observable SO THAT a UI can say so.
    BasePad fx;
    Parameter& depth = fx.document.addParameter("PocketDepth", -5.0, UnitType::Millimeter);
    Sketch& hole = fx.document.addSketch("Sketch002");
    hole.addCircle(Vec2{20, 15}, 5.0);
    Body& body = *fx.document.bodies().front();
    fx.document.addPocketFeature(body, "Pocket001", fx.pad->id(), hole.id(), depth.id());
    ASSERT_TRUE(fx.document.recompute().success);

    // Something to lose.
    const Parameter* length = fx.document.parameters().findByName("PadLength");
    ASSERT_NE(length, nullptr);
    ASSERT_TRUE(fx.document.setParameterValue(length->id(), 25.0));
    ASSERT_GT(fx.document.undoDepth(), 0u);

    // The PAD is consumed by the pocket, so removing it cannot be replayed.
    ASSERT_TRUE(fx.document.removeObject(fx.pad->id()));
    EXPECT_EQ(fx.document.undoDepth(), 0u)
        << "a consumed feature's removal left a history that cannot replay it";
}

TEST(M17EdgeSelection, M17_UNDO_003_DeletingAParameterIsUndoable) {
    // The other side of the same question, so the rule is pinned in both
    // directions rather than only where it says no.
    BasePad fx;
    ASSERT_TRUE(fx.document.recompute().success);
    Parameter& spare = fx.document.addParameter("Spare", 3.0, UnitType::Millimeter);
    const ObjectId spareId = spare.id();
    const std::size_t before = fx.document.undoDepth();

    ASSERT_TRUE(fx.document.removeObject(spareId));
    EXPECT_GT(fx.document.undoDepth(), before) << "deleting a parameter recorded nothing";
    ASSERT_TRUE(fx.document.undo());
    EXPECT_NE(fx.document.parameters().findById(spareId), nullptr);
}
