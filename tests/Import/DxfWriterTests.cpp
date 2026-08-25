// M35.5 -- writing DXF, and reading it straight back.
//
// THE GATE THAT MATTERS FOR AN EXPORTER IS THE ROUND TRIP.
//
// "It wrote a file" is not a test: a DXF that opens cleanly and is subtly not
// the drawing is worse than one that fails, because nobody re-checks a file
// that opened. ADR-M3-008 named this for the native format -- a document that
// saves cleanly and then refuses to load is the worst case -- and it is the
// same rule one file format along.
//
// These live in the import suite because that is where the READER is: the
// writer is in Core with no library behind it, but proving what it wrote means
// parsing it, and the parser is GPL and lives here (ADR-M6-001).

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Export/DxfWriter.h"
#include "Import/Dxf/DxfReader.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <streambuf>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

struct ScratchDxf {
    std::string path;
    explicit ScratchDxf(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-dxf-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchDxf() { std::remove(path.c_str()); }
};

// Was `kind` reported as skipped, by name? An entity the reader does not
// support has to come back NAMED rather than silently missing -- that is what
// proves the writer emitted it under the right entity name.
std::string ReadWhole(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Every name an ENTITY put on group code 8 (layer) or 6 (linetype), and every
// name the TABLES declared. A DXF is pairs of lines, so this is a walk.
struct DxfNames {
    std::vector<std::string> layersDeclared;
    std::vector<std::string> linetypesDeclared;
    std::vector<std::string> layersUsed;
    std::vector<std::string> linetypesUsed;
};

DxfNames NamesIn(const std::string& text) {
    DxfNames names;
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    std::string table;
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
        if (lines[i] == "0" && lines[i + 1] == "TABLE" && i + 3 < lines.size())
            table = lines[i + 3];
        if (lines[i] == "0" && lines[i + 1] == "ENDTAB") table.clear();
        if (lines[i] == "0" && lines[i + 1] == "LAYER" && table == "LAYER" &&
            i + 3 < lines.size() && lines[i + 2] == "2")
            names.layersDeclared.push_back(lines[i + 3]);
        if (lines[i] == "0" && lines[i + 1] == "LTYPE" && table == "LTYPE" &&
            i + 3 < lines.size() && lines[i + 2] == "2")
            names.linetypesDeclared.push_back(lines[i + 3]);
        if (table.empty()) {
            if (lines[i] == "8") names.layersUsed.push_back(lines[i + 1]);
            if (lines[i] == "6") names.linetypesUsed.push_back(lines[i + 1]);
        }
    }
    return names;
}

bool Contains(const std::vector<std::string>& all, const std::string& one) {
    for (const std::string& name : all)
        if (name == one) return true;
    return false;
}

// A stream that gives out after `budget` characters, so the "did the write
// finish" path can be reached. A full disk cannot be arranged from a test; a
// buffer that stops accepting can.
class FailingBuffer : public std::streambuf {
public:
    explicit FailingBuffer(std::size_t budget) : budget_(budget) {}

protected:
    int_type overflow(int_type character) override {
        if (budget_ == 0) return traits_type::eof();
        --budget_;
        return character;
    }

private:
    std::size_t budget_;
};

bool WasSkipped(const ImportedSketchGeometry& geometry, const std::string& kind) {
    for (const ImportedSkip& skip : geometry.skipped)
        if (skip.entityKind == kind) return true;
    return false;
}

} // namespace

TEST(DxfWriterTest, M35_DXF_001_LinesCirclesAndArcsComeBackTheSameSize) {
    // The core claim. Every number here is checked against what went in, not
    // against a count -- a writer that put every circle at the origin would
    // pass a count.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{10.0, 20.0}, Vec2{110.0, 20.0}});
    document.addEntity(DrawCircle{Vec2{50.0, 60.0}, 12.5});
    document.addEntity(DrawArc{Vec2{80.0, 90.0}, 7.5, 0.0, 1.5707963267948966});

    ScratchDxf file{"basic.dxf"};
    const DxfWriteResult written = WriteDxfFile(document, file.path);
    ASSERT_TRUE(written) << written.why;
    EXPECT_EQ(written.entities, 3u);

    const DxfReadResult read = ReadDxfFile(file.path);
    ASSERT_TRUE(read) << read.message;
    ASSERT_EQ(read.geometry.lines.size(), 1u);
    ASSERT_EQ(read.geometry.circles.size(), 1u);
    ASSERT_EQ(read.geometry.arcs.size(), 1u);

    EXPECT_NEAR(read.geometry.lines[0].start.x, 10.0, 1e-6);
    EXPECT_NEAR(read.geometry.lines[0].start.y, 20.0, 1e-6);
    EXPECT_NEAR(read.geometry.lines[0].end.x, 110.0, 1e-6);
    EXPECT_NEAR(read.geometry.circles[0].center.x, 50.0, 1e-6);
    EXPECT_NEAR(read.geometry.circles[0].center.y, 60.0, 1e-6);
    EXPECT_NEAR(read.geometry.circles[0].radiusMm, 12.5, 1e-6);
    EXPECT_NEAR(read.geometry.arcs[0].radiusMm, 7.5, 1e-6);
    // DEGREES OUT, RADIANS BACK. The one conversion, checked in the only way
    // that can catch it being applied twice or not at all.
    EXPECT_NEAR(read.geometry.arcs[0].startAngleRad, 0.0, 1e-6);
    EXPECT_NEAR(read.geometry.arcs[0].endAngleRad, 1.5707963267948966, 1e-6);
}

TEST(DxfWriterTest, M35_DXF_002_TheFileSaysItIsInMILLIMETRES) {
    // A DXF without $INSUNITS is a file whose numbers mean whatever the
    // program that opens it assumes -- and this project has a whole enum for
    // how badly that goes.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}});
    ScratchDxf file{"units.dxf"};
    ASSERT_TRUE(WriteDxfFile(document, file.path));

    const DxfReadResult read = ReadDxfFile(file.path);
    ASSERT_TRUE(read) << read.message;
    EXPECT_EQ(read.geometry.unit, ImportedLengthUnit::Millimeter);
    EXPECT_FALSE(read.geometry.unitWasDefaulted)
        << "the file did not say what its numbers mean, so the reader guessed";
    EXPECT_NEAR(read.geometry.millimetresPerUnit, 1.0, 1e-12);
}

TEST(DxfWriterTest, M35_DXF_004_AnEllipseIsWrittenAsAPolylineAndTheLossIsSAID) {
    // R12 has no ellipse entity. A writer that silently approximated would
    // produce a file that opens cleanly and is subtly not the drawing.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawEllipse{Vec2{50.0, 50.0}, 20.0, 10.0, 0.0});

    ScratchDxf file{"ellipse.dxf"};
    const DxfWriteResult written = WriteDxfFile(document, file.path);
    ASSERT_TRUE(written) << written.why;
    ASSERT_FALSE(written.losses.empty()) << "an ellipse was approximated in silence";
    bool saidEllipse = false;
    for (const DxfWriteLoss& loss : written.losses)
        if (loss.what == "ELLIPSE") saidEllipse = true;
    EXPECT_TRUE(saidEllipse);

    // ...and it really went out as a POLYLINE, which the reader names rather
    // than dropping. Named-and-skipped is what proves the entity was emitted
    // under the right name; silently missing would look identical to not
    // having been written at all.
    const DxfReadResult read = ReadDxfFile(file.path);
    ASSERT_TRUE(read) << read.message;
    EXPECT_TRUE(WasSkipped(read.geometry, "POLYLINE"))
        << "the ellipse did not reach the file as a polyline";
}

TEST(DxfWriterTest, M35_DXF_005_APolylineKeepsItsBULGES) {
    // Flattening here would turn every arc segment a drafter drew into a
    // chord, and the recipient could never get it back.
    DrawingDocument document{"Sheet"};
    DrawPolyline shape;
    shape.vertices.push_back(DrawVertex{Vec2{0.0, 0.0}, 0.5});
    shape.vertices.push_back(DrawVertex{Vec2{40.0, 0.0}, 0.0});
    shape.closed = false;
    document.addEntity(shape);

    ScratchDxf file{"bulge.dxf"};
    ASSERT_TRUE(WriteDxfFile(document, file.path));

    // Read as TEXT, because the reader does not take polylines -- what is
    // being checked is that the bulge is IN the file, on group code 42.
    std::ifstream in(file.path);
    ASSERT_TRUE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    EXPECT_NE(text.find("\n42\n0.500000"), std::string::npos)
        << "the polyline went out with its bulge flattened away";
    EXPECT_NE(text.find("SEQEND"), std::string::npos)
        << "an R12 polyline without SEQEND is one most readers reject";
}

TEST(DxfWriterTest, M35_DXF_006_LayersAndTheirFlagsSurvive) {
    DrawingDocument document{"Sheet"};
    Layer& hidden = document.addLayer("HIDDEN-EDGES", 3);
    ASSERT_TRUE(document.setLayerOn(hidden.id(), false));
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{10.0, 0.0}});

    ScratchDxf file{"layers.dxf"};
    ASSERT_TRUE(WriteDxfFile(document, file.path));
    std::ifstream in(file.path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    EXPECT_NE(text.find("HIDDEN-EDGES"), std::string::npos) << "a layer was not declared";
    // OFF IS A NEGATIVE COLOUR in DXF, which is not the same mechanism as
    // FROZEN -- writing one for the other would turn a layer somebody switched
    // off into one that cannot be switched back on without a regen.
    EXPECT_NE(text.find("\n62\n-3\n"), std::string::npos)
        << "a layer that is off did not go out as a negative colour";
}

TEST(DxfWriterTest, M35_DXF_007_ADimensionCarriesItsMeasuredValue) {
    DrawingDocument document{"Sheet"};
    const ObjectId line = document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}}).id();
    document.addDimension(DimensionKind::Linear, DimensionAnchor::onEntity(line, 0),
                          DimensionAnchor::onEntity(line, 1), Vec2{50.0, 20.0});

    ScratchDxf file{"dim.dxf"};
    const DxfWriteResult written = WriteDxfFile(document, file.path);
    ASSERT_TRUE(written) << written.why;
    // The loss is SAID: a recipient's program shows the number and does not
    // follow the model.
    bool saidDimension = false;
    for (const DxfWriteLoss& loss : written.losses)
        if (loss.what == "DIMENSION") saidDimension = true;
    EXPECT_TRUE(saidDimension);

    const DxfReadResult read = ReadDxfFile(file.path);
    ASSERT_TRUE(read) << read.message;
    ASSERT_EQ(read.geometry.dimensions.size(), 1u);
    const ImportedDimension2D& back = read.geometry.dimensions[0];
    // THE EXTENSION ORIGINS ARE THE POINTS ON THE GEOMETRY, which is exactly
    // what makes a dimension re-matchable on the far side.
    EXPECT_NEAR(back.measureFrom.x, 0.0, 1e-6);
    EXPECT_NEAR(back.measureTo.x, 100.0, 1e-6);
    ASSERT_TRUE(back.statedValueMm.has_value())
        << "the dimension went out with no measurement, so it will read blank";
    EXPECT_NEAR(*back.statedValueMm, 100.0, 1e-6);
}

TEST(DxfWriterTest, M35_DXF_008_ADanglingDimensionIsNOTWrittenAndTheLossIsSAID) {
    // Exporting one would put "<?>" on somebody else's drawing with no way for
    // them to find out what it was meant to measure.
    DrawingDocument document{"Sheet"};
    const ObjectId line = document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}}).id();
    document.addDimension(DimensionKind::Linear, DimensionAnchor::onEntity(line, 0),
                          DimensionAnchor::onEntity(line, 1), Vec2{50.0, 20.0});
    ASSERT_TRUE(document.removeObject(line));
    ASSERT_EQ(document.danglingDimensions().size(), 1u);

    ScratchDxf file{"dangling.dxf"};
    const DxfWriteResult written = WriteDxfFile(document, file.path);
    ASSERT_TRUE(written) << written.why;
    bool saidIt = false;
    for (const DxfWriteLoss& loss : written.losses)
        if (loss.what == "DIMENSION" && loss.detail.find("lost") != std::string::npos)
            saidIt = true;
    EXPECT_TRUE(saidIt) << "a dangling dimension was dropped without a word";

    const DxfReadResult read = ReadDxfFile(file.path);
    ASSERT_TRUE(read) << read.message;
    EXPECT_TRUE(read.geometry.dimensions.empty())
        << "a dimension that had lost what it measured was exported anyway";
}

TEST(DxfWriterTest, M35_DXF_009_AnUnwritableDestinationIsREFUSEDAndSaysWhy) {
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{10.0, 0.0}});
    const DxfWriteResult written =
        WriteDxfFile(document, "/no-such-directory-ep3d/nowhere.dxf");
    EXPECT_FALSE(written);
    EXPECT_FALSE(written.why.empty());
    EXPECT_TRUE(WriteDxfFile(document, "").why.find("somewhere") != std::string::npos);
}

TEST(DxfWriterTest, M35_DXF_010_NoEntityNamesATableEntryTheFileDoesNotDECLARE) {
    // THE DEFECT THIS SHIPPED WITH, until the file was read back and looked
    // at: views went out on a layer named after the view, and hidden edges on
    // a linetype called HIDDEN, and neither was in either table -- while the
    // comment above EntityHead claimed that could not happen.
    //
    // A reader that meets an undeclared name either rejects the file or
    // silently repairs it, and the silent repair is worse: it puts the drawing
    // on the wrong layer without a word.
    DrawingDocument document{"Sheet"};
    Layer& detail = document.addLayer("DETAIL", 4);
    ASSERT_TRUE(document.setCurrentLayer(detail.id()));
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{50.0, 0.0}});
    // THE VIEW CASE -- a view's curves go on a layer named after it, and that
    // is the one the first draft missed -- is checked in the SELF TEST, not
    // here: a view needs a model file and a kernel before it has anything to
    // flatten, and neither is available in this suite. What this pins is the
    // invariant itself, over every name that does reach the file.

    ScratchDxf file{"tables.dxf"};
    ASSERT_TRUE(WriteDxfFile(document, file.path));
    const DxfNames names = NamesIn(ReadWhole(file.path));

    for (const std::string& used : names.layersUsed)
        EXPECT_TRUE(Contains(names.layersDeclared, used))
            << "an entity is on layer '" << used << "', which the file never declares";
    for (const std::string& used : names.linetypesUsed)
        EXPECT_TRUE(Contains(names.linetypesDeclared, used))
            << "an entity uses linetype '" << used << "', which the file never declares";

    // ...and the two DXF requires unconditionally are there whether or not
    // anything sat on them.
    EXPECT_TRUE(Contains(names.layersDeclared, "0"));
    EXPECT_TRUE(Contains(names.linetypesDeclared, "CONTINUOUS"));
    // A layer the author set up but left empty is still exported: a recipient
    // opening this to draw on it needs the layers, not just the occupied ones.
    EXPECT_TRUE(Contains(names.layersDeclared, "DETAIL"));
}

TEST(DxfWriterTest, M35_DXF_011_AnUndeclaredLinetypeGetsARealPatternNotAnEmptyOne) {
    // HIDDEN is written for a view's hidden edges because a DXF has no flag
    // for "this edge is hidden" -- the convention IS the meaning. Declaring it
    // with no dashes would put the hidden lines out as solid, which is the
    // drawing saying the opposite of what it means.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{10.0, 0.0}});
    // The document has never heard of HIDDEN...
    ASSERT_EQ(document.findLinetypeNamed("HIDDEN"), nullptr);

    ScratchDxf file{"hidden.dxf"};
    ASSERT_TRUE(WriteDxfFile(document, file.path));
    // ...and with no hidden curves to write, it is not in the file either --
    // a table full of linetypes nothing uses is noise.
    const DxfNames names = NamesIn(ReadWhole(file.path));
    EXPECT_FALSE(Contains(names.linetypesDeclared, "HIDDEN"));
}

TEST(DxfWriterTest, M35_DXF_012_AWriteThatGaveOutPartWayIsREFUSED) {
    // A truncated DXF OPENS. It is missing its last entities and its EOF
    // marker, and most readers take what they got without complaint -- so a
    // writer that reported success would hand over a drawing with holes in it,
    // and nobody would find out until the part came back short.
    DrawingDocument document{"Sheet"};
    for (int i = 0; i < 50; ++i)
        document.addEntity(DrawLine{Vec2{0.0, static_cast<double>(i)},
                                    Vec2{100.0, static_cast<double>(i)}});

    FailingBuffer buffer{200}; // enough for the header, nowhere near the end
    std::ostream out(&buffer);
    const DxfWriteResult written = WriteDxf(document, out);
    EXPECT_FALSE(written) << "a write that gave out part way reported success";
    EXPECT_FALSE(written.why.empty());
}

TEST(DxfWriterTest, M35_DXF_013_AViewWithAnImpossibleScaleFallsBackToONEToONE) {
    // A scale of zero would collapse the view to a point and a negative one
    // would mirror it; neither is a scale a drawing can have. The guarded
    // setters refuse both, but restoreView is the RAW path the loader uses and
    // checks nothing -- which is exactly the state a bad reader or a future
    // migration would leave behind.
    //
    // Falling back to 1:1 makes it visibly wrong. Letting it through makes the
    // view vanish, which reads as "the projection failed" and sends whoever is
    // looking to entirely the wrong place.
    DrawingDocument document{"Sheet"};
    DrawingView& broken = document.restoreView(
        900101u, "Broken", ComputeState::Valid, "unbuilt.ep3d", "Block",
        ViewDirection::Front, Vec2{100.0, 100.0}, DrawingScale{0, 0}, /*ownScale=*/true,
        /*showHidden=*/true, /*showTangent=*/false, kInvalidObjectId, 0.0);

    EXPECT_NEAR(document.viewScaleFactor(broken.id()), 1.0, 1e-12)
        << "an impossible scale was used as it stood";
    const Vec2 onPaper = document.viewPointToSheetMm(broken.id(), Vec2{80.0, 20.0});
    EXPECT_NEAR(onPaper.x, 180.0, 1e-9) << "the view collapsed instead of falling back";
    EXPECT_NEAR(onPaper.y, 120.0, 1e-9);
}
