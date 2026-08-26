// M45 -- the standard parts library.
//
// After M39 a hole knows what M8 means. There was still no M8 screw to put in
// it: an assembly could describe a bolted joint and could not contain a bolt.
//
// The kernel suite, because the only honest check on a generated solid is how
// much of it there is. A screw's designation promises a head 13 across and 8
// high on a shank 8 across and 30 long, and the volume of that is a number
// this test can work out from the standard and compare -- which is the same
// argument M39's holes are tested by, and for the same reason: a wrong screw
// looks exactly like a right one.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Document/PartDocument.h"
#include "Core/Drawing/BomTable.h"
#include "Core/Library/StandardParts.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

double VolumeOf(const FastenerSpec& spec) {
    OcctGeometryKernel kernel;
    std::unique_ptr<PartDocument> part = BuildStandardPart(spec);
    part->setGeometryKernel(&kernel);
    EXPECT_TRUE(part->recompute().success) << spec.designation() << " did not build";

    const Body* body = part->bodies().empty() ? nullptr : part->bodies().front().get();
    EXPECT_NE(body, nullptr);
    const ISolidFeature* tip = nullptr;
    for (const auto& feature : body->features())
        if (const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get())) tip = solid;
    EXPECT_NE(tip, nullptr);
    const KernelMassPropertiesResult mass = kernel.calculateMassProperties(tip->currentShape());
    EXPECT_TRUE(static_cast<bool>(mass)) << mass.message;
    return mass.properties.volumeMm3;
}

TEST(OcctStandardPartTest, M45_LIB_001_AScrewIsTheHeadAndTheShankTheStandardPublishes) {
    const std::optional<FastenerSpec> spec = LookUpFastener("ISO 4762 M8x30");
    ASSERT_TRUE(spec.has_value());
    EXPECT_NEAR(spec->headDiameterMm, 13.0, 1e-9);
    EXPECT_NEAR(spec->headHeightMm, 8.0, 1e-9);

    // The head is a disc 13 across and 8 high; the shank is 8 across and 30
    // long, MEASURED UNDER THE HEAD -- which is what the designation means and
    // what a joint's stack-up is designed around.
    const double head = kPi * 6.5 * 6.5 * 8.0;
    const double shank = kPi * 4.0 * 4.0 * 30.0;
    EXPECT_NEAR(VolumeOf(*spec), head + shank, 1.0);

    // ...and a longer screw is longer by exactly the shank it gained. Said
    // separately because "near 5600" could pass with the length used as a
    // total height instead of a shank.
    const std::optional<FastenerSpec> longer = LookUpFastener("ISO 4762 M8x40");
    ASSERT_TRUE(longer.has_value());
    EXPECT_NEAR(VolumeOf(*longer) - VolumeOf(*spec), kPi * 4.0 * 4.0 * 10.0, 1.0);
}

TEST(OcctStandardPartTest, M45_LIB_002_ANutIsAHexagonACROSSTHEFLATSWithItsBlankHole) {
    const std::optional<FastenerSpec> spec = LookUpFastener("ISO 4032 M8");
    ASSERT_TRUE(spec.has_value());
    EXPECT_NEAR(spec->acrossFlatsMm, 13.0, 1e-9);
    EXPECT_NEAR(spec->thicknessMm, 6.8, 1e-9);

    // ACROSS THE FLATS, not across the corners. A hexagon whose 13 is taken as
    // a circumradius is 15% too big and still looks exactly like a nut -- and
    // a spanner is the only thing that would ever find out.
    //
    // A regular hexagon with inradius a has area 2*sqrt(3)*a*a.
    const double flatsArea = 2.0 * std::sqrt(3.0) * 6.5 * 6.5;
    // The hole is the TAP DRILL: what the blank has before it is threaded.
    const double bore = kPi * (6.8 / 2.0) * (6.8 / 2.0);
    EXPECT_NEAR(VolumeOf(*spec), (flatsArea - bore) * 6.8, 2.0);

    // Said the other way as well: across the CORNERS would be this, and it is
    // not what came out.
    const double cornersArea = 2.0 * std::sqrt(3.0) * (13.0 / std::sqrt(3.0)) *
                               (13.0 / std::sqrt(3.0));
    EXPECT_GT(std::fabs(VolumeOf(*spec) - (cornersArea - bore) * 6.8), 50.0);
}

TEST(OcctStandardPartTest, M45_LIB_003_AWashersBoreIsTheHoleTheScrewGoesThrough) {
    // ISO 7089's d1 IS ISO 273's close series, which M39 already holds. The
    // library derives it rather than copying the column -- a washer that
    // stopped matching the hole it goes on is a defect nobody would look for.
    for (const double size : {3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0}) {
        const std::optional<FastenerSpec> washer =
            LookUpFastener(FastenerKind::PlainWasher, size);
        ASSERT_TRUE(washer.has_value()) << "M" << size;
        const std::optional<double> hole = ClearanceHoleMm(size, ClearanceFit::Close);
        ASSERT_TRUE(hole.has_value()) << "M" << size;
        EXPECT_NEAR(washer->innerDiameterMm, *hole, 1e-9) << "M" << size;
        EXPECT_GT(washer->outerDiameterMm, washer->innerDiameterMm) << "M" << size;
    }

    const std::optional<FastenerSpec> spec = LookUpFastener("ISO 7089 M8");
    ASSERT_TRUE(spec.has_value());
    const double outer = kPi * 8.0 * 8.0;
    const double inner = kPi * (8.4 / 2.0) * (8.4 / 2.0);
    EXPECT_NEAR(VolumeOf(*spec), (outer - inner) * 1.6, 0.5);
}

TEST(OcctStandardPartTest, M45_LIB_004_ALengthNobodyMakesIsREFUSED) {
    // THE OPPOSITE CALL FROM M41's ROUGHNESS SERIES, and deliberately. A
    // roughness of 1.2 is an instruction a shop can meet; a 33 mm cap screw is
    // a line on a parts list nobody can order.
    EXPECT_TRUE(LookUpFastener("ISO 4762 M8x30").has_value());
    EXPECT_FALSE(LookUpFastener("ISO 4762 M8x33").has_value())
        << "a length that is not in the series was accepted";
    EXPECT_FALSE(LookUpFastener("ISO 4762 M8").has_value())
        << "a screw with no length at all was accepted";
    EXPECT_FALSE(LookUpFastener("ISO 4762 M9x30").has_value());

    // A LENGTH ON SOMETHING THAT HAS NONE names nothing.
    EXPECT_FALSE(LookUpFastener("ISO 4032 M8x30").has_value());
    EXPECT_FALSE(LookUpFastener("ISO 7089 M8x2").has_value());

    // A standard this build does not hold is refused rather than guessed from
    // the size -- which would put a nut where a screw was asked for.
    EXPECT_FALSE(LookUpFastener("ISO 4014 M8x30").has_value());
    EXPECT_FALSE(LookUpFastener("M8x30").has_value());
    EXPECT_FALSE(LookUpFastener("").has_value());
}

TEST(OcctStandardPartTest, M45_LIB_005_TheDesignationARoundTripGivesBackIsTheOneItWasAskedBy) {
    for (const char* text : {"ISO 4762 M8x30", "ISO 4762 M12x100", "ISO 4032 M6",
                             "ISO 7089 M10"}) {
        const std::optional<FastenerSpec> spec = LookUpFastener(text);
        ASSERT_TRUE(spec.has_value()) << text;
        EXPECT_EQ(spec->designation(), text) << "the library renamed the part it was given";
        // ...and the path a document stores round-trips too, which is what
        // makes an instance able to name one at all.
        EXPECT_EQ(FastenerOfPath(StandardPartPath(*spec))->designation(), spec->designation());
    }
    EXPECT_FALSE(FastenerOfPath("D:/parts/bolt.ep3d").has_value())
        << "an ordinary file path was read as a catalogue item";
    EXPECT_TRUE(IsStandardPartPath("std:ISO 4032 M6"));
    EXPECT_FALSE(IsStandardPartPath("D:/parts/bolt.ep3d"));
}

TEST(OcctStandardPartTest, M45_LIB_006_AnAssemblyCanCONTAINABoltAndCountIt) {
    // The whole point of the milestone: before it, an assembly could describe
    // a bolted joint and not contain a bolt.
    AssemblyDocument assembly{"Joint"};
    const std::optional<FastenerSpec> screw = LookUpFastener("ISO 4762 M8x30");
    const std::optional<FastenerSpec> nut = LookUpFastener("ISO 4032 M8");
    ASSERT_TRUE(screw.has_value());
    ASSERT_TRUE(nut.has_value());

    for (int i = 1; i <= 4; ++i)
        assembly.addInstance("Screw" + std::to_string(i), StandardPartPath(*screw), "");
    assembly.addInstance("Nut1", StandardPartPath(*nut), "");

    // AND THE PARTS LIST COUNTS THEM AS PARTS, because the library arrives as
    // a source path and nothing downstream had to learn a new kind of thing.
    const BomContents counted = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(counted.ok) << counted.why;
    ASSERT_EQ(counted.rows.size(), 2u) << "four identical screws were not one line";
    int screws = 0;
    for (const BomRow& row : counted.rows)
        if (row.sourcePath == StandardPartPath(*screw)) screws = row.quantity;
    EXPECT_EQ(screws, 4);
    EXPECT_EQ(counted.totalQuantity(), 5);
}

TEST(OcctStandardPartTest, M45_LIB_007_AnAssemblyOfLibraryPartsBUILDS) {
    // The resolver's own path, end to end: an instance naming a catalogue item
    // has to produce a solid, or everything above -- views, mass, clashes --
    // is describing nothing.
    OcctGeometryKernel kernel;
    AssemblyDocument assembly{"Joint"};
    assembly.setGeometryKernel(&kernel);
    const std::optional<FastenerSpec> screw = LookUpFastener("ISO 4762 M8x30");
    ASSERT_TRUE(screw.has_value());
    Instance& one = assembly.addInstance("Screw1", StandardPartPath(*screw), "");
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_EQ(one.currentState(), ComputeState::Valid);
    EXPECT_TRUE(one.currentShape().isValid());

    // A CATALOGUE ITEM THAT IS NOT IN THE CATALOGUE FAILS LOUDLY, rather than
    // arriving as an empty instance that looks like a part nobody placed.
    Instance& ghost = assembly.addInstance("Ghost", "std:ISO 4762 M8x33", "");
    assembly.recompute();
    EXPECT_EQ(ghost.currentState(), ComputeState::Failed)
        << "a screw in a length nobody makes was built anyway";
}

} // namespace
