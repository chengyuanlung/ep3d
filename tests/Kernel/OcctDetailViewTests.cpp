// M49.3 -- a detail view END TO END: a real part, a real projection, cropped.
//
// The Core suite pins the crop as arithmetic and the letters as document
// order. Neither of them runs `recompute`, and four of M49's rules live only
// there -- so four mutations sailed through the first gate:
//
//   * the crop never happening: the detail draws the WHOLE part at 2:1, which
//     is a picture somebody could easily believe was meant
//   * the direction not being re-read from the parent: turn the parent and the
//     detail goes on showing a face that is no longer there
//   * a detail of nothing being accepted: an empty ring with a caption reads
//     as "this area is featureless"
//   * the extent not being rebuilt: the view reports the whole part's
//     footprint, so the sheet thinks a 2:1 detail of one hole needs the paper
//     the whole part does

#include "Core/Document/PartDocument.h"
#include "Core/Drawing/DrawingDocument.h"
#include "Core/Feature/PadFeature.h"
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
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-detail-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchPart() { std::remove(path.c_str()); }
};

void WriteBlockPart(const std::string& path, double width, double depth, double height) {
    PartDocument part{"Source"};
    Sketch& sketch = part.addSketch("Base");
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 0}, Vec2{width, 0}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{width, 0}, Vec2{width, depth}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{width, depth}, Vec2{0, depth}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, depth}, Vec2{0, 0}});
    Parameter& tall = part.addParameter("H", height, UnitType::Millimeter);
    Body& body = part.addBody("Block");
    part.addPadFeature(body, "Pad", sketch.id(), tall.id());
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

} // namespace

TEST(OcctDetailViewTest, M49_KRN_001_ADetailShowsLESSThanItsParentDoes) {
    // THE MUTATION THAT SURVIVED THE FIRST GATE: skip the crop and the detail
    // projects the whole part. It draws, it is not empty, it has a caption and
    // a circle on its parent -- and it is a picture of the entire block at
    // twice size, which reads as a view somebody put there on purpose.
    OcctGeometryKernel kernel;
    ScratchPart file{"block.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success) << front.diagnostic();
    const std::size_t whole = front.projected().curves.size();
    ASSERT_GT(whole, 0u);

    // A circle round the left-hand end only.
    DrawingView& detail = drawing.addDetailView("Corner", front.id(), Vec2{0.0, 5.0}, 8.0,
                                                DrawingScale{2, 1}, 60.0);
    ASSERT_TRUE(drawing.recompute().success) << detail.diagnostic();
    ASSERT_EQ(detail.currentState(), ComputeState::Valid) << detail.diagnostic();

    EXPECT_LT(detail.projected().curves.size(), whole)
        << "the detail kept as much as its parent, so nothing was cropped";
    EXPECT_GT(detail.projected().curves.size(), 0u);

    // ...AND ITS FOOTPRINT IS THE CROP'S, not the part's. Left as the whole
    // part's, the sheet believes a 2:1 detail of one corner needs the paper
    // the entire block does -- and "will it fit" is answered about the wrong
    // thing.
    EXPECT_LT(detail.projected().extent.widthMm(), front.projected().extent.widthMm())
        << "the detail reports the whole part's footprint";
    EXPECT_LE(detail.projected().extent.widthMm(), 16.0 + 1e-6)
        << "the crop's extent is wider than the circle it was cropped to";
}

TEST(OcctDetailViewTest, M49_KRN_002_ADetailOfNothingFailsRatherThanDrawingAnEmptyRing) {
    // An empty circle with a caption under it does not read as a mistake. It
    // reads as "this area is featureless", which is a statement about the part
    // that nobody made.
    OcctGeometryKernel kernel;
    ScratchPart file{"block.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success);

    // Well off the part.
    DrawingView& nowhere = drawing.addDetailView("Empty", front.id(), Vec2{500.0, 500.0}, 5.0,
                                                 DrawingScale{2, 1}, 60.0);
    drawing.recompute();
    EXPECT_EQ(nowhere.currentState(), ComputeState::Failed)
        << "a detail circle over nothing was accepted";
    EXPECT_TRUE(nowhere.projected().curves.empty());
    EXPECT_FALSE(nowhere.diagnostic().empty()) << "it failed without saying why";
}

TEST(OcctDetailViewTest, M49_KRN_003_TurningTheParentTurnsTheDetail) {
    // THE REASON THE DIRECTION IS NOT STORED. Held on the detail, it would sit
    // still while somebody turned the parent -- and the detail would go on
    // showing a face that is no longer there: correctly drawn, correctly
    // captioned, and of nothing on this drawing.
    OcctGeometryKernel kernel;
    ScratchPart file{"block.ep3d"};
    WriteBlockPart(file.path, 100.0, 40.0, 10.0);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& front = drawing.addView("Front", file.path, "Block", ViewDirection::Front,
                                         Vec2{100.0, 150.0});
    ASSERT_TRUE(drawing.recompute().success);
    DrawingView& detail = drawing.addDetailView("Corner", front.id(), Vec2{0.0, 5.0}, 8.0,
                                                DrawingScale{2, 1}, 60.0);
    ASSERT_TRUE(drawing.recompute().success) << detail.diagnostic();
    ASSERT_EQ(detail.direction(), ViewDirection::Front);

    ASSERT_TRUE(drawing.setViewDirection(front.id(), ViewDirection::Top));
    drawing.recompute();
    EXPECT_EQ(detail.direction(), ViewDirection::Top)
        << "the parent was turned and the detail stayed looking the old way";
}
