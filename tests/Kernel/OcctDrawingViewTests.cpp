// M32.2 -- a drawing view, end to end: open a part file, project it, and get
// curves the right size on the paper.
//
// The kernel suite, because a view has nothing to draw until a solid exists.
// What the Core suite pins is that a drawing is a DOCUMENT; what this pins is
// that a view is a VIEW OF SOMETHING.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <algorithm>
#include <variant>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

struct ScratchPart {
    std::string path;
    explicit ScratchPart(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-dwg-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchPart() { std::remove(path.c_str()); }
};

// A `width` x `depth` x `height` block, written to `path`.
void WriteBlockPart(const std::string& path, double width, double depth, double height,
                    const std::string& bodyName = "Block") {
    PartDocument part{"Source"};
    Sketch& sketch = part.addSketch("Base");
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 0}, Vec2{width, 0}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{width, 0}, Vec2{width, depth}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{width, depth}, Vec2{0, depth}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, depth}, Vec2{0, 0}});
    Parameter& tall = part.addParameter("H", height, UnitType::Millimeter);
    Body& body = part.addBody(bodyName);
    part.addPadFeature(body, "Pad", sketch.id(), tall.id());
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

} // namespace

TEST(OcctDrawingViewTest, M32_VIEW_010_AViewProjectsTheModelItNames) {
    OcctGeometryKernel kernel;
    ScratchPart file{"block.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();

    EXPECT_EQ(front.currentState(), ComputeState::Valid);
    EXPECT_FALSE(front.projected().curves.empty()) << "the view projected nothing";
    // IN MODEL MILLIMETRES: 100 across, 10 up. The scale is applied later and
    // elsewhere, which is what lets a dimension read the true size.
    EXPECT_NEAR(front.projected().extent.widthMm(), 100.0, 1e-6);
    EXPECT_NEAR(front.projected().extent.heightMm(), 10.0, 1e-6);
}

TEST(OcctDrawingViewTest, M32_VIEW_011_TurningAViewChangesWhatItDraws) {
    // Not "the direction field changed" -- the CURVES changed, and to the
    // shape the other direction actually shows.
    OcctGeometryKernel kernel;
    ScratchPart file{"turn.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& view = drawing.addView("V", file.path, "Block", ViewDirection::Front,
                                        Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << view.diagnostic();
    EXPECT_NEAR(view.projected().extent.heightMm(), 10.0, 1e-6);

    ASSERT_TRUE(drawing.setViewDirection(view.id(), ViewDirection::Top));
    ASSERT_TRUE(drawing.recompute().success) << view.diagnostic();
    EXPECT_NEAR(view.projected().extent.heightMm(), 40.0, 1e-6)
        << "the view was turned and drew the same thing";
}

TEST(OcctDrawingViewTest, M32_VIEW_012_ThePaperFootprintIsTheModelExtentTIMESTheScale) {
    // The one place the scale is applied. A caller that multiplied for itself
    // would be the second place, and the first to get it wrong.
    OcctGeometryKernel kernel;
    ScratchPart file{"scale.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();

    EXPECT_NEAR(front.paperWidthMm(drawing.sheet().scale()), 100.0, 1e-6);
    ASSERT_TRUE(drawing.setViewScale(front.id(), DrawingScale{1, 2}));
    EXPECT_NEAR(front.paperWidthMm(drawing.sheet().scale()), 50.0, 1e-6)
        << "a half-scale view still claims full-size paper";
    // ...AND THE MODEL EXTENT DID NOT MOVE. A dimension reads this, and a
    // dimension that shrank with the paper would print a number that is not
    // the part.
    EXPECT_NEAR(front.projected().extent.widthMm(), 100.0, 1e-6)
        << "changing the scale changed the measured size of the part";
}

TEST(OcctDrawingViewTest, M32_VIEW_013_ChangingTheScaleDoesNOTReprojectTheView) {
    // Hidden-line removal is the expensive operation in this block, and the
    // curves are in model millimetres -- so a scale change cannot alter them.
    // Pinned because the cheap mistake is to dirty the node "just in case",
    // and nothing downstream would ever notice the wasted solve.
    OcctGeometryKernel kernel;
    ScratchPart file{"nodirty.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();

    ASSERT_TRUE(drawing.setViewScale(front.id(), DrawingScale{1, 2}));
    ASSERT_TRUE(drawing.setSheetScale(DrawingScale{1, 5}));
    ASSERT_TRUE(drawing.setViewPosition(front.id(), Vec2{200.0, 100.0}));
    // Still valid, and nothing was rebuilt: a dirty node would report itself
    // as needing a recompute.
    EXPECT_EQ(front.currentState(), ComputeState::Valid)
        << "a scale or a move sent the view back for a fresh hidden-line solve";
}

TEST(OcctDrawingViewTest, M32_VIEW_014_TurningOffHiddenLinesDOESReprojectIt) {
    // The opposite case, and the reason the one above is not just "views never
    // reproject": this switch changes which edges are computed at all.
    OcctGeometryKernel kernel;
    ScratchPart file{"hidden.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();

    std::size_t hiddenBefore = 0;
    for (const ProjectedCurve& curve : front.projected().curves)
        if (curve.visibility == ProjectedVisibility::Hidden) ++hiddenBefore;
    ASSERT_GT(hiddenBefore, 0u) << "this block projected with no hidden edges to turn off";

    ASSERT_TRUE(drawing.setViewShowsHiddenLines(front.id(), false));
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();
    std::size_t hiddenAfter = 0;
    for (const ProjectedCurve& curve : front.projected().curves)
        if (curve.visibility == ProjectedVisibility::Hidden) ++hiddenAfter;
    EXPECT_EQ(hiddenAfter, 0u) << "hidden lines were turned off and are still there";
}

TEST(OcctDrawingViewTest, M32_VIEW_015_AModelThatDoesNotBuildLEAVESNOPICTURE) {
    // A view that failed while still holding what it drew last time is a
    // drawing that shows a part which no longer builds -- and the tree says
    // "failed" over a picture that looks fine, which is the worst of both.
    OcctGeometryKernel kernel;
    ScratchPart file{"gone.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success);
    ASSERT_FALSE(front.projected().curves.empty());

    // The file goes away under it, which is the ordinary way this happens.
    std::remove(file.path.c_str());
    ASSERT_TRUE(drawing.markDirty(front.id()));
    EXPECT_FALSE(drawing.recompute().success);
    EXPECT_EQ(front.currentState(), ComputeState::Failed);
    EXPECT_TRUE(front.projected().curves.empty())
        << "a failed view is still drawing the part it can no longer read";
    // ...and it says WHICH file, because the reader's next move is to find it.
    EXPECT_NE(front.diagnostic().find(file.path), std::string::npos) << front.diagnostic();
}

TEST(OcctDrawingViewTest, M32_VIEW_016_AViewAndAnInstanceAgreeAboutWhatIsInAFile) {
    // Both go through the same resolver (M32.2). This is the test that keeps
    // them that way: a multi-body part with no body named is refused with the
    // NAMES, not resolved to the first -- because the first is an order, and
    // order is not identity.
    OcctGeometryKernel kernel;
    ScratchPart file{"twobodies.ep3d"};
    {
        PartDocument part{"Source"};
        Sketch& sketch = part.addSketch("Base");
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 0}, Vec2{20, 0}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{20, 0}, Vec2{20, 20}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{20, 20}, Vec2{0, 20}});
        part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 20}, Vec2{0, 0}});
        Parameter& tall = part.addParameter("H", 5.0, UnitType::Millimeter);
        Body& first = part.addBody("First");
        part.addPadFeature(first, "PadA", sketch.id(), tall.id());
        Body& second = part.addBody("Second");
        part.addPadFeature(second, "PadB", sketch.id(), tall.id());
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& view = drawing.addView("V", file.path, "", ViewDirection::Front,
                                        Vec2{100.0, 150.0});
    EXPECT_FALSE(drawing.recompute().success);
    EXPECT_NE(view.diagnostic().find("First"), std::string::npos) << view.diagnostic();
    EXPECT_NE(view.diagnostic().find("Second"), std::string::npos)
        << "the refusal did not name the bodies to choose between: " << view.diagnostic();

    // ...and naming one works.
    ASSERT_TRUE(drawing.removeObject(view.id()));
    DrawingView& named = drawing.addView("Named", file.path, "Second", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    EXPECT_TRUE(drawing.recompute().success) << named.diagnostic();
}

TEST(OcctDrawingViewTest, M32_STALE_001_ADrawingSaysWHICHViewsAreBehindTheirModels) {
    // A drawing does not watch the disk. It answers when asked, and the shell
    // turns that into "3 views are out of date -- update?" rather than
    // silently rebuilding everything or, worse, showing a picture of a part
    // that no longer looks like that.
    OcctGeometryKernel kernel;
    ScratchPart file{"stale.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{150.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();
    EXPECT_TRUE(drawing.staleViews().empty())
        << "a view was called stale the moment it was drawn";

    // THE MODEL CHANGES UNDER IT -- which is the whole case this exists for.
    WriteBlockPart(file.path, 200.0, 40.0, 10.0);
    const std::vector<ObjectId> behind = drawing.staleViews();
    ASSERT_EQ(behind.size(), 1u) << "the drawing did not notice its model changed";
    EXPECT_EQ(behind.front(), front.id());

    // ...and updating it clears the flag AND redraws it at the new size.
    ASSERT_TRUE(drawing.markDirty(front.id()));
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();
    EXPECT_TRUE(drawing.staleViews().empty()) << "the view was updated and still reads stale";
    EXPECT_NEAR(front.projected().extent.widthMm(), 200.0, 1e-6)
        << "the view says it is up to date and is still drawing the old part";
}

TEST(OcctDrawingViewTest, M32_STALE_002_ABrokenViewIsNotSTALEItIsBROKEN) {
    // Offering to update a view that never built would send the user round a
    // loop that cannot end -- update, fail, still offered. The tree already
    // says "failed"; that is the message that has somewhere to go.
    OcctGeometryKernel kernel;
    ScratchPart file{"broken.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{150.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success);

    std::remove(file.path.c_str());
    ASSERT_TRUE(drawing.markDirty(front.id()));
    EXPECT_FALSE(drawing.recompute().success);
    EXPECT_EQ(front.currentState(), ComputeState::Failed);
    EXPECT_TRUE(drawing.staleViews().empty())
        << "a view that cannot build at all was offered as merely out of date";
}

TEST(OcctDrawingViewTest, M32_STALE_003_ProjectedChildrenGoStaleWithTheirParent) {
    // They read the same file, so they are behind it too -- and a drawing that
    // updated only the base view would show a front view of the new part
    // beside a top view of the old one, which is worse than showing neither.
    OcctGeometryKernel kernel;
    ScratchPart file{"family.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{150.0, 150.0});
    drawing.addProjectedView("Top", front.id(), ViewDirection::Top, 60.0);
    ASSERT_TRUE(drawing.recompute().success);
    ASSERT_TRUE(drawing.staleViews().empty());

    WriteBlockPart(file.path, 200.0, 40.0, 10.0);
    EXPECT_EQ(drawing.staleViews().size(), 2u)
        << "only some of the views that read this model noticed it changed";
}

TEST(OcctDrawingViewTest, M32_STALE_004_TwoSavesInsideOneClockTickAreStillNoticed) {
    // THE FLAKY TEST'S REAL CAUSE, pinned so it cannot come back.
    //
    // M32_STALE_001 passed alone and failed in a full run, because the first
    // version of the stamp hashed `last_write_time` -- and two saves inside one
    // filesystem timestamp tick are indistinguishable by mtime. That is not a
    // test problem: a user who edits and saves quickly would get a drawing that
    // says it is up to date and shows the old part.
    //
    // Writing twice with no delay is exactly that case, deliberately.
    OcctGeometryKernel kernel;
    ScratchPart file{"quick.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{150.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();
    ASSERT_TRUE(drawing.staleViews().empty());

    // No sleep. Whatever the clock does, the CONTENT changed.
    WriteBlockPart(file.path, 250.0, 40.0, 10.0);
    EXPECT_EQ(drawing.staleViews().size(), 1u)
        << "a model rewritten inside one clock tick was not noticed";
}

TEST(OcctDrawingViewTest, M32_STALE_005_RewritingTheSameContentIsNOTAChange) {
    // The other half, and the reason this is a content hash rather than a
    // counter: saving a file without editing it should not send every view on
    // the sheet back through hidden-line removal.
    OcctGeometryKernel kernel;
    ScratchPart file{"same.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{150.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();

    // WRITTEN BACK BYTE FOR BYTE. Calling WriteBlockPart again would NOT do
    // it: a fresh PartDocument takes fresh ObjectIds, so the same part saved
    // twice is genuinely two different files -- which this test discovered by
    // failing, and which is worth knowing about the format.
    std::string bytes;
    {
        std::ifstream in(file.path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    {
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    EXPECT_TRUE(drawing.staleViews().empty())
        << "re-saving an unchanged model marked every view out of date";
}

// =============================================================================
// M34 -- a dimension INSIDE a view
// =============================================================================
//
// These live in the kernel suite because an InView anchor has nothing to
// re-find until a real projection exists. Two mutations survived the Core
// suite for exactly that reason: nothing there could build the state.

TEST(OcctDrawingViewTest, M34_VIEW_020_ADimensionInAViewReadsTheMODELSize) {
    // THE MEASUREMENT IS THE PART'S, not the picture's.
    //
    // The anchors resolve to sheet millimetres -- that is where they are drawn
    // -- so a dimension inside a scaled view has to divide the scale back out.
    // At 1:2 a 100 mm block is 50 mm of paper, and a dimension that read 50
    // would be a drawing stating half the size of the thing being made.
    OcctGeometryKernel kernel;
    ScratchPart file{"dim-scale.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingScale half{1, 2};
    ASSERT_TRUE(drawing.setSheetScale(half));
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{150.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();

    // The block's two bottom corners, IN MODEL MILLIMETRES -- which is what an
    // InView anchor stores, and why a scale change does not reproject.
    const ProjectedExtent extent = front.projected().extent;
    const DrawingDimension& across = drawing.addDimension(
        DimensionKind::Linear,
        DimensionAnchor::inView(front.id(), Vec2{extent.min.x, extent.min.y},
                                ViewPointRole::Corner),
        DimensionAnchor::inView(front.id(), Vec2{extent.max.x, extent.min.y},
                                ViewPointRole::Corner),
        Vec2{150.0, 120.0});

    const DimensionMeasurement measured = drawing.measure(across);
    ASSERT_TRUE(measured.ok) << measured.why;
    EXPECT_NEAR(measured.valueMm, 100.0, 1e-6)
        << "the dimension read the size of the PICTURE, not of the part";
    EXPECT_EQ(drawing.dimensionText(across), "100.00");

    // ...AND CHANGING THE SCALE DOES NOT CHANGE THE SIZE OF THE PART.
    ASSERT_TRUE(drawing.setSheetScale(DrawingScale{1, 5}));
    ASSERT_TRUE(drawing.recompute().success);
    EXPECT_NEAR(drawing.measure(across).valueMm, 100.0, 1e-6)
        << "replotting at a different scale changed what the part measures";
}

TEST(OcctDrawingViewTest, M34_VIEW_021_AnInViewAnchorWillNOTReattachBeyondItsTolerance) {
    // THE APPROXIMATION, PINNED.
    //
    // An InView anchor re-finds the nearest projected snap point after a
    // reprojection. The failure that must never happen is the SILENT one:
    // adopting a different feature and printing a plausible wrong number. So
    // the search is bounded, and past the bound the dimension dangles LOUDLY.
    //
    // Without this, an unbounded search always finds SOMETHING -- and a
    // drawing that quietly re-attached its dimensions to whatever was nearest
    // is worse than one that says it does not know.
    OcctGeometryKernel kernel;
    ScratchPart file{"dim-tolerance.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{150.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();

    const ProjectedExtent extent = front.projected().extent;
    // A point a long way from anything the view draws, with a tight tolerance.
    const DrawingDimension& lost = drawing.addDimension(
        DimensionKind::Linear,
        DimensionAnchor::inView(front.id(), Vec2{extent.min.x, extent.min.y},
                                ViewPointRole::Corner, 1.0),
        DimensionAnchor::inView(front.id(), Vec2{extent.max.x + 500.0, extent.min.y},
                                ViewPointRole::Corner, 1.0),
        Vec2{150.0, 120.0});

    const DimensionMeasurement measured = drawing.measure(lost);
    EXPECT_FALSE(measured.ok)
        << "an anchor 500 mm from any edge re-attached to one anyway, and it "
           "measured " << measured.valueMm;
    EXPECT_EQ(drawing.dimensionText(lost), "<?>");
    ASSERT_EQ(drawing.danglingDimensions().size(), 1u);

    // ...and one INSIDE the tolerance still finds its point, so the bound is
    // a bound and not an off switch.
    const DrawingDimension& found = drawing.addDimension(
        DimensionKind::Linear,
        DimensionAnchor::inView(front.id(), Vec2{extent.min.x, extent.min.y},
                                ViewPointRole::Corner, 1.0),
        DimensionAnchor::inView(front.id(),
                                Vec2{extent.max.x + 0.4, extent.min.y + 0.4},
                                ViewPointRole::Corner, 1.0),
        Vec2{150.0, 110.0});
    const DimensionMeasurement near = drawing.measure(found);
    ASSERT_TRUE(near.ok) << near.why;
    EXPECT_NEAR(near.valueMm, 100.0, 1e-6);
}

// =============================================================================
// M38 -- section views
// =============================================================================
//
// A section is the ordinary projection of a solid with a half-space taken out.
// These live in the kernel suite because the cut needs a real solid; what the
// Core suite can pin on its own is the letter and the undo.

namespace {

// A block with a hole through it, which is the shape a section is FOR: the
// bore is invisible on an outside view and is the whole point of cutting.
void WriteBlockWithBore(const std::string& path) {
    PartDocument part{"Source"};
    Sketch& outline = part.addSketch("Base");
    part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 0}, Vec2{100, 0}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{100, 0}, Vec2{100, 40}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{100, 40}, Vec2{0, 40}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 40}, Vec2{0, 0}});
    Parameter& tall = part.addParameter("H", 30.0, UnitType::Millimeter);
    Body& body = part.addBody("Block");
    PadFeature& pad = part.addPadFeature(body, "Pad", outline.id(), tall.id());

    // The bore is what a section is FOR: invisible from outside, and the whole
    // reason somebody cuts the part open.
    Sketch& bore = part.addSketch("Bore");
    part.addSketchEntity(bore.id(), SketchCircle{Vec2{50, 20}, 10.0});
    Parameter& deep = part.addParameter("D", 30.0, UnitType::Millimeter);
    part.addPocketFeature(body, "Bore", pad.id(), bore.id(), deep.id());
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

} // namespace

TEST(OcctDrawingViewTest, M38_SECTION_001_ASectionShowsWhatTheOutsideViewCannot) {
    OcctGeometryKernel kernel;
    ScratchPart file{"section.ep3d"};
    WriteBlockWithBore(file.path);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success) << top.diagnostic();

    // Cut straight down the middle of the top view, arrows one way.
    DrawingView& section = drawing.addSectionView("A-A", top.id(), Vec2{50.0, -20.0},
                                                  Vec2{50.0, 60.0}, 1, 60.0);
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    EXPECT_EQ(section.currentState(), ComputeState::Valid);
    EXPECT_TRUE(section.isSection());

    // THE CUT FACE IS THERE TO HATCH. Without it a section is a drawing of the
    // inside of a part with no way to tell cut material from what is behind.
    EXPECT_FALSE(section.projected().cutLoops.empty())
        << "the section produced no cut face";

    // AND IT SHOWS THE BORE, which is the entire point of cutting.
    //
    // NOT the extent: a section looks ALONG the cut's normal, so what it shows
    // is the OTHER two axes -- 40 by 30 here -- whatever the cut removed. A
    // first draft measured the width and expected it to halve, which is a
    // check that can never pass and says nothing about whether the knife did
    // anything.
    //
    // What DOES change is the shape of the cut face. The plane at x = 50 runs
    // straight down the bore's axis, and the bore goes right through -- so the
    // face is in TWO pieces, one either side of it.
    EXPECT_EQ(section.projected().cutLoops.size(), 2u)
        << "the section did not cut through the bore";

    // ...and a cut CLEAR of the bore is one solid face.
    ASSERT_TRUE(drawing.setSectionCut(section.id(), Vec2{10.0, -20.0}, Vec2{10.0, 60.0}, 1));
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    EXPECT_EQ(section.projected().cutLoops.size(), 1u)
        << "a cut that misses the bore came back in pieces";
}

TEST(OcctDrawingViewTest, M38_SECTION_002_TurningTheARROWSKeepsTheOtherHalf) {
    // The one thing that is a coin toss if it is not written down, and getting
    // it backwards draws a perfectly plausible picture of the wrong half.
    OcctGeometryKernel kernel;
    ScratchPart file{"arrows.ep3d"};
    WriteBlockWithBore(file.path);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success);

    // The cut is at x = 30 and the bore is at x = 50. One half contains the
    // bore and the other does not, so WHICH HALF SURVIVED is visible in what
    // the section draws -- and that is the question, not the extent.
    DrawingView& section = drawing.addSectionView("A-A", top.id(), Vec2{30.0, -20.0},
                                                  Vec2{30.0, 60.0}, 1, 60.0);
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    const std::size_t oneWay = section.projected().curves.size();

    ASSERT_TRUE(drawing.setSectionCut(section.id(), Vec2{30.0, -20.0}, Vec2{30.0, 60.0}, -1));
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    const std::size_t otherWay = section.projected().curves.size();

    EXPECT_NE(oneWay, otherWay)
        << "reversing the arrows kept the same half: " << oneWay << " curves both times";
}

TEST(OcctDrawingViewTest, M38_SECTION_003_TurningTheParentTURNSTheCut) {
    // THE WHOLE REASON THE CUT LINE IS A SENTENCE ON THE PARENT. A stored 3D
    // plane would keep pointing the old way, and the section would be of a
    // place the line no longer crosses -- a good section of the wrong thing.
    OcctGeometryKernel kernel;
    ScratchPart file{"turned.ep3d"};
    WriteBlockWithBore(file.path);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& parent = drawing.addView("Parent", file.path, "Block", ViewDirection::Top,
                                          Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success);
    DrawingView& section = drawing.addSectionView("A-A", parent.id(), Vec2{50.0, -20.0},
                                                  Vec2{50.0, 60.0}, 1, 60.0);
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    // The parent's page changes under the same cut line, so the section is
    // taken through the part a different way -- and what it DRAWS changes,
    // which is the observable thing. (The extent alone can coincide; the
    // curves cannot.)
    const double before = section.projected().extent.heightMm();

    // The parent becomes a FRONT view. The same cut line on its page now runs
    // through the part a different way, so the section must change.
    ASSERT_TRUE(drawing.setViewDirection(parent.id(), ViewDirection::Front));
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    EXPECT_NE(section.projected().extent.heightMm(), before)
        << "the parent turned and the section did not follow";
}

TEST(OcctDrawingViewTest, M38_SECTION_004_ACutLineOfNOLENGTHIsREFUSED) {
    OcctGeometryKernel kernel;
    ScratchPart file{"nocut.ep3d"};
    WriteBlockWithBore(file.path);
    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success);

    EXPECT_THROW(drawing.addSectionView("A-A", top.id(), Vec2{50.0, 20.0}, Vec2{50.0, 20.0},
                                        1, 60.0),
                 std::invalid_argument);
    // ...and a section of a section, which is a real thing and not this one.
    DrawingView& section = drawing.addSectionView("A-A", top.id(), Vec2{50.0, -20.0},
                                                  Vec2{50.0, 60.0}, 1, 60.0);
    EXPECT_THROW(drawing.addSectionView("B-B", section.id(), Vec2{10.0, -20.0},
                                        Vec2{10.0, 60.0}, 1, 60.0),
                 std::invalid_argument);
}

TEST(OcctDrawingViewTest, M38_SECTION_005_TheLETTERIsDerivedAndTheSameOnBothSides) {
    // The line on the parent and the title under the section have to carry the
    // SAME letter -- the classic "two things that must agree" trap, so neither
    // is typed and both ask the document.
    OcctGeometryKernel kernel;
    ScratchPart file{"letters.ep3d"};
    WriteBlockWithBore(file.path);
    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success);

    DrawingView& first = drawing.addSectionView("Sec1", top.id(), Vec2{30.0, -20.0},
                                                Vec2{30.0, 60.0}, 1, 60.0);
    DrawingView& second = drawing.addSectionView("Sec2", top.id(), Vec2{70.0, -20.0},
                                                 Vec2{70.0, 60.0}, 1, 120.0);
    EXPECT_EQ(drawing.sectionLetterOf(first.id()), "A");
    EXPECT_EQ(drawing.sectionLetterOf(second.id()), "B");
    // A view that is not a section has no letter, which is a real answer.
    EXPECT_TRUE(drawing.sectionLetterOf(top.id()).empty());
}

TEST(OcctDrawingViewTest, M38_SECTION_006_UndoingASectionTakesItsCUTWithIt) {
    // A restored section with no cut line projects the WHOLE part and looks
    // entirely reasonable -- which is why the cut travels in the delta.
    OcctGeometryKernel kernel;
    ScratchPart file{"undo.ep3d"};
    WriteBlockWithBore(file.path);
    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success);
    const ObjectId sectionId = drawing
                                   .addSectionView("A-A", top.id(), Vec2{50.0, -20.0},
                                                   Vec2{50.0, 60.0}, 1, 60.0)
                                   .id();
    ASSERT_TRUE(drawing.recompute().success);

    // MAKING A SECTION IS ONE UNDO STEP: a view with no cut line is a state no
    // drawing was ever in.
    ASSERT_TRUE(drawing.undo());
    EXPECT_EQ(drawing.findView(sectionId), nullptr);
    ASSERT_TRUE(drawing.redo());
    const DrawingView* back = drawing.findView(sectionId);
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(back->isSection()) << "the section came back with no cut line";
    EXPECT_NEAR(back->sectionCut().fromMm.x, 50.0, 1e-9);
    ASSERT_TRUE(drawing.recompute().success);
    // The cut at x = 50 runs down the bore, so the face comes back in two
    // pieces -- which a view projecting the WHOLE part could not do.
    EXPECT_EQ(back->projected().cutLoops.size(), 2u)
        << "the restored section is projecting the whole part";
}

TEST(OcctDrawingViewTest, M38_SECTION_007_ASectionSurvivesASaveAndStillCuts) {
    // THE WRITE SIDE WENT MISSING ONCE and nothing noticed: sections could be
    // read from a file and never written to one, so a drawing saved and
    // reopened came back with the section view projecting the WHOLE part --
    // which looks like a perfectly ordinary view.
    //
    // It was caught by a mutation whose pattern matched nothing, not by a
    // test, which is what this closes.
    OcctGeometryKernel kernel;
    ScratchPart file{"sectionsave.ep3d"};
    WriteBlockWithBore(file.path);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success);
    drawing.addSectionView("A-A", top.id(), Vec2{50.0, -20.0}, Vec2{50.0, 60.0}, 1, 60.0);
    ASSERT_TRUE(drawing.recompute().success);

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(drawing, out));
    const std::string saved = out.str();
    EXPECT_NE(saved.find("fromXMm"), std::string::npos)
        << "the cut line was not written to the file";
    // THE PLANE IS NOT IN THE FILE, only the line -- see the writer.
    EXPECT_EQ(saved.find("normal"), std::string::npos);

    std::istringstream in(saved);
    DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    ASSERT_TRUE(loaded.document->recompute().success);

    const DrawingView* back = nullptr;
    for (const DrawingView* one : loaded.document->views())
        if (one->isSection()) back = one;
    ASSERT_NE(back, nullptr) << "the reopened drawing has no section at all";
    EXPECT_NEAR(back->sectionCut().fromMm.x, 50.0, 1e-9);
    // ...and it STILL CUTS: the plane at x = 50 runs down the bore, so the
    // face comes back in two pieces. A view projecting the whole part could
    // not.
    EXPECT_EQ(back->projected().cutLoops.size(), 2u)
        << "the reopened section is projecting the whole part";
    EXPECT_EQ(loaded.document->sectionLetterOf(back->id()), "A");
}

namespace {

// A STEPPED block: full height at one end, half height at the other.
//
// The shape exists so that "which half was kept" is a question the SILHOUETTE
// answers. A symmetrical part gives the same picture whichever half survives,
// which is exactly why getting this backwards is invisible.
void WriteSteppedBlock(const std::string& path) {
    PartDocument part{"Source"};
    Sketch& outline = part.addSketch("Base");
    part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 0}, Vec2{100, 0}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{100, 0}, Vec2{100, 40}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{100, 40}, Vec2{0, 40}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 40}, Vec2{0, 0}});
    Parameter& tall = part.addParameter("H", 30.0, UnitType::Millimeter);
    Body& body = part.addBody("Block");
    PadFeature& pad = part.addPadFeature(body, "Pad", outline.id(), tall.id());

    // Take the top half off everything past x = 22, leaving a block that is
    // 30 tall at one end and 15 at the other.
    Sketch& step = part.addSketch("Step");
    part.addSketchEntity(step.id(), SketchLine{Vec2{22, -10}, Vec2{130, -10}});
    part.addSketchEntity(step.id(), SketchLine{Vec2{130, -10}, Vec2{130, 50}});
    part.addSketchEntity(step.id(), SketchLine{Vec2{130, 50}, Vec2{22, 50}});
    part.addSketchEntity(step.id(), SketchLine{Vec2{22, 50}, Vec2{22, -10}});
    Parameter& deep = part.addParameter("D", 15.0, UnitType::Millimeter);
    part.addPocketFeature(body, "Step", pad.id(), step.id(), deep.id());
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

// The bounding box of what a view actually drew, in MODEL millimetres.
Box2D DrawnExtent(const DrawingView& view) {
    Box2D box;
    for (const ProjectedCurve& curve : view.projected().curves) {
        if (const auto* line = std::get_if<ProjectedLine>(&curve.shape)) {
            box.grow(line->a);
            box.grow(line->b);
        } else if (const auto* arc = std::get_if<ProjectedArc>(&curve.shape)) {
            box.grow(Vec2{arc->centre.x - arc->radius, arc->centre.y - arc->radius});
            box.grow(Vec2{arc->centre.x + arc->radius, arc->centre.y + arc->radius});
        } else if (const auto* poly = std::get_if<ProjectedPolyline>(&curve.shape)) {
            for (const Vec2 point : poly->points) box.grow(point);
        }
    }
    return box;
}

} // namespace

TEST(OcctDrawingViewTest, M38_SECTION_008_TheHalfBETWEENTheReaderAndThePlaneIsTheOneThatGoes) {
    // WHICH HALF SURVIVES IS A COIN TOSS IF IT IS NOT WRITTEN DOWN, and it is
    // written down in exactly one place: the section's normal points at the
    // material that is REMOVED, which is the side the reader is standing on.
    //
    // Get it backwards and the reader is looking THROUGH the part at the half
    // behind the plane -- a perfectly sharp, perfectly plausible drawing of
    // the wrong half. A symmetrical part cannot tell the difference, so this
    // one is stepped: the far half is 15 tall and the near half 30.
    OcctGeometryKernel kernel;
    ScratchPart file{"stepped.ep3d"};
    WriteSteppedBlock(file.path);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success) << top.diagnostic();

    // A cut across the part at x = 60, arrows pointing along +x -- the reader
    // looks the way the arrows point, so everything nearer than the plane
    // goes. What is left is the far, SHORT half.
    DrawingView& section = drawing.addSectionView("A-A", top.id(), Vec2{60.0, -20.0},
                                                  Vec2{60.0, 60.0}, 1, 60.0);
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    const Box2D shortHalf = DrawnExtent(section);
    ASSERT_FALSE(shortHalf.empty);

    // Which axis is the height depends on how the section's own page came out;
    // the test asks for the SHORTER of the two, which is 15 either way. The
    // other is the part's 40 depth, which both halves share.
    const double thin = std::min(shortHalf.width(), shortHalf.height());
    EXPECT_NEAR(thin, 15.0, 1e-6)
        << "the reader is seeing the half that should have been cut away";

    // TURN THE ARROWS AND THE OTHER HALF IS KEPT -- the tall one, 30.
    ASSERT_TRUE(drawing.setSectionCut(section.id(), Vec2{60.0, -20.0}, Vec2{60.0, 60.0}, -1));
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    const Box2D tallHalf = DrawnExtent(section);
    ASSERT_FALSE(tallHalf.empty);
    const double thick = std::min(tallHalf.width(), tallHalf.height());
    EXPECT_NEAR(thick, 30.0, 1e-6) << "turning the arrows did not keep the other half";
}

TEST(OcctDrawingViewTest, M38_SECTION_009_ACutThatRemovesTheWHOLEPartIsSAIDNotDrawnEmpty) {
    // The plane misses the part and the half-space swallows all of it. An
    // empty drawing reported as a good one is the worst answer available: the
    // sheet shows a view with nothing in it, which reads as a part that has
    // not finished computing rather than as a cut in the wrong place.
    OcctGeometryKernel kernel;
    ScratchPart file{"missed.ep3d"};
    WriteBlockWithBore(file.path);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success) << top.diagnostic();

    // The part runs x = 0 to 100. Cut at x = 400 with the arrows pointing at
    // +x: the reader stands on the far side of the part looking towards the
    // plane, so everything between them -- the whole part -- is what goes.
    DrawingView& section = drawing.addSectionView("A-A", top.id(), Vec2{400.0, -20.0},
                                                  Vec2{400.0, 60.0}, 1, 60.0);
    drawing.recompute();
    EXPECT_EQ(section.currentState(), ComputeState::Failed)
        << "a section that removed the whole part came back as a good, empty drawing";
    EXPECT_FALSE(section.diagnostic().empty());
    EXPECT_TRUE(section.projected().curves.empty());
}

TEST(OcctDrawingViewTest, M38_SECTION_010_ASectionOfASectionIsREFUSEDByTheViewToo) {
    // addSectionView refuses it, but that is not the only way in: a plain
    // projected view can be hung off a section and THEN given a cut line. The
    // check in the view is what catches that, and a rule enforced in one place
    // and relied on in two is this project's recurring defect.
    OcctGeometryKernel kernel;
    ScratchPart file{"nested.ep3d"};
    WriteBlockWithBore(file.path);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success);
    DrawingView& section = drawing.addSectionView("A-A", top.id(), Vec2{50.0, -20.0},
                                                  Vec2{50.0, 60.0}, 1, 60.0);
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();

    // The way round the front door.
    DrawingView& child = drawing.addProjectedView("Detail", section.id(),
                                                  ViewDirection::Front, 60.0);
    ASSERT_TRUE(drawing.setSectionCut(child.id(), Vec2{10.0, -20.0}, Vec2{10.0, 20.0}, 1));
    drawing.recompute();
    EXPECT_EQ(child.currentState(), ComputeState::Failed)
        << "a section of a section was projected from a camera that is itself derived";
    EXPECT_NE(child.diagnostic().find("section of a section"), std::string::npos)
        << child.diagnostic();
}

TEST(OcctDrawingViewTest, M38_SECTION_011_TheHatchRegionIsInSHEETMillimetresNotModelOnes) {
    // A hatch is ANNOTATION, like a dimension: its pitch is a paper
    // measurement. Hatch a 1:2 section in model millimetres and the region is
    // twice the size it will be drawn at, so the lines come out at half the
    // pitch -- and at 1:10 the cut face fills in solid black.
    OcctGeometryKernel kernel;
    ScratchPart file{"scaled.ep3d"};
    WriteBlockWithBore(file.path);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& top = drawing.addView("Top", file.path, "Block", ViewDirection::Top,
                                       Vec2{150.0, 200.0});
    ASSERT_TRUE(drawing.recompute().success) << top.diagnostic();
    DrawingView& section = drawing.addSectionView("A-A", top.id(), Vec2{10.0, -20.0},
                                                  Vec2{10.0, 60.0}, 1, 60.0);
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();

    const Box2D full = drawing.sectionHatchRegionMm(section.id()).bounds();
    ASSERT_FALSE(full.empty);
    ASSERT_FALSE(drawing.sectionHatchRegionMm(section.id()).empty());

    ASSERT_TRUE(drawing.setViewScale(section.id(), DrawingScale{1, 2}));
    ASSERT_TRUE(drawing.recompute().success) << section.diagnostic();
    const Box2D half = drawing.sectionHatchRegionMm(section.id()).bounds();
    ASSERT_FALSE(half.empty);

    EXPECT_NEAR(half.width(), full.width() / 2.0, 1e-6)
        << "the region handed to the hatcher did not shrink with the view's scale";
    EXPECT_NEAR(half.height(), full.height() / 2.0, 1e-6);

    // ...and the hatch itself still fills it, at the pitch the style asked for
    // -- which is the point: the SAME pitch on paper at either scale.
    const HatchLines lines = HatchTheRegion(drawing.sectionHatchRegionMm(section.id()),
                                            drawing.sectionHatchStyle(section.id()));
    EXPECT_TRUE(lines.ok) << lines.why;
}
