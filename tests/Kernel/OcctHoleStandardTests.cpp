// M39.2 -- holes that know what screw they are for.
//
// The kernel suite, because the thing being checked is how much material came
// out. A hole's diameter cannot be read off a solid by asking it; what CAN be
// asked is its volume, and a cylinder's volume pins its diameter exactly.
//
// This is deliberate. The defect these tests exist for is silent: an M8 tapped
// hole drilled to 8 instead of 6.8 looks like a perfectly good hole on the
// screen, and the part comes back from the shop untappable.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

struct ScratchPart {
    std::string path;
    explicit ScratchPart(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-hole-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchPart() { std::remove(path.c_str()); }
};

// A 100 x 60 x 10 plate, and one hole feature drilled through it.
struct Plate {
    PartDocument part{"Plate"};
    OcctGeometryKernel kernel;
    Body* body = nullptr;
    PadFeature* pad = nullptr;

    Plate() {
        part.setGeometryKernel(&kernel);
        Sketch& outline = part.addSketch("Base");
        part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 0}, Vec2{100, 0}});
        part.addSketchEntity(outline.id(), SketchLine{Vec2{100, 0}, Vec2{100, 60}});
        part.addSketchEntity(outline.id(), SketchLine{Vec2{100, 60}, Vec2{0, 60}});
        part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 60}, Vec2{0, 0}});
        Parameter& tall = part.addParameter("H", 10.0, UnitType::Millimeter);
        body = &part.addBody("Plate");
        pad = &part.addPadFeature(*body, "Pad", outline.id(), tall.id());
    }

    HoleFeature& drill(double diameter, double depth = 0.0) {
        Sketch& marks = part.addSketch("Marks");
        part.addSketchEntity(marks.id(), SketchPoint{Vec2{50.0, 30.0}});
        Parameter& across = part.addParameter("D", diameter, UnitType::Millimeter);
        Parameter& deep = part.addParameter("Z", depth, UnitType::Millimeter);
        return part.addHoleFeature(*body, "Hole", pad->id(), marks.id(), across.id(), deep.id());
    }

    double volume(const HoleFeature& hole) {
        const KernelMassPropertiesResult mass =
            kernel.calculateMassProperties(hole.currentShape());
        EXPECT_TRUE(static_cast<bool>(mass)) << mass.message;
        return mass.properties.volumeMm3;
    }
};

constexpr double kPlateVolume = 100.0 * 60.0 * 10.0;

double CylinderVolume(double diameter, double length) {
    return kPi * (diameter / 2.0) * (diameter / 2.0) * length;
}

TEST(OcctHoleStandardTest, M39_HOLE_001_ATappedHoleIsDrilledToTheTAPDrillNotItsThreadSize) {
    // THE ONE THAT SENDS PARTS TO THE SKIP. Nothing here would show on a
    // screen: both holes are round, both are in the right place, and only one
    // of them can be tapped.
    Plate plate;
    HoleFeature& hole = plate.drill(12.0); // a typed diameter, deliberately wrong
    HoleScrew screw;
    screw.designation = "M8";
    screw.tapped = true;
    ASSERT_TRUE(plate.part.setHoleScrew(hole.id(), screw));
    ASSERT_TRUE(plate.part.recompute().success);

    const double removed = kPlateVolume - plate.volume(hole);
    EXPECT_NEAR(removed, CylinderVolume(6.8, 10.0), 1.0)
        << "the hole was not drilled to the M8 tap drill";
    // Said the other way round as well, because "near 363" passing by accident
    // is exactly the failure this is guarding.
    EXPECT_GT(std::fabs(removed - CylinderVolume(12.0, 10.0)), 100.0)
        << "the hole was drilled to its typed diameter, ignoring the thread";
}

TEST(OcctHoleStandardTest, M39_HOLE_002_AClearanceHoleIsDrilledToTheFITThatWasAskedFor) {
    Plate plate;
    HoleFeature& hole = plate.drill(12.0);
    HoleScrew screw;
    screw.designation = "M8";
    screw.tapped = false;
    screw.fit = ClearanceFit::Normal;
    ASSERT_TRUE(plate.part.setHoleScrew(hole.id(), screw));
    ASSERT_TRUE(plate.part.recompute().success);
    EXPECT_NEAR(kPlateVolume - plate.volume(hole), CylinderVolume(9.0, 10.0), 1.0);

    // The SAME screw at the close fit is a measurably different hole -- which
    // is the whole reason the three series exist.
    screw.fit = ClearanceFit::Close;
    ASSERT_TRUE(plate.part.setHoleScrew(hole.id(), screw));
    ASSERT_TRUE(plate.part.recompute().success);
    EXPECT_NEAR(kPlateVolume - plate.volume(hole), CylinderVolume(8.4, 10.0), 1.0);
}

TEST(OcctHoleStandardTest, M39_HOLE_003_ACounterboreTakesOutTheRECESSAsWellAsTheHole) {
    Plate plate;
    HoleFeature& hole = plate.drill(12.0);
    HoleScrew screw;
    screw.designation = "M8"; // clearance 9, counterbore 15 x 8 deep
    ASSERT_TRUE(plate.part.setHoleScrew(hole.id(), screw));
    ASSERT_TRUE(plate.part.setHoleKind(hole.id(), HoleKind::Counterbore));
    ASSERT_TRUE(plate.part.recompute().success);

    // The bore through the plate, plus the ring the recess takes out of the
    // first 8 mm of it.
    const double expected = CylinderVolume(9.0, 10.0) +
                            (CylinderVolume(15.0, 8.0) - CylinderVolume(9.0, 8.0));
    EXPECT_NEAR(kPlateVolume - plate.volume(hole), expected, 2.0);
}

TEST(OcctHoleStandardTest, M39_HOLE_004_ACounterboreOnATHROUGHHoleCutsTheSideTheMATERIALIsOn) {
    // A through hole's tool goes BOTH ways, so it has no direction of its own
    // to hand the recess. Guess wrong and the counterbore is cut in thin air:
    // it removes nothing at all, the solid is a perfectly ordinary drilled
    // plate, and the drawing still says there is a counterbore on it.
    Plate plate;
    HoleFeature& hole = plate.drill(12.0);
    HoleScrew screw;
    screw.designation = "M6"; // clearance 6.6, counterbore 11 x 6 deep
    ASSERT_TRUE(plate.part.setHoleScrew(hole.id(), screw));
    ASSERT_TRUE(plate.part.recompute().success);
    const double plainHole = kPlateVolume - plate.volume(hole);

    ASSERT_TRUE(plate.part.setHoleKind(hole.id(), HoleKind::Counterbore));
    ASSERT_TRUE(plate.part.recompute().success);
    const double withRecess = kPlateVolume - plate.volume(hole);

    EXPECT_GT(withRecess - plainHole, 1.0)
        << "the counterbore removed nothing -- it was cut on the side with no material";
    EXPECT_NEAR(withRecess - plainHole,
                CylinderVolume(11.0, 6.0) - CylinderVolume(6.6, 6.0), 2.0);
}

TEST(OcctHoleStandardTest, M39_HOLE_005_ACountersinkIsACONEOfTheAngleItClaims) {
    // A cone that is the right diameter at the surface but the wrong angle
    // takes a screw head that does not sit flush -- and it looks right in
    // every view except the one nobody draws.
    Plate plate;
    HoleFeature& hole = plate.drill(12.0);
    HoleScrew screw;
    screw.designation = "M8"; // clearance 9, countersink 16 at 90 degrees
    ASSERT_TRUE(plate.part.setHoleScrew(hole.id(), screw));
    ASSERT_TRUE(plate.part.setHoleKind(hole.id(), HoleKind::Countersink));
    ASSERT_TRUE(plate.part.recompute().success);

    // A 90 degree cone widens one for one, so it reaches (8 - 4.5) deep. What
    // the countersink adds is that frustum less the bore already through it.
    const double coneDepth = (16.0 - 9.0) / 2.0;
    const double bigR = 8.0;
    const double smallR = 4.5;
    const double frustum =
        kPi * coneDepth / 3.0 * (bigR * bigR + bigR * smallR + smallR * smallR);
    const double expected = CylinderVolume(9.0, 10.0) + frustum - CylinderVolume(9.0, coneDepth);
    EXPECT_NEAR(kPlateVolume - plate.volume(hole), expected, 3.0);
}

TEST(OcctHoleStandardTest, M39_HOLE_006_AThreadTheBuildCannotSizeSTOPSTheFeature) {
    // The alternative is a hole quietly drilled to its typed diameter with a
    // callout naming a thread it cannot take. A refusal is loud; that is not
    // a drawback here, it is the requirement.
    Plate plate;
    HoleFeature& hole = plate.drill(12.0);
    HoleScrew screw;
    screw.designation = "M9";
    screw.tapped = true;
    // The document refuses it outright rather than storing a hole that cannot
    // recompute -- a feature that goes red later points at a decision the user
    // has stopped thinking about.
    EXPECT_FALSE(plate.part.setHoleScrew(hole.id(), screw));

    // ...and if one ever arrives another way, the feature stops instead of
    // falling back on the diameter parameter.
    HoleScrew real;
    real.designation = "M8";
    real.tapped = true;
    ASSERT_TRUE(plate.part.setHoleScrew(hole.id(), real));
    ASSERT_TRUE(plate.part.recompute().success);
    HoleScrew bad;
    bad.designation = "M9x1.25";
    hole.setScrew(bad);
    ASSERT_TRUE(plate.part.markDirty(hole.id()));
    const DocumentRecomputeReport report = plate.part.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(hole.currentState(), ComputeState::Failed)
        << "an unsizable thread was drilled at the typed diameter instead";
    bool saidWhich = false;
    for (const RecomputeItemReport& item : report.items)
        if (item.id == hole.id() && item.message.find("M9") != std::string::npos)
            saidWhich = true;
    EXPECT_TRUE(saidWhich) << "the refusal did not name the thread it could not size";
}

TEST(OcctHoleStandardTest, M39_HOLE_007_AHolesScrewSurvivesASaveAndStillDrillsTheSameSize) {
    // A file that reopens as a hole of a different diameter is the failure
    // this project names as the worst kind: nothing is reported, and the part
    // is wrong from the moment it is next built.
    Plate plate;
    HoleFeature& hole = plate.drill(12.0);
    HoleScrew screw;
    screw.designation = "M8";
    screw.tapped = true;
    ASSERT_TRUE(plate.part.setHoleScrew(hole.id(), screw));
    ASSERT_TRUE(plate.part.setHoleKind(hole.id(), HoleKind::Counterbore));
    ASSERT_TRUE(plate.part.recompute().success);
    const double before = plate.volume(hole);

    ScratchPart file{"roundtrip.ep3d"};
    ASSERT_TRUE(savePartDocumentToFile(plate.part, file.path));
    LoadResult loaded = loadPartDocumentFromFile(file.path);
    ASSERT_TRUE(loaded) << loaded.message;

    OcctGeometryKernel kernel;
    loaded.document->setGeometryKernel(&kernel);
    ASSERT_TRUE(loaded.document->recompute().success);

    const HoleFeature* back = nullptr;
    for (const std::unique_ptr<Body>& body : loaded.document->bodies())
        for (const std::unique_ptr<Feature>& feature : body->features())
            if (const auto* one = dynamic_cast<const HoleFeature*>(feature.get())) back = one;
    ASSERT_NE(back, nullptr);

    EXPECT_EQ(back->screw().designation, "M8");
    EXPECT_TRUE(back->screw().tapped);
    EXPECT_EQ(back->kind(), HoleKind::Counterbore);
    const KernelMassPropertiesResult mass = kernel.calculateMassProperties(back->currentShape());
    ASSERT_TRUE(static_cast<bool>(mass)) << mass.message;
    EXPECT_NEAR(mass.properties.volumeMm3, before, 1e-6)
        << "the reopened part is not the shape that was saved";
}

} // namespace
