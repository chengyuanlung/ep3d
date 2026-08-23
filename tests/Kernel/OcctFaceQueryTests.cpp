// M17.5 -- reading a plane off a REAL kernel face.
//
// This is the one step of sketch-on-face that a mouse would otherwise be the
// only way to reach, and it is also the step with the trap in it: half the
// faces of every solid are stored REVERSED, so a reader that ignores
// orientation returns the correct plane with the normal pointing the wrong
// way. A sketch built on that plane pads back INTO the part, which on screen
// looks like the command did nothing.
//
// The box below is not a convenience -- it is the oracle. Its six faces have
// six known outward normals, derived from the corner-origin convention
// (spec 8) rather than from anything the code under test computed.
//
// Everything here goes through FacesOf, the kernel-neutral door, because OCCT
// headers are private to the kernel library (ADR-M4-004) and a test has no
// business naming a TopoDS type. FacesOf produces each entry by calling
// PlaneOfFace, so this is the same code the viewer runs on a picked face.

#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/ProfileDefinition.h"
#include "Kernel/Occt/OcctFaceQuery.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kW = 40.0; // +X
constexpr double kH = 30.0; // +Y
constexpr double kD = 20.0; // +Z

bool SameDirection(Vec3 a, Vec3 b) {
    return std::fabs(a.x - b.x) < 1e-9 && std::fabs(a.y - b.y) < 1e-9 &&
           std::fabs(a.z - b.z) < 1e-9;
}

} // namespace

TEST(OcctFaceQueryTest, M17_FQ_001_ABoxOffersSixPlanarFaces) {
    OcctGeometryKernel kernel;
    const ShapeResult box = kernel.createBox(BoxDefinition{kW, kH, kD});
    ASSERT_TRUE(box) << box.message;

    const std::vector<FacePlane> faces = FacesOf(box.shape);
    ASSERT_EQ(faces.size(), 6u);
    for (const FacePlane& face : faces) {
        EXPECT_TRUE(face.isFace);
        EXPECT_TRUE(face.planar);
    }
}

TEST(OcctFaceQueryTest, M17_FQ_002_EveryOutwardNormalPointsOutOfTheBox) {
    // THE test. Each of the six faces must report the normal that leaves the
    // material. A reader that dropped the orientation check would return +Z
    // for the bottom face as well as the top, and three of these six would
    // come back inverted.
    OcctGeometryKernel kernel;
    const ShapeResult box = kernel.createBox(BoxDefinition{kW, kH, kD});
    ASSERT_TRUE(box) << box.message;

    struct Expected {
        Vec3 normal;
        double offset; // distance from the origin ALONG that normal
        const char* what;
    };
    const Expected expected[] = {
        {Vec3{-1, 0, 0}, 0.0, "left"},   {Vec3{1, 0, 0}, kW, "right"},
        {Vec3{0, -1, 0}, 0.0, "front"},  {Vec3{0, 1, 0}, kH, "back"},
        {Vec3{0, 0, -1}, 0.0, "bottom"}, {Vec3{0, 0, 1}, kD, "top"},
    };

    std::vector<bool> seen(6, false);
    for (const FacePlane& face : FacesOf(box.shape)) {
        ASSERT_TRUE(face.planar);
        bool matched = false;
        for (std::size_t i = 0; i < 6; ++i) {
            if (!SameDirection(face.normal, expected[i].normal)) continue;
            EXPECT_FALSE(seen[i]) << "two faces reported the " << expected[i].what << " normal";
            seen[i] = true;
            matched = true;
            // The plane's offset along its own normal, which is what tells the
            // top face from the bottom one -- the normals alone do not, once a
            // reader has them backwards. The near faces sit at 0 and the far
            // ones at the box dimension, in both cases measured outward.
            const double offset = face.point.x * face.normal.x + face.point.y * face.normal.y +
                                  face.point.z * face.normal.z;
            EXPECT_NEAR(offset, expected[i].offset, 1e-9) << expected[i].what;
            break;
        }
        EXPECT_TRUE(matched) << "a face reported a normal no side of the box has: "
                             << face.normal.x << ", " << face.normal.y << ", " << face.normal.z;
    }
    for (std::size_t i = 0; i < 6; ++i) EXPECT_TRUE(seen[i]) << expected[i].what << " missing";
}

TEST(OcctFaceQueryTest, M17_FQ_003_ACurvedFaceIsReportedAsAFaceThatIsNotPlanar) {
    // Both facts matter and they are different: the click DID land on a face
    // (so "click a face first" would be the wrong thing to tell the user), and
    // that face cannot carry a sketch.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.segments = {ProfileCircleSegment{Vec2{0, 0}, 10.0}};
    const ShapeResult cylinder = kernel.extrudeProfile(profile, 25.0);
    ASSERT_TRUE(cylinder) << cylinder.message;

    int curved = 0;
    int flat = 0;
    for (const FacePlane& face : FacesOf(cylinder.shape)) {
        EXPECT_TRUE(face.isFace);
        if (face.planar) {
            ++flat;
        } else {
            ++curved;
        }
    }
    EXPECT_EQ(curved, 1); // the barrel
    EXPECT_EQ(flat, 2);   // the two end caps
}

TEST(OcctFaceQueryTest, M17_FQ_004_TheTwoEndsOfACylinderFaceOppositeWays) {
    // The end caps are the case where "planar" is not enough: both are flat,
    // and a sketch on one must pad the opposite way from a sketch on the other.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.segments = {ProfileCircleSegment{Vec2{0, 0}, 10.0}};
    const ShapeResult cylinder = kernel.extrudeProfile(profile, 25.0);
    ASSERT_TRUE(cylinder) << cylinder.message;

    std::vector<Vec3> capNormals;
    for (const FacePlane& face : FacesOf(cylinder.shape))
        if (face.planar) capNormals.push_back(face.normal);

    ASSERT_EQ(capNormals.size(), 2u);
    const double dot = capNormals[0].x * capNormals[1].x + capNormals[0].y * capNormals[1].y +
                       capNormals[0].z * capNormals[1].z;
    EXPECT_NEAR(dot, -1.0, 1e-9) << "both ends of the cylinder point the same way";
}

TEST(OcctFaceQueryTest, M17_FQ_005_AnEmptyOrForeignShapeYieldsNoFacesRatherThanAGuess) {
    // A default KernelShape holds no handle at all. Returning a face for it --
    // even an "empty" one -- would make "picked nothing" indistinguishable
    // from "picked a plane at the origin", and the origin is a plane a sketch
    // would happily be built on.
    EXPECT_TRUE(FacesOf(KernelShape{}).empty());
}
