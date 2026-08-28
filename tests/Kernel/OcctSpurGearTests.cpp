// M58.3 -- do two generated gears actually mesh?
//
// Every other thing about a gear can be checked in arithmetic, and arithmetic
// is exactly what a wrong flank phase survives: the teeth come out the right
// shape, the right size and the right count, in the wrong PLACE, and the pair
// jams or rattles. The tooth-thickness assertion in the core tests catches the
// gross case; this is the one that puts the two solids at the centre distance
// the pair says and looks at what happens.
//
// The measurement is M55's: the volume two solids share. At the theoretical
// centre distance a standard pair meshes with zero backlash -- flanks tangent,
// no shared volume -- so any real overlap is teeth driven into each other.

#include "Core/Library/SpurGear.h"
#include "Core/Measure/ModelMeasure.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Document/PartDocument.h"
#include "Core/Drawing/BomTable.h"
#include "Core/Feature/ISolidFeature.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kSteelKgPerM3 = 7850.0;

struct Built {
    KernelShape shape;
    std::string why;
    std::unique_ptr<PartDocument> part;
};

Built Build(OcctGeometryKernel& kernel, const SpurGear& gear) {
    Built out;
    out.part = BuildSpurGear(gear);
    if (!out.part) {
        out.why = "refused: " + WhyGearRefused(gear);
        return out;
    }
    out.part->setGeometryKernel(&kernel);
    const DocumentRecomputeReport report = out.part->recompute();
    if (!report.success) {
        for (const RecomputeItemReport& item : report.items)
            if (!item.message.empty())
                out.why += (out.why.empty() ? "" : "; ") + item.message;
        if (out.why.empty()) out.why = "did not build";
        return out;
    }
    for (const auto& feature : out.part->bodies().front()->features())
        if (const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get()))
            out.shape = solid->currentShape();
    return out;
}

SpurGear Gear(double moduleMm, int teeth, double widthMm = 10.0) {
    SpurGear gear;
    gear.moduleMm = moduleMm;
    gear.teeth = teeth;
    gear.faceWidthMm = widthMm;
    return gear;
}

double ItemOf(const MeasureResult& measured, const std::string& label) {
    for (const MeasureItem& item : measured.items)
        if (item.label == label) return item.value;
    ADD_FAILURE() << "no item called " << label;
    return 0.0;
}

double OverlapOf(OcctGeometryKernel& kernel, const KernelShape& a, const KernelShape& b) {
    const MeasureResult between = MeasureBetweenSolids(kernel, a, b);
    EXPECT_TRUE(between.ok) << between.message;
    return ItemOf(between, "Overlap volume");
}

TEST(OcctSpurGearTest, M58_KRN_001_AGearWeighsWhatAGearOfThatSizeWeighs) {
    OcctGeometryKernel kernel;
    const SpurGear gear = Gear(2.0, 20);
    const Built built = Build(kernel, gear);
    ASSERT_TRUE(built.shape.isValid()) << built.why;

    const MeasureResult measured = MeasureSolid(kernel, built.shape, kSteelKgPerM3);
    ASSERT_TRUE(measured.ok) << measured.message;

    // A TOOTHED WHEEL SITS BETWEEN ITS ROOT AND TIP CIRCLES, and it is nearer
    // the pitch circle than either -- roughly, because a tooth and the space
    // beside it are about the same width. Bracketing it that way catches a
    // profile that came out as a plain disc (too heavy) or as spokes (too
    // light) without pretending to a precision the chord approximation does
    // not have.
    const double volume = ItemOf(measured, "Volume");
    const auto area = [](double diameter) { return kPi * diameter * diameter / 4.0; };
    EXPECT_LT(volume, area(gear.tipDiameterMm()) * gear.faceWidthMm);
    EXPECT_GT(volume, area(gear.rootDiameterMm()) * gear.faceWidthMm);
    EXPECT_NEAR(volume, area(gear.pitchDiameterMm()) * gear.faceWidthMm,
                area(gear.pitchDiameterMm()) * gear.faceWidthMm * 0.06)
        << "a gear is close to a disc at its pitch diameter, and this one is not";

    // THE SOLID IS AS WIDE AS THE TIP CIRCLE and as thick as the face width.
    const KernelBoundsResult bounds = kernel.boundsOfShape(built.shape);
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.x - bounds.min.x, gear.tipDiameterMm(), 0.02);
    EXPECT_NEAR(bounds.max.y - bounds.min.y, gear.tipDiameterMm(), 0.02);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, gear.faceWidthMm, 1e-6);
}

TEST(OcctSpurGearTest, M58_KRN_002_TWOGEARSATTHEDERIVEDCENTREDISTANCEMESH) {
    // THE TEST THAT CANNOT BE FAKED, and the analogue of M56's "does the frame
    // close". A flank drawn at the wrong phase gives teeth of the right shape
    // in the wrong place: the arithmetic all checks out and the pair jams.
    OcctGeometryKernel kernel;
    // THE SMALLEST PAIR THAT STILL MEANS SOMETHING. Four booleans between two
    // toothed solids is the expensive part of this suite, and the cost goes
    // with the face count -- a 20/40 pair took forty seconds where an 18/20
    // one takes a handful. The pair is still two DIFFERENT gears, which is
    // what the phase arithmetic has to get right; making them identical would
    // have hidden a half-tooth computed off the wrong gear.
    const SpurGear pinion = Gear(2.0, 18, 4.0);
    const SpurGear wheel = Gear(2.0, 20, 4.0);
    const double centres = CentreDistanceMm(pinion, wheel);
    ASSERT_NEAR(centres, 38.0, 1e-9);

    const Built first = Build(kernel, pinion);
    const Built second = Build(kernel, wheel);
    ASSERT_TRUE(first.shape.isValid()) << first.why;
    ASSERT_TRUE(second.shape.isValid()) << second.why;

    // BOTH GEARS ARE BUILT WITH A TOOTH CENTRED ON +X, so at the meshing
    // position the second has to be turned by half a tooth to put a SPACE
    // where the first has a tooth. That half-tooth is the whole phase
    // relationship of a gear pair, and it is arithmetic rather than a fitted
    // number: half of one pitch, on the wheel's own tooth count.
    const double halfTooth = kPi / static_cast<double>(wheel.teeth);
    const ShapeResult turned =
        kernel.rotateShape(second.shape, Vec3{}, Vec3{0.0, 0.0, 1.0}, halfTooth);
    ASSERT_EQ(turned.error, KernelError::None) << turned.message;
    const ShapeResult placed = kernel.translateShape(turned.shape, Vec3{centres, 0.0, 0.0});
    ASSERT_EQ(placed.error, KernelError::None) << placed.message;

    // THEY TOUCH AND DO NOT BITE. A standard pair at the theoretical centre
    // distance has zero backlash: the flanks are tangent. What overlap remains
    // is the chord approximation of the involute, which is microns of profile
    // over ten millimetres of face -- so a cubic millimetre is a generous
    // ceiling and a jammed pair is thousands.
    const double overlap = OverlapOf(kernel, first.shape, placed.shape);
    EXPECT_LT(overlap, 1.0)
        << "the teeth are driven into each other, so the flanks are at the wrong phase";

    // AND THE TEST HAS TEETH ITSELF. Half a tooth the other way puts tooth on
    // tooth, which is what a phase error looks like -- if this does not
    // overlap grossly then the check above is not measuring anything.
    const ShapeResult jammed =
        kernel.translateShape(second.shape, Vec3{centres, 0.0, 0.0});
    ASSERT_EQ(jammed.error, KernelError::None);
    EXPECT_GT(OverlapOf(kernel, first.shape, jammed.shape), 10.0)
        << "tooth against tooth did not overlap, so this pair is not really in mesh at all";

    // ...and moved apart by a millimetre they do not touch, which says the
    // centre distance is a real fit rather than a number that happened to
    // leave them clear.
    const ShapeResult apart =
        kernel.translateShape(turned.shape, Vec3{centres + 1.0, 0.0, 0.0});
    ASSERT_EQ(apart.error, KernelError::None);
    EXPECT_NEAR(OverlapOf(kernel, first.shape, apart.shape), 0.0, 1e-9);
    // ...and a millimetre closer they bite.
    const ShapeResult tight =
        kernel.translateShape(turned.shape, Vec3{centres - 1.0, 0.0, 0.0});
    ASSERT_EQ(tight.error, KernelError::None);
    EXPECT_GT(OverlapOf(kernel, first.shape, tight.shape), 1.0);
}

TEST(OcctSpurGearTest, M58_KRN_003_AGearIsResolvedFromItsPathLikeAnyOtherPart) {
    // The `gear:` scheme, doing what `std:` and `frm:` do: nothing below
    // mentions gears.
    OcctGeometryKernel kernel;
    AssemblyDocument assembly{"Drive"};
    assembly.setGeometryKernel(&kernel);
    Instance& pinion = assembly.addInstance("Pinion", "gear:m2 z18 b4", "");
    assembly.addInstance("Wheel", "gear:m2 z20 b4", "");
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_EQ(pinion.currentState(), ComputeState::Valid);
    ASSERT_TRUE(pinion.currentShape().isValid());

    const KernelBoundsResult bounds = kernel.boundsOfShape(pinion.currentShape());
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.x - bounds.min.x, 40.0, 0.02);

    // TWO ROWS ON A PARTS LIST, because the path is the gear.
    const BomContents counted = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(counted.ok) << counted.why;
    EXPECT_EQ(counted.rows.size(), 2u);

    // AN UNDERCUT PINION FAILS LOUDLY, with the reason being the tooth count
    // rather than a note about syntax -- somebody who typed a valid path and
    // got told it was malformed would go and check their typing.
    Instance& bad = assembly.addInstance("Tiny", "gear:m2 z12 b4", "");
    const DocumentRecomputeReport report = assembly.recompute();
    EXPECT_EQ(bad.currentState(), ComputeState::Failed);
    std::string said;
    for (const RecomputeItemReport& item : report.items)
        if (item.id == bad.id()) said = item.message;
    EXPECT_NE(said.find("undercut"), std::string::npos)
        << "the reason given was about syntax rather than about the tooth count: " << said;
}

} // namespace
