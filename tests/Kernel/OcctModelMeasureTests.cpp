// M55.2 -- measuring a solid, and the question this program cannot answer.
//
// The valuable part of this file is the refusal. "How far apart are these two
// bodies" is what a user asks of two solids, and EP3D cannot answer it: the
// kernel exposes no signed-distance query, and measureInterference returns a
// VOLUME -- zero for every pair that does not already overlap, and therefore
// silent about the gap.
//
// A tool that reported "0.0 mm" would give the same number for a hair's
// breadth and a metre, and it is the kind of number somebody clears a fixture
// with.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Measure/ModelMeasure.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

const MeasureItem* ItemNamed(const MeasureResult& result, const std::string& label) {
    for (const MeasureItem& item : result.items)
        if (item.label == label) return &item;
    return nullptr;
}

KernelShape Block(OcctGeometryKernel& kernel, double width, double depth, double height,
                  double atX = 0.0) {
    BoxDefinition box;
    box.widthMm = width;
    box.heightMm = height;
    box.depthMm = depth;
    ShapeResult made = kernel.createBox(box);
    EXPECT_EQ(made.error, KernelError::None) << made.message;
    if (atX == 0.0) return made.shape;
    ShapeResult moved = kernel.translateShape(made.shape, Vec3{atX, 0.0, 0.0});
    EXPECT_EQ(moved.error, KernelError::None) << moved.message;
    return moved.shape;
}

TEST(OcctModelMeasureTest, M55_KRN_001_ASolidSaysHowMuchOfItThereIs) {
    OcctGeometryKernel kernel;
    const KernelShape block = Block(kernel, 100.0, 40.0, 10.0);

    const MeasureResult measured = MeasureSolid(kernel, block, 0.0);
    ASSERT_TRUE(measured.ok) << measured.message;

    const MeasureItem* volume = ItemNamed(measured, "Volume");
    ASSERT_NE(volume, nullptr);
    EXPECT_NEAR(volume->value, 100.0 * 40.0 * 10.0, 1.0);
    // AND IT IS CUBIC MILLIMETRES, not millimetres. M18 paid for this once:
    // an area printed as "mm" is a plausible number with the wrong unit on it.
    EXPECT_EQ(volume->unit, MeasureUnit::CubicMillimetre);
    EXPECT_EQ(std::string(MeasureUnitSuffix(volume->unit)), "mm^3");

    // The extents are what "will it fit in the machine" asks about, and are
    // not any dimension on the drawing.
    //
    // A BOX'S FIELDS ARE NOT ITS AXES, which this test learned the hard way:
    // BoxDefinition's width, height and depth land on X, Y and Z in that
    // order, so a box built 100 x 40 x 10 as (width, depth, height) is 100
    // along X, 10 along Y and 40 along Z. Written out rather than assumed,
    // because the volume is right either way and only the per-axis numbers
    // say which is which.
    ASSERT_NE(ItemNamed(measured, "Extent X"), nullptr);
    EXPECT_NEAR(ItemNamed(measured, "Extent X")->value, 100.0, 1e-6);
    EXPECT_NEAR(ItemNamed(measured, "Extent Y")->value, 10.0, 1e-6);
    EXPECT_NEAR(ItemNamed(measured, "Extent Z")->value, 40.0, 1e-6);

    // AN EXTENT IS A SPAN AND NOT A CORNER. Found by the mutation gate: the
    // block above starts at the origin, so its far corner and its span are the
    // same number and reporting either passed. Moved along X they part, and
    // "how much room does it take" has only one right answer.
    const KernelShape moved = Block(kernel, 100.0, 40.0, 10.0, 500.0);
    const MeasureResult overThere = MeasureSolid(kernel, moved, 0.0);
    ASSERT_TRUE(overThere.ok) << overThere.message;
    EXPECT_NEAR(ItemNamed(overThere, "Extent X")->value, 100.0, 1e-6)
        << "the extent grew with the part's position, so it is a corner and not a span";
    // ...while the centre of mass DID move, which is what says the block
    // really is somewhere else.
    EXPECT_NEAR(ItemNamed(overThere, "Centre of mass X")->value,
                ItemNamed(measured, "Centre of mass X")->value + 500.0, 1e-6);
}

TEST(OcctModelMeasureTest, M55_KRN_002_NoMaterialMeansNoMassAndNotZeroGrams) {
    // "This part has no material yet" is a different sentence from "this part
    // weighs nothing", and only one of them belongs in a lifting calculation.
    OcctGeometryKernel kernel;
    const KernelShape block = Block(kernel, 100.0, 40.0, 10.0);

    const MeasureResult without = MeasureSolid(kernel, block, 0.0);
    ASSERT_TRUE(without.ok) << without.message;
    EXPECT_EQ(ItemNamed(without, "Mass"), nullptr)
        << "a part with no material was given a mass anyway";

    // Steel, and the number is the one a shop would expect: 40 000 mm^3 of it
    // is a little over 300 grams.
    const MeasureResult withSteel = MeasureSolid(kernel, block, 7850.0);
    ASSERT_TRUE(withSteel.ok) << withSteel.message;
    const MeasureItem* mass = ItemNamed(withSteel, "Mass");
    ASSERT_NE(mass, nullptr);
    EXPECT_EQ(mass->unit, MeasureUnit::Kilogram);
    // AND IT PRINTS AS ONE. The enum being right is half of it; the suffix is
    // what a reader sees, and a mass shown as "mm" is a number somebody puts
    // in a lifting calculation.
    EXPECT_EQ(std::string(MeasureUnitSuffix(mass->unit)), "kg");
    EXPECT_NEAR(mass->value, 100.0 * 40.0 * 10.0 / 1.0e9 * 7850.0, 1e-6);
    EXPECT_NEAR(mass->value, 0.314, 0.01);
}

TEST(OcctModelMeasureTest, M55_KRN_003_TwoSolidsThatOverlapSayHowMuch) {
    OcctGeometryKernel kernel;
    const KernelShape a = Block(kernel, 100.0, 40.0, 10.0);
    const KernelShape b = Block(kernel, 100.0, 40.0, 10.0, 60.0);   // 40 of overlap

    const MeasureResult measured = MeasureBetweenSolids(kernel, a, b);
    ASSERT_TRUE(measured.ok) << measured.message;
    ASSERT_NE(ItemNamed(measured, "Overlapping"), nullptr);
    EXPECT_EQ(ItemNamed(measured, "Overlapping")->value, 1.0);
    ASSERT_NE(ItemNamed(measured, "Overlap volume"), nullptr);
    EXPECT_NEAR(ItemNamed(measured, "Overlap volume")->value, 40.0 * 40.0 * 10.0, 1.0);
    // Nothing is refused here: the question asked has an answer.
    EXPECT_TRUE(measured.message.empty());
}

TEST(OcctModelMeasureTest, M55_KRN_004_TheGapIsREFUSEDRatherThanReportedAsZero) {
    // THE POINT OF THIS FILE. Two blocks a long way apart share no volume, and
    // volume is all the kernel can say -- so "0" is the overlap and NOT the
    // distance, and the tool has to be the thing that knows the difference.
    OcctGeometryKernel kernel;
    const KernelShape a = Block(kernel, 100.0, 40.0, 10.0);
    const KernelShape far = Block(kernel, 100.0, 40.0, 10.0, 500.0);

    const MeasureResult measured = MeasureBetweenSolids(kernel, a, far);
    ASSERT_TRUE(measured.ok) << "comparing two solids failed outright";
    EXPECT_EQ(ItemNamed(measured, "Overlapping")->value, 0.0);
    EXPECT_NEAR(ItemNamed(measured, "Overlap volume")->value, 0.0, 1e-9);

    // AND IT SAYS SO. Not silence, which reads as "nothing to report", and
    // certainly not a distance.
    EXPECT_FALSE(measured.message.empty())
        << "two solids that do not touch came back with no word about the gap";
    EXPECT_NE(measured.message.find("HOW FAR APART"), std::string::npos)
        << measured.message;

    // NO ITEM ANYWHERE CLAIMS A DISTANCE BETWEEN THEM. This is the assertion
    // that would fail the day somebody "helpfully" added one.
    for (const MeasureItem& item : measured.items) {
        EXPECT_EQ(item.label.find("Distance"), std::string::npos) << item.label;
        EXPECT_EQ(item.label.find("Gap"), std::string::npos) << item.label;
        EXPECT_EQ(item.label.find("Clearance"), std::string::npos) << item.label;
    }

    // ...and a block right next to another, not touching, gets the SAME
    // treatment -- because the kernel cannot tell that case from this one.
    const KernelShape almost = Block(kernel, 100.0, 40.0, 10.0, 100.5);
    const MeasureResult close = MeasureBetweenSolids(kernel, a, almost);
    ASSERT_TRUE(close.ok) << close.message;
    EXPECT_FALSE(close.message.empty())
        << "half a millimetre of gap was reported as though it were known";
}

TEST(OcctModelMeasureTest, M55_KRN_005_NothingToMeasureIsSaidRatherThanAnswered) {
    OcctGeometryKernel kernel;
    const KernelShape nothing;
    const MeasureResult measured = MeasureSolid(kernel, nothing, 7850.0);
    EXPECT_FALSE(measured.ok) << "an empty shape came back with numbers";
    EXPECT_FALSE(measured.message.empty());

    const KernelShape block = Block(kernel, 10.0, 10.0, 10.0);
    const MeasureResult pair = MeasureBetweenSolids(kernel, block, nothing);
    EXPECT_FALSE(pair.ok);

    // AND THE REASON IS ABOUT THE PICK, not about the kernel.
    //
    // The mutation gate showed both of these guards can be deleted without
    // changing WHETHER the measurement is refused -- OCCT turns them down
    // anyway. What changes is what the user is told: a message about a shape
    // that could not be built, for a selection that was simply empty. The
    // message is what these two are for, so the message is what is pinned.
    EXPECT_NE(measured.message.find("no solid here"), std::string::npos)
        << measured.message;
    EXPECT_NE(pair.message.find("two solids are needed"), std::string::npos) << pair.message;
}

} // namespace
