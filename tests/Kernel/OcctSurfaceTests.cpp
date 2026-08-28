// M59 -- surfaces, and the zero they would otherwise have been weighed as.
//
// Everything in EP3D up to here was a solid, and every consumer assumed one
// without asking because the assumption could not be wrong. A shell breaks
// that in the dangerous way: it is a perfectly valid KernelShape that builds,
// draws, has a bounding box, and that OCCT will hand back mass properties for
// -- a volume of ZERO. Zero is a number. It goes into an items list as a
// volume, into a mass as nothing, onto a cut list as 0 kg, and the only sign
// is a part that weighs nothing.
//
// The arc this closes is M57's. Most IGES in circulation has no solid in it,
// because trimmed surfaces are what the format was built for, and M57 could
// only say so honestly. Reading those surfaces and giving them a thickness is
// what a shop actually does with a supplier's surface model, and it is the
// commonest surfacing operation in mechanical CAD.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/ImportFeature.h"
#include "Core/Export/ExchangeFormat.h"
#include "Core/Kernel/ShapeKind.h"
#include "Core/Measure/ModelMeasure.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

std::filesystem::path Scratch(const std::string& name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "ep3d-surface-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

KernelShape Block(OcctGeometryKernel& kernel, double w, double d, double h) {
    BoxDefinition box;
    box.widthMm = w;
    box.heightMm = h;
    box.depthMm = d;
    const ShapeResult made = kernel.createBox(box);
    EXPECT_EQ(made.error, KernelError::None) << made.message;
    return made.shape;
}

// A SURFACE-ONLY FILE, made the only honest way: write a real solid out as
// IGES in surface mode and read its faces back. Hand-writing IGES entities
// would be testing a fixture rather than the format.
KernelShape SkinOf(OcctGeometryKernel& kernel, const KernelShape& solid,
                   const std::string& name) {
    const std::filesystem::path file = Scratch(name);
    std::filesystem::remove(file);
    EXPECT_TRUE(kernel.exportIges(solid, file.string()).ok);
    const ShapeResult read = kernel.importSurfaces(file.string());
    EXPECT_EQ(read.error, KernelError::None) << read.message;
    return read.shape;
}

std::string SaveToString(const PartDocument& document) {
    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_TRUE(saved) << saved.message;
    return out.str();
}

double ItemOf(const MeasureResult& measured, const std::string& label) {
    for (const MeasureItem& item : measured.items)
        if (item.label == label) return item.value;
    return -1.0;
}

TEST(OcctSurfaceTest, M59_KRN_001_TheKindIsASKEDAndAShellIsNotASolid) {
    OcctGeometryKernel kernel;
    const KernelShape solid = Block(kernel, 60.0, 40.0, 20.0);
    ASSERT_EQ(kernel.kindOfShape(solid), ShapeKind::Solid);
    EXPECT_TRUE(WhyNotASolid(ShapeKind::Solid).empty());

    const KernelShape skin = SkinOf(kernel, solid, "block.igs");
    ASSERT_TRUE(skin.isValid());
    // THE FILE HELD FACES AND THEY SEWED INTO A SKIN. Whether that skin closed
    // is the kernel's answer and not an assumption -- and either way it is not
    // a solid.
    const ShapeKind kind = kernel.kindOfShape(skin);
    EXPECT_TRUE(kind == ShapeKind::Shell || kind == ShapeKind::Face ||
                kind == ShapeKind::Compound)
        << "the surfaces came back as " << NameOf(kind);
    EXPECT_NE(kernel.kindOfShape(skin), ShapeKind::Solid);

    EXPECT_EQ(kernel.kindOfShape(KernelShape{}), ShapeKind::Empty);
}

TEST(OcctSurfaceTest, M59_KRN_002_ASHELLISREFUSEDRatherThanWeighedAsNothing) {
    // THE POINT OF THE MILESTONE. Before M59 this measurement came back ok,
    // with a volume of zero -- and a mass of zero for a part somebody was
    // about to put in a lifting calculation.
    OcctGeometryKernel kernel;
    const KernelShape skin = SkinOf(kernel, Block(kernel, 60.0, 40.0, 20.0), "weigh.igs");
    ASSERT_TRUE(skin.isValid());

    const MeasureResult measured = MeasureSolid(kernel, skin, 7850.0);
    EXPECT_FALSE(measured.ok) << "a skin was weighed and came back with numbers";
    EXPECT_FALSE(measured.message.empty());
    // AND THE MESSAGE SAYS WHAT TO DO. Somebody holding a closed skin has done
    // nearly all the work; "this is not a solid" tells them nothing they can
    // act on.
    EXPECT_NE(measured.message.find("thicken"), std::string::npos) << measured.message;
    EXPECT_TRUE(measured.items.empty()) << "a refused measurement still handed back numbers";

    // ...and comparing a solid with a skin is refused too, because an
    // interference with something that has no inside is zero however deep it
    // is buried -- and zero reads as "these do not touch".
    const KernelShape solid = Block(kernel, 10.0, 10.0, 10.0);
    const MeasureResult between = MeasureBetweenSolids(kernel, solid, skin);
    EXPECT_FALSE(between.ok);
    EXPECT_NE(between.message.find("two solids are needed"), std::string::npos)
        << between.message;
}

TEST(OcctSurfaceTest, M59_KRN_003_ACLOSEDSkinIsMadeSolidRatherThanGivenAWall) {
    // THE CORRECTION M59 NEEDED, and it is a fact about the geometry rather
    // than about OCCT. Offsetting a skin that already closes gives another
    // closed skin: there is no free boundary to build a wall at. The first
    // draft called that operation "thicken" and it quietly produced something
    // that still could not be weighed -- the exact failure this milestone is
    // about, committed by the milestone's own new code.
    //
    // A closed skin does not want a thickness. It wants to be TOLD it encloses
    // material, which is the only thing a solid has that a closed shell does
    // not.
    OcctGeometryKernel kernel;
    const KernelShape skin = SkinOf(kernel, Block(kernel, 60.0, 40.0, 20.0), "thicken.igs");
    ASSERT_TRUE(skin.isValid());
    ASSERT_EQ(kernel.kindOfShape(skin), ShapeKind::Shell);

    const ShapeResult solid = kernel.solidFromSkin(skin);
    ASSERT_EQ(solid.error, KernelError::None) << solid.message;
    ASSERT_EQ(kernel.kindOfShape(solid.shape), ShapeKind::Solid);

    // AND NOW IT WEIGHS THE BLOCK IT ALWAYS WAS: 60 x 40 x 20.
    const MeasureResult measured = MeasureSolid(kernel, solid.shape, 7850.0);
    ASSERT_TRUE(measured.ok) << measured.message;
    EXPECT_NEAR(ItemOf(measured, "Volume"), 48000.0, 1.0);
    EXPECT_GT(ItemOf(measured, "Mass"), 0.0);

    // A CLOSED SKIN IS REFUSED A THICKNESS, by name and with what to do
    // instead -- which is the difference between a message and a shrug.
    const ShapeResult walled = kernel.thickenSurface(skin, 2.0);
    EXPECT_NE(walled.error, KernelError::None)
        << "a closed skin was 'thickened' into another skin";
    EXPECT_NE(walled.message.find("already closes"), std::string::npos) << walled.message;
    EXPECT_NE(walled.message.find("Hollow it afterwards"), std::string::npos) << walled.message;

    // A solid handed to solidFromSkin is already what was asked for, and comes
    // straight back rather than being refused for being finished.
    const KernelShape block = Block(kernel, 10.0, 10.0, 10.0);
    const ShapeResult already = kernel.solidFromSkin(block);
    EXPECT_EQ(already.error, KernelError::None) << already.message;
    EXPECT_EQ(kernel.kindOfShape(already.shape), ShapeKind::Solid);
}

TEST(OcctSurfaceTest, M59_KRN_004_ThickeningRefusesWhatItCannotWork) {
    // WHAT IS NOT COVERED HERE, said out loud: thickening an OPEN skin -- the
    // supplier's surface model with holes in it, which is the case the
    // operation exists for.
    //
    // Nothing in this program can produce one. Surfaces arrive only from files,
    // and the only surface files this suite can write come from closed solids,
    // so every skin it can make closes. Hand-writing an IGES file with a
    // subset of faces in it would be testing a fixture rather than the code.
    // The path is reachable from a real supplier file and is not exercised
    // here; its REFUSALS are, which is what is available to check.
    OcctGeometryKernel kernel;
    const KernelShape skin = SkinOf(kernel, Block(kernel, 60.0, 40.0, 20.0), "refuse.igs");

    // ZERO IS NOT A THIN PART. An offset of nothing is the surface back again,
    // and handing that back as a solid is the confusion this milestone removes.
    const ShapeResult none = kernel.thickenSurface(skin, 0.0);
    EXPECT_NE(none.error, KernelError::None);
    EXPECT_NE(none.message.find("not a part"), std::string::npos) << none.message;

    // A SOLID IS NOT A SURFACE, and thickening one is a question about the
    // wrong kind of thing -- refused with what it actually is.
    //
    // MATCHED ON A PHRASE ONLY THIS REFUSAL HAS. The first draft looked for
    // "a solid", which the CLOSED-SKIN refusal also contains ("can be made a
    // solid directly") -- so deleting the kind check entirely still passed,
    // because a solid's edges each have two faces and it fell through to that
    // one instead. An assertion that several different refusals satisfy is not
    // asserting which one happened.
    const ShapeResult wrong = kernel.thickenSurface(Block(kernel, 10.0, 10.0, 10.0), 2.0);
    EXPECT_NE(wrong.error, KernelError::None);
    EXPECT_NE(wrong.message.find("thickening wants a surface"), std::string::npos)
        << wrong.message;
    EXPECT_EQ(wrong.message.find("already closes"), std::string::npos)
        << "a solid was refused as a closed skin rather than as a solid: " << wrong.message;

    // Nothing at all is nothing at all, for both operations.
    EXPECT_NE(kernel.thickenSurface(KernelShape{}, 2.0).error, KernelError::None);
    EXPECT_NE(kernel.solidFromSkin(KernelShape{}).error, KernelError::None);
}

TEST(OcctSurfaceTest, M59_KRN_004B_AnIgesFileWithNoSolidBecomesAPartTHROUGHTheFeatureTree) {
    // M57 CLOSED, FROM THE OTHER SIDE. That milestone could only tell a user
    // their IGES file has no solid in it and that this is normal for the
    // format. This is what to do about it, and it happens where every other
    // part is built: in the feature tree, rebuilt every pass.
    //
    // THE FILE HAS TO REALLY LACK A SOLID, and the first draft of this test
    // did not manage that: M57 writes IGES in B-rep mode, so exporting a solid
    // and reading it back finds the solid and the surface path never runs.
    // Writing the SKIN out is what produces a file of the shape a supplier
    // sends.
    OcctGeometryKernel kernel;
    const KernelShape skin = SkinOf(kernel, Block(kernel, 60.0, 40.0, 20.0), "seed.igs");
    const std::filesystem::path file = Scratch("supplier.igs");
    std::filesystem::remove(file);
    ASSERT_TRUE(kernel.exportIges(skin, file.string()).ok);
    // M57's importer refuses it, by name and for the right reason.
    const ShapeResult asSolid = kernel.importIges(file.string());
    ASSERT_NE(asSolid.error, KernelError::None)
        << "the file still has a solid in it, so this test proves nothing";
    EXPECT_NE(asSolid.message.find("ordinary case for IGES"), std::string::npos)
        << asSolid.message;

    // WITHOUT A THICKNESS the M22 behaviour stands: refused. That is still
    // right for STEP, where a missing solid means something went wrong rather
    // than that the format works this way.
    PartDocument plain{"Bought in"};
    plain.setGeometryKernel(&kernel);
    Body& plainBody = plain.addBody("Housing");
    ImportFeature& bare = plain.addImportFeature(plainBody, "Plain", file.string());
    plain.recompute();
    EXPECT_EQ(bare.currentState(), ComputeState::Failed)
        << "a file with no solid built anyway, with no thickness to build it from";

    // WITH ONE, the surfaces are read, sewn, and -- because this skin closes --
    // made solid. The thickness is what to do when it does NOT close.
    PartDocument part{"Bought in"};
    part.setGeometryKernel(&kernel);
    Body& body = part.addBody("Housing");
    Parameter& wall = part.addParameter("Wall", 2.0, UnitType::Millimeter);
    ImportFeature& made = part.addImportFeature(body, "Skin", file.string(), wall.id());
    ASSERT_TRUE(part.recompute().success) << "the surfaces could not be turned into a part";
    ASSERT_EQ(made.currentState(), ComputeState::Valid);
    EXPECT_EQ(kernel.kindOfShape(made.currentShape()), ShapeKind::Solid);
    EXPECT_NEAR(ItemOf(MeasureSolid(kernel, made.currentShape(), 0.0), "Volume"), 48000.0, 1.0);

    // AND IT SURVIVES A SAVE (v54). A thickness that was not written down is a
    // part that stops building the next time the file is opened, and the file
    // it fails on is the one the user did not change.
    const std::string saved = SaveToString(part);
    EXPECT_NE(saved.find("thicknessParameterId"), std::string::npos);
    std::istringstream in(saved);
    LoadResult reopened = loadPartDocument(in);
    ASSERT_TRUE(static_cast<bool>(reopened)) << reopened.message;
    const Body* reopenedBody = reopened.document->bodies().front().get();
    const auto* reopenedImport =
        dynamic_cast<const ImportFeature*>(reopenedBody->features().front().get());
    ASSERT_NE(reopenedImport, nullptr);
    EXPECT_EQ(reopenedImport->thicknessParameterId(), wall.id())
        << "the thickness did not survive the round trip, so the part stops building";
}

TEST(OcctSurfaceTest, M59_KRN_006_AFileWithNoSurfacesEitherIsSaidRatherThanSewn) {
    // FOUND BY THE MUTATION GATE. Every surface file in this suite came from a
    // real solid, so "there are no faces in here" was a branch nothing reached
    // -- and a sewing pass over nothing produces something, which the next
    // check then has to be right about.
    OcctGeometryKernel kernel;
    const std::filesystem::path empty = Scratch("nothing.igs");
    {
        std::ofstream out(empty, std::ios::binary);
        const std::string pad(72, ' ');
        out << pad << "S      1\n";
        out << "1H,,1H;,7Hnothing,                                                       G      1\n";
        out << pad << "T      1\n";
    }
    const ShapeResult read = kernel.importSurfaces(empty.string());
    EXPECT_NE(read.error, KernelError::None) << "an empty file sewed into something";
    EXPECT_NE(read.message.find("no surfaces in it either"), std::string::npos) << read.message;

    // "Either" is doing work in that sentence: the caller has already been
    // told there is no solid, and this says the fallback found nothing too.
    EXPECT_NE(kernel.importSurfaces(Scratch("not-here.igs").string()).error, KernelError::None);
    EXPECT_NE(kernel.importSurfaces("").error, KernelError::None);
}

TEST(OcctSurfaceTest, M59_KRN_007_SurfacesAreReadFromTheFILEAndNotItsNAME) {
    // M57's rule, which the surface reader has to follow too: exchange files
    // get renamed, and a supplier's `housing.stp` that is IGES inside would
    // otherwise be handed to the STEP reader and refused for a syntax error
    // about a perfectly good file.
    //
    // Found by the mutation gate -- every surface file in this suite was named
    // after what it held, so reading the name passed all of them.
    OcctGeometryKernel kernel;
    const KernelShape skin = SkinOf(kernel, Block(kernel, 60.0, 40.0, 20.0), "seed2.igs");
    const std::filesystem::path lying = Scratch("housing.stp");
    std::filesystem::remove(lying);
    ASSERT_TRUE(kernel.exportIges(skin, lying.string()).ok);
    ASSERT_EQ(*FormatOfName(lying.string()), ExchangeFormat::Step);
    ASSERT_EQ(*FormatOfContents(lying.string()), ExchangeFormat::Iges);

    const ShapeResult read = kernel.importSurfaces(lying.string());
    ASSERT_EQ(read.error, KernelError::None)
        << "an IGES file called .stp was handed to the STEP reader: " << read.message;
    EXPECT_EQ(kernel.kindOfShape(read.shape), ShapeKind::Shell);
}

TEST(OcctSurfaceTest, M59_KRN_008_ThePREFERENCEIsSolidThenClosedSkinThenThickness) {
    // Three ways a file can become a part, in the order that keeps a real
    // solid from being replaced by an approximation of its own skin. The
    // mutation gate showed the order was not being checked at all: with a
    // block, the solid in the file and the solid made from its sewn skin come
    // out the same size, so measuring the result cannot tell which ran.
    //
    // WHAT DOES TELL THEM APART is what happens when the surface path CANNOT
    // run: with the order right, a file that has a solid does not care.
    OcctGeometryKernel kernel;
    const std::filesystem::path file = Scratch("solid-and-gone.step");
    std::filesystem::remove(file);
    ASSERT_TRUE(kernel.exportStep(Block(kernel, 60.0, 40.0, 20.0), file.string()).ok);

    PartDocument part{"Bought in"};
    part.setGeometryKernel(&kernel);
    Body& body = part.addBody("Housing");
    Parameter& wall = part.addParameter("Wall", 2.0, UnitType::Millimeter);
    ImportFeature& made = part.addImportFeature(body, "Solid", file.string(), wall.id());
    ASSERT_TRUE(part.recompute().success);

    // THE THICKNESS IS TAKEN AWAY. A file with a real solid in it must go on
    // building: the thickness was never relevant to it, and a part that broke
    // because an unrelated parameter was deleted is a part nobody can trust.
    ASSERT_TRUE(part.removeObject(wall.id()));
    ASSERT_TRUE(part.recompute().success)
        << "a file with a solid in it went looking for a thickness it never needed";
    EXPECT_EQ(made.currentState(), ComputeState::Valid);
    EXPECT_NEAR(ItemOf(MeasureSolid(kernel, made.currentShape(), 0.0), "Volume"), 48000.0, 1.0);
}

TEST(OcctSurfaceTest, M59_KRN_009_TheTwoWaysToHaveNoThicknessSayDifferentThings) {
    // A file with no solid and NO thickness set is the M22 behaviour: refused
    // because there is no solid. A file with no solid and a thickness whose
    // parameter has been DELETED is a different failure, with a different next
    // move -- and the mutation gate found both reading the same, because the
    // test only checked that the feature had failed.
    OcctGeometryKernel kernel;
    const KernelShape skin = SkinOf(kernel, Block(kernel, 60.0, 40.0, 20.0), "seed3.igs");
    const std::filesystem::path file = Scratch("surfaces-only.igs");
    std::filesystem::remove(file);
    ASSERT_TRUE(kernel.exportIges(skin, file.string()).ok);
    ASSERT_NE(kernel.importIges(file.string()).error, KernelError::None);

    PartDocument bare{"Bought in"};
    bare.setGeometryKernel(&kernel);
    Body& bareBody = bare.addBody("Housing");
    ImportFeature& plain = bare.addImportFeature(bareBody, "Plain", file.string());
    const DocumentRecomputeReport first = bare.recompute();
    EXPECT_FALSE(first.success);
    std::string plainWhy;
    for (const RecomputeItemReport& item : first.items)
        if (item.id == plain.id()) plainWhy = item.message;
    EXPECT_NE(plainWhy.find("no solid"), std::string::npos)
        << "a file with no solid and no thickness complained about the thickness: " << plainWhy;
    EXPECT_EQ(plainWhy.find("thickness"), std::string::npos) << plainWhy;

    PartDocument gone{"Bought in"};
    gone.setGeometryKernel(&kernel);
    Body& goneBody = gone.addBody("Housing");
    Parameter& wall = gone.addParameter("Wall", 2.0, UnitType::Millimeter);
    ImportFeature& orphan = gone.addImportFeature(goneBody, "Skin", file.string(), wall.id());
    ASSERT_TRUE(gone.recompute().success);
    ASSERT_TRUE(gone.removeObject(wall.id()));
    const DocumentRecomputeReport second = gone.recompute();
    EXPECT_FALSE(second.success);
    std::string orphanWhy;
    for (const RecomputeItemReport& item : second.items)
        if (item.id == orphan.id()) orphanWhy = item.message;
    EXPECT_NE(orphanWhy.find("thickness"), std::string::npos)
        << "the missing thickness was not named: " << orphanWhy;
}

TEST(OcctSurfaceTest, M59_KRN_010_DeletingTheImportAndUndoingKeepsItsTHICKNESS) {
    // THE SHAPE THIS PROJECT HAS PAID FOR THREE TIMES (M49, M50, M54): delete
    // an object, undo, and one of its fields is quietly gone. Saving goes
    // straight to the feature, so the round-trip test above never touched the
    // snapshot path -- and the snapshot is what undo restores from.
    //
    // Found by the mutation gate: dropping the field from FeatureSnapshot broke
    // nothing that was being checked.
    OcctGeometryKernel kernel;
    const KernelShape skin = SkinOf(kernel, Block(kernel, 60.0, 40.0, 20.0), "seed4.igs");
    const std::filesystem::path file = Scratch("undo.igs");
    std::filesystem::remove(file);
    ASSERT_TRUE(kernel.exportIges(skin, file.string()).ok);

    PartDocument part{"Bought in"};
    part.setGeometryKernel(&kernel);
    Body& body = part.addBody("Housing");
    Parameter& wall = part.addParameter("Wall", 2.0, UnitType::Millimeter);
    const ObjectId importId =
        part.addImportFeature(body, "Skin", file.string(), wall.id()).id();
    ASSERT_TRUE(part.recompute().success);

    ASSERT_TRUE(part.removeObject(importId));
    ASSERT_TRUE(part.undo());

    const Body* back = part.bodies().front().get();
    ASSERT_FALSE(back->features().empty());
    const auto* restored = dynamic_cast<const ImportFeature*>(back->features().front().get());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->thicknessParameterId(), wall.id())
        << "the thickness did not come back with the feature, so the part that built before "
           "the delete does not build after the undo";
    EXPECT_TRUE(part.recompute().success);
}

TEST(OcctSurfaceTest, M59_KRN_005_ASOLIDINTHEFILEStillWins) {
    // A file that HAS a solid is never approximated by an offset of its skin,
    // thickness set or not -- and a file that gains one later stops being
    // approximated without anybody noticing and clearing a field.
    OcctGeometryKernel kernel;
    const std::filesystem::path file = Scratch("real.step");
    std::filesystem::remove(file);
    const KernelShape solid = Block(kernel, 60.0, 40.0, 20.0);
    ASSERT_TRUE(kernel.exportStep(solid, file.string()).ok);

    PartDocument part{"Bought in"};
    part.setGeometryKernel(&kernel);
    Body& body = part.addBody("Housing");
    Parameter& wall = part.addParameter("Wall", 2.0, UnitType::Millimeter);
    ImportFeature& made = part.addImportFeature(body, "Solid", file.string(), wall.id());
    ASSERT_TRUE(part.recompute().success);

    // THE WHOLE BLOCK, not a 2 mm skin of it. 60 x 40 x 20 is 48 000 mm3 and
    // the skin would have been about a fifth of that.
    const double volume = ItemOf(MeasureSolid(kernel, made.currentShape(), 0.0), "Volume");
    EXPECT_NEAR(volume, 48000.0, 1.0)
        << "a file with a real solid in it was thickened from its own surfaces instead";
}

} // namespace
