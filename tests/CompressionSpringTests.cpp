// M60 -- the compression spring, and the length that is not an identity.
//
// The failures this file is here to catch:
//
//   * a spring index nobody can wind -- a coil tighter than drawn wire takes,
//     which looks exactly like a spring in every picture and cracks on the
//     mandrel
//   * a rate computed without Wahl's correction, which designs the wire twenty
//     to forty percent light exactly where it breaks
//   * a solid height taken from the wrong end style, so a spring is asked to
//     compress past coil-on-coil
//   * a fitted length in the PATH, which would put the same spring on a parts
//     list twice and order two where one is needed

#include "Core/Document/PartDocument.h"
#include "Core/Feature/HelixFeature.h"
#include "Core/Library/CompressionSpring.h"

#include "Fakes/FakeGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

CompressionSpring Spring(double wireMm, double meanMm, double coils, double freeMm,
                         SpringEnds ends = SpringEnds::ClosedGround) {
    CompressionSpring spring;
    spring.wireMm = wireMm;
    spring.meanDiameterMm = meanMm;
    spring.activeCoils = coils;
    spring.freeLengthMm = freeMm;
    spring.ends = ends;
    return spring;
}

TEST(CompressionSpringTest, M60_SPRING_001_EverythingComesOutOfFiveNumbers) {
    const CompressionSpring spring = Spring(2.0, 16.0, 8.0, 50.0);
    EXPECT_NEAR(spring.outerDiameterMm(), 18.0, 1e-9);
    EXPECT_NEAR(spring.innerDiameterMm(), 14.0, 1e-9);
    EXPECT_NEAR(spring.springIndex(), 8.0, 1e-9);
    // Closed ends add two dead coils; ground ones stack nt thicknesses when shut.
    EXPECT_NEAR(spring.totalCoilsCount(), 10.0, 1e-9);
    EXPECT_NEAR(spring.solidLengthMm(), 20.0, 1e-9);
    EXPECT_NEAR(spring.maxDeflectionMm(), 30.0, 1e-9);

    // THE RATE: G d^4 / (8 D^3 n) = 81500 * 16 / (8 * 4096 * 8).
    EXPECT_NEAR(spring.rateNPerMm(), 81500.0 * 16.0 / (8.0 * 4096.0 * 8.0), 1e-9);
    EXPECT_NEAR(spring.rateNPerMm(), 4.974, 0.002); // about 5 N per mm of squash

    EXPECT_EQ(spring.designation(), "d2 D16 n8 L50");
    // The end style is written only when it is not the ordinary one, so two of
    // the common spring are one line on a parts list.
    EXPECT_EQ(Spring(2.0, 16.0, 8.0, 50.0, SpringEnds::Plain).designation(),
              "d2 D16 n8 L50 ep");
}

TEST(CompressionSpringTest, M60_SPRING_002_TheENDSTYLEChangesWhatIsDeadAndWhatIsShut) {
    // Three styles, and every number that differs between them comes from one
    // table rather than a switch in each function.
    const CompressionSpring plain = Spring(2.0, 16.0, 8.0, 50.0, SpringEnds::Plain);
    const CompressionSpring closed = Spring(2.0, 16.0, 8.0, 50.0, SpringEnds::Closed);
    const CompressionSpring ground = Spring(2.0, 16.0, 8.0, 50.0, SpringEnds::ClosedGround);

    // PLAIN: every coil is live, and the coil simply stops.
    EXPECT_NEAR(plain.totalCoilsCount(), 8.0, 1e-9);
    EXPECT_NEAR(plain.solidLengthMm(), 18.0, 1e-9);
    // CLOSED: two dead coils, and the ends still have their own rise.
    EXPECT_NEAR(closed.totalCoilsCount(), 10.0, 1e-9);
    EXPECT_NEAR(closed.solidLengthMm(), 26.0, 1e-9);
    // GROUND: the same two dead coils with their rise taken off.
    EXPECT_NEAR(ground.solidLengthMm(), 20.0, 1e-9);
    EXPECT_LT(ground.solidLengthMm(), closed.solidLengthMm())
        << "grinding did not make the spring shorter when shut";

    // THE RATE DOES NOT CARE, because it depends on the ACTIVE coils and all
    // three of these have eight.
    EXPECT_NEAR(plain.rateNPerMm(), ground.rateNPerMm(), 1e-9);
    EXPECT_NEAR(closed.rateNPerMm(), ground.rateNPerMm(), 1e-9);
}

TEST(CompressionSpringTest, M60_SPRING_003_AnIndexNobodyCanWindIsREFUSED) {
    // The one that matters. A spring of index 2 looks exactly like a spring in
    // every picture this program can draw, and the wire cracks on the mandrel.
    const std::string tight = WhySpringRefused(Spring(4.0, 8.0, 8.0, 50.0));
    EXPECT_FALSE(tight.empty());
    EXPECT_NE(tight.find("index of 2"), std::string::npos) << tight;
    EXPECT_NE(tight.find("cracks"), std::string::npos) << tight;
    EXPECT_NE(tight.find("4"), std::string::npos)
        << "the refusal does not say how loose it would have to be: " << tight;

    const std::string slack = WhySpringRefused(Spring(0.5, 10.0, 8.0, 50.0));
    EXPECT_FALSE(slack.empty());
    EXPECT_NE(slack.find("too slack"), std::string::npos) << slack;

    // Four and twelve are in; either side is out.
    EXPECT_TRUE(WhySpringRefused(Spring(2.0, 8.0, 8.0, 40.0)).empty());
    EXPECT_TRUE(WhySpringRefused(Spring(2.0, 24.0, 8.0, 60.0)).empty());

    // A WIRE IS WHAT COMES OFF THE REEL, not a number to choose.
    const std::string odd = WhySpringRefused(Spring(1.9, 16.0, 8.0, 50.0));
    EXPECT_NE(odd.find("off the reel"), std::string::npos) << odd;
}

TEST(CompressionSpringTest, M60_SPRING_004_ASpringAlreadySHUTIsRefused) {
    // 8 active coils, closed and ground, is 20 mm of solid height on 2 mm
    // wire. A free length of 20 is a spring that cannot move: it has a rate, a
    // mass and a picture, and no travel at all.
    const CompressionSpring shut = Spring(2.0, 16.0, 8.0, 20.0);
    EXPECT_NEAR(shut.maxDeflectionMm(), 0.0, 1e-9);
    const std::string why = WhySpringRefused(shut);
    EXPECT_FALSE(why.empty());
    EXPECT_NE(why.find("already shut"), std::string::npos) << why;
    EXPECT_NE(why.find("20"), std::string::npos) << why;

    // FEWER THAN TWO ACTIVE COILS IS NOT A SPRING EITHER, and the rate formula
    // divides by the coil count, so it runs away as that approaches nothing.
    const std::string few = WhySpringRefused(Spring(2.0, 16.0, 1.0, 50.0));
    EXPECT_NE(few.find("not a spring"), std::string::npos) << few;
}

TEST(CompressionSpringTest, M60_SPRING_005_TheFORCEAndTheSTRESSAtAWorkingLength) {
    const CompressionSpring spring = Spring(2.0, 16.0, 8.0, 50.0);
    const double rate = spring.rateNPerMm();

    // Squashed 12 mm, from 50 to 38.
    EXPECT_NEAR(spring.forceAtLengthN(38.0), rate * 12.0, 1e-9);
    // AT ITS FREE LENGTH IT PUSHES NOTHING, and longer than that it still
    // pushes nothing -- a compression spring that is not touching anything is
    // not pulling either, and a negative force would describe one welded to
    // both faces.
    EXPECT_NEAR(spring.forceAtLengthN(50.0), 0.0, 1e-12);
    EXPECT_NEAR(spring.forceAtLengthN(80.0), 0.0, 1e-12);

    // WAHL'S CORRECTION IS IN THE STRESS, and it is what makes the number
    // usable. At an index of 8 it is about 1.18, so the stress is that much
    // above the plain torsion formula.
    const double force = spring.forceAtLengthN(38.0);
    const double plain = 8.0 * force * spring.meanDiameterMm /
                         (kPi * spring.wireMm * spring.wireMm * spring.wireMm);
    const double corrected = spring.shearStressAtLengthMPa(38.0);
    EXPECT_GT(corrected, plain) << "the stress is the uncorrected torsion formula";
    EXPECT_NEAR(corrected / plain, 1.184, 0.005);

    // AND THE CORRECTION GROWS AS THE COIL TIGHTENS, which is the whole reason
    // it exists: the inside of a tight coil is where a spring breaks.
    const CompressionSpring tighter = Spring(2.0, 8.0, 8.0, 40.0);
    const double tightRatio =
        tighter.shearStressAtLengthMPa(30.0) /
        (8.0 * tighter.forceAtLengthN(30.0) * tighter.meanDiameterMm /
         (kPi * tighter.wireMm * tighter.wireMm * tighter.wireMm));
    EXPECT_GT(tightRatio, corrected / plain)
        << "a tighter coil got the same correction as a looser one";
}

TEST(CompressionSpringTest, M60_SPRING_006_ASlenderSpringIsWARNEDAboutRatherThanRefused) {
    // Half the springs in a machine are slender; they simply have to be guided.
    // Refusing them would be refusing the product.
    const CompressionSpring stubby = Spring(2.0, 16.0, 8.0, 35.0);
    EXPECT_LT(stubby.slendernessRatio(), 2.6);
    EXPECT_TRUE(WhySpringMayBuckle(stubby).empty());
    EXPECT_TRUE(WhySpringRefused(stubby).empty());

    const CompressionSpring lanky = Spring(2.0, 16.0, 20.0, 90.0);
    EXPECT_GT(lanky.slendernessRatio(), 2.6);
    EXPECT_TRUE(WhySpringRefused(lanky).empty()) << "a slender spring was refused outright";
    const std::string watch = WhySpringMayBuckle(lanky);
    EXPECT_FALSE(watch.empty());
    EXPECT_NE(watch.find("folds sideways"), std::string::npos) << watch;
    // ...and it says what to do about it, which is the difference between a
    // caution and a shrug.
    EXPECT_NE(watch.find("bore"), std::string::npos) << watch;

    // A spring that cannot be made at all gets no buckling note: it has one
    // problem and that is the one to read.
    EXPECT_TRUE(WhySpringMayBuckle(Spring(4.0, 8.0, 20.0, 90.0)).empty());
}

TEST(CompressionSpringTest, M60_SPRING_007_THELENGTHISNOTINTHEPATH) {
    // THE THESIS. A frame member's length IS its identity: a 600 stick and a
    // 400 stick are two things to cut, and M56 put the length in the path for
    // that reason. A SPRING'S FITTED LENGTH IS NOT. The same spring is 50 long
    // on the shelf and 38 long in the machine, and it is one part number.
    const CompressionSpring spring = Spring(2.0, 16.0, 8.0, 50.0);
    const std::string path = CompressionSpringPath(spring);
    EXPECT_EQ(path, "spr:d2 D16 n8 L50");

    // The FREE length is in the path, because that is a property of the
    // spring. Working at 38 does not change the path, and therefore does not
    // add a second line to a parts list.
    EXPECT_GT(spring.forceAtLengthN(38.0), 0.0);
    EXPECT_EQ(CompressionSpringPath(spring), path)
        << "asking what the spring does at a length changed what the spring IS";

    const std::optional<CompressionSpring> back = CompressionSpringOfPath(path);
    ASSERT_TRUE(back.has_value());
    EXPECT_NEAR(back->wireMm, 2.0, 1e-9);
    EXPECT_NEAR(back->meanDiameterMm, 16.0, 1e-9);
    EXPECT_NEAR(back->activeCoils, 8.0, 1e-9);
    EXPECT_NEAR(back->freeLengthMm, 50.0, 1e-9);
    EXPECT_EQ(back->ends, SpringEnds::ClosedGround);

    const std::optional<CompressionSpring> plain =
        CompressionSpringOfPath("spr:d2 D16 n8 L50 ep");
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->ends, SpringEnds::Plain);

    // A SPRING NOBODY CAN WIND HAS NO PATH, so nothing can place one by
    // writing the string out by hand either.
    EXPECT_FALSE(CompressionSpringOfPath("spr:d4 D8 n8 L50").has_value());
    EXPECT_FALSE(CompressionSpringOfPath("spr:d1.9 D16 n8 L50").has_value());
    EXPECT_FALSE(CompressionSpringOfPath("spr:d2 D16 n8").has_value());
    EXPECT_FALSE(CompressionSpringOfPath("spr:d2 D16 n8 L50 ez").has_value());
    EXPECT_FALSE(CompressionSpringOfPath("D:/parts/spring.ep3d").has_value());

    // ...but the text can still be READ, so a resolver can give the real
    // reason rather than complaining about syntax at somebody whose typing
    // was fine.
    const std::optional<CompressionSpring> tight = ParseSpringDesignation("d4 D8 n8 L50");
    ASSERT_TRUE(tight.has_value());
    EXPECT_NE(WhySpringRefused(*tight).find("index"), std::string::npos);
}

TEST(CompressionSpringTest, M60_SPRING_008_ACoilWhoseNumberIsGoneSaysSoRatherThanBuilding) {
    // FOUND BY THE MUTATION GATE, and it is the same gap M59 closed for the
    // import feature: nothing had ever deleted a parameter a feature depends
    // on. Without the check the feature reads through a null pointer, which is
    // not a failure with a message -- it is a crash, in a rebuild, on a file
    // the user did not think they had broken.
    FakeGeometryKernel kernel;
    PartDocument part{"Spring"};
    part.setGeometryKernel(&kernel);
    Body& body = part.addBody("Coil");
    Parameter& wire = part.addParameter("Wire", 2.0, UnitType::Millimeter);
    Parameter& mean = part.addParameter("Mean diameter", 16.0, UnitType::Millimeter);
    Parameter& pitch = part.addParameter("Pitch", 5.0, UnitType::Millimeter);
    Parameter& turns = part.addParameter("Turns", 8.0, UnitType::Unitless);
    HelixFeature& coil =
        part.addHelixFeature(body, "Coil", wire.id(), mean.id(), pitch.id(), turns.id());
    ASSERT_TRUE(part.recompute().success);

    ASSERT_TRUE(part.removeObject(pitch.id()));
    const DocumentRecomputeReport report = part.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(coil.currentState(), ComputeState::Failed);
    std::string said;
    for (const RecomputeItemReport& item : report.items)
        if (item.id == coil.id()) said = item.message;
    EXPECT_NE(said.find("pitch"), std::string::npos)
        << "the coil did not say which of its four numbers went missing: " << said;

    // AND THE LAST GOOD COIL IS STILL THERE. A failed rebuild leaves the shape
    // byte for byte alone (ADR-M3-001); staleness is carried by the state.
    EXPECT_TRUE(coil.currentShape().isValid());
}

} // namespace
