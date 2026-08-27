// M52.2 -- the contour flange, END TO END: a real solid out of a real kernel.
//
// SheetContourTests pins the walk as arithmetic. What only a kernel can say is
// whether the section it produces is a WIRE OCCT will accept -- closed,
// non-self-intersecting, every arc running the way the loop needs -- and
// whether the solid that comes out holds the metal the section says it does.
//
// AND THE ONE THAT LOOKS LIKE A BUG: the solid's volume is NOT the flat
// blank's length times width times thickness. It differs by exactly what the K
// factor says, because K is a statement about metal stretching and not a
// property of the folded shape. A test that demanded they match would be
// demanding K = 0.5, quietly, for every material.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/SheetContour.h"
#include "Core/Feature/SheetContourFeature.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

ContourStep Step(double flangeMm, double angleDeg, double radiusMm, bool left = true) {
    ContourStep step;
    step.flangeMm = flangeMm;
    step.bend.angleDeg = angleDeg;
    step.bend.innerRadiusMm = radiusMm;
    step.turnsLeft = left;
    return step;
}

SheetContour Channel() {
    SheetContour contour;
    contour.steps.push_back(Step(30.0, 90.0, 2.0, true));
    contour.steps.push_back(Step(60.0, 90.0, 2.0, true));
    contour.lastFlangeMm = 30.0;
    return contour;
}

SheetMetalSettings MildSteel2mm() {
    SheetMetalSettings settings;
    settings.isSheetMetal = true;
    settings.thicknessMm = 2.0;
    settings.material = SheetMaterial::MildSteelAluminium;
    settings.defaultBendRadiusMm = 2.0;
    return settings;
}

// WHAT WENT WRONG, out of the report the engine hands back -- a feature does
// not keep its own last message, and a bare "recompute failed" is a test that
// makes you rebuild to find out anything.
std::string WhyItFailed(const DocumentRecomputeReport& report) {
    std::string out;
    for (const RecomputeItemReport& item : report.items)
        if (!item.message.empty()) out += item.message + "; ";
    return out.empty() ? std::string("no message") : out;
}

struct Folded {
    OcctGeometryKernel kernel;
    PartDocument part{"Bracket"};
    Body* body = nullptr;
    SheetContourFeature* feature = nullptr;
    ObjectId widthId = kInvalidObjectId;

    explicit Folded(const SheetContour& contour, double widthMm = 100.0) {
        part.setGeometryKernel(&kernel);
        part.setSheetMetal(MildSteel2mm());
        Parameter& width = part.addParameter("W", widthMm, UnitType::Millimeter);
        widthId = width.id();
        body = &part.addBody("Sheet");
        feature = &part.addSheetContourFeature(*body, "Contour", contour, width.id());
    }
};

TEST(OcctSheetContourTest, M52_KRN_001_TheSectionIsAWireTheKernelAccepts) {
    // A closed loop on paper is not the same as a wire OCCT will build: an arc
    // left running the other way closes geometrically and folds the face back
    // on itself, and the message that comes back is about topology rather than
    // about the part.
    Folded folded{Channel()};
    const DocumentRecomputeReport built1 = folded.part.recompute();
    ASSERT_TRUE(built1.success) << WhyItFailed(built1);
    EXPECT_EQ(folded.feature->currentState(), ComputeState::Valid);
    EXPECT_TRUE(folded.feature->currentShape().isValid());
    EXPECT_EQ(folded.kernel.countSolids(folded.feature->currentShape()), 1)
        << "the section built more than one solid, so the wire was not one loop";
}

TEST(OcctSheetContourTest, M52_KRN_002_TheSolidHoldsWhatTheSectionSays) {
    // The section's area times the width, to the kernel's own arithmetic.
    // Anything else means the walk and the solid disagree about the part.
    const double width = 100.0;
    const SheetContour channel = Channel();
    Folded folded{channel, width};
    const DocumentRecomputeReport built1 = folded.part.recompute();
    ASSERT_TRUE(built1.success) << WhyItFailed(built1);

    const KernelMassPropertiesResult mass =
        folded.kernel.calculateMassProperties(folded.feature->currentShape());
    ASSERT_TRUE(mass) << mass.message;
    const double expected = FoldedSectionAreaMm2(channel, 2.0) * width;
    EXPECT_NEAR(mass.properties.volumeMm3, expected, expected * 1e-6)
        << "the solid does not hold what the section says it does";
}

TEST(OcctSheetContourTest, M52_KRN_003_TheSolidIsNOTTheFlatBlankAndThatIsTheKFactor) {
    // THE ONE THAT LOOKS LIKE A BUG. Making these agree sets K to one half and
    // throws the material table away -- and nothing downstream would say so,
    // because the blank would still be self-consistent.
    const double width = 100.0;
    const double t = 2.0;
    const SheetContour channel = Channel();
    Folded folded{channel, width};
    const DocumentRecomputeReport built1 = folded.part.recompute();
    ASSERT_TRUE(built1.success) << WhyItFailed(built1);

    const KernelMassPropertiesResult mass =
        folded.kernel.calculateMassProperties(folded.feature->currentShape());
    ASSERT_TRUE(mass) << mass.message;

    const FlatPatternResult flat =
        ContourFlatLength(channel, SheetMaterial::MildSteelAluminium, t);
    ASSERT_TRUE(flat.ok) << flat.why;
    const double blankVolume = flat.lengthMm * width * t;

    EXPECT_GT(std::fabs(mass.properties.volumeMm3 - blankVolume), 1.0)
        << "the folded solid and the flat blank hold the same metal, which means K has been "
           "set to one half somewhere";

    // ...AND BY EXACTLY THE PREDICTED AMOUNT, which is what makes this a
    // measurement rather than a shrug. Per bend: angle * T^2 * (1/2 - K),
    // times the width.
    const double k = KFactorFor(SheetMaterial::MildSteelAluminium, 2.0, t);
    double predicted = 0.0;
    for (const ContourStep& step : channel.steps)
        predicted += (step.bend.angleDeg * kPi / 180.0) * t * t * (0.5 - k) * width;
    EXPECT_NEAR(mass.properties.volumeMm3 - blankVolume, predicted,
                std::fabs(predicted) * 1e-4);
}

TEST(OcctSheetContourTest, M52_KRN_004_TheThicknessFollowsThePartAndNotTheFeature) {
    // THE REASON THE FEATURE DOES NOT CARRY ONE. Set the part to 3 mm and the
    // walls have to follow -- a feature holding its own thickness would go on
    // building 2 mm walls that fold to a blank the part says is a different
    // length, with both answers self-consistent.
    //
    // The radii here are R4, which mild steel takes at both thicknesses. That
    // is not incidental: see the second half of this test.
    const double width = 100.0;
    SheetContour channel;
    channel.steps.push_back(Step(30.0, 90.0, 4.0, true));
    channel.steps.push_back(Step(60.0, 90.0, 4.0, true));
    channel.lastFlangeMm = 30.0;

    Folded folded{channel, width};
    const DocumentRecomputeReport built1 = folded.part.recompute();
    ASSERT_TRUE(built1.success) << WhyItFailed(built1);
    const KernelMassPropertiesResult thin =
        folded.kernel.calculateMassProperties(folded.feature->currentShape());
    ASSERT_TRUE(thin) << thin.message;
    EXPECT_NEAR(thin.properties.volumeMm3, FoldedSectionAreaMm2(channel, 2.0) * width,
                thin.properties.volumeMm3 * 1e-6);

    SheetMetalSettings thicker = MildSteel2mm();
    thicker.thicknessMm = 3.0;
    thicker.defaultBendRadiusMm = 3.0;
    ASSERT_TRUE(folded.part.setSheetMetal(thicker));
    folded.part.markDirty(folded.feature->id());
    const DocumentRecomputeReport built2 = folded.part.recompute();
    ASSERT_TRUE(built2.success) << WhyItFailed(built2);

    const KernelMassPropertiesResult thick =
        folded.kernel.calculateMassProperties(folded.feature->currentShape());
    ASSERT_TRUE(thick) << thick.message;
    EXPECT_GT(thick.properties.volumeMm3, thin.properties.volumeMm3 * 1.4)
        << "the part got half again as thick and the solid did not follow";
    EXPECT_NEAR(thick.properties.volumeMm3, FoldedSectionAreaMm2(channel, 3.0) * width,
                thick.properties.volumeMm3 * 1e-6);
}

TEST(OcctSheetContourTest, M52_KRN_004B_ThickeningAPartCanMakeItsOwnBendsIllegal) {
    // AND THE PART SAYS SO RATHER THAN BUILDING IT.
    //
    // Found by writing the test above with R2 bends and taking the part from
    // 2 mm to 3: mild steel's tightest bend is one thickness, so radii that
    // were fine at 2 crack at 3. The feature does not quietly keep its old
    // thickness, and it does not quietly open the radii either -- it fails,
    // with the reason, and the drafter decides whether to open the bends or
    // stay thin.
    //
    // The alternative is a part that got thicker, rebuilt without a word, and
    // splits along every bend at the press brake.
    const double width = 100.0;
    Folded folded{Channel(), width};   // R2 bends, legal at 2 mm
    const DocumentRecomputeReport built1 = folded.part.recompute();
    ASSERT_TRUE(built1.success) << WhyItFailed(built1);

    SheetMetalSettings thicker = MildSteel2mm();
    thicker.thicknessMm = 3.0;
    thicker.defaultBendRadiusMm = 3.0;
    ASSERT_TRUE(folded.part.setSheetMetal(thicker));
    folded.part.markDirty(folded.feature->id());
    const DocumentRecomputeReport built2 = folded.part.recompute();

    EXPECT_FALSE(built2.success)
        << "the part got thicker and its R2 bends were built anyway, which cracks";
    EXPECT_NE(WhyItFailed(built2).find("cracks"), std::string::npos) << WhyItFailed(built2);
    EXPECT_EQ(folded.feature->currentState(), ComputeState::Failed);
    // AND THE LAST GOOD SHAPE IS STILL THERE, byte for byte -- a failed
    // rebuild does not leave the part with nothing (ADR-M3-001).
    EXPECT_TRUE(folded.feature->currentShape().isValid());
}

TEST(OcctSheetContourTest, M52_KRN_004C_TurningSheetMetalOFFStopsTheContourBuilding) {
    // FOUND BY THE MUTATION GATE, and it is a real thing to do: a part is made
    // sheet metal, folded, and then somebody turns that off.
    //
    // The document refuses to ADD a contour to a plain part, which is why the
    // feature's own guard looked untested -- but this is the way in. Without
    // it the feature would fold to whatever thickness happened to be left in
    // the setting, which for a cleared one is nothing.
    Folded folded{Channel(), 100.0};
    const DocumentRecomputeReport built1 = folded.part.recompute();
    ASSERT_TRUE(built1.success) << WhyItFailed(built1);

    ASSERT_TRUE(folded.part.clearSheetMetal());
    folded.part.markDirty(folded.feature->id());
    const DocumentRecomputeReport built2 = folded.part.recompute();
    EXPECT_FALSE(built2.success)
        << "the part stopped being sheet metal and the fold carried on regardless";
    EXPECT_NE(WhyItFailed(built2).find("not sheet metal"), std::string::npos)
        << WhyItFailed(built2);
    EXPECT_EQ(folded.feature->currentState(), ComputeState::Failed);
}

TEST(OcctSheetContourTest, M52_KRN_004D_AWidthOfNothingIsASectionAndNotAPart) {
    // A contour extruded by nothing is a drawing of a cross-section. The
    // kernel would refuse it too, but with a message about a distance rather
    // than about the part -- and a zero that reached the kernel is a zero
    // nobody was asked about.
    Folded folded{Channel(), 100.0};
    const DocumentRecomputeReport built1 = folded.part.recompute();
    ASSERT_TRUE(built1.success) << WhyItFailed(built1);

    ASSERT_TRUE(folded.part.setParameterValue(folded.widthId, 0.0));
    const DocumentRecomputeReport built2 = folded.part.recompute();
    EXPECT_FALSE(built2.success) << "a contour with no width was built as a part";
    EXPECT_NE(WhyItFailed(built2).find("width"), std::string::npos) << WhyItFailed(built2);
}

TEST(OcctSheetContourTest, M52_KRN_005_APartThatIsNotSheetMetalIsRefusedBeforeItIsBuilt) {
    // A contour on a part that has not said what it is made of would sit in
    // the tree as a feature that will not build, pointing at a thickness
    // nobody has chosen.
    OcctGeometryKernel kernel;
    PartDocument part{"Plate"};
    part.setGeometryKernel(&kernel);
    Parameter& width = part.addParameter("W", 100.0, UnitType::Millimeter);
    Body& body = part.addBody("Sheet");
    EXPECT_THROW(part.addSheetContourFeature(body, "Contour", Channel(), width.id()),
                 std::invalid_argument);
    // AND THE REASON SAYS WHAT TO DO ABOUT IT. Without this guard the contour
    // still fails -- on the thickness of nothing, one check further down --
    // but the message is about a section having no thickness, which is not
    // advice to somebody who has not said what the part is made of.
    try {
        part.addSheetContourFeature(body, "Contour", Channel(), width.id());
        FAIL() << "a contour on a plain part was accepted";
    } catch (const std::invalid_argument& refused) {
        EXPECT_NE(std::string(refused.what()).find("what it is made of"), std::string::npos)
            << refused.what();
    }
}

TEST(OcctSheetContourTest, M52_KRN_005B_ABendThatCracksIsRefusedWHENADDEDNotLater) {
    // A contour with a radius the material will not take, added to a proper
    // sheet metal part. Checked at the door rather than at the next rebuild:
    // otherwise it sits in the tree as a feature that will not build, and the
    // drafter has moved on by the time it says so.
    Folded folded{Channel(), 100.0};
    SheetContour tight = Channel();
    tight.steps[0].bend.innerRadiusMm = 0.5;   // mild steel at 2 mm needs 2
    Parameter& width = folded.part.addParameter("W2", 100.0, UnitType::Millimeter);
    EXPECT_THROW(folded.part.addSheetContourFeature(*folded.body, "Bad", tight, width.id()),
                 std::invalid_argument);
    try {
        folded.part.addSheetContourFeature(*folded.body, "Bad", tight, width.id());
        FAIL() << "a bend that cracks was added and left to fail later";
    } catch (const std::invalid_argument& refused) {
        EXPECT_NE(std::string(refused.what()).find("cracks"), std::string::npos)
            << refused.what();
    }
}

TEST(OcctSheetContourTest, M52_KRN_006_AZFoldsToADifferentSolidFromAChannel) {
    // Same three lengths, same two right angles, and the only difference is
    // which way each bend turns. The BLANK is identical -- which is exactly
    // why the blank cannot be the only thing the shop is given.
    const double width = 100.0;
    SheetContour zed;
    zed.steps.push_back(Step(30.0, 90.0, 2.0, true));
    zed.steps.push_back(Step(60.0, 90.0, 2.0, false));
    zed.lastFlangeMm = 30.0;

    Folded channel{Channel(), width};
    Folded shape{zed, width};
    const DocumentRecomputeReport built1 = channel.part.recompute();
    ASSERT_TRUE(built1.success) << WhyItFailed(built1);
    const DocumentRecomputeReport built2 = shape.part.recompute();
    ASSERT_TRUE(built2.success) << WhyItFailed(built2);

    const KernelBoundsResult channelBox =
        channel.kernel.boundsOfShape(channel.feature->currentShape());
    const KernelBoundsResult zedBox = shape.kernel.boundsOfShape(shape.feature->currentShape());
    ASSERT_TRUE(channelBox.ok) << channelBox.message;
    ASSERT_TRUE(zedBox.ok) << zedBox.message;

    const double channelWidth = channelBox.max.x - channelBox.min.x;
    const double zedWidth = zedBox.max.x - zedBox.min.x;
    EXPECT_GT(std::fabs(channelWidth - zedWidth), 1.0)
        << "the channel and the Z folded to the same shape";

    // ...and they weigh the same, because the same metal went into both.
    const KernelMassPropertiesResult a =
        channel.kernel.calculateMassProperties(channel.feature->currentShape());
    const KernelMassPropertiesResult b =
        shape.kernel.calculateMassProperties(shape.feature->currentShape());
    ASSERT_TRUE(a) << a.message;
    ASSERT_TRUE(b) << b.message;
    EXPECT_NEAR(a.properties.volumeMm3, b.properties.volumeMm3,
                a.properties.volumeMm3 * 1e-6);
}

} // namespace
