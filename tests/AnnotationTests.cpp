// M41.1 -- surface finish and geometric tolerance, as rules rather than boxes.
//
// Nothing in this file is about whether a symbol draws. Every one of these
// specifications draws perfectly: a frame is a frame, a tick with a number
// beside it is a surface finish. What they are about is whether the drawing
// SAYS WHAT THE DESIGNER MEANT, and every failure below is one a reader would
// accept without blinking:
//
//   * a flatness frame with a datum in it -- meaningless, and looks stricter
//   * a position frame with none -- unmeasurable, and looks complete
//   * a positional zone that lost its diameter symbol -- 40% looser at the
//     corners, and reads as an ordinary frame
//   * "material must NOT be removed" with a machining allowance beside it

#include "Core/Drawing/Annotation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

const std::string kDia = "\xE2\x8C\x80";
const std::string kMmc = "\xE2\x93\x82";

TEST(AnnotationTest, M41_GDT_001_FormToleranceRefusesADatumAndARelationshipINSISTSOnOne) {
    // THE TWO RULES THAT FAIL SILENTLY, and they fail in opposite directions.
    FeatureControlFrameSpec flat;
    flat.characteristic = GeometricCharacteristic::Flatness;
    flat.toleranceMm = 0.05;
    flat.diametricZone = false;
    EXPECT_TRUE(WhyFrameRefused(flat).empty()) << WhyFrameRefused(flat);

    flat.datums.push_back(DatumReference{4242, MaterialCondition::RegardlessOfFeatureSize});
    const std::string why = WhyFrameRefused(flat);
    EXPECT_FALSE(why.empty()) << "flatness accepted a datum, which means nothing";
    EXPECT_NE(why.find("flatness"), std::string::npos) << why;

    // ...and the other way. A position frame with no datum is unmeasurable and
    // draws as a perfectly complete frame.
    FeatureControlFrameSpec position;
    position.characteristic = GeometricCharacteristic::Position;
    position.toleranceMm = 0.2;
    EXPECT_FALSE(WhyFrameRefused(position).empty())
        << "a position tolerance with nothing to be positioned against was accepted";
    position.datums.push_back(DatumReference{4242, MaterialCondition::RegardlessOfFeatureSize});
    EXPECT_TRUE(WhyFrameRefused(position).empty()) << WhyFrameRefused(position);
}

TEST(AnnotationTest, M41_GDT_002_EveryCharacteristicIsInEXACTLYOneOfTheThreeGroups) {
    // The grouping is the whole rule above, so a characteristic that fell out
    // of the table would silently take the default -- and the default is
    // "needs a datum", which is right for eight of the fourteen and wrong for
    // the six that matter most here.
    struct Known {
        GeometricCharacteristic characteristic;
        DatumNeed need;
    };
    const Known chart[] = {
        {GeometricCharacteristic::Straightness, DatumNeed::Never},
        {GeometricCharacteristic::Flatness, DatumNeed::Never},
        {GeometricCharacteristic::Roundness, DatumNeed::Never},
        {GeometricCharacteristic::Cylindricity, DatumNeed::Never},
        {GeometricCharacteristic::LineProfile, DatumNeed::Either},
        {GeometricCharacteristic::SurfaceProfile, DatumNeed::Either},
        {GeometricCharacteristic::Parallelism, DatumNeed::Always},
        {GeometricCharacteristic::Perpendicularity, DatumNeed::Always},
        {GeometricCharacteristic::Angularity, DatumNeed::Always},
        {GeometricCharacteristic::Position, DatumNeed::Always},
        {GeometricCharacteristic::Concentricity, DatumNeed::Always},
        {GeometricCharacteristic::Symmetry, DatumNeed::Always},
        {GeometricCharacteristic::CircularRunout, DatumNeed::Always},
        {GeometricCharacteristic::TotalRunout, DatumNeed::Always},
    };
    ASSERT_EQ(std::size(chart), 14u) << "ISO 1101 has fourteen characteristics";
    for (const Known& one : chart)
        EXPECT_EQ(DatumNeedOf(one.characteristic), one.need)
            << toString(one.characteristic);

    // ...and each of the fourteen has its own name and its own glyph. Two that
    // shared either would be two the reader cannot tell apart.
    std::vector<std::string> names;
    for (const Known& one : chart) names.push_back(std::string(toString(one.characteristic)));
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end())
        << "two characteristics share a name";

    // A PROFILE IS BOTH, and that is not a hedge: without a datum it controls
    // the shape, with one it controls the shape and where it sits.
    FeatureControlFrameSpec profile;
    profile.characteristic = GeometricCharacteristic::SurfaceProfile;
    profile.toleranceMm = 0.3;
    profile.diametricZone = false;
    EXPECT_TRUE(WhyFrameRefused(profile).empty()) << WhyFrameRefused(profile);
    profile.datums.push_back(DatumReference{7, MaterialCondition::RegardlessOfFeatureSize});
    EXPECT_TRUE(WhyFrameRefused(profile).empty()) << WhyFrameRefused(profile);
}

TEST(AnnotationTest, M41_GDT_003_ADiameterSymbolOnASURFACEToleranceIsREFUSED) {
    // A cylindrical zone is a zone about an axis. On flatness or roundness it
    // is not tighter, not looser -- it is nonsense, and it draws as an
    // ordinary frame with a diameter sign in it.
    FeatureControlFrameSpec round;
    round.characteristic = GeometricCharacteristic::Roundness;
    round.toleranceMm = 0.02;
    round.diametricZone = true;
    EXPECT_FALSE(WhyFrameRefused(round).empty()) << "a roundness zone was made cylindrical";
    round.diametricZone = false;
    EXPECT_TRUE(WhyFrameRefused(round).empty()) << WhyFrameRefused(round);

    // ...and on a position it is not only allowed, it is what is nearly always
    // meant: the square zone's corner is 1.4 times further out than the
    // circle's edge, so a lost diameter symbol is a 40% looser part.
    FeatureControlFrameSpec position;
    position.characteristic = GeometricCharacteristic::Position;
    position.toleranceMm = 0.2;
    position.diametricZone = true;
    position.datums.push_back(DatumReference{7, MaterialCondition::RegardlessOfFeatureSize});
    EXPECT_TRUE(WhyFrameRefused(position).empty()) << WhyFrameRefused(position);
}

TEST(AnnotationTest, M41_GDT_004_AFrameHasRoomForTHREEDatumsAndNoRepeats) {
    FeatureControlFrameSpec spec;
    spec.characteristic = GeometricCharacteristic::Position;
    spec.toleranceMm = 0.2;
    spec.datums = {DatumReference{1}, DatumReference{2}, DatumReference{3}};
    EXPECT_TRUE(WhyFrameRefused(spec).empty()) << WhyFrameRefused(spec);

    spec.datums.push_back(DatumReference{4});
    EXPECT_FALSE(WhyFrameRefused(spec).empty()) << "a fourth datum was accepted";

    // THE SAME DATUM TWICE constrains nothing the first mention did not, and
    // reads as a three-datum reference frame that is really one.
    spec.datums = {DatumReference{1}, DatumReference{2}, DatumReference{1}};
    const std::string why = WhyFrameRefused(spec);
    EXPECT_FALSE(why.empty()) << "the same datum was named twice";
    EXPECT_NE(why.find("twice"), std::string::npos) << why;

    // A datum reference to nothing at all.
    spec.datums = {DatumReference{kInvalidObjectId}};
    EXPECT_FALSE(WhyFrameRefused(spec).empty());

    // A zone of no size.
    spec.datums = {DatumReference{1}};
    spec.toleranceMm = 0.0;
    EXPECT_FALSE(WhyFrameRefused(spec).empty());
}

TEST(AnnotationTest, M41_GDT_005_TheFrameIsWrittenWithTheLettersItsDatumsCARRY) {
    // The letters are handed in, because the document derives them from the
    // order the datums were placed. A frame that stored its own would be the
    // second copy of a letter -- and M38's section letters are the same
    // lesson, arriving on a different symbol.
    FeatureControlFrameSpec spec;
    spec.characteristic = GeometricCharacteristic::Position;
    spec.toleranceMm = 0.25;
    spec.diametricZone = true;
    spec.condition = MaterialCondition::Maximum;
    spec.datums = {DatumReference{11, MaterialCondition::RegardlessOfFeatureSize},
                   DatumReference{12, MaterialCondition::Maximum}};

    const std::string text = FrameText(spec, {"A", "B"});
    EXPECT_NE(text.find(kDia + "0.25"), std::string::npos) << text;
    EXPECT_NE(text.find("| A"), std::string::npos) << text;
    EXPECT_NE(text.find("| B" + kMmc), std::string::npos)
        << "the secondary datum's material condition was dropped: " << text;
    // ...and RFS is written by writing NOTHING, which is why the primary datum
    // has no modifier after it.
    EXPECT_EQ(text.find("| A" + kMmc), std::string::npos) << text;

    // A CALLER WHO RESOLVED THE WRONG NUMBER OF LETTERS GETS NOTHING, rather
    // than a frame whose letters belong to some other datum.
    EXPECT_TRUE(FrameText(spec, {"A"}).empty());
    EXPECT_TRUE(FrameText(spec, {"A", "B", "C"}).empty());
    // ...and a refused frame writes nothing at all.
    FeatureControlFrameSpec broken = spec;
    broken.characteristic = GeometricCharacteristic::Flatness;
    EXPECT_TRUE(FrameText(broken, {"A", "B"}).empty());
}

TEST(AnnotationTest, M41_SURF_001_TheTHREESymbolsAreThreeDifferentInstructions) {
    // Machined where AsCast was meant scraps a casting; AsCast where Machined
    // was meant ships an unfinished face. Both draw a tick with a number.
    EXPECT_NE(toString(SurfaceSymbol::Basic), toString(SurfaceSymbol::Machined));
    EXPECT_NE(toString(SurfaceSymbol::Machined), toString(SurfaceSymbol::AsCast));

    SurfaceSymbol read = SurfaceSymbol::Basic;
    EXPECT_TRUE(ParseSurfaceSymbol("as-cast", read));
    EXPECT_EQ(read, SurfaceSymbol::AsCast);
    EXPECT_FALSE(ParseSurfaceSymbol("machining", read))
        << "a name this build does not know was read as one it does";
    EXPECT_EQ(read, SurfaceSymbol::AsCast) << "a failed parse changed the value anyway";

    // MATERIAL MUST NOT BE REMOVED, AND LEAVE 2 MM FOR MACHINING. One of the
    // two has to go, and neither looks wrong on its own.
    SurfaceFinishSpec cast;
    cast.symbol = SurfaceSymbol::AsCast;
    cast.raMicrometres = 12.5;
    cast.machiningAllowanceMm = 2.0;
    EXPECT_FALSE(WhySurfaceFinishRefused(cast).empty())
        << "a face that must not be machined was given a machining allowance";
    cast.machiningAllowanceMm = 0.0;
    EXPECT_TRUE(WhySurfaceFinishRefused(cast).empty()) << WhySurfaceFinishRefused(cast);
}

TEST(AnnotationTest, M41_SURF_002_ARoughnessOfNothingAndAnEmptyBandAreREFUSED) {
    SurfaceFinishSpec spec;
    spec.raMicrometres = 0.0;
    EXPECT_FALSE(WhySurfaceFinishRefused(spec).empty())
        << "Ra 0 is a surface nobody can make, and it draws as a very good one";

    spec.raMicrometres = 1.6;
    spec.raLowerMicrometres = 3.2;
    EXPECT_FALSE(WhySurfaceFinishRefused(spec).empty())
        << "a band whose floor is above its ceiling was accepted";
    spec.raLowerMicrometres = 0.8;
    EXPECT_TRUE(WhySurfaceFinishRefused(spec).empty()) << WhySurfaceFinishRefused(spec);
    EXPECT_EQ(SurfaceFinishText(spec), "Ra 0.8-1.6");

    spec.raLowerMicrometres = 1.6;
    EXPECT_FALSE(WhySurfaceFinishRefused(spec).empty())
        << "a band with no width was accepted";
}

TEST(AnnotationTest, M41_SURF_003_AnyPositiveRaIsLegalAndTheSeriesIsOnlyASuggestion) {
    // THE RULE THAT DOES NOT APPLY HERE, said out loud because the last two
    // milestones both ended with "refuse anything not in the table". A fit
    // this build cannot compute is a standard it made up; a roughness of 1.2
    // is an instruction a designer chose, and refusing it would refuse
    // perfectly good drawings.
    const std::vector<double>& series = PreferredRaSeries();
    EXPECT_NE(std::find(series.begin(), series.end(), 1.6), series.end());
    EXPECT_EQ(std::find(series.begin(), series.end(), 1.2), series.end())
        << "1.2 is not in the R10 series";

    SurfaceFinishSpec odd;
    odd.raMicrometres = 1.2;
    EXPECT_TRUE(WhySurfaceFinishRefused(odd).empty())
        << "a roughness outside the preferred series was refused";
    EXPECT_EQ(SurfaceFinishText(odd), "Ra 1.2");

    // ...and the series really is the published one, in order.
    ASSERT_FALSE(series.empty());
    EXPECT_TRUE(std::is_sorted(series.begin(), series.end()));
    EXPECT_NEAR(series.front(), 0.012, 1e-9);
    EXPECT_NEAR(series.back(), 50.0, 1e-9);
}

TEST(AnnotationTest, M41_SURF_004_TheTextCarriesTheProcessTheAllowanceAndTheLAY) {
    // Each of these is a separate instruction, and a symbol that dropped one
    // is a surface with a requirement nobody was told about.
    SurfaceFinishSpec spec;
    spec.symbol = SurfaceSymbol::Machined;
    spec.raMicrometres = 0.8;
    spec.process = "ground";
    spec.lay = SurfaceLay::Perpendicular;
    spec.machiningAllowanceMm = 0.5;

    const std::string text = SurfaceFinishText(spec);
    EXPECT_NE(text.find("ground"), std::string::npos) << text;
    EXPECT_NE(text.find("Ra 0.8"), std::string::npos) << text;
    EXPECT_NE(text.find("+0.5"), std::string::npos) << text;
    EXPECT_NE(text.find(SymbolOfLay(SurfaceLay::Perpendicular)), std::string::npos) << text;

    // The lay symbols are distinct -- marks running the wrong way across a
    // sealing face leak, and = versus X is the whole message.
    std::vector<std::string> marks;
    for (const SurfaceLay lay :
         {SurfaceLay::Parallel, SurfaceLay::Perpendicular, SurfaceLay::Crossed,
          SurfaceLay::Multi, SurfaceLay::Circular, SurfaceLay::Radial,
          SurfaceLay::Particulate})
        marks.push_back(SymbolOfLay(lay));
    std::sort(marks.begin(), marks.end());
    EXPECT_EQ(std::adjacent_find(marks.begin(), marks.end()), marks.end())
        << "two lay directions share a symbol";
    EXPECT_TRUE(SymbolOfLay(SurfaceLay::Unspecified).empty())
        << "an unspecified lay drew a symbol anyway";
}

} // namespace
