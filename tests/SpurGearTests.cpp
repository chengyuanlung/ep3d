// M58 -- the design accelerator, and the ratio that stops being typed.
//
// EP3D has coupled two rotations by a typed ratio since M31 and can build the
// gears themselves since M58. Those are two halves of one fact, and the whole
// milestone is about not keeping two copies of it.
//
// The failures this file is here to catch:
//
//   * a pair whose ratio says 2:1 while the models say 24:40 -- every number
//     plausible, nothing dangling, the machine wrong
//   * an undercut pinion, generated quietly: a tooth that is still a tooth and
//     is weaker and rougher than the drawing says
//   * two gears of different module put on one shaft centre, which no distance
//     makes run
//   * a tooth thickness that is not half the pitch, which is the one property
//     that makes gears mesh at all

#include "Core/Library/SpurGear.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

SpurGear Gear(double moduleMm, int teeth, double widthMm = 10.0, double angleDeg = 20.0) {
    SpurGear gear;
    gear.moduleMm = moduleMm;
    gear.teeth = teeth;
    gear.faceWidthMm = widthMm;
    gear.pressureAngleDeg = angleDeg;
    return gear;
}

TEST(SpurGearTest, M58_GEAR_001_EverythingAboutAGearComesOutOfFourNumbers) {
    const SpurGear gear = Gear(2.0, 20);
    EXPECT_NEAR(gear.pitchDiameterMm(), 40.0, 1e-9);
    EXPECT_NEAR(gear.tipDiameterMm(), 44.0, 1e-9);
    EXPECT_NEAR(gear.rootDiameterMm(), 35.0, 1e-9);
    EXPECT_NEAR(gear.baseDiameterMm(), 40.0 * std::cos(20.0 * kPi / 180.0), 1e-9);
    // HALF THE CIRCULAR PITCH, which is the property that makes two gears of
    // the same module mesh: a tooth and the space beside it share the pitch.
    EXPECT_NEAR(gear.toothThicknessMm(), kPi * 2.0 / 2.0, 1e-9);

    EXPECT_EQ(gear.designation(), "m2 z20 b10");
    // The pressure angle appears only when it is not the ordinary one, so two
    // ordinary gears of a size are one line on a parts list.
    EXPECT_EQ(Gear(2.0, 40, 10.0, 14.5).designation(), "m2 z40 b10 a14.5");
}

TEST(SpurGearTest, M58_GEAR_002_AnUndercutPinionIsREFUSEDWithTheCount) {
    // The tooth would still be there and would still look like a tooth. That
    // is the whole danger: nothing about a 12-tooth pinion says it is wrong
    // until it wears out early.
    EXPECT_EQ(MinimumTeethWithoutUndercut(20.0), 18);
    EXPECT_EQ(MinimumTeethWithoutUndercut(14.5), 32);

    const std::string why = WhyGearRefused(Gear(2.0, 12));
    EXPECT_FALSE(why.empty());
    EXPECT_NE(why.find("undercut"), std::string::npos) << why;
    EXPECT_NE(why.find("18"), std::string::npos)
        << "the refusal does not say how many teeth would be enough: " << why;

    EXPECT_TRUE(WhyGearRefused(Gear(2.0, 18)).empty());
    // ...and at 14.5 degrees the same 18 teeth undercuts, because the limit
    // moves with the pressure angle rather than being a number to remember.
    EXPECT_FALSE(WhyGearRefused(Gear(2.0, 18, 10.0, 14.5)).empty());
    EXPECT_TRUE(WhyGearRefused(Gear(2.0, 32, 10.0, 14.5)).empty());

    // A MODULE IS A CUTTER, not a number to choose.
    const std::string odd = WhyGearRefused(Gear(1.7, 20));
    EXPECT_NE(odd.find("cutter"), std::string::npos) << odd;
}

TEST(SpurGearTest, M58_GEAR_003_ThePairsNumbersAreDerivedAndTheMismatchIsNamed) {
    const SpurGear pinion = Gear(2.0, 20);
    const SpurGear wheel = Gear(2.0, 40);

    EXPECT_TRUE(WhyPairRefused(pinion, wheel).empty());
    EXPECT_NEAR(CentreDistanceMm(pinion, wheel), 60.0, 1e-9);
    // NEGATIVE, because two external gears turn opposite ways. That is a fact
    // about the machine, so it is in the number rather than in a flag beside
    // it that somebody sets separately.
    EXPECT_NEAR(GearRatio(pinion, wheel), -0.5, 1e-9);
    EXPECT_NEAR(GearRatio(wheel, pinion), -2.0, 1e-9);

    // DIFFERENT MODULES ARE THE MISTAKE, and no centre distance fixes it.
    const std::string why = WhyPairRefused(pinion, Gear(2.5, 40));
    EXPECT_FALSE(why.empty());
    EXPECT_NE(why.find("different size"), std::string::npos) << why;
    // ...and the numbers that depend on the pair come back unusable rather
    // than plausible, so a caller that ignored the refusal cannot go on.
    EXPECT_EQ(CentreDistanceMm(pinion, Gear(2.5, 40)), 0.0);
    EXPECT_EQ(GearRatio(pinion, Gear(2.5, 40)), 0.0);

    // Different pressure angles are different flank curves.
    const std::string flanks = WhyPairRefused(pinion, Gear(2.0, 40, 10.0, 14.5));
    EXPECT_NE(flanks.find("flank"), std::string::npos) << flanks;
}

TEST(SpurGearTest, M58_GEAR_004_TheContactRatioSaysWhetherItWillActuallyRUN) {
    // A pair can mesh geometrically and still not turn: below 1.0 the teeth
    // let go of each other between one and the next. It cannot be seen in the
    // model at all, which is why it is a number this reports rather than
    // something to find on the bench.
    const SpurGear pinion = Gear(2.0, 20);
    const SpurGear wheel = Gear(2.0, 40);
    const double contact = ContactRatio(pinion, wheel);
    // A standard 20-degree pair of this size runs at about 1.6-1.7, which is
    // the ordinary figure a gear book gives.
    EXPECT_GT(contact, 1.5);
    EXPECT_LT(contact, 1.8);
    EXPECT_GT(contact, 1.2) << "this pair would run rough";

    // FEWER TEETH IS LESS CONTACT, which is the direction that matters: a
    // small pinion is the one that runs out of overlap.
    EXPECT_LT(ContactRatio(Gear(2.0, 18), Gear(2.0, 18)), contact);
    // A 14.5 degree pair has MORE overlap, which is why old machines are
    // quieter and weaker -- and a check that got the direction backwards would
    // still produce a number between 1 and 2.
    EXPECT_GT(ContactRatio(Gear(2.0, 40, 10.0, 14.5), Gear(2.0, 40, 10.0, 14.5)),
              ContactRatio(Gear(2.0, 40), Gear(2.0, 40)));

    EXPECT_EQ(ContactRatio(pinion, Gear(2.5, 40)), 0.0) << "a refused pair got a contact ratio";
}

TEST(SpurGearTest, M58_GEAR_005_TheOutlineHasTheRightTeethInTheRightPlaces) {
    const SpurGear gear = Gear(2.0, 20);
    const std::vector<Vec2> loop = SpurGearOutline(gear);
    ASSERT_FALSE(loop.empty());

    // Nothing is outside the tip circle or inside the root circle: a point
    // beyond either is a profile that has escaped its own gear.
    const double tip = gear.tipDiameterMm() / 2.0;
    const double root = gear.rootDiameterMm() / 2.0;
    int atTip = 0;
    for (const Vec2& point : loop) {
        const double at = std::hypot(point.x, point.y);
        EXPECT_LE(at, tip + 1e-6);
        EXPECT_GE(at, root - 1e-6);
        if (at > tip - 1e-6) ++atTip;
    }
    // AT LEAST TWO PER TOOTH -- the top of each flank -- plus however many the
    // tip land's arc takes. Counted as a floor rather than an exact number,
    // because how finely the land is drawn is this file's business and not the
    // test's; the exact tooth count is asserted below, off the pitch circle,
    // where it is a fact about meshing rather than about resolution.
    EXPECT_GE(atTip, 2 * gear.teeth);

    // THE TIP LAND IS AN ARC, so a tooth's outermost point IS the tip radius
    // and not a chord's worth inside it. Found by the kernel tests, where a
    // 44 mm gear measured 43.978 across -- and the outside diameter is the
    // number a gear is measured over.
    double furthest = 0.0;
    for (const Vec2& point : loop) furthest = std::max(furthest, std::hypot(point.x, point.y));
    EXPECT_NEAR(furthest, tip, 1e-9);

    // THE TOOTH THICKNESS AT THE PITCH CIRCLE, measured off the drawn profile
    // rather than off the formula that drew it.
    //
    // This is the assertion the whole file is for. Everything else about an
    // involute can be a little wrong and the gears still turn; get the flank's
    // phase wrong and the teeth are the correct shape in the wrong place, and
    // the pair jams or rattles.
    const double pitch = gear.pitchDiameterMm() / 2.0;
    std::vector<double> crossings;
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Vec2& a = loop[i];
        const Vec2& b = loop[(i + 1) % loop.size()];
        const double ra = std::hypot(a.x, a.y);
        const double rb = std::hypot(b.x, b.y);
        if ((ra - pitch) * (rb - pitch) > 0.0) continue;
        const double at = (pitch - ra) / (rb - ra);
        crossings.push_back(std::atan2(a.y + (b.y - a.y) * at, a.x + (b.x - a.x) * at));
    }
    ASSERT_EQ(crossings.size(), static_cast<std::size_t>(2 * gear.teeth))
        << "the profile does not cross the pitch circle twice per tooth";
    // The first tooth is centred on zero, so its two crossings are +/- half the
    // tooth's angular thickness.
    const double halfAngle = gear.toothThicknessMm() / 2.0 / pitch;
    EXPECT_NEAR(std::fabs(crossings[0]), halfAngle, 1e-4)
        << "the tooth is not half the pitch thick, so it will not mesh";
}

TEST(SpurGearTest, M58_GEAR_006_APathRoundTripsAndARefusedGearHasNone) {
    const SpurGear gear = Gear(2.0, 20);
    EXPECT_EQ(SpurGearPath(gear), "gear:m2 z20 b10");
    const std::optional<SpurGear> back = SpurGearOfPath("gear:m2 z20 b10");
    ASSERT_TRUE(back.has_value());
    EXPECT_NEAR(back->moduleMm, 2.0, 1e-9);
    EXPECT_EQ(back->teeth, 20);
    EXPECT_NEAR(back->faceWidthMm, 10.0, 1e-9);
    EXPECT_NEAR(back->pressureAngleDeg, 20.0, 1e-9);

    const std::optional<SpurGear> old = SpurGearOfPath("gear:m2 z40 b10 a14.5");
    ASSERT_TRUE(old.has_value());
    EXPECT_NEAR(old->pressureAngleDeg, 14.5, 1e-9);

    // A GEAR NOBODY CAN CUT HAS NO PATH, so nothing can place one by writing
    // the string out by hand either.
    EXPECT_FALSE(SpurGearOfPath("gear:m2 z12 b10").has_value());
    EXPECT_FALSE(SpurGearOfPath("gear:m1.7 z20 b10").has_value());
    EXPECT_FALSE(SpurGearOfPath("gear:m2 z20").has_value());
    EXPECT_FALSE(SpurGearOfPath("gear:m2 z20.5 b10").has_value());
    EXPECT_FALSE(SpurGearOfPath("D:/parts/gear.ep3d").has_value());

    // ...but the text can still be READ, so the resolver can say WHY rather
    // than complaining about syntax at somebody whose typing was fine.
    const std::optional<SpurGear> undercut = ParseGearDesignation("m2 z12 b10");
    ASSERT_TRUE(undercut.has_value());
    EXPECT_EQ(undercut->teeth, 12);
    EXPECT_NE(WhyGearRefused(*undercut).find("undercut"), std::string::npos);
    EXPECT_FALSE(ParseGearDesignation("m2 z20").has_value());
}

} // namespace
