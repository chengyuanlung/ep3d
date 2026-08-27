// M53.2 -- the flat pattern as a VIEW, end to end: fold a part, save it, and
// put the blank on a drawing.
//
// FlatPatternTests pins the arithmetic. What only this can say is that the
// drawing and the part are talking about the same object -- that the blank on
// the paper is the blank for THAT part, at THAT thickness, and that changing
// the part changes the drawing.
//
// AND WHAT IT CANNOT DO IS SAID OUT LOUD. A solid with no chain -- an import,
// a plain pad -- has no record of which faces were flanges or which way the
// metal went. The view refuses rather than handing back a rectangle somebody
// would cut.

#include "Core/Document/PartDocument.h"
#include "Core/Drawing/DrawingDocument.h"
#include "Core/Drawing/FlatPattern.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/SheetContour.h"
#include "Core/Feature/SheetContourFeature.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

struct ScratchPart {
    std::string path;
    explicit ScratchPart(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-flat-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchPart() { std::remove(path.c_str()); }
};

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

// Writes a folded sheet metal part to `path`, and hands back the blank it
// should flatten to.
FlatPatternResultGeometry WriteFoldedPart(const std::string& path, double thicknessMm = 2.0,
                                          double widthMm = 100.0) {
    PartDocument part{"Bracket"};
    SheetMetalSettings settings;
    settings.isSheetMetal = true;
    settings.thicknessMm = thicknessMm;
    settings.material = SheetMaterial::MildSteelAluminium;
    settings.defaultBendRadiusMm = thicknessMm;
    EXPECT_TRUE(part.setSheetMetal(settings));
    Parameter& width = part.addParameter("W", widthMm, UnitType::Millimeter);
    Body& body = part.addBody("Sheet");
    SheetContour contour = Channel();
    for (ContourStep& step : contour.steps) step.bend.innerRadiusMm = thicknessMm;
    part.addSheetContourFeature(body, "Contour", contour, width.id());
    EXPECT_TRUE(savePartDocumentToFile(part, path));
    return FlatPatternOf(contour, settings.material, thicknessMm, widthMm);
}

TEST(OcctFlatPatternViewTest, M53_KRN_001_TheBlankOnThePaperIsTheBlankForThatPart) {
    OcctGeometryKernel kernel;
    ScratchPart file{"bracket.ep3d"};
    const FlatPatternResultGeometry expected = WriteFoldedPart(file.path);
    ASSERT_TRUE(expected.ok) << expected.why;

    DrawingDocument drawing{"Bracket"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& flat =
        drawing.addFlatPatternView("Blank", file.path, "Sheet", Vec2{40.0, 120.0});
    ASSERT_TRUE(drawing.recompute().success) << flat.diagnostic();
    ASSERT_EQ(flat.currentState(), ComputeState::Valid) << flat.diagnostic();

    // The outline and both edges of each bend band.
    EXPECT_EQ(flat.projected().curves.size(), 4u + expected.bendLines.size() * 2u);
    EXPECT_NEAR(flat.projected().extent.widthMm(), expected.lengthMm, 1e-9);
    EXPECT_NEAR(flat.projected().extent.heightMm(), expected.widthMm, 1e-9);
    // ...AND IT IS THE BLANK, NOT THE FOLDED PART. A channel projected from
    // the front is 60 wide; its blank is over 120 long, which is the whole
    // point of cutting it flat.
    EXPECT_GT(flat.projected().extent.widthMm(), 120.0);
}

TEST(OcctFlatPatternViewTest, M53_KRN_002_ThickeningThePartLENGTHENSTheBlank) {
    // The drawing is not holding a copy of the blank. Thicker metal stretches
    // further round a bend, so the same three flanges need more material --
    // and a flat pattern that did not follow would be a blank for a part that
    // no longer exists, at exactly the moment somebody sends it to the laser.
    OcctGeometryKernel kernel;
    ScratchPart thin{"thin.ep3d"};
    ScratchPart thick{"thick.ep3d"};
    const FlatPatternResultGeometry thinBlank = WriteFoldedPart(thin.path, 2.0);
    const FlatPatternResultGeometry thickBlank = WriteFoldedPart(thick.path, 3.0);
    ASSERT_TRUE(thinBlank.ok);
    ASSERT_TRUE(thickBlank.ok);

    DrawingDocument drawing{"Bracket"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& a = drawing.addFlatPatternView("Thin", thin.path, "Sheet", Vec2{40.0, 40.0});
    DrawingView& b = drawing.addFlatPatternView("Thick", thick.path, "Sheet", Vec2{40.0, 160.0});
    ASSERT_TRUE(drawing.recompute().success) << a.diagnostic() << b.diagnostic();

    EXPECT_GT(b.projected().extent.widthMm(), a.projected().extent.widthMm())
        << "the thicker part's blank is not longer, so the drawing is not reading the part";
    EXPECT_NEAR(a.projected().extent.widthMm(), thinBlank.lengthMm, 1e-9);
    EXPECT_NEAR(b.projected().extent.widthMm(), thickBlank.lengthMm, 1e-9);
}

TEST(OcctFlatPatternViewTest, M53_KRN_003_APartWithNoChainGetsNoBlank) {
    // A pad is a solid, and nothing about it says which faces were flanges.
    // Handing back a rectangle would be a blank somebody would cut.
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    {
        PartDocument part{"Plate"};
        part.setGeometryKernel(&kernel);
        Sketch& sketch = part.addSketch("Base");
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 0}, Vec2{80, 0}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{80, 0}, Vec2{80, 40}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{80, 40}, Vec2{0, 40}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 40}, Vec2{0, 0}});
        Parameter& tall = part.addParameter("H", 2.0, UnitType::Millimeter);
        Body& body = part.addBody("Plate");
        part.addPadFeature(body, "Pad", sketch.id(), tall.id());
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& flat =
        drawing.addFlatPatternView("Blank", file.path, "Plate", Vec2{40.0, 120.0});
    drawing.recompute();
    EXPECT_EQ(flat.currentState(), ComputeState::Failed)
        << "a part with no folded section was handed a blank anyway";
    EXPECT_NE(flat.diagnostic().find("not a sheet metal part"), std::string::npos)
        << flat.diagnostic();
    EXPECT_TRUE(flat.projected().curves.empty());
}

TEST(OcctFlatPatternViewTest, M53_KRN_003B_SheetMetalWithNothingFoldedIsStillRefused) {
    // The test above uses a part that never said it was sheet metal, so it
    // stops at the first gate. This is the one past it: a part that IS sheet
    // metal, with a plain pad in it and no folded section at all. There is
    // still no chain, and still nothing to unfold.
    OcctGeometryKernel kernel;
    ScratchPart file{"unfolded.ep3d"};
    {
        PartDocument part{"Plate"};
        part.setGeometryKernel(&kernel);
        SheetMetalSettings settings;
        settings.isSheetMetal = true;
        settings.thicknessMm = 2.0;
        settings.material = SheetMaterial::MildSteelAluminium;
        settings.defaultBendRadiusMm = 2.0;
        ASSERT_TRUE(part.setSheetMetal(settings));
        Sketch& sketch = part.addSketch("Base");
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 0}, Vec2{80, 0}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{80, 0}, Vec2{80, 40}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{80, 40}, Vec2{0, 40}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 40}, Vec2{0, 0}});
        Parameter& tall = part.addParameter("H", 2.0, UnitType::Millimeter);
        Body& body = part.addBody("Plate");
        part.addPadFeature(body, "Pad", sketch.id(), tall.id());
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& flat =
        drawing.addFlatPatternView("Blank", file.path, "Plate", Vec2{40.0, 120.0});
    drawing.recompute();
    EXPECT_EQ(flat.currentState(), ComputeState::Failed)
        << "a sheet metal part with nothing folded was handed a blank";
    EXPECT_NE(flat.diagnostic().find("no record of how to unfold"), std::string::npos)
        << flat.diagnostic();
}

TEST(OcctFlatPatternViewTest, M53_KRN_003C_TwoFoldedSectionsAreTwoBlanksAndAreRefused) {
    // A drawing that showed one of them without saying which is a drawing of
    // half a part -- and the half it picked would be whichever the loop met
    // first, which is not a decision anybody made.
    OcctGeometryKernel kernel;
    ScratchPart file{"two.ep3d"};
    {
        PartDocument part{"Twin"};
        part.setGeometryKernel(&kernel);
        SheetMetalSettings settings;
        settings.isSheetMetal = true;
        settings.thicknessMm = 2.0;
        settings.material = SheetMaterial::MildSteelAluminium;
        settings.defaultBendRadiusMm = 2.0;
        ASSERT_TRUE(part.setSheetMetal(settings));
        Parameter& width = part.addParameter("W", 100.0, UnitType::Millimeter);
        Body& body = part.addBody("Sheet");
        part.addSheetContourFeature(body, "First", Channel(), width.id());
        part.addSheetContourFeature(body, "Second", Channel(), width.id());
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    DrawingDocument drawing{"Twin"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& flat =
        drawing.addFlatPatternView("Blank", file.path, "Sheet", Vec2{40.0, 120.0});
    drawing.recompute();
    EXPECT_EQ(flat.currentState(), ComputeState::Failed)
        << "one of two folded sections was picked silently";
    EXPECT_NE(flat.diagnostic().find("more than one"), std::string::npos) << flat.diagnostic();
}

TEST(OcctFlatPatternViewTest, M53_KRN_003D_TheBODYNamedIsTheBodyUnfolded) {
    // Two bodies, each folded to a different length. Ignoring the name gives
    // whichever came first -- a blank for the other part, at the right
    // thickness, with the right number of folds.
    OcctGeometryKernel kernel;
    ScratchPart file{"pair.ep3d"};
    SheetContour longer = Channel();
    longer.lastFlangeMm = 200.0;
    {
        PartDocument part{"Pair"};
        part.setGeometryKernel(&kernel);
        SheetMetalSettings settings;
        settings.isSheetMetal = true;
        settings.thicknessMm = 2.0;
        settings.material = SheetMaterial::MildSteelAluminium;
        settings.defaultBendRadiusMm = 2.0;
        ASSERT_TRUE(part.setSheetMetal(settings));
        Parameter& width = part.addParameter("W", 100.0, UnitType::Millimeter);
        Body& small = part.addBody("Small");
        part.addSheetContourFeature(small, "SmallFold", Channel(), width.id());
        Body& big = part.addBody("Big");
        part.addSheetContourFeature(big, "BigFold", longer, width.id());
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    DrawingDocument drawing{"Pair"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& flat =
        drawing.addFlatPatternView("Blank", file.path, "Big", Vec2{40.0, 120.0});
    ASSERT_TRUE(drawing.recompute().success) << flat.diagnostic();
    const FlatPatternResultGeometry expected =
        FlatPatternOf(longer, SheetMaterial::MildSteelAluminium, 2.0, 100.0);
    ASSERT_TRUE(expected.ok) << expected.why;
    EXPECT_NEAR(flat.projected().extent.widthMm(), expected.lengthMm, 1e-9)
        << "the view unfolded a body other than the one it names";
}

TEST(OcctFlatPatternViewTest, M53_KRN_003E_AWidthOfNothingFailsTheVIEWToo) {
    // The width lives in the part, so a blank can be refused long after the
    // view was made. It fails rather than drawing an outline of nothing.
    OcctGeometryKernel kernel;
    ScratchPart file{"narrow.ep3d"};
    {
        PartDocument part{"Bracket"};
        part.setGeometryKernel(&kernel);
        SheetMetalSettings settings;
        settings.isSheetMetal = true;
        settings.thicknessMm = 2.0;
        settings.material = SheetMaterial::MildSteelAluminium;
        settings.defaultBendRadiusMm = 2.0;
        ASSERT_TRUE(part.setSheetMetal(settings));
        Parameter& width = part.addParameter("W", 100.0, UnitType::Millimeter);
        Body& body = part.addBody("Sheet");
        part.addSheetContourFeature(body, "Contour", Channel(), width.id());
        ASSERT_TRUE(part.setParameterValue(width.id(), 0.0));
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    DrawingDocument drawing{"Bracket"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& flat =
        drawing.addFlatPatternView("Blank", file.path, "Sheet", Vec2{40.0, 120.0});
    drawing.recompute();
    EXPECT_EQ(flat.currentState(), ComputeState::Failed)
        << "a blank with no width was drawn";
    EXPECT_TRUE(flat.projected().curves.empty());
}

TEST(OcctFlatPatternViewTest, M53_KRN_003F_AFlatPatternOfNoPartIsRefusedAtTheDoor) {
    OcctGeometryKernel kernel;
    DrawingDocument drawing{"Nothing"};
    drawing.setGeometryKernel(&kernel);
    EXPECT_THROW(drawing.addFlatPatternView("Blank", "", "Sheet", Vec2{40.0, 120.0}),
                 std::invalid_argument);
}

TEST(OcctFlatPatternViewTest, M53_KRN_004_TheCaptionSaysItIsTheBlank) {
    // A blank and a folded view of the same bracket are both
    // rectangles-with-lines at a glance, and the one that goes to the laser is
    // not the one that goes to the fitter.
    OcctGeometryKernel kernel;
    ScratchPart file{"bracket.ep3d"};
    WriteFoldedPart(file.path);

    DrawingDocument drawing{"Bracket"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& flat =
        drawing.addFlatPatternView("Blank", file.path, "Sheet", Vec2{40.0, 120.0});
    ASSERT_TRUE(drawing.recompute().success) << flat.diagnostic();
    EXPECT_NE(drawing.viewLabelText(flat.id()).find("FLAT PATTERN"), std::string::npos)
        << drawing.viewLabelText(flat.id());
}

TEST(OcctFlatPatternViewTest, M53_KRN_005_TheFlagSurvivesTheFile) {
    // Restored without it, a blank comes back as a projection of the folded
    // part -- which for a bracket is a rectangle with lines on it either way.
    OcctGeometryKernel kernel;
    ScratchPart file{"bracket.ep3d"};
    WriteFoldedPart(file.path);

    DrawingDocument drawing{"Bracket"};
    drawing.setGeometryKernel(&kernel);
    const ObjectId id =
        drawing.addFlatPatternView("Blank", file.path, "Sheet", Vec2{40.0, 120.0}).id();
    ASSERT_TRUE(drawing.recompute().success);

    std::ostringstream out;
    ASSERT_EQ(saveDrawingDocument(drawing, out).error, SerializationError::None);
    std::istringstream in(out.str());
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingView* back = loaded.document->findView(id);
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(back->showsFlatPattern()) << "the blank came back as a folded view";
    // ...and hidden lines are still off, because there is nothing behind a
    // flat sheet and a laser would go looking for the dashes.
    EXPECT_FALSE(back->showsHiddenLines());
}

TEST(OcctFlatPatternViewTest, M53_KRN_005B_WhichWayEachFoldTurnsSurvivesThePartFILE) {
    // A Z and a channel are the same three lengths and the same two bends.
    // Written as always-left, or defaulted to left on the way in, a Z reopens
    // as a channel -- same blank, same fold positions, different part, and
    // nothing on the screen looks wrong.
    OcctGeometryKernel kernel;
    ScratchPart file{"zed.ep3d"};
    SheetContour zed;
    zed.steps.push_back(Step(30.0, 90.0, 2.0, true));
    zed.steps.push_back(Step(60.0, 90.0, 2.0, false));
    zed.lastFlangeMm = 30.0;
    {
        PartDocument part{"Zed"};
        part.setGeometryKernel(&kernel);
        SheetMetalSettings settings;
        settings.isSheetMetal = true;
        settings.thicknessMm = 2.0;
        settings.material = SheetMaterial::MildSteelAluminium;
        settings.defaultBendRadiusMm = 2.0;
        ASSERT_TRUE(part.setSheetMetal(settings));
        Parameter& width = part.addParameter("W", 100.0, UnitType::Millimeter);
        Body& body = part.addBody("Sheet");
        part.addSheetContourFeature(body, "Contour", zed, width.id());
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    const LoadResult loaded = loadPartDocumentFromFile(file.path);
    ASSERT_TRUE(loaded) << loaded.message;
    const SheetContourFeature* back = nullptr;
    for (const std::unique_ptr<Body>& body : loaded.document->bodies())
        for (const std::unique_ptr<Feature>& feature : body->features())
            if (const auto* folded = dynamic_cast<const SheetContourFeature*>(feature.get()))
                back = folded;
    ASSERT_NE(back, nullptr) << "the folded section did not survive the file at all";
    ASSERT_EQ(back->contour().steps.size(), 2u);
    EXPECT_TRUE(back->contour().steps[0].turnsLeft);
    EXPECT_FALSE(back->contour().steps[1].turnsLeft)
        << "a Z came back as a channel";
    EXPECT_NEAR(back->contour().lastFlangeMm, 30.0, 1e-9);
}

TEST(OcctFlatPatternViewTest, M53_KRN_005C_DeletingAFoldedSectionAndUndoingBringsTheChainBack) {
    // Undo does not go through the file: it goes through FeatureSnapshot, and
    // that is a SECOND place the chain has to be carried. A snapshot that took
    // everything except the chain restores a folded section with nothing in
    // it -- which then refuses to build, long after the delete it came from.
    OcctGeometryKernel kernel;
    PartDocument part{"Bracket"};
    part.setGeometryKernel(&kernel);
    SheetMetalSettings settings;
    settings.isSheetMetal = true;
    settings.thicknessMm = 2.0;
    settings.material = SheetMaterial::MildSteelAluminium;
    settings.defaultBendRadiusMm = 2.0;
    ASSERT_TRUE(part.setSheetMetal(settings));
    Parameter& width = part.addParameter("W", 100.0, UnitType::Millimeter);
    Body& body = part.addBody("Sheet");
    const ObjectId id = part.addSheetContourFeature(body, "Contour", Channel(), width.id()).id();
    ASSERT_TRUE(part.recompute().success);

    ASSERT_TRUE(part.removeObject(id));
    ASSERT_TRUE(part.undo());

    const SheetContourFeature* back = nullptr;
    for (const std::unique_ptr<Body>& one : part.bodies())
        for (const std::unique_ptr<Feature>& feature : one->features())
            if (const auto* folded = dynamic_cast<const SheetContourFeature*>(feature.get()))
                back = folded;
    ASSERT_NE(back, nullptr) << "undo did not bring the folded section back at all";
    EXPECT_EQ(back->contour().steps.size(), 2u) << "it came back with no chain in it";
    EXPECT_NEAR(back->contour().lastFlangeMm, 30.0, 1e-9);
    // ...and it still builds, which is what an empty chain would not.
    EXPECT_TRUE(part.recompute().success);
}

TEST(OcctFlatPatternViewTest, M53_KRN_006_ABlankCanBeBrokenLikeAnyOtherView) {
    // The whole reason a flat pattern is a KIND of view rather than a type of
    // its own: everything else on the drawing works on it. A three metre strip
    // needs a break as much as a folded one does.
    OcctGeometryKernel kernel;
    ScratchPart file{"bracket.ep3d"};
    const FlatPatternResultGeometry blank = WriteFoldedPart(file.path);
    ASSERT_TRUE(blank.ok);

    DrawingDocument drawing{"Bracket"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& flat =
        drawing.addFlatPatternView("Blank", file.path, "Sheet", Vec2{40.0, 120.0});
    ASSERT_TRUE(drawing.recompute().success) << flat.diagnostic();

    // Break out the middle of the blank, between the two bend bands.
    const double from = blank.bendLines.front().toMm + 5.0;
    const double to = blank.bendLines.back().fromMm - 5.0;
    ASSERT_GT(to, from);
    EXPECT_TRUE(drawing.setBreakSpan(flat.id(), from, to, true, 3.0));
    EXPECT_TRUE(flat.isBroken());
    // ...and a dimension across it would still read the whole blank, which is
    // M50's guarantee applying to a view M50 had never heard of.
    const Vec2 leftEnd = drawing.viewPointToSheetMm(flat.id(), Vec2{0.0, 0.0});
    const Vec2 rightEnd = drawing.viewPointToSheetMm(flat.id(), Vec2{blank.lengthMm, 0.0});
    EXPECT_LT(rightEnd.x - leftEnd.x, blank.lengthMm)
        << "the break took nothing off the paper";
    const Vec2 backLeft = drawing.sheetPointToViewMm(flat.id(), leftEnd);
    const Vec2 backRight = drawing.sheetPointToViewMm(flat.id(), rightEnd);
    EXPECT_NEAR(backRight.x - backLeft.x, blank.lengthMm, 1e-6)
        << "the blank measured short through the break";
}

} // namespace
