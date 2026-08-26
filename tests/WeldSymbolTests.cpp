// M47.1 -- the weld symbol, as rules rather than glyphs.
//
// Every specification refused below DRAWS PERFECTLY. A triangle is a triangle
// whichever line it sits on; three numbers in a bracket are three numbers
// whichever standard you read them by. What is at stake is whether the shop
// makes the joint the designer meant:
//
//   * a bead with no side -- there is nowhere to put one, by construction
//   * a5 and z5 -- different welds, one number, thirty per cent of the metal
//   * n x l (e) -- ISO's gap, not AWS's pitch, and the run ends 60 short
//   * staggered against nothing -- a word with no second half
//   * a throat on a butt weld -- a letter telling the shop to measure air

#include "Core/Drawing/WeldSymbol.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

const std::string kFillet = "\xE2\x97\xBA";

WeldBead Fillet(double sizeMm, FilletSizeKind kind) {
    WeldBead bead;
    bead.type = WeldType::Fillet;
    bead.sizeMm = sizeMm;
    bead.sizeKind = kind;
    return bead;
}

TEST(WeldSymbolTest, M47_WELD_001_ABeadHasNowhereToGoExceptASide) {
    // THE WHOLE POINT OF THE TYPE. A weld symbol is not "a weld plus a side
    // field"; the side IS where the bead is stored, so there is no way to
    // write down a weld whose side is wrong, unset, or copied from the last
    // one. What remains possible is writing down NO weld, and that is refused.
    WeldSymbolSpec empty;
    EXPECT_FALSE(WhyWeldRefused(empty).empty())
        << "a symbol with no bead on either side was accepted";
    EXPECT_TRUE(WeldSymbolText(empty).empty());

    WeldSymbolSpec arrow;
    arrow.arrowSide = Fillet(5.0, FilletSizeKind::Throat);
    EXPECT_TRUE(WhyWeldRefused(arrow).empty()) << WhyWeldRefused(arrow);

    // A WELD ON THE FAR SIDE ONLY IS ORDINARY, not an error -- the arrow side
    // being empty is a real instruction, not a missing field.
    WeldSymbolSpec other;
    other.otherSide = Fillet(5.0, FilletSizeKind::Throat);
    EXPECT_TRUE(WhyWeldRefused(other).empty()) << WhyWeldRefused(other);

    // AND THE TEXT SAYS WHICH. On paper the side is which line the triangle
    // sits on; in text there is no line, so the word has to be there or the
    // ambiguity comes straight back.
    EXPECT_NE(WeldSymbolText(arrow), WeldSymbolText(other));
    EXPECT_NE(WeldSymbolText(arrow).find("arrow"), std::string::npos);
    EXPECT_NE(WeldSymbolText(other).find("other"), std::string::npos);
}

TEST(WeldSymbolTest, M47_WELD_002_AFilletSizeHasToSayThroatOrLeg) {
    // a5 AND z5 ARE DIFFERENT WELDS. Left unsaid, whichever one the program
    // assumed would be right for half its users and silently wrong for the
    // rest -- and the drawing would carry one number either way.
    WeldSymbolSpec spec;
    spec.arrowSide = Fillet(5.0, FilletSizeKind::Unspecified);
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a fillet size with no letter was accepted";
    // AND NOTHING REACHES THE PAPER. Also found by the mutation gate: the
    // empty-spec check above passes whether or not the refusal gate is in
    // WeldSymbolText, because an empty spec has no bead to write either way.
    // THIS one has a bead -- so without the gate it draws "a5" and a triangle,
    // which is a complete-looking instruction the program has already said it
    // will not stand behind.
    EXPECT_TRUE(WeldSymbolText(spec).empty())
        << "a refused weld still wrote a size: " << WeldSymbolText(spec);

    spec.arrowSide = Fillet(5.0, FilletSizeKind::Throat);
    const std::string throat = WeldSymbolText(spec);
    spec.arrowSide = Fillet(5.0, FilletSizeKind::Leg);
    const std::string leg = WeldSymbolText(spec);
    EXPECT_NE(throat, leg) << "a throat and a leg of the same number drew the same";
    EXPECT_NE(throat.find("a5"), std::string::npos) << throat;
    EXPECT_NE(leg.find("z5"), std::string::npos) << leg;

    // AND THE CONVERSION IS ONE FUNCTION. A leg of 5 is a throat of 3.54, so a
    // z5 fillet carries about thirty per cent less metal than an a5 one.
    EXPECT_NEAR(ThroatOfMm(5.0, FilletSizeKind::Throat), 5.0, 1e-9);
    EXPECT_NEAR(ThroatOfMm(5.0, FilletSizeKind::Leg), 5.0 / std::sqrt(2.0), 1e-9);
    EXPECT_LT(ThroatOfMm(5.0, FilletSizeKind::Leg), 0.75 * 5.0);
}

TEST(WeldSymbolTest, M47_WELD_003_AFilletNeedsASizeAndAButtCannotHaveALeg) {
    WeldSymbolSpec spec;
    spec.arrowSide = Fillet(0.0, FilletSizeKind::Throat);
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a fillet with no size was accepted";

    // A FULL-PENETRATION BUTT WELD LEGITIMATELY HAS NO SIZE. The rule is about
    // fillets, not about sizes in general, and a blanket "every weld needs a
    // number" would refuse correct drawings.
    WeldBead butt;
    butt.type = WeldType::SingleV;
    spec.arrowSide = butt;
    EXPECT_TRUE(WhyWeldRefused(spec).empty()) << WhyWeldRefused(spec);

    // BUT IT CANNOT BE SIZED AS A THROAT OR A LEG. Those letters mean the
    // triangle inside a fillet; on a V they would print an instruction to
    // measure something that is not there.
    butt.sizeMm = 10.0;
    butt.sizeKind = FilletSizeKind::Leg;
    spec.arrowSide = butt;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a butt weld accepted a leg size";
}

TEST(WeldSymbolTest, M47_WELD_004_TheBracketIsAGapAndNotAPitch) {
    // ISO 2553 PUTS THE SPACE BETWEEN WELDS IN THE BRACKETS; AWS A2.4 PUTS THE
    // PITCH THERE. Same three numbers, same layout, welds in different places.
    WeldRun run;
    run.count = 4;
    run.lengthMm = 40.0;
    run.gapMm = 60.0;

    EXPECT_NEAR(DepositedLengthMm(run), 160.0, 1e-9);
    // Four forties and THREE gaps of sixty: 340 from the first weld to the
    // last. Read the American way -- sixty as the centre-to-centre pitch --
    // the same symbol covers three pitches plus one weld, 220, and the run
    // ends 120 short of where the designer put it. Nothing on the paper looks
    // wrong either way, which is the whole reason the field is named gapMm.
    EXPECT_NEAR(RunExtentMm(run), 340.0, 1e-9);
    const double misreadAsAws = 3.0 * run.gapMm + run.lengthMm;
    EXPECT_NEAR(misreadAsAws, 220.0, 1e-9);
    EXPECT_GT(RunExtentMm(run), misreadAsAws);
    // AND THE PITCH IS OFFERED, so nobody derives it from the wrong end.
    EXPECT_NEAR(PitchMm(run), 100.0, 1e-9);
    EXPECT_GT(RunExtentMm(run), DepositedLengthMm(run));

    WeldBead bead = Fillet(6.0, FilletSizeKind::Leg);
    bead.run = run;
    WeldSymbolSpec spec;
    spec.arrowSide = bead;
    ASSERT_TRUE(WhyWeldRefused(spec).empty()) << WhyWeldRefused(spec);
    const std::string text = WeldSymbolText(spec);
    EXPECT_NE(text.find("(60)"), std::string::npos) << text;
    EXPECT_NE(text.find(kFillet), std::string::npos) << text;
}

TEST(WeldSymbolTest, M47_WELD_005_ARunAndItsGapHaveToAgree) {
    WeldBead bead = Fillet(6.0, FilletSizeKind::Leg);
    WeldSymbolSpec spec;

    WeldRun none;
    none.count = 0;
    none.lengthMm = 50.0;
    bead.run = none;
    spec.arrowSide = bead;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a run of no welds was accepted";

    // SEVERAL WELDS WITH NO GAP ARE ONE CONTINUOUS WELD written the hard way,
    // and it draws as an intermittent one.
    WeldRun touching;
    touching.count = 3;
    touching.lengthMm = 50.0;
    touching.gapMm = 0.0;
    bead.run = touching;
    spec.arrowSide = bead;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "welds with no gap were accepted";

    // AND ONE WELD HAS NOTHING TO BE SPACED FROM.
    WeldRun lonely;
    lonely.count = 1;
    lonely.lengthMm = 50.0;
    lonely.gapMm = 30.0;
    bead.run = lonely;
    spec.arrowSide = bead;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a single weld carried a gap";

    lonely.gapMm = 0.0;
    bead.run = lonely;
    spec.arrowSide = bead;
    EXPECT_TRUE(WhyWeldRefused(spec).empty()) << WhyWeldRefused(spec);

    // A LENGTH IS REQUIRED FOR A WELD YOU RUN ALONG.
    WeldRun nolength;
    nolength.count = 3;
    nolength.lengthMm = 0.0;
    nolength.gapMm = 30.0;
    bead.run = nolength;
    spec.arrowSide = bead;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "an intermittent weld with no length passed";
}

TEST(WeldSymbolTest, M47_WELD_006_ASpotIsCountedAndNotRunAlong) {
    WeldBead spot;
    spot.type = WeldType::Spot;
    spot.sizeMm = 0.0;
    WeldSymbolSpec spec;
    spec.arrowSide = spot;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a spot weld with no diameter was accepted";

    spot.sizeMm = 6.0;
    WeldRun run;
    run.count = 5;
    run.lengthMm = 40.0;   // a spot has no length
    run.gapMm = 40.0;
    spot.run = run;
    spec.arrowSide = spot;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a spot weld carried a length";

    run.lengthMm = 0.0;
    spot.run = run;
    spec.arrowSide = spot;
    ASSERT_TRUE(WhyWeldRefused(spec).empty()) << WhyWeldRefused(spec);
    // A COUNTED WELD WRITES NO LENGTH AND NO SIZE LETTER: five spots at forty,
    // not "s6" and not "5 x 0".
    const std::string text = WeldSymbolText(spec);
    EXPECT_EQ(text.find("s6"), std::string::npos) << text;
    EXPECT_EQ(text.find("a6"), std::string::npos) << text;
    EXPECT_NE(text.find("(40)"), std::string::npos) << text;
}

TEST(WeldSymbolTest, M47_WELD_006B_APlugIsCountedTheSameWayASpotIs) {
    // FOUND BY THE MUTATION GATE. Making IsCounted answer for the spot alone
    // changed nothing any test could see: every counted-weld check used a
    // spot, so the plug went through the rules meant for welds you run along.
    // A plug would then have been allowed a LENGTH -- a number the shop cannot
    // act on, printed as though it could.
    WeldBead plug;
    plug.type = WeldType::Plug;
    plug.sizeMm = 0.0;
    WeldSymbolSpec spec;
    spec.arrowSide = plug;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a plug weld with no width was accepted";

    plug.sizeMm = 12.0;
    WeldRun run;
    run.count = 4;
    run.lengthMm = 25.0;   // a plug is a hole, not a run
    run.gapMm = 60.0;
    plug.run = run;
    spec.arrowSide = plug;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "a plug weld carried a length";

    run.lengthMm = 0.0;
    plug.run = run;
    spec.arrowSide = plug;
    ASSERT_TRUE(WhyWeldRefused(spec).empty()) << WhyWeldRefused(spec);
    const std::string text = WeldSymbolText(spec);
    EXPECT_EQ(text.find("s12"), std::string::npos) << text;
    EXPECT_NE(text.find("(60)"), std::string::npos) << text;
}

TEST(WeldSymbolTest, M47_WELD_007_StaggeredNeedsTwoRunsToBeStaggeredAgainst) {
    // A STAGGERED WELD IS THE RELATIONSHIP BETWEEN TWO INTERMITTENT RUNS. With
    // one run, or none, the word describes nothing -- and the symbol draws as
    // an ordinary double fillet, so nobody notices the instruction went away.
    WeldRun run;
    run.count = 4;
    run.lengthMm = 40.0;
    run.gapMm = 60.0;

    WeldBead arrow = Fillet(5.0, FilletSizeKind::Throat);
    arrow.run = run;
    WeldSymbolSpec spec;
    spec.arrowSide = arrow;
    spec.staggered = true;
    EXPECT_FALSE(WhyWeldRefused(spec).empty()) << "staggered against nothing was accepted";

    // Both sides present but the far one continuous: still nothing to stagger.
    spec.otherSide = Fillet(5.0, FilletSizeKind::Throat);
    EXPECT_FALSE(WhyWeldRefused(spec).empty())
        << "staggered was accepted against a continuous weld";

    WeldBead other = Fillet(5.0, FilletSizeKind::Throat);
    other.run = run;
    spec.otherSide = other;
    ASSERT_TRUE(WhyWeldRefused(spec).empty()) << WhyWeldRefused(spec);
    EXPECT_NE(WeldSymbolText(spec).find("staggered"), std::string::npos);
}

TEST(WeldSymbolTest, M47_WELD_008_AllRoundAndFieldBelongToTheSymbolNotASide) {
    // THEY ARE DRAWN ON THE ELBOW, and they describe the whole instruction.
    // Held on a bead they would allow "weld all round, but only on the arrow
    // side", which is not something a welder can be told -- so the type does
    // not have a place to say it.
    WeldSymbolSpec spec;
    spec.arrowSide = Fillet(4.0, FilletSizeKind::Leg);
    spec.allAround = true;
    spec.fieldWeld = true;
    spec.tail = "ISO 4063-135";
    ASSERT_TRUE(WhyWeldRefused(spec).empty()) << WhyWeldRefused(spec);

    const std::string text = WeldSymbolText(spec);
    EXPECT_NE(text.find("all round"), std::string::npos) << text;
    EXPECT_NE(text.find("field weld"), std::string::npos) << text;
    EXPECT_NE(text.find("ISO 4063-135"), std::string::npos) << text;
}

TEST(WeldSymbolTest, M47_WELD_009_TheProcessListNamesButDoesNotGate) {
    // THE TAIL IS FREE TEXT and stays free text: what goes there is the shop's
    // own reference. The lookup exists so a user interface can say what 135
    // means, and knowing nothing about a reference is NOT a refusal -- the
    // opposite call from M37's fits and M39's tap drills, made deliberately
    // and for the reason M41 made it for Ra.
    EXPECT_EQ(NameOfWeldProcess("135"), NameOfWeldProcess("ISO 4063-135"));
    EXPECT_FALSE(NameOfWeldProcess("135").empty());
    EXPECT_FALSE(NameOfWeldProcess("111").empty());
    EXPECT_TRUE(NameOfWeldProcess("WPS-17/b").empty());

    WeldSymbolSpec spec;
    spec.arrowSide = Fillet(4.0, FilletSizeKind::Leg);
    spec.tail = "WPS-17/b";
    EXPECT_TRUE(WhyWeldRefused(spec).empty())
        << "an unrecognised process reference was refused, which would refuse real drawings";
}

TEST(WeldSymbolTest, M47_WELD_010_EveryTypeAndContourSurvivesItsOwnNames) {
    // WRITTEN AND READ FROM ONE LIST. A name that round-trips through the
    // wrong branch does not throw: it becomes a fillet, and a butt weld read
    // as a fillet is a joint with no penetration at all.
    const WeldType types[] = {WeldType::SquareButt, WeldType::SingleV,   WeldType::SingleBevel,
                              WeldType::SingleU,    WeldType::SingleJ,   WeldType::Fillet,
                              WeldType::Plug,       WeldType::Spot,      WeldType::Seam,
                              WeldType::Backing,    WeldType::Surfacing, WeldType::Edge};
    for (const WeldType type : types) {
        WeldType back = WeldType::Fillet;
        ASSERT_TRUE(ParseWeldType(toString(type), back)) << toString(type);
        EXPECT_EQ(back, type);
        EXPECT_FALSE(SymbolOfWeldType(type).empty());
    }
    // A NAME THIS BUILD DOES NOT KNOW IS A NO, not a fillet.
    WeldType unknown = WeldType::Fillet;
    EXPECT_FALSE(ParseWeldType("double-v", unknown));
    EXPECT_EQ(unknown, WeldType::Fillet);

    const WeldContour contours[] = {WeldContour::AsWelded, WeldContour::Flat,
                                    WeldContour::Convex, WeldContour::Concave,
                                    WeldContour::Blended};
    for (const WeldContour contour : contours) {
        WeldContour back = WeldContour::AsWelded;
        ASSERT_TRUE(ParseWeldContour(toString(contour), back)) << toString(contour);
        EXPECT_EQ(back, contour);
    }
    // AS-WELDED IS THE ABSENCE OF A SYMBOL, and every other contour has one.
    EXPECT_TRUE(SymbolOfContour(WeldContour::AsWelded).empty());
    EXPECT_FALSE(SymbolOfContour(WeldContour::Flat).empty());
    EXPECT_FALSE(SymbolOfContour(WeldContour::Convex).empty());
    EXPECT_FALSE(SymbolOfContour(WeldContour::Concave).empty());
    EXPECT_FALSE(SymbolOfContour(WeldContour::Blended).empty());
    EXPECT_NE(SymbolOfContour(WeldContour::Convex), SymbolOfContour(WeldContour::Concave));

    for (const FilletSizeKind kind :
         {FilletSizeKind::Unspecified, FilletSizeKind::Throat, FilletSizeKind::Leg}) {
        FilletSizeKind back = FilletSizeKind::Unspecified;
        ASSERT_TRUE(ParseFilletSizeKind(toString(kind), back)) << toString(kind);
        EXPECT_EQ(back, kind);
    }
}

} // namespace
