// M37 -- tolerances.
//
// THE RISK HERE IS NOT LOGIC, IT IS DATA.
//
// A fit table with a typo produces a drawing that looks completely normal and
// specifies the wrong clearance. Nobody re-derives an H7 by hand, so nothing
// catches it until a batch of parts does not go together. That is why every
// number below is checked against the PUBLISHED ISO 286 table by name, and why
// a code the implementation cannot compute must come back as nothing rather
// than as a plausible answer.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Drawing/Tolerance.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

using namespace paramcad;

// Micrometres, because that is the unit the standard is printed in and
// comparing in millimetres hides which digit is wrong.
double Micro(double millimetres) { return millimetres * 1000.0; }

} // namespace

// =============================================================================
// The IT grades
// =============================================================================

TEST(ToleranceTest, M37_IT_001_TheGradeWidthsMatchThePublishedTable) {
    // ISO 286-1 table 1. Each of these is a number somebody can look up.
    struct Case { double size; int grade; double micrometres; };
    const Case cases[] = {
        {25.0, 6, 13.0},   {25.0, 7, 21.0},   {25.0, 8, 33.0},
        {25.0, 9, 52.0},   {25.0, 10, 84.0},  {25.0, 11, 130.0},
        {10.0, 7, 15.0},   {50.0, 7, 25.0},   {100.0, 7, 35.0},
        {50.0, 6, 16.0},   {80.0, 9, 74.0},   {80.0, 10, 120.0},
        {3.0, 7, 10.0},    {200.0, 7, 46.0},  {500.0, 7, 63.0},
    };
    for (const Case& one : cases) {
        const std::optional<double> width = StandardToleranceMm(one.size, one.grade);
        ASSERT_TRUE(width.has_value())
            << "IT" << one.grade << " at " << one.size << " came back as nothing";
        EXPECT_NEAR(Micro(*width), one.micrometres, 0.5)
            << "IT" << one.grade << " at " << one.size << " mm";
    }
}

TEST(ToleranceTest, M37_IT_002_TheSizeStepBoundARIESAreINCLUSIVEAtTheTop) {
    // The steps are "over A up to and INCLUDING B". Getting 30 into the 30..50
    // step instead of 18..30 shifts every tolerance on a 30 mm shaft by a whole
    // band -- and the drawing looks exactly the same.
    const std::optional<double> at30 = StandardToleranceMm(30.0, 7);
    const std::optional<double> justOver = StandardToleranceMm(30.001, 7);
    ASSERT_TRUE(at30.has_value());
    ASSERT_TRUE(justOver.has_value());
    EXPECT_NEAR(Micro(*at30), 21.0, 0.5) << "30 mm fell into the wrong step";
    EXPECT_NEAR(Micro(*justOver), 25.0, 0.5);
}

TEST(ToleranceTest, M37_IT_003_AGradeOrSizeOutsideTheTableIsREFUSED) {
    // Not extrapolated. A tolerance for a size the standard does not cover is
    // a number this build invented.
    EXPECT_FALSE(StandardToleranceMm(25.0, 4).has_value()) << "IT4 is not implemented";
    EXPECT_FALSE(StandardToleranceMm(25.0, 14).has_value());
    EXPECT_FALSE(StandardToleranceMm(600.0, 7).has_value()) << "past 500 mm";
    EXPECT_FALSE(StandardToleranceMm(0.0, 7).has_value());
    EXPECT_FALSE(StandardToleranceMm(-5.0, 7).has_value());
}

// =============================================================================
// The fits
// =============================================================================

TEST(ToleranceTest, M37_FIT_001_TheCommonFitsMatchThePublishedTable) {
    struct Case { double size; const char* code; double upper; double lower; };
    const Case cases[] = {
        // Hole basis: H is the hole, and its lower deviation is zero by
        // definition -- that is what "basis" means.
        {25.0, "H7", 21.0, 0.0},
        {50.0, "H7", 25.0, 0.0},
        {10.0, "H7", 15.0, 0.0},
        {25.0, "H8", 33.0, 0.0},
        // Clearance shafts.
        {25.0, "h6", 0.0, -13.0},
        {50.0, "h6", 0.0, -16.0},
        {25.0, "g6", -7.0, -20.0},
        {30.0, "f7", -20.0, -41.0},
        {50.0, "f7", -25.0, -50.0},
        {25.0, "e8", -40.0, -73.0},
        {25.0, "d9", -65.0, -117.0},
        // Transition and interference.
        {25.0, "k6", 2.0, 15.0},
        {25.0, "m6", 8.0, 21.0},
        {25.0, "n6", 15.0, 28.0},
        {25.0, "p6", 22.0, 35.0},
        {10.0, "p6", 15.0, 24.0},
        {50.0, "p6", 26.0, 42.0},
        {25.0, "s6", 35.0, 48.0},
        {50.0, "s6", 43.0, 59.0},
    };
    for (const Case& one : cases) {
        const std::optional<Deviations> got = FitDeviation(one.size, one.code);
        ASSERT_TRUE(got.has_value())
            << one.code << " at " << one.size << " mm came back as nothing";
        // The published table lists the two deviations; which is "upper"
        // depends on the letter, so both are compared against the pair
        // whichever way round they fall.
        const double high = std::max(Micro(got->upperMm), Micro(got->lowerMm));
        const double low = std::min(Micro(got->upperMm), Micro(got->lowerMm));
        EXPECT_NEAR(high, std::max(one.upper, one.lower), 0.6)
            << one.code << " at " << one.size << " mm, upper";
        EXPECT_NEAR(low, std::min(one.upper, one.lower), 0.6)
            << one.code << " at " << one.size << " mm, lower";
        EXPECT_TRUE(got->ordered()) << one.code << " came back upside down";
    }
}

TEST(ToleranceTest, M37_FIT_002_TheClassicH7g6ClearanceComesOutRight) {
    // The one fit every mechanical engineer knows by heart: at 25 mm, H7/g6
    // gives between 7 and 41 micrometres of clearance. Checking the PAIR
    // rather than the two halves is what catches a sign error -- each half can
    // be plausible on its own while the fit they make is an interference.
    const std::optional<Deviations> hole = FitDeviation(25.0, "H7");
    const std::optional<Deviations> shaft = FitDeviation(25.0, "g6");
    ASSERT_TRUE(hole.has_value());
    ASSERT_TRUE(shaft.has_value());

    const double minimumClearance = Micro(hole->lowerMm - shaft->upperMm);
    const double maximumClearance = Micro(hole->upperMm - shaft->lowerMm);
    EXPECT_NEAR(minimumClearance, 7.0, 0.6);
    EXPECT_NEAR(maximumClearance, 41.0, 0.6);
    EXPECT_GT(minimumClearance, 0.0) << "H7/g6 came out as an interference fit";
}

TEST(ToleranceTest, M37_FIT_003_H7p6IsAnINTERFERENCEFitAtEverySize) {
    // The other end of the same check. A p6 that came out two micrometres
    // light is a press fit that has quietly become a transition fit, and
    // nothing on the drawing says so -- which is why p and s carry the
    // published increments rather than a formula that nearly works.
    for (const double size : {10.0, 25.0, 50.0}) {
        const std::optional<Deviations> hole = FitDeviation(size, "H7");
        const std::optional<Deviations> shaft = FitDeviation(size, "p6");
        ASSERT_TRUE(hole.has_value());
        ASSERT_TRUE(shaft.has_value()) << size;
        // The smallest shaft is still bigger than the biggest hole.
        EXPECT_GT(Micro(shaft->lowerMm), Micro(hole->lowerMm))
            << "p6 at " << size << " mm is not an interference fit";
    }
}

TEST(ToleranceTest, M37_FIT_004_AFitThisBuildCannotComputeIsREFUSED) {
    // NOT defaulted to zero. Turning an unknown fit into "no tolerance" would
    // silently unspecify a feature somebody chose a fit for on purpose.
    EXPECT_FALSE(FitDeviation(25.0, "J7").has_value()) << "only H is implemented for holes";
    // A TWO-LETTER CODE IS REFUSED BY THE GRADE PARSE, before the letter is
    // ever looked at -- so it does NOT test the letter. A first draft used
    // only this one and a mutation that turned every unknown shaft letter into
    // h survived, because nothing ever reached that branch.
    EXPECT_FALSE(FitDeviation(25.0, "zc6").has_value());
    // These do reach it: one letter, a real grade, and not implemented.
    EXPECT_FALSE(FitDeviation(25.0, "u6").has_value())
        << "an unimplemented shaft letter was answered as h";
    EXPECT_FALSE(FitDeviation(25.0, "c11").has_value());
    EXPECT_FALSE(FitDeviation(25.0, "j6").has_value());
    EXPECT_FALSE(FitDeviation(25.0, "r6").has_value());
    EXPECT_FALSE(FitDeviation(25.0, "H").has_value()) << "no grade";
    EXPECT_FALSE(FitDeviation(25.0, "7").has_value()) << "no letter";
    EXPECT_FALSE(FitDeviation(25.0, "H7x").has_value()) << "a typo is not a fit";
    EXPECT_FALSE(FitDeviation(25.0, "").has_value());
    // ...and s beyond where the increments are written down.
    EXPECT_FALSE(FitDeviation(100.0, "s6").has_value())
        << "s above 50 mm was answered from a table that does not cover it";
}

// =============================================================================
// General tolerances
// =============================================================================

TEST(ToleranceTest, M37_GEN_001_ISO2768MatchesItsPublishedTable) {
    struct Case { double size; GeneralToleranceClass klass; double millimetres; };
    const Case cases[] = {
        {10.0, GeneralToleranceClass::Fine, 0.1},
        {10.0, GeneralToleranceClass::Medium, 0.2},
        {10.0, GeneralToleranceClass::Coarse, 0.5},
        {10.0, GeneralToleranceClass::VeryCoarse, 1.0},
        {100.0, GeneralToleranceClass::Medium, 0.3},
        {200.0, GeneralToleranceClass::Medium, 0.5},
        {2.0, GeneralToleranceClass::Medium, 0.1},
        {500.0, GeneralToleranceClass::Coarse, 2.0},
    };
    for (const Case& one : cases) {
        const std::optional<double> got = GeneralToleranceMm(one.size, one.klass);
        ASSERT_TRUE(got.has_value()) << one.size << " mm, " << toString(one.klass);
        EXPECT_NEAR(*got, one.millimetres, 1e-9)
            << one.size << " mm, " << toString(one.klass);
    }
}

TEST(ToleranceTest, M37_GEN_002_WhereTheStandardSaysNOTHINGSoDoesThis) {
    // Below 0.5 mm ISO 2768 asks for the size to be marked individually. An
    // answer there would be putting a tolerance on a size the standard
    // deliberately leaves open.
    EXPECT_FALSE(GeneralToleranceMm(0.3, GeneralToleranceClass::Medium).has_value());
    // ...and the classes the table does not cover at a given size.
    EXPECT_FALSE(GeneralToleranceMm(2.0, GeneralToleranceClass::VeryCoarse).has_value())
        << "v is not defined below 3 mm";
    EXPECT_FALSE(GeneralToleranceMm(3000.0, GeneralToleranceClass::Fine).has_value())
        << "f is not defined above 2000 mm";
    // ...and 2000 itself IS covered, because the bands are inclusive at the
    // top. A first draft asserted the opposite here and the table was right.
    EXPECT_TRUE(GeneralToleranceMm(2000.0, GeneralToleranceClass::Fine).has_value());
    // No class at all means the drawing says nothing about unmarked sizes,
    // which is a real state and not an error.
    EXPECT_FALSE(GeneralToleranceMm(10.0, GeneralToleranceClass::None).has_value());
}

TEST(ToleranceTest, M37_GEN_003_TheNoteIsWhatTheDrawingPRINTS) {
    EXPECT_EQ(GeneralToleranceNote(GeneralToleranceClass::Medium), "ISO 2768-m");
    EXPECT_EQ(GeneralToleranceNote(GeneralToleranceClass::Fine), "ISO 2768-f");
    EXPECT_TRUE(GeneralToleranceNote(GeneralToleranceClass::None).empty())
        << "a drawing with no general class must print nothing, not the words 'none'";
}

TEST(ToleranceTest, M37_ENUM_001_EverySpellingSurvivesBeingWrittenAndReadBack) {
    // The pair toString / Parse is two things that must agree, and this is what
    // makes disagreeing impossible to ship.
    for (const ToleranceKind kind :
         {ToleranceKind::None, ToleranceKind::Symmetric, ToleranceKind::Deviation,
          ToleranceKind::Limits, ToleranceKind::Basic, ToleranceKind::Fit}) {
        ToleranceKind back = ToleranceKind::None;
        ASSERT_TRUE(ParseToleranceKind(toString(kind), back)) << toString(kind);
        EXPECT_EQ(back, kind);
    }
    for (const GeneralToleranceClass klass :
         {GeneralToleranceClass::None, GeneralToleranceClass::Fine,
          GeneralToleranceClass::Medium, GeneralToleranceClass::Coarse,
          GeneralToleranceClass::VeryCoarse}) {
        GeneralToleranceClass back = GeneralToleranceClass::None;
        ASSERT_TRUE(ParseGeneralToleranceClass(toString(klass), back)) << toString(klass);
        EXPECT_EQ(back, klass);
    }
    ToleranceKind unknown = ToleranceKind::Fit;
    EXPECT_FALSE(ParseToleranceKind("Approximate", unknown));
    EXPECT_EQ(unknown, ToleranceKind::Fit) << "a failed parse still wrote";
}

// =============================================================================
// On a dimension
// =============================================================================

namespace {

// A 25 mm line with a dimension on it, which is what every case below needs.
struct Rig {
    DrawingDocument document{"Sheet"};
    ObjectId line = kInvalidObjectId;
    ObjectId dimension = kInvalidObjectId;

    Rig() {
        line = document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{25.0, 0.0}}).id();
        dimension = document
                        .addDimension(DimensionKind::Linear,
                                      DimensionAnchor::onEntity(line, 0),
                                      DimensionAnchor::onEntity(line, 1), Vec2{12.5, 15.0})
                        .id();
    }
    const DrawingDimension& dim() const { return *document.findDimension(dimension); }
};

} // namespace

TEST(ToleranceTest, M37_DIM_001_ASymmetricToleranceFollowsTheSize) {
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Symmetric;
    tolerance.upperMm = 0.1;
    tolerance.lowerMm = -0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    // U+00B1, the plus-minus sign.
    EXPECT_EQ(rig.document.dimensionText(rig.dim()), "25.00 \xC2\xB1" "0.100");
}

TEST(ToleranceTest, M37_DIM_002_ADeviationKeepsItsSIGNSBothWays) {
    // "+0.20/+0.10" is a real specification -- both limits above nominal -- and
    // dropping the second plus would read as a symmetric-ish pair it is not.
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Deviation;
    tolerance.upperMm = 0.2;
    tolerance.lowerMm = 0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    EXPECT_EQ(rig.document.dimensionToleranceText(rig.dim()), "+0.200/+0.100");

    tolerance.upperMm = 0.2;
    tolerance.lowerMm = -0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    EXPECT_EQ(rig.document.dimensionToleranceText(rig.dim()), "+0.200/-0.100");
}

TEST(ToleranceTest, M37_DIM_003_LIMITSREPLACETheSizeRatherThanFollowingIt) {
    // A drawing showing "25.00 25.10/24.90" states the same size twice. The
    // PAIR is the dimension.
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Limits;
    tolerance.upperMm = 0.1;
    tolerance.lowerMm = -0.1;
    tolerance.decimals = 2;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    EXPECT_EQ(rig.document.dimensionText(rig.dim()), "25.10/24.90");
}

TEST(ToleranceTest, M37_DIM_004_AFitDERIVESItsNumbersFromTheSizeRIGHTNOW) {
    // THE WHOLE REASON A FIT STORES ITS CODE. An H7 on a 25 bore that became a
    // 30 bore is still an H7, and its numbers are not the ones it had
    // yesterday. A drawing that stored +0.021 would keep printing it.
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Fit;
    tolerance.fitCode = "H7";
    tolerance.decimals = 3;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    EXPECT_EQ(rig.document.dimensionText(rig.dim()), "25.00 H7 0.021/0.000");

    // The line grows to 50. Same fit, different numbers.
    ASSERT_TRUE(rig.document.transformEntities({rig.line},
                                               Matrix2D::scaleAbout(Vec2{0, 0}, 2.0)));
    EXPECT_EQ(rig.document.dimensionText(rig.dim()), "50.00 H7 0.025/0.000")
        << "the fit kept the numbers it had at the old size";
}

TEST(ToleranceTest, M37_DIM_005_AFitThisBuildCannotComputeIsREFUSEDAtTheDocument) {
    // Accepted-and-blank would leave a drawing that looks toleranced and
    // specifies nothing.
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Fit;
    tolerance.fitCode = "J7";
    EXPECT_FALSE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    EXPECT_EQ(rig.dim().tolerance().kind, ToleranceKind::None)
        << "a refused fit was applied anyway";
}

TEST(ToleranceTest, M37_DIM_006_ABasicDimensionIsBOXEDAndStatesNoNumbers) {
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Basic;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    EXPECT_TRUE(rig.document.dimensionIsBasic(rig.dim()));
    EXPECT_TRUE(rig.document.dimensionToleranceText(rig.dim()).empty())
        << "a basic dimension printed a tolerance, which it has none of";
    EXPECT_EQ(rig.document.dimensionText(rig.dim()), "25.00");
}

TEST(ToleranceTest, M37_DIM_007_TheToleranceShowsONEMOREDecimalThanTheSize) {
    // A tolerance shown to fewer decimals than the size it qualifies rounds
    // away the thing it exists to state.
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Symmetric;
    tolerance.upperMm = 0.05;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    // The style is ISO-25, two decimals, so the tolerance gets three.
    EXPECT_EQ(rig.document.dimensionToleranceText(rig.dim()), "\xC2\xB1" "0.050");
    EXPECT_NE(rig.document.dimensionToleranceText(rig.dim()), "\xC2\xB1" "0.05");
}

TEST(ToleranceTest, M37_DIM_007b_AToleranceNEVERRoundsToZERO) {
    // A drawing that states a fit and prints 0.0 is worse than one that prints
    // nothing, because it looks finished.
    //
    // "One more decimal than the size" is the starting point, but on a sheet
    // styled to whole millimetres that is ONE decimal, and an H7 at 60 mm
    // rounds to 0.0. Found by the self test on a sheet an earlier check had
    // restyled.
    Rig rig;
    DimensionStyle whole{"tmp"};
    whole.setDecimals(0);
    ASSERT_TRUE(rig.document.editDimensionStyle(rig.document.currentDimensionStyleId(),
                                                whole));
    DimensionTolerance fit;
    fit.kind = ToleranceKind::Fit;
    fit.fitCode = "H7";
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, fit));

    const std::string text = rig.document.dimensionToleranceText(rig.dim());
    EXPECT_NE(text.find("0.02"), std::string::npos)
        << "H7 at 25 mm printed as '" << text << "', which states no tolerance at all";

    // ...and a tolerance that is ALREADY visible at one decimal does not grow
    // extra digits it does not need.
    DimensionTolerance coarse;
    coarse.kind = ToleranceKind::Symmetric;
    coarse.upperMm = 0.5;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, coarse));
    EXPECT_EQ(rig.document.dimensionToleranceText(rig.dim()), "\xC2\xB1" "0.5");
}

TEST(ToleranceTest, M37_DIM_008_TheTextAndTheToleranceCannotDISAGREE) {
    // dimensionText is BUILT FROM dimensionToleranceText -- the canvas needs
    // the two halves separately to set the tolerance smaller, and defining the
    // whole in terms of the part is what stops them saying different things.
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Symmetric;
    tolerance.upperMm = 0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    const std::string whole = rig.document.dimensionText(rig.dim());
    const std::string part = rig.document.dimensionToleranceText(rig.dim());
    ASSERT_FALSE(part.empty());
    EXPECT_NE(whole.find(part), std::string::npos)
        << "the dimension text does not contain the tolerance text";
}

TEST(ToleranceTest, M37_DIM_009_ADanglingDimensionShowsNoToleranceEither) {
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Symmetric;
    tolerance.upperMm = 0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    ASSERT_TRUE(rig.document.removeObject(rig.line));
    // "<?> ±0.100" would be a drawing stating how close to hold a size it
    // cannot state.
    EXPECT_EQ(rig.document.dimensionText(rig.dim()), "<?>");
}

TEST(ToleranceTest, M37_DIM_010_EveryToleranceEditComesBack) {
    Rig rig;
    DimensionTolerance first;
    first.kind = ToleranceKind::Symmetric;
    first.upperMm = 0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, first));
    DimensionTolerance second;
    second.kind = ToleranceKind::Fit;
    second.fitCode = "g6";
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, second));

    ASSERT_TRUE(rig.document.undo());
    EXPECT_EQ(rig.dim().tolerance().kind, ToleranceKind::Symmetric);
    EXPECT_NEAR(rig.dim().tolerance().upperMm, 0.1, 1e-12);
    ASSERT_TRUE(rig.document.undo());
    EXPECT_EQ(rig.dim().tolerance().kind, ToleranceKind::None);

    while (rig.document.canRedo()) ASSERT_TRUE(rig.document.redo());
    EXPECT_EQ(rig.dim().tolerance().kind, ToleranceKind::Fit);
    EXPECT_EQ(rig.dim().tolerance().fitCode, "g6");
}

// =============================================================================
// The sheet's general class
// =============================================================================

TEST(ToleranceTest, M37_GEN_010_TheGeneralClassIsTheSHEETSAndComesBack) {
    DrawingDocument document{"Sheet"};
    EXPECT_TRUE(document.generalToleranceNote().empty())
        << "a drawing with no class stated printed one";
    ASSERT_TRUE(document.setGeneralToleranceClass(GeneralToleranceClass::Medium));
    EXPECT_EQ(document.generalToleranceNote(), "ISO 2768-m");
    // Setting it to what it already is changes nothing and records nothing.
    EXPECT_FALSE(document.setGeneralToleranceClass(GeneralToleranceClass::Medium));

    ASSERT_TRUE(document.undo());
    EXPECT_EQ(document.generalToleranceClass(), GeneralToleranceClass::None);
    ASSERT_TRUE(document.redo());
    EXPECT_EQ(document.generalToleranceClass(), GeneralToleranceClass::Medium);
}

// =============================================================================
// The file
// =============================================================================

TEST(ToleranceTest, M37_SER_001_TolerancesSurviveASaveAndTheFITSNumbersAreNotInIt) {
    Rig rig;
    DimensionTolerance fit;
    fit.kind = ToleranceKind::Fit;
    fit.fitCode = "H7";
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, fit));
    ASSERT_TRUE(rig.document.setGeneralToleranceClass(GeneralToleranceClass::Medium));

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(rig.document, out));
    const std::string saved = out.str();
    // THE CODE, NOT THE NUMBERS. A file carrying +0.021 would keep printing it
    // the day the table is corrected.
    EXPECT_NE(saved.find("H7"), std::string::npos);
    EXPECT_EQ(saved.find("0.021"), std::string::npos)
        << "the fit's deviations were written into the file";

    std::istringstream in(saved);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingDocument& back = *loaded.document;
    ASSERT_EQ(back.dimensions().size(), 1u);
    EXPECT_EQ(back.dimensions().front()->tolerance().kind, ToleranceKind::Fit);
    EXPECT_EQ(back.dimensions().front()->tolerance().fitCode, "H7");
    EXPECT_EQ(back.generalToleranceClass(), GeneralToleranceClass::Medium);
    // ...and it still DERIVES, which a restored-but-unhooked fit would fail.
    EXPECT_EQ(back.dimensionText(*back.dimensions().front()), "25.00 H7 0.021/0.000");
    EXPECT_EQ(back.undoDepth(), 0u);
}

TEST(ToleranceTest, M37_SER_001b_AFitCarriesNoLEFTOVERNumbersIntoTheFile) {
    // The numbers a fit does not use must not be written, and the reason is
    // not tidiness.
    //
    // Switching a dimension from "±0.1" to "H7" leaves 0.1 and -0.1 sitting in
    // the value -- the kind changed, the fields did not. Writing them beside
    // the fit puts a SECOND, STALE answer about that size in the file, and the
    // day something reads it, the drawing states a tolerance nobody set.
    //
    // The first version of this suite could not see it: every fit it built had
    // zeroes in those fields, so writing them changed nothing.
    Rig rig;
    DimensionTolerance leftovers;
    leftovers.kind = ToleranceKind::Fit;
    leftovers.fitCode = "H7";
    leftovers.upperMm = 0.1;  // what a previous ±0.1 left behind
    leftovers.lowerMm = -0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, leftovers));

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(rig.document, out));
    const std::string saved = out.str();
    EXPECT_NE(saved.find("H7"), std::string::npos);
    EXPECT_EQ(saved.find("0.1"), std::string::npos)
        << "a fit carried the numbers of the tolerance it replaced into the file";

    // ...and what it reads is the FIT, not the leftovers.
    std::istringstream in(saved);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->dimensionToleranceText(*loaded.document->dimensions().front()),
              rig.document.dimensionToleranceText(rig.dim()));
}

TEST(ToleranceTest, M37_SER_002_AToleranceKindThisBuildDoesNotKnowIsREFUSED) {
    // Defaulting it to None would turn a toleranced size into an untoleranced
    // one, silently, on a feature somebody toleranced on purpose.
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Symmetric;
    tolerance.upperMm = 0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(rig.document, out));
    std::string text = out.str();
    const std::string was = "\"kind\": \"Symmetric\"";
    const std::size_t at = text.find(was);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, was.size(), "\"kind\": \"Statistical\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::InvalidEnumValue);
}

TEST(ToleranceTest, M37_SER_003_AToleranceUPSIDEDOWNIsREFUSEDByTheLoader) {
    // An upper below its lower describes a size nothing can be made to.
    // setDimensionTolerance is not where this arrives -- a hand-edited file is.
    Rig rig;
    DimensionTolerance tolerance;
    tolerance.kind = ToleranceKind::Symmetric;
    tolerance.upperMm = 0.1;
    tolerance.lowerMm = -0.1;
    ASSERT_TRUE(rig.document.setDimensionTolerance(rig.dimension, tolerance));
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(rig.document, out));
    std::string text = out.str();
    const std::size_t at = text.find("\"upperMm\"");
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t comma = text.find(',', at);
    ASSERT_NE(comma, std::string::npos);
    text.replace(at, comma - at, "\"upperMm\": -0.5");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "a tolerance whose upper is below its lower loaded cleanly";
}

TEST(ToleranceTest, M37_SER_004_SavingIsREFUSEDWhenAFitCannotBeComputed) {
    // ADR-M3-008 again: the save checks what the load would. The state arrives
    // through the raw restore path, which is what a bad reader leaves behind.
    Rig rig;
    DrawingDimension* raw =
        const_cast<DrawingDimension*>(rig.document.findDimension(rig.dimension));
    ASSERT_NE(raw, nullptr);
    DimensionTolerance impossible;
    impossible.kind = ToleranceKind::Fit;
    impossible.fitCode = "J7";
    raw->setTolerance(impossible);

    std::ostringstream out;
    const SaveResult saved = saveDrawingDocument(rig.document, out);
    EXPECT_FALSE(saved) << "a fit this build cannot compute saved cleanly";
}
