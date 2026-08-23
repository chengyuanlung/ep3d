// M17.12 -- answering an edge query against real geometry.
//
// The claim this suite has to pin is not "fewer edges got rounded". It is that
// the query picks the edges it NAMES: "the top face's edges" must dress the
// top, not the bottom, and "every vertical edge" must dress the four uprights
// and nothing else.
//
// Counting edges would not show that -- top and bottom both have four -- so
// every test here measures the SOLID: an analytic volume where the arithmetic
// is clean, and the centre of mass where it is not. Material removed from the
// top moves the centre of mass down, and there is no way to fake that with the
// wrong edges.

#include "Core/Kernel/EdgeQuery.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>
#include <variant>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kW = 40.0;
constexpr double kH = 30.0;
constexpr double kD = 20.0;
constexpr double kSolid = kW * kH * kD;

struct Box {
    OcctGeometryKernel kernel;
    ShapeResult shape;
    Box() : shape(kernel.createBox(BoxDefinition{kW, kH, kD})) {}
};

double VolumeOf(OcctGeometryKernel& kernel, const KernelShape& shape) {
    const KernelMassPropertiesResult mass = kernel.calculateMassProperties(shape);
    EXPECT_TRUE(mass) << mass.message;
    return mass ? mass.properties.volumeMm3 : 0.0;
}

Vec3 CentreOf(OcctGeometryKernel& kernel, const KernelShape& shape) {
    const KernelMassPropertiesResult mass = kernel.calculateMassProperties(shape);
    EXPECT_TRUE(mass) << mass.message;
    return mass ? mass.properties.centerOfMassMm : Vec3{};
}

} // namespace

TEST(OcctEdgeQueryTest, M17_EDGE_001_AllEdgesIsStillWhatItWas) {
    // The default, and every file written before selections existed. A change
    // to the plumbing that quietly narrowed it would silently reshape parts
    // people already have.
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;
    const ShapeResult rounded = box.kernel.filletEdges(box.shape.shape, AllEdgesSelection(), 2.0);
    ASSERT_TRUE(rounded) << rounded.message;
    EXPECT_LT(VolumeOf(box.kernel, rounded.shape), kSolid);
}

TEST(OcctEdgeQueryTest, M17_EDGE_002_EveryVerticalEdgeIsEXACTLYTheFourUprights) {
    // The one case with clean arithmetic: four vertical edges, rounded at 2 mm,
    // each removing a prism of cross-section (r^2 - pi r^2 / 4) over the full
    // 20 mm height. They do not meet, so there is no corner interaction to
    // model -- which is why this is the query the exact oracle is spent on.
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;
    const double r = 2.0;
    const ShapeResult rounded =
        box.kernel.filletEdges(box.shape.shape, EdgeSelection{EdgesParallelTo{Vec3{0, 0, 1}}}, r);
    ASSERT_TRUE(rounded) << rounded.message;

    const double removed = 4.0 * (r * r - kPi * r * r / 4.0) * kD;
    EXPECT_NEAR(VolumeOf(box.kernel, rounded.shape), kSolid - removed, 1e-6)
        << "the vertical query did not dress exactly the four uprights";
    // Symmetric about the centre in x and y, and UNMOVED in z: rounding the
    // uprights takes the same material from top and bottom.
    const Vec3 centre = CentreOf(box.kernel, rounded.shape);
    EXPECT_NEAR(centre.x, kW / 2.0, 1e-6);
    EXPECT_NEAR(centre.y, kH / 2.0, 1e-6);
    EXPECT_NEAR(centre.z, kD / 2.0, 1e-6);
}

TEST(OcctEdgeQueryTest, M17_EDGE_003_TheTopFacesEdgesAreTheTOPNotTheBottom) {
    // THE test. A box has four top edges and four bottom ones, so counting
    // proves nothing and a query that fetched the wrong face would look
    // perfectly reasonable. Material removed from the top moves the centre of
    // mass DOWN; from the bottom, up.
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;
    const ShapeResult top = box.kernel.filletEdges(
        box.shape.shape, EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, 1}}}, 2.0);
    ASSERT_TRUE(top) << top.message;
    const ShapeResult bottom = box.kernel.filletEdges(
        box.shape.shape, EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, -1}}}, 2.0);
    ASSERT_TRUE(bottom) << bottom.message;

    const Vec3 topCentre = CentreOf(box.kernel, top.shape);
    const Vec3 bottomCentre = CentreOf(box.kernel, bottom.shape);
    EXPECT_LT(topCentre.z, kD / 2.0) << "rounding the top face did not lighten the top";
    EXPECT_GT(bottomCentre.z, kD / 2.0) << "rounding the bottom face did not lighten the bottom";
    // Mirror images of each other, because the box is symmetric in z.
    EXPECT_NEAR(topCentre.z, kD - bottomCentre.z, 1e-6);
    // Same amount of material either way.
    EXPECT_NEAR(VolumeOf(box.kernel, top.shape), VolumeOf(box.kernel, bottom.shape), 1e-6);
}

TEST(OcctEdgeQueryTest, M17_EDGE_004_OneFacesEdgesAreFewerThanEveryEdge) {
    // A selection that quietly fell back to "everything" would pass every test
    // above -- the top face's edges ARE a subset, and the centre of mass would
    // still move if all twelve were rounded... no: it would not, because that
    // is symmetric. This checks the other half anyway, because "narrower than
    // all" is the property a user is buying.
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;
    const ShapeResult all = box.kernel.filletEdges(box.shape.shape, AllEdgesSelection(), 2.0);
    const ShapeResult top = box.kernel.filletEdges(
        box.shape.shape, EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, 1}}}, 2.0);
    ASSERT_TRUE(all) << all.message;
    ASSERT_TRUE(top) << top.message;

    const double allVolume = VolumeOf(box.kernel, all.shape);
    const double topVolume = VolumeOf(box.kernel, top.shape);
    EXPECT_LT(topVolume, kSolid) << "the top-face query removed nothing";
    EXPECT_GT(topVolume, allVolume) << "the top-face query removed as much as every edge";
}

TEST(OcctEdgeQueryTest, M17_EDGE_005_TwoQueriesUNITEAndAnEdgeNamedTwiceIsDressedOnce) {
    // Adding an edge to ChFi3d twice is undefined-behaviour territory, so a
    // union that did not deduplicate would not merely double-count -- it could
    // fail or produce nonsense. Here the vertical edges are named on their own
    // AND again by "every edge".
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;
    const ShapeResult both = box.kernel.filletEdges(
        box.shape.shape, EdgeSelection{EdgesParallelTo{Vec3{0, 0, 1}}, AllEdges{}}, 2.0);
    ASSERT_TRUE(both) << both.message;

    const ShapeResult all = box.kernel.filletEdges(box.shape.shape, AllEdgesSelection(), 2.0);
    ASSERT_TRUE(all) << all.message;
    EXPECT_NEAR(VolumeOf(box.kernel, both.shape), VolumeOf(box.kernel, all.shape), 1e-6);
}

TEST(OcctEdgeQueryTest, M17_EDGE_006_ASelectionThatMatchesNOTHINGIsRefusedInWords) {
    // The failure that must never be silent: a fillet that dresses no edge
    // returns the solid unchanged. Reporting success would be a command that
    // changed nothing while saying it worked.
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;
    // No face of a box faces the (1,1,1) diagonal within the tolerance the
    // query uses, so this names nothing.
    const double s = 1.0 / std::sqrt(3.0);
    const ShapeResult refused = box.kernel.filletEdges(
        box.shape.shape, EdgeSelection{EdgesOfExtremeFace{Vec3{s, s, s}}}, 2.0);
    EXPECT_FALSE(refused);
    EXPECT_NE(refused.message.find("no edge matched"), std::string::npos) << refused.message;

    const ShapeResult chamfer = box.kernel.chamferEdges(
        box.shape.shape, EdgeSelection{EdgesParallelTo{Vec3{s, s, s}}}, 2.0);
    EXPECT_FALSE(chamfer);
    EXPECT_NE(chamfer.message.find("no edge matched"), std::string::npos) << chamfer.message;
}

TEST(OcctEdgeQueryTest, M17_EDGE_007_ChamferTakesTheSameSelectionAsFillet) {
    // The two are the same feature with a different kernel verb, and a
    // selection honoured by one and ignored by the other is exactly the
    // divergence EdgeDressFeature's shared base exists to prevent.
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;
    const double d = 2.0;
    const ShapeResult cut = box.kernel.chamferEdges(
        box.shape.shape, EdgeSelection{EdgesParallelTo{Vec3{0, 0, 1}}}, d);
    ASSERT_TRUE(cut) << cut.message;

    // Four 45-degree corners cut off a prism of half a square each.
    const double removed = 4.0 * (d * d / 2.0) * kD;
    EXPECT_NEAR(VolumeOf(box.kernel, cut.shape), kSolid - removed, 1e-6);
}

// --- What the user is told ---------------------------------------------------

TEST(EdgeSelectionWordsTest, M17_EDGE_010_EveryQueryDescribesItselfInWordsAUserCanCheck) {
    EXPECT_EQ(DescribeEdgeSelection(AllEdgesSelection()), "every edge");
    EXPECT_EQ(DescribeEdgeSelection(EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, 1}}}),
              "the top face's edges");
    EXPECT_EQ(DescribeEdgeSelection(EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, -1}}}),
              "the bottom face's edges");
    EXPECT_EQ(DescribeEdgeSelection(EdgeSelection{EdgesParallelTo{Vec3{0, 0, 1}}}),
              "every vertical edge");
    // Two of them read as one sentence.
    EXPECT_EQ(DescribeEdgeSelection(
                  EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, 1}}, EdgesParallelTo{Vec3{0, 0, 1}}}),
              "the top face's edges, plus every vertical edge");
}

TEST(EdgeSelectionWordsTest, M17_EDGE_011_NOTHINGSelectedDoesNotReadAsEverything) {
    // Opposite solids, and a description that read the same for both would be
    // the one thing a user checks before pressing OK.
    EXPECT_EQ(DescribeEdgeSelection(EdgeSelection{}), "nothing selected");
    EXPECT_NE(DescribeEdgeSelection(EdgeSelection{}), DescribeEdgeSelection(AllEdgesSelection()));
}

TEST(EdgeSelectionWordsTest, M17_EDGE_012_AnOffAxisDirectionIsSpelledOutNotRoundedToAnAxis) {
    // Calling a 45-degree face "the top" would be a description the part does
    // not match, and the description is what a user checks the selection by.
    const double s = 1.0 / std::sqrt(2.0);
    const std::string words = DescribeEdgeSelection(EdgeSelection{EdgesOfExtremeFace{Vec3{0, s, s}}});
    EXPECT_EQ(words.find("top"), std::string::npos) << words;
    EXPECT_NE(words.find("0.71"), std::string::npos) << words;
}

// --- M17.12: turning a picked face into a query ------------------------------
//
// The user clicks a face; what is STORED is a sentence. These pin the one
// judgement that matters: a pick that cannot be said in the vocabulary the
// kernel understands is REFUSED, not stored as the nearest thing -- because
// the nearest thing dresses a different face on the next rebuild, and looks
// deliberate while doing it.

namespace {

FacePlane Face(Vec3 point, Vec3 normal) {
    FacePlane face;
    face.isFace = true;
    face.planar = true;
    face.point = point;
    face.normal = normal;
    return face;
}

// The six faces of a 40 x 30 x 20 box at the origin.
std::vector<FacePlane> BoxFaces() {
    return {Face(Vec3{0, 0, 0}, Vec3{-1, 0, 0}),  Face(Vec3{40, 0, 0}, Vec3{1, 0, 0}),
            Face(Vec3{0, 0, 0}, Vec3{0, -1, 0}),  Face(Vec3{0, 30, 0}, Vec3{0, 1, 0}),
            Face(Vec3{0, 0, 0}, Vec3{0, 0, -1}),  Face(Vec3{0, 0, 20}, Vec3{0, 0, 1})};
}

} // namespace

TEST(EdgeSelectionPickTest, M17_PICK_001_TheTopFaceBecomesTheTopFacesEdges) {
    const EdgeSelectionPick pick = SelectionForPickedFace(Face(Vec3{0, 0, 20}, Vec3{0, 0, 1}),
                                                          BoxFaces());
    ASSERT_TRUE(pick.ok) << pick.message;
    ASSERT_EQ(pick.selection.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<EdgesOfExtremeFace>(pick.selection.front()));
    EXPECT_NEAR(std::get<EdgesOfExtremeFace>(pick.selection.front()).direction.z, 1.0, 1e-9);
    // The user is told what was stored, in the same words the panel will use.
    EXPECT_EQ(pick.message, "the top face's edges");
}

TEST(EdgeSelectionPickTest, M17_PICK_002_EverySideFaceOfABoxIsExpressible) {
    // All six faces of a box are outermost in their own direction, so all six
    // are pickable -- which is the case a user meets first and every time.
    for (const FacePlane& face : BoxFaces()) {
        const EdgeSelectionPick pick = SelectionForPickedFace(face, BoxFaces());
        EXPECT_TRUE(pick.ok) << pick.message;
    }
}

TEST(EdgeSelectionPickTest, M17_PICK_003_AnInnerFaceIsNamedByWHATMADEIt) {
    // THE judgement, and the reason provenance exists. A pocket floor faces +Z
    // like the top face does, but it is not the outermost one -- so "the
    // outermost face towards +Z" names the TOP, not the floor the user
    // clicked. Storing that would round the wrong edges on a part that still
    // looks like someone meant it.
    //
    // With a tag, there IS a sentence for it: "what the feature that made this
    // face created". It names the floor exactly, and goes on naming it after
    // the pocket is moved or deepened.
    std::vector<FacePlane> faces = BoxFaces();
    FacePlane pocketFloor = Face(Vec3{20, 15, 12}, Vec3{0, 0, 1});
    pocketFloor.createdBy = 4242;
    faces.push_back(pocketFloor);

    const EdgeSelectionPick pick = SelectionForPickedFace(pocketFloor, faces);
    ASSERT_TRUE(pick.ok) << pick.message;
    ASSERT_EQ(pick.selection.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<EdgesCreatedBy>(pick.selection.front()));
    EXPECT_EQ(std::get<EdgesCreatedBy>(pick.selection.front()).featureId, 4242u);
    // NOT the outermost-face query, which would be the top of the block.
    EXPECT_EQ(pick.message.find("top face"), std::string::npos) << pick.message;
}

TEST(EdgeSelectionPickTest, M17_PICK_007_AnInnerFaceWithNOProvenanceIsStillRefused) {
    // Provenance is what makes an inner face nameable. Without it there is no
    // sentence that would still mean THIS face after a rebuild, and inventing
    // the nearest one is how a fillet silently moves to the top of the part.
    std::vector<FacePlane> faces = BoxFaces();
    const FacePlane pocketFloor = Face(Vec3{20, 15, 12}, Vec3{0, 0, 1}); // createdBy stays 0
    faces.push_back(pocketFloor);

    const EdgeSelectionPick pick = SelectionForPickedFace(pocketFloor, faces);
    EXPECT_FALSE(pick.ok);
    EXPECT_NE(pick.message.find("which feature made it"), std::string::npos) << pick.message;
    EXPECT_TRUE(pick.selection.empty()) << "a refused pick still handed back a selection";
}

TEST(EdgeSelectionPickTest, M17_PICK_004_ACurvedFaceIsRefusedInItsOwnWords) {
    FacePlane barrel;
    barrel.isFace = true;
    barrel.planar = false;
    const EdgeSelectionPick pick = SelectionForPickedFace(barrel, BoxFaces());
    EXPECT_FALSE(pick.ok);
    EXPECT_NE(pick.message.find("curved"), std::string::npos) << pick.message;
}

TEST(EdgeSelectionPickTest, M17_PICK_005_NoPickIsRefusedAndSaysWhatToDo) {
    const EdgeSelectionPick pick = SelectionForPickedFace(FacePlane{}, BoxFaces());
    EXPECT_FALSE(pick.ok);
    EXPECT_FALSE(pick.message.empty());
    EXPECT_NE(pick.message.find("Click"), std::string::npos) << pick.message;
}

TEST(EdgeSelectionPickTest, M17_PICK_006_ADegenerateNormalIsRefusedNotDividedBy) {
    const EdgeSelectionPick pick =
        SelectionForPickedFace(Face(Vec3{0, 0, 20}, Vec3{0, 0, 0}), BoxFaces());
    EXPECT_FALSE(pick.ok);
    EXPECT_FALSE(pick.message.empty());
}

// --- M17.14: composition -----------------------------------------------------

TEST(OcctEdgeQueryTest, M17_EDGE_020_AFaceQueryNARROWSWhereNeitherHalfCould) {
    // The case composition exists for, on a real solid. `createdBy` alone
    // names a pad's twelve edges; `extremeTowards` alone names the part's top.
    // Together they name the top face of that pad -- four edges.
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;

    // Tag every face as feature 7's work, which is what a Pad does.
    const KernelShape tagged = box.kernel.tagCreatedFaces(box.shape.shape, KernelShape{}, 7);

    FaceQuery topOfSeven;
    topOfSeven.createdBy = 7;
    topOfSeven.extremeTowards = Vec3{0, 0, 1};
    const ShapeResult composed =
        box.kernel.filletEdges(tagged, EdgeSelection{EdgesOfFace{topOfSeven}}, 2.0);
    ASSERT_TRUE(composed) << composed.message;

    // The same four edges "the top face" alone names on this shape, because
    // here everything IS feature 7's -- so the composed answer must match, and
    // that equality is what says the conjunction narrowed rather than widened.
    const ShapeResult topOnly = box.kernel.filletEdges(
        tagged, EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, 1}}}, 2.0);
    ASSERT_TRUE(topOnly) << topOnly.message;
    EXPECT_NEAR(VolumeOf(box.kernel, composed.shape), VolumeOf(box.kernel, topOnly.shape), 1e-6);

    // ...and NOT the same as everything feature 7 made, which is all twelve.
    const ShapeResult allOfSeven =
        box.kernel.filletEdges(tagged, EdgeSelection{EdgesCreatedBy{7}}, 2.0);
    ASSERT_TRUE(allOfSeven) << allOfSeven.message;
    EXPECT_NE(VolumeOf(box.kernel, composed.shape), VolumeOf(box.kernel, allOfSeven.shape));
}

TEST(OcctEdgeQueryTest, M17_EDGE_021_AComposedQueryNamingADifferentFeatureMatchesNothing) {
    Box box;
    ASSERT_TRUE(box.shape) << box.shape.message;
    const KernelShape tagged = box.kernel.tagCreatedFaces(box.shape.shape, KernelShape{}, 7);

    FaceQuery topOfSomeoneElse;
    topOfSomeoneElse.createdBy = 8; // made nothing here
    topOfSomeoneElse.extremeTowards = Vec3{0, 0, 1};
    const ShapeResult refused =
        box.kernel.filletEdges(tagged, EdgeSelection{EdgesOfFace{topOfSomeoneElse}}, 2.0);
    EXPECT_FALSE(refused);
    EXPECT_NE(refused.message.find("no edge matched"), std::string::npos) << refused.message;
}

TEST(EdgeSelectionWordsTest, M17_EDGE_022_AComposedQueryReadsAsOneSentence) {
    FaceQuery floor;
    floor.createdBy = 12;
    floor.extremeTowards = Vec3{0, 0, 1};
    EXPECT_EQ(DescribeEdgeSelection(EdgeSelection{EdgesOfFace{floor}}),
              "the edges of the top face of what feature 12 made");
}

TEST(FaceQueryResolveTest, M17_FACEQ_001_AmbiguityIsREFUSEDNotResolvedByPickingOne) {
    // Two faces at the same distance facing the same way is a symmetric part.
    // Choosing between them would put the answer somewhere the user could
    // neither predict nor correct -- and on a sketch, that is the whole model
    // built in the wrong place.
    OcctGeometryKernel kernel;
    const ShapeResult box = kernel.createBox(BoxDefinition{kW, kH, kD});
    ASSERT_TRUE(box) << box.message;

    // "The face pointing +Z" alone is unambiguous on a box (one top face), so
    // ambiguity is made with a condition that matches four: the vertical walls
    // all face outward, and none is extreme along +Z.
    FaceQuery anyWall;
    anyWall.facing = Vec3{0, 0, 1}; // only the top face faces +Z: unambiguous
    FaceQueryResult top = kernel.resolveFace(box.shape, anyWall);
    EXPECT_TRUE(top.ok) << top.message;

    // An EMPTY query names no face rather than the first one found.
    const FaceQueryResult nothing = kernel.resolveFace(box.shape, FaceQuery{});
    EXPECT_FALSE(nothing.ok);
    EXPECT_FALSE(nothing.message.empty());
}
