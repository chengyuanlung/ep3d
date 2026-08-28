// M60 -- the helix, and the spring wound on it.
//
// THE FIRST TEST IS THE ONE THAT MATTERS, and it is a length.
//
// A helix laid out on a cylinder's (u, v) parameters is exact -- but the
// parameterisation is easy to get wrong in a way that produces a perfectly
// good-looking coil of the wrong length. Give a gp_Dir2d and a distance and
// the direction is normalised, so the coil comes out short by cos(the helix
// angle): every turn present, every diameter right, the wire a few percent
// too short. A spring's rate depends on the wire's length, so what comes out
// is a spring that is stiffer than it says it is, in a machine, and nothing
// on the screen looks wrong.
//
// So the wire is WEIGHED, against the length a helix has: turns times
// sqrt((2 pi R)^2 + pitch^2).

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Document/PartDocument.h"
#include "Core/Drawing/BomTable.h"
#include "Core/Feature/HelixFeature.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/ShapeKind.h"
#include "Core/Library/CompressionSpring.h"
#include "Core/Measure/ModelMeasure.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kSteelKgPerM3 = 7850.0;

double ItemOf(const MeasureResult& measured, const std::string& label) {
    for (const MeasureItem& item : measured.items)
        if (item.label == label) return item.value;
    ADD_FAILURE() << "no item called " << label;
    return 0.0;
}

// The length of wire a helix takes: one turn is the hypotenuse of the
// circumference and the rise, and there are `turns` of them.
double WireLengthMm(double meanRadiusMm, double pitchMm, double turns) {
    const double round = 2.0 * kPi * meanRadiusMm;
    return turns * std::sqrt(round * round + pitchMm * pitchMm);
}

KernelShape Coil(OcctGeometryKernel& kernel, double wireMm, double meanMm, double pitchMm,
                 double turns) {
    HelixDefinition helix;
    helix.wireRadiusMm = wireMm / 2.0;
    helix.helixRadiusMm = meanMm / 2.0;
    helix.pitchMm = pitchMm;
    helix.turns = turns;
    const ShapeResult made = kernel.createHelicalWire(helix);
    EXPECT_EQ(made.error, KernelError::None) << made.message;
    return made.shape;
}

TEST(OcctSpringTest, M60_KRN_001_THEWIREISASLONGASAHELIXOFTHATPITCHIS) {
    OcctGeometryKernel kernel;
    const double wire = 2.0;
    const double mean = 16.0;
    const double pitch = 5.0;
    const double turns = 8.0;

    const KernelShape coil = Coil(kernel, wire, mean, pitch, turns);
    ASSERT_TRUE(coil.isValid());
    ASSERT_EQ(kernel.kindOfShape(coil), ShapeKind::Solid);

    const MeasureResult measured = MeasureSolid(kernel, coil, kSteelKgPerM3);
    ASSERT_TRUE(measured.ok) << measured.message;

    // A ROUND WIRE OF THAT LENGTH. Within a fraction of a percent -- the ends
    // of the coil are cut square across the wire rather than perpendicular to
    // the axis, which is a wire-radius' worth of difference at each end.
    const double expected =
        kPi * (wire / 2.0) * (wire / 2.0) * WireLengthMm(mean / 2.0, pitch, turns);
    EXPECT_NEAR(ItemOf(measured, "Volume"), expected, expected * 0.01)
        << "the coil is not as long as a helix of this pitch and diameter";

    // AND THE COS(HELIX ANGLE) MISTAKE IS OUTSIDE THAT WINDOW. At this pitch
    // the angle is about 5.7 degrees, so a coil laid out by arc length instead
    // of by turn would be half a percent short of the RIGHT answer but a whole
    // turn short of the right SHAPE -- so the envelope is checked too.
    const KernelBoundsResult bounds = kernel.boundsOfShape(coil);
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.x - bounds.min.x, mean + wire, 0.01) << "the coil is the wrong width";
    EXPECT_NEAR(bounds.max.y - bounds.min.y, mean + wire, 0.01);
    // Eight turns at 5 mm apart, plus the wire's own thickness.
    EXPECT_NEAR(bounds.max.z - bounds.min.z, pitch * turns + wire, 0.02)
        << "the coil is not as tall as eight turns of this pitch";
}

TEST(OcctSpringTest, M60_KRN_002_ACoilThatWouldWindThroughItselfIsREFUSED) {
    // A pitch under the wire's thickness is thickness wound onto thickness.
    // OCCT does not fail on it -- it builds a self-intersecting pipe, and what
    // comes back looks like a solid and has the wrong volume. Refused before
    // it gets there.
    OcctGeometryKernel kernel;
    HelixDefinition tooTight;
    tooTight.wireRadiusMm = 1.0;
    tooTight.helixRadiusMm = 8.0;
    tooTight.pitchMm = 1.0; // half the wire's thickness
    tooTight.turns = 4.0;
    const ShapeResult refused = kernel.createHelicalWire(tooTight);
    EXPECT_NE(refused.error, KernelError::None) << "a coil wound through itself was built";
    EXPECT_NE(refused.message.find("winds through itself"), std::string::npos)
        << refused.message;

    // AND IT HAS TO CLEAR THE AXIS. A wire thicker than the coil is wide folds
    // through the middle.
    HelixDefinition throughTheMiddle;
    throughTheMiddle.wireRadiusMm = 5.0;
    throughTheMiddle.helixRadiusMm = 3.0;
    throughTheMiddle.pitchMm = 12.0;
    throughTheMiddle.turns = 3.0;
    EXPECT_NE(kernel.createHelicalWire(throughTheMiddle).error, KernelError::None);

    // Nothing at all is nothing at all.
    EXPECT_NE(kernel.createHelicalWire(HelixDefinition{}).error, KernelError::None);
}

TEST(OcctSpringTest, M60_KRN_003_ASpringIsAPartWithAFeatureTreeLikeAnyOther) {
    OcctGeometryKernel kernel;
    const std::optional<CompressionSpring> spring = LookUpSpring("d2 D16 n8 L50");
    ASSERT_TRUE(spring.has_value());

    std::unique_ptr<PartDocument> part = BuildCompressionSpring(*spring, kernel);
    ASSERT_NE(part, nullptr);
    ASSERT_TRUE(part->recompute().success);

    const Body* body = part->bodies().front().get();
    ASSERT_FALSE(body->features().empty());
    const auto* coil = dynamic_cast<const HelixFeature*>(body->features().front().get());
    ASSERT_NE(coil, nullptr) << "the spring is not built from a helix feature";
    ASSERT_TRUE(coil->currentShape().isValid());

    // THE ENVELOPE IS THE SPRING'S, which is what a clearance check needs: the
    // outside diameter it has to fit inside, and the free length it takes up.
    const KernelBoundsResult bounds = kernel.boundsOfShape(coil->currentShape());
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.x - bounds.min.x, spring->outerDiameterMm(), 0.02);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, spring->freeLengthMm, 0.05)
        << "the modelled spring is not its own free length";

    // IT IS PARAMETRIC, which is the whole reason it is a feature and not a
    // shape: change a number and the coil is a different coil.
    const Parameter* mean = nullptr;
    for (const auto& parameter : part->parameters().items())
        if (parameter->name() == "Mean diameter") mean = parameter.get();
    ASSERT_NE(mean, nullptr);
    const double before =
        ItemOf(MeasureSolid(kernel, coil->currentShape(), 0.0), "Volume");
    ASSERT_TRUE(part->setParameterValue(mean->id(), 24.0));
    ASSERT_TRUE(part->recompute().success);
    const double after = ItemOf(MeasureSolid(kernel, coil->currentShape(), 0.0), "Volume");
    EXPECT_GT(after, before * 1.3) << "the mean diameter is not driving the coil";
}

TEST(OcctSpringTest, M60_KRN_004_ASpringIsPlacedAndCountedLikeAnyOtherPart) {
    // The fourth use of the scheme M45 introduced, and again nothing below
    // mentions springs.
    OcctGeometryKernel kernel;
    AssemblyDocument assembly{"Valve"};
    assembly.setGeometryKernel(&kernel);
    Instance& first = assembly.addInstance("Return spring", "spr:d2 D16 n8 L50", "");
    assembly.addInstance("Spare", "spr:d2 D16 n8 L50", "");
    assembly.addInstance("Detent", "spr:d1 D8 n6 L20", "");
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_EQ(first.currentState(), ComputeState::Valid);
    ASSERT_TRUE(first.currentShape().isValid());

    // TWO ROWS, NOT THREE. The two identical springs are one line with a
    // quantity of two, because the path is the spring.
    const BomContents counted = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(counted.ok) << counted.why;
    ASSERT_EQ(counted.rows.size(), 2u);
    EXPECT_EQ(counted.totalQuantity(), 3);

    // A SPRING NOBODY CAN WIND FAILS LOUDLY, with the reason being the index
    // rather than a note about syntax -- somebody who typed a valid path and
    // got told it was malformed would go and check their typing.
    Instance& tight = assembly.addInstance("Impossible", "spr:d4 D8 n8 L50", "");
    const DocumentRecomputeReport report = assembly.recompute();
    EXPECT_EQ(tight.currentState(), ComputeState::Failed);
    std::string said;
    for (const RecomputeItemReport& item : report.items)
        if (item.id == tight.id()) said = item.message;
    EXPECT_NE(said.find("index"), std::string::npos) << said;
}

TEST(OcctSpringTest, M60_KRN_005_AHelixSurvivesASaveAndAnUNDO) {
    // v55. A feature whose four references are not written down is a part that
    // stops building the next time the file is opened -- and the snapshot path
    // is the one M59 found nothing was testing, because saving goes straight to
    // the feature while UNDO restores from the snapshot.
    OcctGeometryKernel kernel;
    PartDocument part{"Spring"};
    part.setGeometryKernel(&kernel);
    Body& body = part.addBody("Coil");
    Parameter& wire = part.addParameter("Wire", 2.0, UnitType::Millimeter);
    Parameter& mean = part.addParameter("Mean diameter", 16.0, UnitType::Millimeter);
    Parameter& pitch = part.addParameter("Pitch", 5.0, UnitType::Millimeter);
    Parameter& turns = part.addParameter("Turns", 8.0, UnitType::Unitless);
    const ObjectId coilId =
        part.addHelixFeature(body, "Coil", wire.id(), mean.id(), pitch.id(), turns.id()).id();
    ASSERT_TRUE(part.recompute().success);

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(part, out));
    const std::string saved = out.str();
    EXPECT_NE(saved.find("\"Helix\""), std::string::npos);
    EXPECT_NE(saved.find("wireDiameterParameterId"), std::string::npos);

    std::istringstream in(saved);
    LoadResult reopened = loadPartDocument(in);
    ASSERT_TRUE(static_cast<bool>(reopened)) << reopened.message;
    reopened.document->setGeometryKernel(&kernel);
    const auto* back = dynamic_cast<const HelixFeature*>(
        reopened.document->bodies().front()->features().front().get());
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->wireDiameterParameterId(), wire.id());
    EXPECT_EQ(back->meanDiameterParameterId(), mean.id());
    EXPECT_EQ(back->pitchParameterId(), pitch.id());
    EXPECT_EQ(back->turnsParameterId(), turns.id());
    EXPECT_TRUE(reopened.document->recompute().success);

    // AND THE SAME FOUR COME BACK FROM AN UNDO, which is a different path
    // through different code.
    ASSERT_TRUE(part.removeObject(coilId));
    ASSERT_TRUE(part.undo());
    const auto* restored = dynamic_cast<const HelixFeature*>(
        part.bodies().front()->features().front().get());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->pitchParameterId(), pitch.id())
        << "the coil came back without its pitch, so the part no longer builds";
    EXPECT_TRUE(part.recompute().success);
}

} // namespace
