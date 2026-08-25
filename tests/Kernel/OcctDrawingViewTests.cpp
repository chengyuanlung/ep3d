// M32.2 -- a drawing view, end to end: open a part file, project it, and get
// curves the right size on the paper.
//
// The kernel suite, because a view has nothing to draw until a solid exists.
// What the Core suite pins is that a drawing is a DOCUMENT; what this pins is
// that a view is a VIEW OF SOMETHING.

#include "Core/Document/PartDocument.h"
#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        DimensionAnchor::inView(front.id(), Vec2{extent.min.x, extent.min.y}),
        DimensionAnchor::inView(front.id(), Vec2{extent.max.x, extent.min.y}),
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
        DimensionAnchor::inView(front.id(), Vec2{extent.min.x, extent.min.y}, 1.0),
        DimensionAnchor::inView(front.id(), Vec2{extent.max.x + 500.0, extent.min.y}, 1.0),
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
        DimensionAnchor::inView(front.id(), Vec2{extent.min.x, extent.min.y}, 1.0),
        DimensionAnchor::inView(front.id(),
                                Vec2{extent.max.x + 0.4, extent.min.y + 0.4}, 1.0),
        Vec2{150.0, 110.0});
    const DimensionMeasurement near = drawing.measure(found);
    ASSERT_TRUE(near.ok) << near.why;
    EXPECT_NEAR(near.valueMm, 100.0, 1e-6);
}
