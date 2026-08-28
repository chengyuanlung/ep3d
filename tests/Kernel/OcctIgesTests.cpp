// M57.2 -- IGES out and back.
//
// The thing worth testing here is not "does OCCT write a file". It is that
// what comes back is a SOLID.
//
// OCCT's IGES writer defaults to faces-as-trimmed-surfaces, and that default
// is wrong for a program whose only export is a solid: the file still opens,
// still looks right on screen, and arrives at the far end as a bag of surfaces
// that nothing can cut, fillet or weigh. Nobody notices until somebody tries
// to use it -- and by then it is a supplier's problem, at their end, with this
// program's name on the file.
//
// So the round trip is measured rather than looked at: the same volume out as
// in, and a real solid to measure it on.

#include "Core/Document/PartDocument.h"
#include "Core/Export/ExchangeFormat.h"
#include "Core/Feature/ImportFeature.h"
#include "Core/Measure/ModelMeasure.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace paramcad;

std::filesystem::path Scratch(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ep3d-iges-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

KernelShape BlockWithAHole(OcctGeometryKernel& kernel) {
    BoxDefinition box;
    box.widthMm = 60.0;
    box.heightMm = 20.0;
    box.depthMm = 40.0;
    const ShapeResult block = kernel.createBox(box);
    EXPECT_EQ(block.error, KernelError::None) << block.message;

    // A POCKET THAT DOES NOT REACH THE FAR SIDE, so the solid has inner faces
    // and cannot be mistaken for a plain block that a surface-mode round trip
    // happened to sew back up.
    BoxDefinition slot;
    slot.widthMm = 20.0;
    slot.heightMm = 10.0;
    slot.depthMm = 20.0;
    const ShapeResult drill = kernel.createBox(slot);
    EXPECT_EQ(drill.error, KernelError::None) << drill.message;
    const ShapeResult placed = kernel.translateShape(drill.shape, Vec3{20.0, 12.0, 10.0});
    EXPECT_EQ(placed.error, KernelError::None) << placed.message;
    const ShapeResult cut = kernel.subtractShape(block.shape, placed.shape);
    EXPECT_EQ(cut.error, KernelError::None) << cut.message;
    return cut.shape;
}

double VolumeOf(OcctGeometryKernel& kernel, const KernelShape& shape) {
    const MeasureResult measured = MeasureSolid(kernel, shape, 0.0);
    EXPECT_TRUE(measured.ok) << measured.message;
    for (const MeasureItem& item : measured.items)
        if (item.label == "Volume") return item.value;
    return -1.0;
}

TEST(OcctIgesTest, M57_KRN_001_ASolidSurvivesTheRoundTripAsASolid) {
    OcctGeometryKernel kernel;
    const KernelShape original = BlockWithAHole(kernel);
    ASSERT_TRUE(original.isValid());
    const double before = VolumeOf(kernel, original);
    ASSERT_GT(before, 0.0);

    const std::filesystem::path file = Scratch("block.igs");
    std::filesystem::remove(file);
    const IoResult written = kernel.exportIges(original, file.string());
    ASSERT_TRUE(written.ok) << written.message;
    ASSERT_TRUE(std::filesystem::exists(file));
    ASSERT_GT(std::filesystem::file_size(file), 0u);

    // IT IS AN IGES FILE BY ITS OWN CONTENT, not because we named it .igs --
    // which is the same question the importer asks (M57.1).
    ASSERT_TRUE(FormatOfContents(file.string()).has_value());
    EXPECT_EQ(*FormatOfContents(file.string()), ExchangeFormat::Iges);

    const ShapeResult back = kernel.importIges(file.string());
    ASSERT_EQ(back.error, KernelError::None) << back.message;
    ASSERT_TRUE(back.shape.isValid());

    // THE POINT: a solid, with the volume it left with. A surface-mode export
    // comes back as faces and this measurement is what refuses it.
    const double after = VolumeOf(kernel, back.shape);
    EXPECT_NEAR(after, before, before * 1e-4)
        << "the solid came back with a different amount of it, so what was written was not "
           "the solid";
}

TEST(OcctIgesTest, M57_KRN_002_ASurfaceOnlyFileIsRefusedWithTheREASONBeingTheFormat) {
    // MOST IGES IN CIRCULATION HAS NO SOLID IN IT. The format was built for
    // trimmed surfaces, so "this file holds no solid" is the ordinary answer
    // here, not an unusual one -- and a user told their file is empty will go
    // looking for a fault that is not there.
    //
    // A one-line IGES fragment is enough: it parses as IGES and contains no
    // geometry at all, which is the same shape of answer a real surface file
    // gives.
    OcctGeometryKernel kernel;
    const std::filesystem::path file = Scratch("surfaces.igs");
    {
        std::ofstream out(file, std::ios::binary);
        const std::string pad(72, ' ');
        out << pad << "S      1\n";
        out << "1H,,1H;,8Hsurfaces,10Hsurfaces.igs,                                    G      1\n";
        out << pad << "T      1\n";
    }

    const ShapeResult back = kernel.importIges(file.string());
    EXPECT_NE(back.error, KernelError::None) << "a file with no solid came back with one";
    EXPECT_NE(back.message.find("no solid"), std::string::npos) << back.message;
    // AND IT SAYS THIS IS NORMAL FOR IGES, which is the whole difference
    // between a message that helps and one that sends somebody hunting.
    EXPECT_NE(back.message.find("ordinary case for IGES"), std::string::npos) << back.message;
    EXPECT_NE(back.message.find("STEP"), std::string::npos)
        << "the message does not say what to ask for instead: " << back.message;
}

TEST(OcctIgesTest, M57_KRN_003_STEPAndIGESRefuseTheSameThingsThroughTheSameFunction) {
    // Two importers with two copies of "one solid, or say how many" is how the
    // two come to disagree about what several means. They share it, and this
    // is what says so.
    OcctGeometryKernel kernel;

    const ShapeResult noStep = kernel.importStep("");
    EXPECT_NE(noStep.error, KernelError::None);
    const ShapeResult noIges = kernel.importIges("");
    EXPECT_NE(noIges.error, KernelError::None);
    EXPECT_EQ(noStep.message, noIges.message)
        << "the two importers word an empty file name differently";

    const ShapeResult missing = kernel.importIges(Scratch("not-here.igs").string());
    EXPECT_NE(missing.error, KernelError::None);
    EXPECT_NE(missing.message.find("could not read"), std::string::npos) << missing.message;

    // TWO SOLIDS ARE REFUSED WITH THE COUNT, through the shared walk. Taking
    // the first would import a different part than the file holds and fusing
    // them would invent material between parts deliberately apart -- and both
    // look like success.
    BoxDefinition box;
    box.widthMm = 10.0;
    box.heightMm = 10.0;
    box.depthMm = 10.0;
    const ShapeResult a = kernel.createBox(box);
    ASSERT_EQ(a.error, KernelError::None);
    const ShapeResult b = kernel.translateShape(a.shape, Vec3{100.0, 0.0, 0.0});
    ASSERT_EQ(b.error, KernelError::None);
    const ShapeResult pair = kernel.compoundOf({a.shape, b.shape});
    ASSERT_EQ(pair.error, KernelError::None);

    const std::filesystem::path two = Scratch("two.igs");
    std::filesystem::remove(two);
    ASSERT_TRUE(kernel.exportIges(pair.shape, two.string()).ok);
    const ShapeResult back = kernel.importIges(two.string());
    EXPECT_NE(back.error, KernelError::None) << "two solids came back as one part";
    EXPECT_NE(back.message.find("2 solids"), std::string::npos) << back.message;
}

TEST(OcctIgesTest, M57_KRN_005_AnImportFeatureReadsTheFILEAndNotItsNAME) {
    // THE MILESTONE'S THESIS, and until the mutation gate asked, nothing
    // tested it: reading the format off the file's NAME passed every test in
    // the suite, because every file in every test was named after what it
    // held. Exchange files in a shop are not.
    //
    // A supplier sends `housing.stp` that is IGES inside -- a pass-through
    // wrote it, or somebody renamed it to get it past a filter. Trusting the
    // name means reporting a STEP syntax error about a perfectly good IGES
    // file, and the reader then goes looking for a fault in the file.
    OcctGeometryKernel kernel;
    const KernelShape original = BlockWithAHole(kernel);
    const double before = VolumeOf(kernel, original);

    const std::filesystem::path lying = Scratch("housing.stp");
    std::filesystem::remove(lying);
    ASSERT_TRUE(kernel.exportIges(original, lying.string()).ok);
    // The NAME says STEP and the CONTENT says IGES. That is the disagreement.
    ASSERT_TRUE(FormatOfName(lying.string()).has_value());
    EXPECT_EQ(*FormatOfName(lying.string()), ExchangeFormat::Step);
    ASSERT_TRUE(FormatOfContents(lying.string()).has_value());
    EXPECT_EQ(*FormatOfContents(lying.string()), ExchangeFormat::Iges);

    PartDocument part{"Brought in"};
    part.setGeometryKernel(&kernel);
    Body& body = part.addBody("Housing");
    ImportFeature& brought = part.addImportFeature(body, "Housing import", lying.string());
    const DocumentRecomputeReport report = part.recompute();
    ASSERT_TRUE(report.success) << "the importer went by the name and refused the file";
    ASSERT_EQ(brought.currentState(), ComputeState::Valid);
    ASSERT_TRUE(brought.currentShape().isValid());
    EXPECT_NEAR(VolumeOf(kernel, brought.currentShape()), before, before * 1e-4);

    // ...and the other way round: STEP content under an IGES name.
    const std::filesystem::path other = Scratch("bracket.igs");
    std::filesystem::remove(other);
    ASSERT_TRUE(kernel.exportStep(original, other.string()).ok);
    ASSERT_TRUE(FormatOfContents(other.string()).has_value());
    EXPECT_EQ(*FormatOfContents(other.string()), ExchangeFormat::Step);

    PartDocument second{"Brought in too"};
    second.setGeometryKernel(&kernel);
    Body& secondBody = second.addBody("Bracket");
    ImportFeature& also =
        second.addImportFeature(secondBody, "Bracket import", other.string());
    ASSERT_TRUE(second.recompute().success)
        << "a STEP file called .igs was handed to the IGES reader";
    EXPECT_NEAR(VolumeOf(kernel, also.currentShape()), before, before * 1e-4);
}

TEST(OcctIgesTest, M57_KRN_004_NothingToExportIsSaidRatherThanWritten) {
    OcctGeometryKernel kernel;
    const KernelShape nothing;
    const IoResult refused = kernel.exportIges(nothing, Scratch("ghost.igs").string());
    EXPECT_FALSE(refused.ok);
    EXPECT_FALSE(refused.message.empty());
    // No file left behind for somebody to find and wonder about.
    EXPECT_FALSE(std::filesystem::exists(Scratch("ghost.igs")));

    const KernelShape real = BlockWithAHole(kernel);
    const IoResult noName = kernel.exportIges(real, "");
    EXPECT_FALSE(noName.ok);
    EXPECT_FALSE(noName.message.empty());
}

} // namespace
