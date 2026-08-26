// M51.1 -- the K factor, and the flat pattern that is wrong in silence.
//
// Everything refused below would otherwise produce a NUMBER: a flat length,
// entirely self-consistent, that cuts cleanly and folds cleanly and gives a
// part the wrong size. There is nothing on the drawing to look at, because the
// drawing is right about the blank it describes.
//
//   * a K taken from the wrong band -- about a sixth of a millimetre a bend at
//     2 mm thick, which over the eight bends of an enclosure is more than a
//     millimetre on a lid that has to close
//   * an allowance used where a deduction belongs -- out by two setbacks per
//     bend, and out in the direction nobody checks
//   * a radius tighter than the material takes -- a correct blank for a bend
//     that cracks at the press brake

#include "Core/Feature/SheetMetalStandards.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

SheetBend RightAngle(double radiusMm) {
    SheetBend bend;
    bend.angleDeg = 90.0;
    bend.innerRadiusMm = radiusMm;
    return bend;
}

TEST(SheetMetalStandardsTest, M51_SHEET_001_TheKFactorComesFromBandsAndNotACurve) {
    // THREE BANDS, and the boundaries are where the published table put them:
    // tighter than one thickness, between one and three, and looser than
    // three. Interpolating between them would be inventing a number and
    // calling it a standard -- M37's fits and M39's tap drills, again.
    const double t = 2.0;
    EXPECT_NEAR(KFactorFor(SheetMaterial::MildSteelAluminium, 1.0, t), 0.38, 1e-9);
    EXPECT_NEAR(KFactorFor(SheetMaterial::MildSteelAluminium, 2.0, t), 0.43, 1e-9);
    EXPECT_NEAR(KFactorFor(SheetMaterial::MildSteelAluminium, 6.0, t), 0.43, 1e-9);
    EXPECT_NEAR(KFactorFor(SheetMaterial::MildSteelAluminium, 6.1, t), 0.46, 1e-9);

    // ...and the material matters. Soft brass and spring steel are a tenth
    // apart, which is about a third of a millimetre a bend.
    EXPECT_LT(KFactorFor(SheetMaterial::SoftBrassCopper, 2.0, t),
              KFactorFor(SheetMaterial::HardBronzeSpringSteel, 2.0, t));
    EXPECT_NEAR(KFactorFor(SheetMaterial::SoftBrassCopper, 2.0, t), 0.40, 1e-9);
    EXPECT_NEAR(KFactorFor(SheetMaterial::HardBronzeSpringSteel, 2.0, t), 0.45, 1e-9);
}

TEST(SheetMetalStandardsTest, M51_SHEET_002_ARadiusTighterThanTheMaterialTakesIsRefused) {
    // The blank would be correct. The bend cracks, and it cracks at the press
    // brake with the sheets already cut.
    const double t = 2.0;
    EXPECT_NEAR(MinimumBendRadiusMm(SheetMaterial::MildSteelAluminium, t), 2.0, 1e-9);
    EXPECT_NEAR(MinimumBendRadiusMm(SheetMaterial::HardBronzeSpringSteel, t), 4.0, 1e-9);
    EXPECT_NEAR(MinimumBendRadiusMm(SheetMaterial::SoftBrassCopper, t), 1.0, 1e-9);

    EXPECT_FALSE(WhyBendRefused(RightAngle(1.0), SheetMaterial::MildSteelAluminium, t).empty())
        << "a bend tighter than mild steel takes was accepted";
    EXPECT_TRUE(WhyBendRefused(RightAngle(2.0), SheetMaterial::MildSteelAluminium, t).empty());
    // The SAME radius in spring steel still cracks, which is the whole reason
    // the minimum is per material rather than a constant.
    EXPECT_FALSE(
        WhyBendRefused(RightAngle(2.0), SheetMaterial::HardBronzeSpringSteel, t).empty());
}

TEST(SheetMetalStandardsTest, M51_SHEET_003_ABendOfNothingAndAHemAreBothRefused) {
    const double t = 2.0;
    SheetBend flat = RightAngle(2.0);
    flat.angleDeg = 0.0;
    EXPECT_FALSE(WhyBendRefused(flat, SheetMaterial::MildSteelAluminium, t).empty());

    // A HALF TURN IS A HEM: folded flat against itself, with arithmetic of its
    // own. Run through the formula below it gives a setback that goes to
    // infinity, so the refusal is not tidiness -- it is the difference between
    // a message and a number nobody can use.
    SheetBend hem = RightAngle(2.0);
    hem.angleDeg = 180.0;
    EXPECT_FALSE(WhyBendRefused(hem, SheetMaterial::MildSteelAluminium, t).empty());

    EXPECT_FALSE(WhyBendRefused(RightAngle(2.0), SheetMaterial::MildSteelAluminium, 0.0)
                     .empty());
}

TEST(SheetMetalStandardsTest, M51_SHEET_004_AnAllowanceIsNotADeduction) {
    // THE CLASSIC SHOP ERROR. They differ by two setbacks per bend -- and a
    // blank a few millimetres wrong folds up looking entirely ordinary until
    // the last flange will not close.
    const double t = 2.0;
    const SheetBend bend = RightAngle(2.0);
    const double k = KFactorFor(SheetMaterial::MildSteelAluminium, 2.0, t);
    const double allowance = BendAllowanceMm(bend, t, k);
    const double setback = OutsideSetbackMm(bend, t);
    const double deduction = BendDeductionMm(bend, t, k);

    // A 90 degree bend, R2 in 2 mm: the neutral line runs at 2 + 0.43*2 =
    // 2.86, a quarter turn of it is 4.49; the outside corner sits (2+2)*tan45
    // = 4 from each tangent line.
    EXPECT_NEAR(allowance, (kPi / 2.0) * 2.86, 1e-6);
    EXPECT_NEAR(setback, 4.0, 1e-9);
    EXPECT_NEAR(deduction, 2.0 * setback - allowance, 1e-12);
    EXPECT_NE(allowance, deduction);
    // ABOUT A MILLIMETRE ON THIS BEND -- 4.49 against 3.51 -- and it grows with
    // the radius. The measurement is here rather than in a comment so the size
    // of the mistake is on the record.
    EXPECT_NEAR(std::fabs(allowance - deduction), 0.985, 0.01);
    EXPECT_GT(std::fabs(allowance - deduction), 0.5)
        << "on this bend the two are close enough that swapping them would not show, which "
           "would make this test measure nothing";
}

TEST(SheetMetalStandardsTest, M51_SHEET_005_BothRoutesToTheFlatLengthGiveOneAnswer) {
    // THE INVARIANT NO SINGLE METHOD CAN CHECK. Outside dimensions less
    // deductions, and tangent lengths plus allowances, are the same number
    // reached from opposite ends -- so a sign error in either shows up here as
    // the two disagreeing, which neither could see alone.
    const double t = 2.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    const std::vector<SheetBend> bends{RightAngle(2.0), RightAngle(3.0), RightAngle(2.0)};

    // Four flanges, three bends. The tangent lengths are the outside ones less
    // the setback each adjoining bend takes off.
    const std::vector<double> outside{40.0, 60.0, 60.0, 40.0};
    std::vector<double> tangents(outside.size(), 0.0);
    for (std::size_t i = 0; i < outside.size(); ++i) {
        tangents[i] = outside[i];
        if (i > 0) tangents[i] -= OutsideSetbackMm(bends[i - 1], t);
        if (i < bends.size()) tangents[i] -= OutsideSetbackMm(bends[i], t);
    }

    const FlatPatternResult fromOutside = FlatLengthFromOutside(outside, bends, steel, t);
    const FlatPatternResult fromTangents = FlatLengthFromTangents(tangents, bends, steel, t);
    ASSERT_TRUE(fromOutside.ok) << fromOutside.why;
    ASSERT_TRUE(fromTangents.ok) << fromTangents.why;
    EXPECT_NEAR(fromOutside.lengthMm, fromTangents.lengthMm, 1e-9)
        << "the two ways of stating the same job gave different blanks";

    // AND THE BLANK IS SHORTER THAN THE PART'S OUTSIDE. If it were not, the
    // bends would be adding metal.
    double outsideTotal = 0.0;
    for (const double flange : outside) outsideTotal += flange;
    EXPECT_LT(fromOutside.lengthMm, outsideTotal);
}

TEST(SheetMetalStandardsTest, M51_SHEET_006_TheWrongKIsWorthMillimetres) {
    // Not a rule -- a MEASUREMENT, kept so the size of the mistake is on the
    // record rather than claimed in a comment.
    //
    // Eight bends of an enclosure at 2 mm thick, R4 -- a radius both soft
    // brass and spring steel will take, which is what makes the two comparable
    // at all. Costing the brass job with the spring steel's K is worth 0.16 mm
    // a bend and 1.3 mm over the eight: nothing on any single bend, and a lid
    // that will not close.
    const double t = 2.0;
    const std::vector<SheetBend> bends(8, RightAngle(4.0));
    const std::vector<double> tangents(9, 30.0);

    const FlatPatternResult soft =
        FlatLengthFromTangents(tangents, bends, SheetMaterial::SoftBrassCopper, t);
    const FlatPatternResult hard =
        FlatLengthFromTangents(tangents, bends, SheetMaterial::HardBronzeSpringSteel, t);
    ASSERT_TRUE(soft.ok) << soft.why;
    ASSERT_TRUE(hard.ok) << hard.why;

    const double difference = hard.lengthMm - soft.lengthMm;
    EXPECT_GT(difference, 1.0)
        << "eight bends of the wrong K came to less than a millimetre, so this test is "
           "measuring nothing";
    EXPECT_NEAR(difference, 1.26, 0.05);

    // AND THE OTHER HALF OF THE SAME PROTECTION: at a tighter radius the wrong
    // material is refused outright, before it can give a wrong number at all.
    const std::vector<SheetBend> tight(8, RightAngle(2.0));
    EXPECT_TRUE(FlatLengthFromTangents(tangents, tight, SheetMaterial::MildSteelAluminium, t)
                    .ok);
    EXPECT_FALSE(
        FlatLengthFromTangents(tangents, tight, SheetMaterial::HardBronzeSpringSteel, t).ok)
        << "spring steel accepted a bend it cracks at";
}

TEST(SheetMetalStandardsTest, M51_SHEET_007_AFlangeCountThatCannotBeAPartIsRefused) {
    const double t = 2.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    // Two bends need three flanges. Truncating to the shorter list would hand
    // back a length for a different part.
    EXPECT_FALSE(FlatLengthFromOutside({40.0, 60.0}, {RightAngle(2.0), RightAngle(2.0)}, steel,
                                       t)
                     .ok);
    EXPECT_FALSE(FlatLengthFromOutside({}, {}, steel, t).ok);
    // A single flange with no bends is a flat plate, and that is a real part.
    const FlatPatternResult plate = FlatLengthFromOutside({40.0}, {}, steel, t);
    EXPECT_TRUE(plate.ok) << plate.why;
    EXPECT_NEAR(plate.lengthMm, 40.0, 1e-9);
}

TEST(SheetMetalStandardsTest, M51_SHEET_008_BendsThatEatThePartAreRefused) {
    // On short flanges with a generous radius the deductions can exceed what
    // the flanges have. A negative blank is not a small one -- it is a part
    // that cannot be made this way, and it would otherwise be cut as an
    // absolute value or a zero.
    const double t = 2.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    const FlatPatternResult tiny =
        FlatLengthFromOutside({3.0, 3.0, 3.0}, {RightAngle(8.0), RightAngle(8.0)}, steel, t);
    EXPECT_FALSE(tiny.ok) << "a blank of less than nothing was handed back";
    EXPECT_FALSE(tiny.why.empty());
}

TEST(SheetMetalStandardsTest, M51_SHEET_008B_AFlangeOfNoLengthIsRefused) {
    // FOUND BY THE MUTATION GATE. A zero among the flanges is not caught by
    // the blank being negative -- the other flanges carry it -- so the job
    // costs out fine and the part has a side that is not there. On the flat
    // pattern it is two bend lines with nothing between them, which reads as a
    // tight fold rather than as a mistake.
    const double t = 2.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    const std::vector<SheetBend> bends{RightAngle(2.0), RightAngle(2.0)};
    EXPECT_FALSE(FlatLengthFromOutside({40.0, 0.0, 40.0}, bends, steel, t).ok)
        << "a flange of no length was costed as a part";
    EXPECT_FALSE(FlatLengthFromOutside({40.0, -5.0, 40.0}, bends, steel, t).ok);
    EXPECT_TRUE(FlatLengthFromOutside({40.0, 40.0, 40.0}, bends, steel, t).ok);

    // The tangent route has the matching rule, and it is a DIFFERENT one: a
    // flat of zero between two bends is ordinary -- that is a fold straight
    // back on itself with no straight in between -- so only a negative is
    // refused there.
    EXPECT_TRUE(FlatLengthFromTangents({40.0, 0.0, 40.0}, bends, steel, t).ok);
    EXPECT_FALSE(FlatLengthFromTangents({40.0, -5.0, 40.0}, bends, steel, t).ok);
}

TEST(SheetMetalStandardsTest, M51_SHEET_009_EveryMaterialSurvivesItsOwnName) {
    // A name that fell back would become mild steel, and mild steel's K is a
    // tenth from spring steel's.
    for (const SheetMaterial material :
         {SheetMaterial::SoftBrassCopper, SheetMaterial::MildSteelAluminium,
          SheetMaterial::HardBronzeSpringSteel}) {
        SheetMaterial back = SheetMaterial::MildSteelAluminium;
        ASSERT_TRUE(ParseSheetMaterial(toString(material), back)) << toString(material);
        EXPECT_EQ(back, material);
        EXPECT_GT(KFactorFor(material, 2.0, 2.0), 0.0);
        EXPECT_GT(MinimumBendRadiusMm(material, 2.0), 0.0);
    }
    SheetMaterial unknown = SheetMaterial::MildSteelAluminium;
    EXPECT_FALSE(ParseSheetMaterial("titanium", unknown));
    EXPECT_EQ(unknown, SheetMaterial::MildSteelAluminium);
}

} // namespace
