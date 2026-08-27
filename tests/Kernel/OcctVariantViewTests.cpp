// M54.2 -- two sizes of one part, on one sheet, end to end.
//
// THE FAILURE THIS MILESTONE IS FOR, and this is where it would actually
// happen: a view captioned B, projected from A. Both are real sizes of a real
// part, every dimension reads a number the part has had, nothing is dangling
// and nothing is red. The wrong bracket gets made, in the right quantity, to a
// drawing that checks out.
//
// It cannot happen because the caption and the projection are THE SAME FIELD.
// The view holds one variant name; the resolver builds the part at that size
// before anything is projected, and the caption is written from it.

#include "Core/Document/PartDocument.h"
#include "Core/Drawing/DrawingDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Parameter/PartVariant.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

struct ScratchPart {
    std::string path;
    explicit ScratchPart(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-var-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchPart() { std::remove(path.c_str()); }
};

// A plate whose LENGTH is what the variants vary. Written with two sizes in
// its table and left sitting on the short one.
void WritePlateWithSizes(const std::string& path, OcctGeometryKernel& kernel) {
    PartDocument part{"Plate"};
    part.setGeometryKernel(&kernel);
    Parameter& length = part.addParameter("L", 100.0, UnitType::Millimeter);
    Parameter& tall = part.addParameter("H", 10.0, UnitType::Millimeter);
    Sketch& sketch = part.addSketch("Base");
    // The sketch is drawn at the short size; the variant changes the
    // parameter, and the SKETCH follows because that is what a parameter is
    // for. Here the rectangle is built from the parameter's current value at
    // sketch time, so the test varies the PAD instead -- the point being that
    // a variant is parameter edits and nothing else.
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 0}, Vec2{60, 0}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{60, 0}, Vec2{60, 40}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{60, 40}, Vec2{0, 40}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, 40}, Vec2{0, 0}});
    Body& body = part.addBody("Plate");
    part.addPadFeature(body, "Pad", sketch.id(), tall.id());

    PartVariant thin;
    thin.name = "Thin";
    thin.values[tall.id()] = 10.0;
    thin.values[length.id()] = 100.0;
    PartVariant thick;
    thick.name = "Thick";
    thick.values[tall.id()] = 30.0;
    thick.values[length.id()] = 100.0;
    ASSERT_TRUE(part.addVariant(thin));
    ASSERT_TRUE(part.addVariant(thick));
    ASSERT_TRUE(part.recompute().success);
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

TEST(OcctVariantViewTest, M54_KRN_001_TwoSizesOfOnePartOnOneSheet) {
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& thin = drawing.addView("Thin", file.path, "Plate", ViewDirection::Front,
                                        Vec2{40.0, 40.0});
    DrawingView& thick = drawing.addView("Thick", file.path, "Plate", ViewDirection::Front,
                                         Vec2{40.0, 160.0});
    ASSERT_TRUE(drawing.setViewVariant(thin.id(), "Thin"));
    ASSERT_TRUE(drawing.setViewVariant(thick.id(), "Thick"));
    ASSERT_TRUE(drawing.recompute().success) << thin.diagnostic() << thick.diagnostic();

    // ONE FILE, TWO SIZES, SIDE BY SIDE. A front view of a 10 tall plate and a
    // 30 tall one are different heights, and that is the whole feature.
    EXPECT_NEAR(thin.projected().extent.heightMm(), 10.0, 1e-6);
    EXPECT_NEAR(thick.projected().extent.heightMm(), 30.0, 1e-6);
    EXPECT_GT(thick.projected().extent.heightMm(), thin.projected().extent.heightMm());
}

TEST(OcctVariantViewTest, M54_KRN_002_TheCaptionAndThePictureAreTheSameField) {
    // Not "they agree" -- they are ONE field, read twice. There is nothing to
    // keep in step.
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& view = drawing.addView("Plate", file.path, "Plate", ViewDirection::Front,
                                        Vec2{40.0, 40.0});
    ASSERT_TRUE(drawing.setViewVariant(view.id(), "Thick"));
    ASSERT_TRUE(drawing.recompute().success) << view.diagnostic();
    EXPECT_NE(drawing.viewLabelText(view.id()).find("[Thick]"), std::string::npos)
        << drawing.viewLabelText(view.id());
    EXPECT_NEAR(view.projected().extent.heightMm(), 30.0, 1e-6);

    // CHANGE THE SIZE AND BOTH FOLLOW, because there is only one of them.
    ASSERT_TRUE(drawing.setViewVariant(view.id(), "Thin"));
    ASSERT_TRUE(drawing.recompute().success) << view.diagnostic();
    EXPECT_NE(drawing.viewLabelText(view.id()).find("[Thin]"), std::string::npos);
    EXPECT_NEAR(view.projected().extent.heightMm(), 10.0, 1e-6);
}

TEST(OcctVariantViewTest, M54_KRN_003_ASizeThisPartDoesNotHaveIsRefusedLOUDLY) {
    // Building the default and carrying on is exactly how a drawing ends up
    // labelled B and projected from A.
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& view = drawing.addView("Plate", file.path, "Plate", ViewDirection::Front,
                                        Vec2{40.0, 40.0});
    ASSERT_TRUE(drawing.setViewVariant(view.id(), "Enormous"));
    drawing.recompute();
    EXPECT_EQ(view.currentState(), ComputeState::Failed)
        << "a view asked for a size the part does not have and drew something anyway";
    EXPECT_NE(view.diagnostic().find("no variant called"), std::string::npos)
        << view.diagnostic();
    // ...AND IT SAYS WHAT THE PART DOES HAVE, because "no" without a list is
    // an afternoon of opening files.
    EXPECT_NE(view.diagnostic().find("Thin"), std::string::npos) << view.diagnostic();
}

TEST(OcctVariantViewTest, M54_KRN_004_TheOldPictureStaysUntilTheDrawingIsRebuilt) {
    // Changing the size marks the view for rebuild; it does not redraw it on
    // the spot. What is on the paper until then is the size it WAS, which is
    // deliberate -- a drawing that reprojected itself the moment a field
    // changed would rebuild a hundred views on a keystroke.
    //
    // The first draft of this test asserted currentState() was no longer
    // Valid. It is: that enum is a cache the VIEW keeps, and only a recompute
    // writes it. Whether a view needs rebuilding is the graph's answer, and
    // the graph's answer is delivered by the next recompute producing
    // different curves -- which is what is checked here instead of an
    // implementation detail the codebase does not maintain.
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    DrawingView& view = drawing.addView("Plate", file.path, "Plate", ViewDirection::Front,
                                        Vec2{40.0, 40.0});
    ASSERT_TRUE(drawing.setViewVariant(view.id(), "Thin"));
    ASSERT_TRUE(drawing.recompute().success);
    ASSERT_NEAR(view.projected().extent.heightMm(), 10.0, 1e-6);

    // The size is changed and nothing is rebuilt yet.
    ASSERT_TRUE(drawing.setViewVariant(view.id(), "Thick"));
    EXPECT_NEAR(view.projected().extent.heightMm(), 10.0, 1e-6)
        << "the view reprojected itself on a field change";

    // ...and the rebuild picks it up, which is the part that must not be
    // missed: a view left un-dirtied would go on drawing the old size for
    // ever, under the new caption.
    ASSERT_TRUE(drawing.recompute().success) << view.diagnostic();
    EXPECT_NEAR(view.projected().extent.heightMm(), 30.0, 1e-6)
        << "the rebuild did not pick up the new size, so the change never marked it";
}

TEST(OcctVariantViewTest, M54_KRN_005_TheSizeSurvivesTheFile) {
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    const ObjectId id = drawing
                            .addView("Plate", file.path, "Plate", ViewDirection::Front,
                                     Vec2{40.0, 40.0})
                            .id();
    ASSERT_TRUE(drawing.setViewVariant(id, "Thick"));
    ASSERT_TRUE(drawing.recompute().success);

    std::ostringstream out;
    ASSERT_EQ(saveDrawingDocument(drawing, out).error, SerializationError::None);
    std::istringstream in(out.str());
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingView* back = loaded.document->findView(id);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->variantName(), "Thick")
        << "the drawing came back showing the part's own numbers under a variant's name";
    EXPECT_NE(loaded.document->viewLabelText(id).find("[Thick]"), std::string::npos);
}

TEST(OcctVariantViewTest, M54_KRN_006_TheSizeTableSurvivesThePartFile) {
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    const LoadResult loaded = loadPartDocumentFromFile(file.path);
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->variants().size(), 2u);
    EXPECT_EQ(loaded.document->variants()[0].name, "Thin");
    EXPECT_EQ(loaded.document->variants()[1].name, "Thick");
    // ...AND WHICH ONE IT IS ON is worked out from the parameters, not read
    // from the file -- so a hand-edited parameter cannot leave the file
    // claiming a size it is not at.
    EXPECT_EQ(loaded.document->activeVariantName(), "Thin");
}

TEST(OcctVariantViewTest, M54_KRN_006B_WhatTheSaverRefusesTheLoaderRefuses) {
    // ADR-M3-008, on the size table. A hand-edited file whose rows disagree
    // about which columns the table has is a table nothing can compare
    // against -- so "which size is this" would have no answer, on a part that
    // opens and builds perfectly.
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    std::ifstream in(file.path);
    ASSERT_TRUE(in.is_open());
    const std::string original((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    in.close();

    // Two rows called the same thing: a drawing asking for that name would get
    // whichever was found first.
    std::string duplicated = original;
    const std::string::size_type at = duplicated.find("\"name\": \"Thick\"");
    ASSERT_NE(at, std::string::npos);
    duplicated.replace(at, std::string("\"name\": \"Thick\"").size(), "\"name\": \"Thin\"");
    {
        std::ofstream out(file.path);
        out << duplicated;
    }
    EXPECT_FALSE(loadPartDocumentFromFile(file.path))
        << "a part with two sizes of the same name loaded anyway";

    // ...and a row setting a parameter the part does not have is a size that
    // cannot be applied, found on the day somebody picks it.
    //
    // THE EDIT IS CHECKED BEFORE IT IS TRUSTED. The first version of this cut
    // the file with an offset and produced something that would not parse --
    // so the load refused, the test passed, and it was measuring the JSON
    // reader rather than the rule. A test that passes for the wrong reason is
    // worse than one that fails.
    std::string ghost = original;
    const std::string::size_type variants = ghost.find("\"variants\"");
    ASSERT_NE(variants, std::string::npos) << "the file has no variant table to edit";
    const std::string::size_type param = ghost.find("\"parameterId\": \"", variants);
    ASSERT_NE(param, std::string::npos) << "the variant table has no parameterId to edit";
    const std::string::size_type open = param + std::string("\"parameterId\": \"").size();
    const std::string::size_type close = ghost.find('\"', open);
    ASSERT_NE(close, std::string::npos);
    const std::string was = ghost.substr(open, close - open);
    ASSERT_FALSE(was.empty());
    // EVERY ROW, not just the first. Changing one row's parameter makes the
    // rows disagree about which columns the table has -- and THAT rule refuses
    // the file, so the check being tested here is never reached. The mutation
    // gate found exactly that: the test passed with the existence check gone.
    //
    // Renaming the column in every row leaves a table that is internally
    // consistent and names a parameter this part does not have, which is the
    // one thing left to refuse it.
    const std::string token = "\"parameterId\": \"" + was + "\"";
    const std::string swapped = "\"parameterId\": \"424242\"";
    for (std::string::size_type found = ghost.find(token, variants);
         found != std::string::npos; found = ghost.find(token, found + swapped.size()))
        ghost.replace(found, token.size(), swapped);
    EXPECT_EQ(ghost.find(token, variants), std::string::npos)
        << "not every row was renamed, so the columns disagree and a different rule refuses";
    {
        std::ofstream out(file.path);
        out << ghost;
    }
    // The ONLY difference from a file that loads is which parameter that row
    // names -- so a refusal here is the rule and nothing else.
    EXPECT_NE(ghost, original);
    EXPECT_FALSE(loadPartDocumentFromFile(file.path))
        << "a size naming a parameter this part does not have loaded anyway";
}

TEST(OcctVariantViewTest, M54_KRN_006C_DeletingAViewAndUndoingBringsItsSizeBack) {
    // THE THIRD TIME THIS SHAPE HAS APPEARED. M49's detail circle and M50's
    // break span both went missing from the undo applier's hand-written
    // restore list, one milestone at a time; this is the variant name.
    //
    // Restored without it, the view comes back projecting the part's own
    // numbers under a caption naming a size -- which is the failure this whole
    // milestone exists to prevent, arriving through undo.
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    const ObjectId id = drawing
                            .addView("Plate", file.path, "Plate", ViewDirection::Front,
                                     Vec2{40.0, 40.0})
                            .id();
    ASSERT_TRUE(drawing.setViewVariant(id, "Thick"));
    ASSERT_TRUE(drawing.recompute().success);

    ASSERT_TRUE(drawing.removeObject(id));
    ASSERT_EQ(drawing.findView(id), nullptr);
    ASSERT_TRUE(drawing.undo());

    const DrawingView* back = drawing.findView(id);
    ASSERT_NE(back, nullptr) << "undo did not bring the view back at all";
    EXPECT_EQ(back->variantName(), "Thick")
        << "the view came back at the part's own size under a caption naming a variant";
    EXPECT_NE(drawing.viewLabelText(id).find("[Thick]"), std::string::npos);
}

TEST(OcctVariantViewTest, M54_KRN_007_UndoTakesBackTheSize) {
    OcctGeometryKernel kernel;
    ScratchPart file{"plate.ep3d"};
    WritePlateWithSizes(file.path, kernel);

    DrawingDocument drawing{"Plate"};
    drawing.setGeometryKernel(&kernel);
    const ObjectId id = drawing
                            .addView("Plate", file.path, "Plate", ViewDirection::Front,
                                     Vec2{40.0, 40.0})
                            .id();
    ASSERT_TRUE(drawing.setViewVariant(id, "Thin"));
    ASSERT_TRUE(drawing.setViewVariant(id, "Thick"));
    ASSERT_EQ(drawing.findView(id)->variantName(), "Thick");

    ASSERT_TRUE(drawing.undo());
    EXPECT_EQ(drawing.findView(id)->variantName(), "Thin")
        << "undo left the view at the size it had been changed to";
    ASSERT_TRUE(drawing.undo());
    EXPECT_EQ(drawing.findView(id)->variantName(), "");
}

} // namespace
