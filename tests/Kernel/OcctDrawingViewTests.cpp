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
#include <string>

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
