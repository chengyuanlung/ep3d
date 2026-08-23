// Schema v13 (M17): the two legs of a point-to-point distance.
//
// HorizontalDistance and VerticalDistance are SIGNED (see
// HorizontalDistanceConstraint), which makes the ORDER of their two references
// part of what the constraint means rather than an implementation detail. That
// is the thing this file is really about: a round trip that swapped a and b
// would come back measuring the same magnitude with the opposite sign, and
// every downstream check -- the value, the drawing, even the solve -- would
// still look right until the geometry moved.

#include "Core/Document/PartDocument.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Body/Body.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Kernel/EdgeQuery.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Kernel/FaceQuery.h"
#include "Core/Dependency/DependencyGraph.h"
#include <algorithm>
#include <vector>
#include "Core/Feature/ShellFeature.h"
#include "Core/Feature/DraftFeature.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Feature/SweepFeature.h"
#include "Core/Feature/LoftFeature.h"
#include "Core/Sketch/Sketch.h"

#include "Support/SchemaVersionText.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>
#include <variant>

namespace {

using namespace paramcad;

std::string SaveToString(const PartDocument& document) {
    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_TRUE(saved) << saved.message;
    return out.str();
}

LoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadPartDocument(in);
}

// Two points and a dimension on each axis between them.
struct AxisDoc {
    PartDocument document{"V13Doc"};
    ObjectId sketchId = kInvalidObjectId;
    SketchEntityId a{kInvalidSketchEntityId};
    SketchEntityId b{kInvalidSketchEntityId};
    SketchConstraintId horizontal{kInvalidSketchConstraintId};
    SketchConstraintId vertical{kInvalidSketchConstraintId};

    AxisDoc() {
        Parameter& dx = document.addParameter("Dx", 80.0, UnitType::Millimeter);
        Parameter& dy = document.addParameter("Dy", 40.0, UnitType::Millimeter);
        Sketch& sketch = document.addSketch("Sketch001");
        sketchId = sketch.id();
        a = sketch.addPoint(Vec2{10.0, 20.0});
        b = sketch.addPoint(Vec2{90.0, 60.0});

        horizontal = document.addSketchConstraint(
            sketchId, HorizontalDistanceConstraint{SketchElementRef{a, SketchSubElement::Whole},
                                                   SketchElementRef{b, SketchSubElement::Whole},
                                                   dx.id()});
        vertical = document.addSketchConstraint(
            sketchId, VerticalDistanceConstraint{SketchElementRef{a, SketchSubElement::Whole},
                                                 SketchElementRef{b, SketchSubElement::Whole},
                                                 dy.id()});
    }
};

const Sketch& OnlySketch(const PartDocument& document) {
    return *document.sketches().front();
}

} // namespace

TEST(SerializationV13Test, M17_SER_001_TheSchemaVersionIsStamped) {
    AxisDoc source;
    // THE ONE LITERAL. Every other suite asks CurrentSchemaVersion() instead,
    // so a bump lands here and nowhere else -- and it has to land somewhere, or
    // a format change that forgot to bump would write files an older loader
    // silently mis-reads.
    EXPECT_NE(SaveToString(source.document).find("\"schemaVersion\": 26"), std::string::npos);
}

TEST(SerializationV13Test, M17_SER_002_BothKindsAreWrittenUnderTheirOwnNames) {
    AxisDoc source;
    const std::string saved = SaveToString(source.document);
    // By NAME, because the loader dispatches on it. A kind written under a name
    // the loader does not know is not a forward-compatibility problem -- it is
    // a constraint that vanishes.
    EXPECT_NE(saved.find("\"HorizontalDistance\""), std::string::npos);
    EXPECT_NE(saved.find("\"VerticalDistance\""), std::string::npos);
}

TEST(SerializationV13Test, M17_SER_003_BothSurviveARoundTripWithTheirParameters) {
    AxisDoc source;
    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& sketch = OnlySketch(*loaded.document);

    const SketchConstraint* dx = sketch.findConstraint(source.horizontal);
    ASSERT_NE(dx, nullptr);
    ASSERT_TRUE(std::holds_alternative<HorizontalDistanceConstraint>(dx->data));
    const SketchConstraint* dy = sketch.findConstraint(source.vertical);
    ASSERT_NE(dy, nullptr);
    ASSERT_TRUE(std::holds_alternative<VerticalDistanceConstraint>(dy->data));

    const Parameter* dxParameter = loaded.document->parameters().findById(
        std::get<HorizontalDistanceConstraint>(dx->data).parameterId);
    ASSERT_NE(dxParameter, nullptr);
    EXPECT_NEAR(dxParameter->value(), 80.0, 1e-9);
    const Parameter* dyParameter = loaded.document->parameters().findById(
        std::get<VerticalDistanceConstraint>(dy->data).parameterId);
    ASSERT_NE(dyParameter, nullptr);
    EXPECT_NEAR(dyParameter->value(), 40.0, 1e-9);
}

TEST(SerializationV13Test, M17_SER_004_TheORDEROfTheTwoReferencesSurvives) {
    AxisDoc source;
    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& sketch = OnlySketch(*loaded.document);

    const SketchConstraint* dx = sketch.findConstraint(source.horizontal);
    ASSERT_NE(dx, nullptr);
    const HorizontalDistanceConstraint& restored =
        std::get<HorizontalDistanceConstraint>(dx->data);
    // a is the LEFT point and b the right one, because the value is b.x - a.x.
    // A loader that normalised the pair, or wrote them in whichever order was
    // convenient, would restore a constraint measuring -80.
    EXPECT_EQ(restored.a.entityId, source.a);
    EXPECT_EQ(restored.b.entityId, source.b);
}

TEST(SerializationV13Test, M17_SER_005_AV12FileWithoutTheNewKindsStillLoads) {
    AxisDoc source;
    std::string saved = SaveToString(source.document);

    // Claim v12 -- a file written before these two kinds existed would simply
    // not contain them, and must keep loading. The version number is the only
    // thing changed here; the constraints stay, because a v12 loader refusing
    // its own future is a different bug from a v13 loader refusing its past.
    const std::size_t versionPos = saved.find(paramcad::testing::CurrentSchemaVersionField());
    ASSERT_NE(versionPos, std::string::npos);
    saved = paramcad::testing::WithSchemaVersion(saved, 12);

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(OnlySketch(*loaded.document).constraints().size(), 2u);
}

// --- v14: construction geometry ---------------------------------------------

TEST(SerializationV13Test, M17_SER_006_ConstructionSurvivesARoundTripAndOnlyWritesWhenTrue) {
    PartDocument document{"V14Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId normal = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId centreline = sketch.addLine(Vec2{50, -10}, Vec2{50, 110});
    ASSERT_EQ(document.setSketchEntitiesConstruction(sketch.id(), {centreline}, true), 1u);

    const std::string saved = SaveToString(document);
    // ONE occurrence: the flag is written only when true, so a sketch with no
    // construction geometry is byte-identical to what v13 wrote.
    std::size_t occurrences = 0;
    for (std::size_t at = saved.find("\"construction\""); at != std::string::npos;
         at = saved.find("\"construction\"", at + 1))
        ++occurrences;
    EXPECT_EQ(occurrences, 1u);

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& after = OnlySketch(*loaded.document);
    EXPECT_TRUE(after.isConstruction(centreline));
    EXPECT_FALSE(after.isConstruction(normal));
}

TEST(SerializationV13Test, M17_SER_007_AV13FileWithNoConstructionFieldLoadsAsNormalGeometry) {
    PartDocument document{"V14Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});

    std::string saved = SaveToString(document);
    ASSERT_EQ(saved.find("\"construction\""), std::string::npos);
    const std::size_t versionPos = saved.find(paramcad::testing::CurrentSchemaVersionField());
    ASSERT_NE(versionPos, std::string::npos);
    saved = paramcad::testing::WithSchemaVersion(saved, 13);

    // Absent means normal. There is no third state for the loader to invent.
    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_FALSE(OnlySketch(*loaded.document).isConstruction(line));
}

TEST(SerializationV13Test, M17_SER_008_AConstructionFieldOfTheWrongTypeIsREFUSED) {
    PartDocument document{"V14Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    ASSERT_EQ(document.setSketchEntitiesConstruction(sketch.id(), {line}, true), 1u);

    std::string saved = SaveToString(document);
    const std::size_t at = saved.find("\"construction\": true");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, 21, "\"construction\": \"yes\"");

    // Reading a string as false would turn a corrupt file into a quietly
    // different model -- a solid with an edge the author did not draw.
    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
}

// --- v17: the projected reference underlay -----------------------------------

TEST(SerializationV13Test, M17_SER_009_ReferencesSurviveARoundTripAndStayOutOfTheEntities) {
    PartDocument document{"V17Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId drawn = sketch.addLine(Vec2{0, 0}, Vec2{10, 0});
    const SketchReferenceId line = sketch.addReference(SketchLine{Vec2{0, 20}, Vec2{40, 20}});
    const SketchReferenceId hole = sketch.addReference(SketchCircle{Vec2{20, 15}, 5.0});
    const SketchReferenceId corner = sketch.addReference(SketchPoint{Vec2{0, 20}});
    ASSERT_NE(line, kInvalidSketchReferenceId);

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& after = OnlySketch(*loaded.document);

    // THE separation, checked after a round trip rather than assumed: an
    // underlay that came back as entities would be padded into the solid.
    EXPECT_EQ(after.entities().size(), 1u);
    EXPECT_NE(after.findEntity(drawn), nullptr);
    ASSERT_EQ(after.references().size(), 3u);

    const SketchReference* restoredLine = after.findReference(line);
    ASSERT_NE(restoredLine, nullptr);
    const auto* geometry = std::get_if<SketchLine>(&restoredLine->geometry);
    ASSERT_NE(geometry, nullptr);
    EXPECT_NEAR(geometry->start.y, 20.0, 1e-9);
    EXPECT_NEAR(geometry->end.x, 40.0, 1e-9);

    const SketchReference* restoredHole = after.findReference(hole);
    ASSERT_NE(restoredHole, nullptr);
    EXPECT_NEAR(std::get<SketchCircle>(restoredHole->geometry).radiusMm, 5.0, 1e-9);
    EXPECT_NE(after.findReference(corner), nullptr);
}

TEST(SerializationV13Test, M17_SER_010_ASketchWithNoUnderlayWritesNoReferencesArray) {
    // A sketch that never touched a face must produce the bytes v16 produced.
    // An empty array written unconditionally is a diff in every file anybody
    // has ever saved, for a feature they did not use.
    PartDocument document{"V17Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{10, 0});
    EXPECT_EQ(SaveToString(document).find("\"references\""), std::string::npos);
}

TEST(SerializationV13Test, M17_SER_011_AV16FileWithNoReferencesArrayStillLoads) {
    PartDocument document{"V17Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{10, 0});

    std::string saved = SaveToString(document);
    const std::size_t versionPos = saved.find(paramcad::testing::CurrentSchemaVersionField());
    ASSERT_NE(versionPos, std::string::npos);
    saved = paramcad::testing::WithSchemaVersion(saved, 16);

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_TRUE(OnlySketch(*loaded.document).references().empty());
    EXPECT_EQ(OnlySketch(*loaded.document).entities().size(), 1u);
}

TEST(SerializationV13Test, M17_SER_012_ADuplicateReferenceIdIsREFUSED) {
    PartDocument document{"V17Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addReference(SketchLine{Vec2{0, 0}, Vec2{10, 0}});
    sketch.addReference(SketchLine{Vec2{0, 5}, Vec2{10, 5}});

    std::string saved = SaveToString(document);
    // Rewrite the SECOND reference's id to match the first. Two references
    // with one id is a file whose author's intent cannot be recovered, and
    // silently keeping one of them would drop geometry without saying so.
    const std::size_t arrayAt = saved.find("\"references\"");
    ASSERT_NE(arrayAt, std::string::npos);
    const std::size_t firstId = saved.find("\"id\"", arrayAt);
    ASSERT_NE(firstId, std::string::npos);
    const std::size_t firstQuote = saved.find('"', saved.find(':', firstId)) + 1;
    const std::size_t firstEnd = saved.find('"', firstQuote);
    const std::string id = saved.substr(firstQuote, firstEnd - firstQuote);

    const std::size_t secondId = saved.find("\"id\"", firstEnd);
    ASSERT_NE(secondId, std::string::npos);
    const std::size_t secondQuote = saved.find('"', saved.find(':', secondId)) + 1;
    const std::size_t secondEnd = saved.find('"', secondQuote);
    saved.replace(secondQuote, secondEnd - secondQuote, id);

    EXPECT_FALSE(LoadFromString(saved));
}

// --- v18: which edges a fillet dresses ---------------------------------------

TEST(SerializationV13Test, M17_SER_013_AnEdgeSelectionSurvivesARoundTrip) {
    PartDocument document{"V18Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Parameter& radius = document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    sketch.addLine(Vec2{40, 0}, Vec2{40, 30});
    sketch.addLine(Vec2{40, 30}, Vec2{0, 30});
    sketch.addLine(Vec2{0, 30}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    FilletFeature& fillet =
        document.addFilletFeature(body, "Fillet001", pad.id(), radius.id());
    document.setFeatureEdgeSelection(fillet.id(), 
        EdgeSelection{EdgesOfExtremeFace{Vec3{0, 0, 1}}, EdgesParallelTo{Vec3{0, 0, 1}}});

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;

    const FilletFeature* restored = nullptr;
    for (const auto& feature : loaded.document->bodies().front()->features())
        if (const auto* f = dynamic_cast<const FilletFeature*>(feature.get())) restored = f;
    ASSERT_NE(restored, nullptr);

    // BOTH queries, in order, with their directions intact. A round trip that
    // kept only the first, or normalised the directions, would come back
    // dressing a different set of edges on a solid that still looks plausible.
    ASSERT_EQ(restored->edgeSelection().size(), 2u);
    ASSERT_TRUE(std::holds_alternative<EdgesOfExtremeFace>(restored->edgeSelection()[0]));
    EXPECT_NEAR(std::get<EdgesOfExtremeFace>(restored->edgeSelection()[0]).direction.z, 1.0, 1e-9);
    ASSERT_TRUE(std::holds_alternative<EdgesParallelTo>(restored->edgeSelection()[1]));
    EXPECT_EQ(DescribeEdgeSelection(restored->edgeSelection()),
              "the top face's edges, plus every vertical edge");
}

TEST(SerializationV13Test, M17_SER_014_ADefaultSelectionWritesNothingAndLoadsAsEveryEdge) {
    // Every fillet anybody already has says nothing about edges, so a file with
    // a default selection must be byte-identical to what v17 wrote.
    PartDocument document{"V18Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Parameter& radius = document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    sketch.addLine(Vec2{40, 0}, Vec2{40, 30});
    sketch.addLine(Vec2{40, 30}, Vec2{0, 30});
    sketch.addLine(Vec2{0, 30}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    document.addFilletFeature(body, "Fillet001", pad.id(), radius.id());

    std::string saved = SaveToString(document);
    EXPECT_EQ(saved.find("\"edgeSelection\""), std::string::npos);

    // ...and a v17 file loads with every edge, which is what it meant.
    const std::size_t versionPos = saved.find(paramcad::testing::CurrentSchemaVersionField());
    ASSERT_NE(versionPos, std::string::npos);
    saved = paramcad::testing::WithSchemaVersion(saved, 17);
    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    for (const auto& feature : loaded.document->bodies().front()->features())
        if (const auto* f = dynamic_cast<const FilletFeature*>(feature.get()))
            EXPECT_TRUE(IsAllEdges(f->edgeSelection()));
}

TEST(SerializationV13Test, M17_SER_015_AnEMPTYSelectionIsREFUSEDNotReadAsEverything) {
    // "Nothing" and "everything" are opposite solids. A loader that treated an
    // empty array as the default would turn a corrupt file into a part the
    // author never drew, silently.
    PartDocument document{"V18Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Parameter& radius = document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    sketch.addLine(Vec2{40, 0}, Vec2{40, 30});
    sketch.addLine(Vec2{40, 30}, Vec2{0, 30});
    sketch.addLine(Vec2{0, 30}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    FilletFeature& fillet = document.addFilletFeature(body, "Fillet001", pad.id(), radius.id());
    document.setFeatureEdgeSelection(fillet.id(), EdgeSelection{EdgesParallelTo{Vec3{0, 0, 1}}});

    std::string saved = SaveToString(document);
    const std::size_t at = saved.find("\"edgeSelection\"");
    ASSERT_NE(at, std::string::npos);
    const std::size_t open = saved.find('[', at);
    const std::size_t close = saved.find(']', open);
    saved.replace(open, close - open + 1, "[]");

    EXPECT_FALSE(LoadFromString(saved));
}

TEST(SerializationV13Test, M17_SER_016_AnUnknownQueryTypeIsREFUSED) {
    // A query written under a name this loader does not know is not a
    // forward-compatibility problem: it is a fillet that silently reverts to
    // every edge, on a part the author shaped deliberately.
    PartDocument document{"V18Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Parameter& radius = document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    sketch.addLine(Vec2{40, 0}, Vec2{40, 30});
    sketch.addLine(Vec2{40, 30}, Vec2{0, 30});
    sketch.addLine(Vec2{0, 30}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    FilletFeature& fillet = document.addFilletFeature(body, "Fillet001", pad.id(), radius.id());
    document.setFeatureEdgeSelection(fillet.id(), EdgeSelection{EdgesParallelTo{Vec3{0, 0, 1}}});

    std::string saved = SaveToString(document);
    const std::size_t at = saved.find("\"EdgesParallelTo\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, 18, "\"EdgesOfTomorrow\"");

    EXPECT_FALSE(LoadFromString(saved));
}

TEST(SerializationV13Test, M17_SER_017_ACreatedByQuerySurvivesWithTheFeatureItNames) {
    // The query kind that holds an ID rather than a direction. It was added
    // after the writer's if/else chain was written, and the chain's bare
    // `else` reached for the wrong alternative and threw on SAVE -- a document
    // that could be built and not written down.
    PartDocument document{"V18Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Parameter& radius = document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    sketch.addLine(Vec2{40, 0}, Vec2{40, 30});
    sketch.addLine(Vec2{40, 30}, Vec2{0, 30});
    sketch.addLine(Vec2{0, 30}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    FilletFeature& fillet = document.addFilletFeature(body, "Fillet001", pad.id(), radius.id());
    ASSERT_TRUE(document.setFeatureEdgeSelection(fillet.id(),
                                                 EdgeSelection{EdgesCreatedBy{pad.id()}}));

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;

    const FilletFeature* restored = nullptr;
    for (const auto& feature : loaded.document->bodies().front()->features())
        if (const auto* f = dynamic_cast<const FilletFeature*>(feature.get())) restored = f;
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->edgeSelection().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<EdgesCreatedBy>(restored->edgeSelection().front()));
    // The ID it names, not merely "some CreatedBy". A round trip that dropped
    // the id would come back naming feature 0 and dressing nothing, on a
    // fillet that still looks like the author meant it.
    EXPECT_EQ(std::get<EdgesCreatedBy>(restored->edgeSelection().front()).featureId, pad.id());
}

// --- v19: a sketch that follows a face ---------------------------------------

TEST(SerializationV13Test, M17_SER_018_ATrackedFaceSurvivesAndKeepsItsGraphEdge) {
    PartDocument document{"V19Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& base = document.addSketch("Sketch001");
    base.addLine(Vec2{0, 0}, Vec2{40, 0});
    base.addLine(Vec2{40, 0}, Vec2{40, 30});
    base.addLine(Vec2{40, 30}, Vec2{0, 30});
    base.addLine(Vec2{0, 30}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", base.id(), length.id());

    Sketch& tracked = document.addSketch("Sketch002");
    FaceQuery query;
    query.createdBy = pad.id();
    query.extremeTowards = Vec3{0, 0, 1};
    ASSERT_TRUE(document.setSketchTrackedFace(tracked.id(), query));

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;

    const Sketch* restored = nullptr;
    for (const Sketch* sketch : loaded.document->sketches())
        if (sketch->name() == "Sketch002") restored = sketch;
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(restored->trackedFace().has_value());
    EXPECT_EQ(*restored->trackedFace()->createdBy, pad.id());
    ASSERT_TRUE(restored->trackedFace()->extremeTowards.has_value());
    EXPECT_NEAR(restored->trackedFace()->extremeTowards->z, 1.0, 1e-9);

    // THE GRAPH EDGE, not just the query. A restored sketch holding the query
    // with no dependency on the pad would be clean when the pad moved, and
    // would report the plane from before the move -- which is the whole defect
    // tracking exists to remove, reintroduced by the loader.
    const std::vector<ObjectId> prerequisites =
        loaded.document->dependencyGraph().prerequisitesOf(restored->id());
    EXPECT_NE(std::find(prerequisites.begin(), prerequisites.end(), pad.id()),
              prerequisites.end())
        << "the restored sketch does not depend on the feature whose face it follows";
}

TEST(SerializationV13Test, M17_SER_019_AnUntrackedSketchWritesNoTrackedFace) {
    PartDocument document{"V19Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{10, 0});
    EXPECT_EQ(SaveToString(document).find("\"trackedFace\""), std::string::npos);
}

TEST(SerializationV13Test, M17_SER_020_ATrackedFaceNamingNOTHINGIsREFUSED) {
    // An empty query matches every face, so it names none. A sketch holding
    // one would fail on every recompute from now on with nothing to fix.
    PartDocument document{"V19Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& base = document.addSketch("Sketch001");
    base.addLine(Vec2{0, 0}, Vec2{40, 0});
    base.addLine(Vec2{40, 0}, Vec2{40, 30});
    base.addLine(Vec2{40, 30}, Vec2{0, 30});
    base.addLine(Vec2{0, 30}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", base.id(), length.id());
    Sketch& tracked = document.addSketch("Sketch002");
    FaceQuery query;
    query.createdBy = pad.id();
    query.extremeTowards = Vec3{0, 0, 1};
    ASSERT_TRUE(document.setSketchTrackedFace(tracked.id(), query));

    std::string saved = SaveToString(document);
    const std::size_t at = saved.find("\"trackedFace\"");
    ASSERT_NE(at, std::string::npos);
    const std::size_t open = saved.find('{', at);
    const std::size_t close = saved.find('}', open);
    saved.replace(open, close - open + 1, "{}");

    EXPECT_FALSE(LoadFromString(saved));
}

// --- v21: WHERE a tangency holds --------------------------------------------

namespace {

// A line whose end is pinned on a circle, with the tangency saying so.
struct TouchDoc {
    PartDocument document{"V21Doc"};
    ObjectId sketchId = kInvalidObjectId;
    SketchConstraintId tangent{kInvalidSketchConstraintId};

    TouchDoc() {
        Sketch& sketch = document.addSketch("Sketch001");
        sketchId = sketch.id();
        const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 10.0);
        const SketchEntityId line = sketch.addLine(Vec2{30, 20}, Vec2{7, 7});
        document.addSketchConstraint(
            sketchId,
            PointOnObjectConstraint{SketchElementRef{line, SketchSubElement::EndPoint}, circle});
        tangent = document.addSketchConstraint(
            sketchId, TangentConstraint{line, circle, false, SketchSubElement::EndPoint});
    }
};

const TangentConstraint* TangentIn(const PartDocument& document) {
    for (const Sketch* sketch : document.sketches())
        for (const SketchConstraint& constraint : sketch->constraints())
            if (const auto* value = std::get_if<TangentConstraint>(&constraint.data)) return value;
    return nullptr;
}

} // namespace

TEST(SerializationV13Test, M17_SER_021_WhereATangencyHoldsSurvivesTheRoundTrip) {
    TouchDoc source;
    ASSERT_NE(source.tangent, kInvalidSketchConstraintId);

    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const TangentConstraint* restored = TangentIn(*loaded.document);
    ASSERT_NE(restored, nullptr);
    // NOT Whole. A tangency that forgot where it holds still loads, still
    // solves, and holds nothing -- the DOF would go up by one per corner and
    // the shape would kink on the next edit, with no error anywhere.
    EXPECT_EQ(restored->at, SketchSubElement::EndPoint);
}

TEST(SerializationV13Test, M17_SER_022_AnUnpinnedTangencyWritesNOAtField) {
    // Every file written before v21 meant a line free to slide, which is what
    // Whole says -- so omitting it keeps those documents byte-identical rather
    // than restamping them with something they never said.
    PartDocument document{"V21Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 10.0);
    const SketchEntityId line = sketch.addLine(Vec2{-40, 25}, Vec2{40, 25});
    ASSERT_NE(document.addSketchConstraint(sketch.id(), TangentConstraint{line, circle, false}),
              kInvalidSketchConstraintId);

    const std::string saved = SaveToString(document);
    EXPECT_NE(saved.find("\"Tangent\""), std::string::npos);
    EXPECT_EQ(saved.find("\"at\""), std::string::npos);

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    const TangentConstraint* restored = TangentIn(*loaded.document);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->at, SketchSubElement::Whole);
}

TEST(SerializationV13Test, M17_SER_023_AV20TangencyWithNoAtFieldStillLoads) {
    // The back-compatibility direction: a file this build wrote, relabelled as
    // the version before the field existed, must still load -- and as a sliding
    // tangency, which is the only thing v20 could have meant.
    PartDocument document{"V21Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 10.0);
    const SketchEntityId line = sketch.addLine(Vec2{-40, 25}, Vec2{40, 25});
    ASSERT_NE(document.addSketchConstraint(sketch.id(), TangentConstraint{line, circle, false}),
              kInvalidSketchConstraintId);

    const std::string saved =
        paramcad::testing::WithSchemaVersion(SaveToString(document), 20);
    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    const TangentConstraint* restored = TangentIn(*loaded.document);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->at, SketchSubElement::Whole);
}

TEST(SerializationV13Test, M17_SER_024_AnUnREADABLEAtFieldIsRefusedNotDefaulted) {
    // Present-but-broken is not the same as absent. Defaulting it would turn a
    // corrupt file into a valid document whose tangencies hold nothing -- the
    // failure this whole milestone exists to make impossible, arriving through
    // the loader instead.
    TouchDoc source;
    std::string saved = SaveToString(source.document);
    const std::size_t at = saved.find("\"at\": \"EndPoint\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, std::string("\"at\": \"EndPoint\"").size(), "\"at\": \"Middle\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::InvalidEnumValue);
}

// --- v22: ellipses ----------------------------------------------------------

namespace {

struct EllipseDoc {
    PartDocument document{"V22Doc"};
    ObjectId sketchId = kInvalidObjectId;
    SketchEntityId full{kInvalidSketchEntityId};
    SketchEntityId piece{kInvalidSketchEntityId};

    EllipseDoc() {
        Sketch& sketch = document.addSketch("Sketch001");
        sketchId = sketch.id();
        full = sketch.addEllipse(Vec2{3.5, -2.25}, 40.0, 15.0, 0.7);
        piece = sketch.addEllipticalArc(Vec2{-8.0, 6.0}, 20.0, 9.0, -0.4, 0.25, 2.75, false);
        Parameter& a = document.addParameter("A", 40.0, UnitType::Millimeter);
        Parameter& b = document.addParameter("B", 15.0, UnitType::Millimeter);
        Parameter& t = document.addParameter("T", 0.7, UnitType::Radian);
        document.addSketchConstraint(sketchId, EllipseAxisConstraint{full, a.id(), false});
        document.addSketchConstraint(sketchId, EllipseAxisConstraint{full, b.id(), true});
        document.addSketchConstraint(sketchId, EllipseRotationConstraint{full, t.id()});
    }
};

} // namespace

TEST(SerializationV13Test, M17_SER_030_AnEllipseSurvivesTheRoundTripExactly) {
    EllipseDoc source;
    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->sketches().size(), 1u);
    const Sketch& sketch = *loaded.document->sketches().front();

    const SketchEllipse* full = nullptr;
    const SketchEllipticalArc* piece = nullptr;
    for (const SketchEntity& entity : sketch.entities()) {
        if (const auto* e = std::get_if<SketchEllipse>(&entity.geometry)) full = e;
        if (const auto* a = std::get_if<SketchEllipticalArc>(&entity.geometry)) piece = a;
    }
    ASSERT_NE(full, nullptr);
    ASSERT_NE(piece, nullptr);
    EXPECT_DOUBLE_EQ(full->center.x, 3.5);
    EXPECT_DOUBLE_EQ(full->center.y, -2.25);
    EXPECT_DOUBLE_EQ(full->majorRadiusMm, 40.0);
    EXPECT_DOUBLE_EQ(full->minorRadiusMm, 15.0);
    EXPECT_DOUBLE_EQ(full->rotationRad, 0.7);
    // THE ARC'S DIRECTION TOO. It was written counter-clockwise=false on
    // purpose: a round trip that defaulted it would come back as the
    // complementary arc -- the same two ends, the other piece of curve.
    EXPECT_DOUBLE_EQ(piece->startParamRad, 0.25);
    EXPECT_DOUBLE_EQ(piece->endParamRad, 2.75);
    EXPECT_FALSE(piece->counterClockwise);
    EXPECT_DOUBLE_EQ(piece->rotationRad, -0.4);
}

TEST(SerializationV13Test, M17_SER_031_ThePARAMETERSAreNotWrittenAsAngles) {
    // The key names carry the warning. Calling them "startAngleRad" like a
    // circular arc's would invite the next reader to feed one to atan2, which
    // is right at the axes and wrong everywhere between them.
    EllipseDoc source;
    const std::string saved = SaveToString(source.document);
    EXPECT_NE(saved.find("\"startParamRad\""), std::string::npos);
    EXPECT_NE(saved.find("\"EllipticalArc\""), std::string::npos);
    EXPECT_NE(saved.find("\"majorRadiusMm\""), std::string::npos);
}

TEST(SerializationV13Test, M17_SER_032_TheAxisAndAngleDimensionsSurvive) {
    EllipseDoc source;
    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& sketch = *loaded.document->sketches().front();

    int major = 0, minor = 0, rotation = 0;
    for (const SketchConstraint& constraint : sketch.constraints()) {
        if (const auto* axis = std::get_if<EllipseAxisConstraint>(&constraint.data))
            (axis->minor ? minor : major)++;
        if (std::holds_alternative<EllipseRotationConstraint>(constraint.data)) ++rotation;
    }
    // WHICH AXIS is part of what the constraint means. A round trip that lost
    // `minor` would come back as two dimensions on the same axis: consistent,
    // solvable, and a different shape.
    EXPECT_EQ(major, 1);
    EXPECT_EQ(minor, 1);
    EXPECT_EQ(rotation, 1);
}

TEST(SerializationV13Test, M17_SER_033_AnAxisDimensionWithNoMinorFieldIsREFUSED) {
    EllipseDoc source;
    std::string saved = SaveToString(source.document);
    const std::size_t at = saved.find("\"minor\": true");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, std::string("\"minor\": true").size(), "\"other\": true");
    EXPECT_FALSE(LoadFromString(saved));
}

// --- v23: splines -----------------------------------------------------------

TEST(SerializationV13Test, M17_SER_040_ASplineSurvivesTheRoundTripPointForPoint) {
    PartDocument document{"V23Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const std::vector<Vec2> points = {Vec2{0, 0}, Vec2{20.5, 30.25}, Vec2{50, -10},
                                      Vec2{80, 20.125}};
    ASSERT_NE(sketch.addSpline(points, false), kInvalidSketchEntityId);
    ASSERT_NE(sketch.addSpline({Vec2{0, 100}, Vec2{40, 130}, Vec2{80, 100}}, true),
              kInvalidSketchEntityId);

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& back = *loaded.document->sketches().front();

    const SketchSpline* open = nullptr;
    const SketchSpline* closed = nullptr;
    for (const SketchEntity& entity : back.entities()) {
        const auto* spline = std::get_if<SketchSpline>(&entity.geometry);
        if (spline == nullptr) continue;
        (spline->closed ? closed : open) = spline;
    }
    ASSERT_NE(open, nullptr);
    ASSERT_NE(closed, nullptr);
    ASSERT_EQ(open->points.size(), points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        EXPECT_DOUBLE_EQ(open->points[i].x, points[i].x);
        EXPECT_DOUBLE_EQ(open->points[i].y, points[i].y);
    }
    // ...IN ORDER. A round trip that sorted or reversed them would come back
    // as a different curve through the same set of positions.
    EXPECT_DOUBLE_EQ(open->points.front().x, 0.0);
    EXPECT_DOUBLE_EQ(open->points.back().x, 80.0);
    EXPECT_TRUE(closed->closed);
    EXPECT_EQ(closed->points.size(), 3u);
}

TEST(SerializationV13Test, M17_SER_041_ASplineWithNoClosedFieldIsREFUSED) {
    // Closed and open are different curves -- one is a loop on its own and the
    // other has ends a profile chains through. A file missing the flag would
    // load as the other shape rather than as a default.
    PartDocument document{"V23Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    ASSERT_NE(sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}}, true),
              kInvalidSketchEntityId);

    std::string saved = SaveToString(document);
    const std::size_t at = saved.find("\"closed\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, std::string("\"closed\"").size(), "\"clsed\"");
    EXPECT_FALSE(LoadFromString(saved));
}

TEST(SerializationV13Test, M17_SER_042_ASplinePointMissingACoordinateIsREFUSED) {
    PartDocument document{"V23Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    ASSERT_NE(sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}}, false),
              kInvalidSketchEntityId);

    std::string saved = SaveToString(document);
    // Break the SECOND point's v: a reader that only checked the first would
    // load a curve through a point at the origin and never say so.
    const std::size_t first = saved.find("\"points\"");
    ASSERT_NE(first, std::string::npos);
    const std::size_t second = saved.find("\"v\"", saved.find("\"v\"", first) + 1);
    ASSERT_NE(second, std::string::npos);
    saved.replace(second, 3, "\"w\"");
    EXPECT_FALSE(LoadFromString(saved));
}

TEST(SerializationV13Test, M17_SER_043_EVERYDimensionalKindComesBackBoundToItsParameter) {
    // The reader used to assign the bound Parameter inside each kind's own
    // branch -- eight copies of one statement, and no copy at all for the two
    // dimensions M17.25 added. Those saved perfectly and reloaded bound to
    // nothing, so a document that solved before it was written refused to solve
    // after it was read, naming a constraint id and no cause.
    //
    // Walked by CAPABILITY rather than by a list this test would also have to
    // remember to extend: every constraint that IsDimensional says reads a
    // Parameter must still have one after a round trip.
    PartDocument document{"BoundDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    const SketchEntityId other = sketch.addLine(Vec2{0, 10}, Vec2{40, 40});
    const SketchEntityId circle = sketch.addCircle(Vec2{80, 0}, 10.0);
    const SketchEntityId ellipse = sketch.addEllipse(Vec2{0, 60}, 40.0, 15.0, 0.4);
    const SketchEntityId point = sketch.addPoint(Vec2{5, 5});

    Parameter& mm = document.addParameter("L", 40.0, UnitType::Millimeter);
    Parameter& rad = document.addParameter("A", 0.4, UnitType::Radian);
    document.addSketchConstraint(sketch.id(), LengthConstraint{line, mm.id()});
    document.addSketchConstraint(sketch.id(), RadiusConstraint{circle, mm.id()});
    document.addSketchConstraint(sketch.id(), DiameterConstraint{circle, mm.id()});
    document.addSketchConstraint(sketch.id(), AngleConstraint{line, other, rad.id()});
    document.addSketchConstraint(
        sketch.id(),
        DistanceConstraint{SketchElementRef{point, SketchSubElement::Whole},
                           SketchElementRef{line, SketchSubElement::StartPoint}, mm.id()});
    document.addSketchConstraint(
        sketch.id(),
        HorizontalDistanceConstraint{SketchElementRef{point, SketchSubElement::Whole},
                                     SketchElementRef{line, SketchSubElement::StartPoint},
                                     mm.id()});
    document.addSketchConstraint(
        sketch.id(),
        VerticalDistanceConstraint{SketchElementRef{point, SketchSubElement::Whole},
                                   SketchElementRef{line, SketchSubElement::StartPoint},
                                   mm.id()});
    document.addSketchConstraint(
        sketch.id(),
        PointLineDistanceConstraint{SketchElementRef{point, SketchSubElement::Whole}, line,
                                    mm.id()});
    document.addSketchConstraint(sketch.id(), EllipseAxisConstraint{ellipse, mm.id(), false});
    document.addSketchConstraint(sketch.id(), EllipseAxisConstraint{ellipse, mm.id(), true});
    document.addSketchConstraint(sketch.id(), EllipseRotationConstraint{ellipse, rad.id()});

    int dimensional = 0;
    for (const SketchConstraint& constraint : sketch.constraints())
        if (IsDimensional(constraint.data)) ++dimensional;
    ASSERT_EQ(dimensional, 11) << "this test stopped covering every dimensional kind";

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& back = *loaded.document->sketches().front();
    int checked = 0;
    for (const SketchConstraint& constraint : back.constraints()) {
        if (!IsDimensional(constraint.data)) continue;
        ++checked;
        EXPECT_NE(BoundParameterId(constraint.data), kInvalidObjectId)
            << ConstraintKindName(constraint.data) << " came back bound to nothing";
    }
    EXPECT_EQ(checked, dimensional);
}


// --- M17.30: THE INDEX SURVIVES A ROUND TRIP ---------------------------------

TEST(SerializationV13Test, M17_SER_044_ASplinePointsINDEXComesBack) {
    // A sub-element name alone is not enough for a spline point: "SplinePoint"
    // without its number reads back as point 0, so a constraint the user put on
    // point 3 would come back holding point 0 -- the same shape on screen, a
    // different thing the moment they drag it.
    PartDocument document{"SplineDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline = sketch.addSpline(
        {Vec2{0, 0}, Vec2{20, 30}, Vec2{50, -10}, Vec2{80, 20}, Vec2{110, 0}}, false);
    Parameter& lift = document.addParameter("Y", 44.0, UnitType::Millimeter);
    document.addSketchConstraint(
        sketch.id(),
        VerticalDistanceConstraint{SketchElementRef{spline, SketchSubElement::StartPoint},
                                   SketchElementRef{spline, SketchSubElement::SplinePoint, 3},
                                   lift.id()});

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch* back = loaded.document->findSketch(loaded.document->sketches().front()->id());
    ASSERT_NE(back, nullptr);
    ASSERT_EQ(back->constraints().size(), 1u);
    const auto* dimension =
        std::get_if<VerticalDistanceConstraint>(&back->constraints().front().data);
    ASSERT_NE(dimension, nullptr);
    EXPECT_EQ(dimension->b.subElement, SketchSubElement::SplinePoint);
    EXPECT_EQ(dimension->b.index, 3);
}

TEST(SerializationV13Test, M17_SER_045_AnIndexThatIsNotAWholePointIsREFUSED) {
    // Refused, not rounded. A file claiming point 2.5 is a file this program
    // did not write, and quietly reading it as point 2 puts a constraint on a
    // point nobody chose.
    PartDocument document{"SplineDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{0, 0}, Vec2{20, 30}, Vec2{50, -10}}, false);
    const SketchEntityId point = sketch.addPoint(Vec2{5, 5});
    document.addSketchConstraint(
        sketch.id(),
        CoincidentConstraint{SketchElementRef{point, SketchSubElement::Whole},
                             SketchElementRef{spline, SketchSubElement::SplinePoint, 1}});

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"index\": 1");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"index\": 1").size(), "\"index\": 2.5");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("index"), std::string::npos) << loaded.message;
}

TEST(SerializationV13Test, M17_SER_046_NoIndexIsWrittenForAnOrdinarySubElement) {
    // Every file written before M17.30 is still byte-identical, because the
    // field only appears where it means something. A field written everywhere
    // would change every document on disk the first time it was resaved, which
    // makes a diff useless for telling what the user actually changed.
    PartDocument document{"LineDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    const SketchEntityId point = sketch.addPoint(Vec2{5, 5});
    document.addSketchConstraint(
        sketch.id(), CoincidentConstraint{SketchElementRef{point, SketchSubElement::Whole},
                                          SketchElementRef{line, SketchSubElement::StartPoint}});

    const std::string text = SaveToString(document);
    EXPECT_EQ(text.find("\"index\""), std::string::npos) << text;
}

// --- M18 (v24): SPLINE TANGENT HANDLES ---------------------------------------

TEST(SerializationV13Test, M18_SER_001_AHandleSurvivesARoundTrip) {
    // A spline with a handle and one without are DIFFERENT CURVES through the
    // same points. A round trip that dropped the handle would reload a
    // different shape while every point matched, which is the hardest kind of
    // wrong to notice.
    PartDocument document{"HandleDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    SketchSpline geometry{{Vec2{0, 0}, Vec2{40, 30}, Vec2{90, 10}}, false};
    geometry.handles[1] = Vec2{25.5, -12.25};
    const SketchEntityId spline = sketch.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(document.setSketchEntityGeometry(sketch.id(), spline, geometry));

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch* back = loaded.document->findSketch(loaded.document->sketches().front()->id());
    ASSERT_NE(back, nullptr);
    const auto& reloaded = std::get<SketchSpline>(back->entities().front().geometry);
    ASSERT_EQ(reloaded.handles.size(), 1u);
    ASSERT_NE(reloaded.handles.find(1), reloaded.handles.end());
    EXPECT_DOUBLE_EQ(reloaded.handles.at(1).x, 25.5);
    EXPECT_DOUBLE_EQ(reloaded.handles.at(1).y, -12.25);
}

TEST(SerializationV13Test, M18_SER_002_ASplineWithNoHandlesWritesNoHandlesField) {
    // Every spline written before v24 is still byte-identical, because the
    // field only appears where it means something -- and absent reads as "no
    // handles", which is exactly what those files meant.
    PartDocument document{"HandleDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{90, 10}}, false);

    const std::string text = SaveToString(document);
    EXPECT_EQ(text.find("\"handles\""), std::string::npos) << text;
}

TEST(SerializationV13Test, M18_SER_003_AHandleOnAPointThatDoesNotExistIsREFUSED) {
    // Refused, not skipped. A handle that quietly failed to load would give the
    // reader a different curve through the same points, which is the one thing
    // a handle exists to make different.
    PartDocument document{"HandleDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    SketchSpline geometry{{Vec2{0, 0}, Vec2{40, 30}, Vec2{90, 10}}, false};
    geometry.handles[1] = Vec2{25.0, 0.0};
    const SketchEntityId spline = sketch.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(document.setSketchEntityGeometry(sketch.id(), spline, geometry));

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"point\": 1");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"point\": 1").size(), "\"point\": 9");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("point"), std::string::npos) << loaded.message;
}

// --- M19 (v25): SWEEP and LOFT survive a round trip --------------------------

TEST(SerializationV13Test, M19_SER_001_ASweepKeepsBOTHOfItsSketches) {
    // A sweep is the first feature that consumes two sketches, and the one that
    // would be easy to persist half of: `sketchId` alone loads, builds and
    // fails at recompute time with a message about a missing path -- long after
    // the save that lost it.
    PartDocument document{"SweepDoc"};
    Sketch& section = document.addSketch("Section");
    section.addLine(Vec2{0, 0}, Vec2{20, 0});
    Sketch& spine = document.addSketch("Spine");
    spine.addLine(Vec2{0, 0}, Vec2{0, 50});
    Body& body = document.addBody("Body");
    document.addSweepFeature(body, "Sweep1", section.id(), spine.id());

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Feature* feature = loaded.document->bodies().front()->features().front().get();
    const auto* sweep = dynamic_cast<const SweepFeature*>(feature);
    ASSERT_NE(sweep, nullptr) << "a Sweep came back as something else";
    EXPECT_EQ(sweep->profileSketchId(), section.id());
    EXPECT_EQ(sweep->pathSketchId(), spine.id());
}

TEST(SerializationV13Test, M19_SER_002_ALoftKeepsItsSectionsINORDER) {
    // The order IS the shape: lofting A-B-C and A-C-B are different solids. A
    // round trip that sorted them -- by id, by name, by anything -- would
    // reload a different part from the same file.
    PartDocument document{"LoftDoc"};
    Sketch& a = document.addSketch("A");
    Sketch& b = document.addSketch("B");
    Sketch& c = document.addSketch("C");
    Body& body = document.addBody("Body");
    // Deliberately NOT in id order, so a sort would be visible.
    document.addLoftFeature(body, "Loft1", {c.id(), a.id(), b.id()});

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const auto* loft =
        dynamic_cast<const LoftFeature*>(loaded.document->bodies().front()->features().front().get());
    ASSERT_NE(loft, nullptr) << "a Loft came back as something else";
    ASSERT_EQ(loft->sectionSketchIds().size(), 3u);
    EXPECT_EQ(loft->sectionSketchIds()[0], c.id());
    EXPECT_EQ(loft->sectionSketchIds()[1], a.id());
    EXPECT_EQ(loft->sectionSketchIds()[2], b.id());
}

TEST(SerializationV13Test, M19_SER_003_ALoftOfOneSectionIsREFUSEDAtTheDoor) {
    // A file claiming a one-section loft describes a feature this program
    // cannot build. Letting it in would trade a clear load error for a feature
    // that fails later, with the reason a long way from the cause.
    PartDocument document{"LoftDoc"};
    Sketch& a = document.addSketch("A");
    Sketch& b = document.addSketch("B");
    Body& body = document.addBody("Body");
    document.addLoftFeature(body, "Loft1", {a.id(), b.id()});

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"sectionSketchIds\"");
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t open = text.find('[', at);
    const std::size_t close = text.find(']', open);
    ASSERT_NE(close, std::string::npos);
    text.replace(open, close - open + 1, "[\"" + std::to_string(a.id()) + "\"]");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("at least two sections"), std::string::npos) << loaded.message;
}

TEST(SerializationV13Test, M19_SER_004_ASweepNamingAMissingSketchIsREFUSEDATSAVETIME) {
    // ADR-M3-008's named worst class: a document that SAVES cleanly and then
    // refuses to load. Every reference the loader checks is checked here too,
    // and a sweep has two of them.
    PartDocument document{"SweepDoc"};
    Sketch& section = document.addSketch("Section");
    Body& body = document.addBody("Body");
    // A real ObjectId that is NOT a sketch. A bare invalid id would be caught
    // by any check at all; a live id belonging to something else is the case
    // that needs the set lookup, and it is what a stale reference looks like.
    Parameter& notASketch = document.addParameter("L", 10.0, UnitType::Millimeter);
    document.addSweepFeature(body, "Sweep1", section.id(), notASketch.id());

    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("path sketch id"), std::string::npos) << saved.message;
}

// --- M20 (v26): SHELL, DRAFT and HOLE survive a round trip -------------------

TEST(SerializationV13Test, M20_SER_001_AShellKeepsItsFaceQUERIESAndItsThickness) {
    // The faces are sentences, not indices -- so what has to come back is the
    // sentence. A round trip that stored "face 3" would open whatever is third
    // in the reloaded solid, which is a different part with no complaint.
    PartDocument document{"ShellDoc"};
    Sketch& sketch = document.addSketch("Base");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    Parameter& tall = document.addParameter("H", 20.0, UnitType::Millimeter);
    Parameter& wall = document.addParameter("W", 3.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId pad = document.addPadFeature(body, "Pad1", sketch.id(), tall.id()).id();

    FaceQuery top;
    top.extremeTowards = Vec3{0, 0, 1};
    FaceQuery madeByPad;
    madeByPad.createdBy = pad;
    madeByPad.facing = Vec3{0, 1, 0};
    document.addShellFeature(body, "Shell1", pad, {top, madeByPad}, wall.id());

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const auto* shell =
        dynamic_cast<const ShellFeature*>(loaded.document->bodies().front()->features()[1].get());
    ASSERT_NE(shell, nullptr) << "a Shell came back as something else";
    ASSERT_EQ(shell->openFaces().size(), 2u);
    EXPECT_TRUE(shell->openFaces()[0].extremeTowards.has_value());
    EXPECT_FALSE(shell->openFaces()[0].createdBy.has_value());
    ASSERT_TRUE(shell->openFaces()[1].createdBy.has_value());
    EXPECT_EQ(*shell->openFaces()[1].createdBy, pad);
    ASSERT_TRUE(shell->openFaces()[1].facing.has_value());
    EXPECT_NEAR(shell->openFaces()[1].facing->y, 1.0, 1e-12);
    EXPECT_EQ(shell->thicknessParameterId(), wall.id());
}

TEST(SerializationV13Test, M20_SER_002_ADraftKeepsItsNEUTRALFaceToo) {
    // The neutral face is not one of the tapered ones: it decides where the
    // taper pivots AND which way the part is pulled. A round trip that kept the
    // list and dropped it would reload a draft that cannot be built.
    PartDocument document{"DraftDoc"};
    Sketch& sketch = document.addSketch("Base");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    Parameter& tall = document.addParameter("H", 20.0, UnitType::Millimeter);
    Parameter& angle = document.addParameter("A", 0.12, UnitType::Radian);
    Body& body = document.addBody("Body");
    const ObjectId pad = document.addPadFeature(body, "Pad1", sketch.id(), tall.id()).id();

    FaceQuery wall;
    wall.facing = Vec3{0, 1, 0};
    FaceQuery neutral;
    neutral.extremeTowards = Vec3{0, 0, -1};
    document.addDraftFeature(body, "Draft1", pad, {wall}, neutral, angle.id());

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const auto* draft =
        dynamic_cast<const DraftFeature*>(loaded.document->bodies().front()->features()[1].get());
    ASSERT_NE(draft, nullptr) << "a Draft came back as something else";
    ASSERT_EQ(draft->faces().size(), 1u);
    ASSERT_TRUE(draft->neutralFace().extremeTowards.has_value());
    EXPECT_NEAR(draft->neutralFace().extremeTowards->z, -1.0, 1e-12);
    EXPECT_EQ(draft->angleParameterId(), angle.id());
}

TEST(SerializationV13Test, M20_SER_003_AHoleKeepsAllFOUROfItsReferences) {
    PartDocument document{"HoleDoc"};
    Sketch& base = document.addSketch("Base");
    base.addLine(Vec2{0, 0}, Vec2{40, 0});
    Sketch& holes = document.addSketch("Holes");
    holes.addPoint(Vec2{10, 10});
    Parameter& tall = document.addParameter("H", 20.0, UnitType::Millimeter);
    Parameter& bore = document.addParameter("D", 6.0, UnitType::Millimeter);
    Parameter& deep = document.addParameter("Z", -8.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId pad = document.addPadFeature(body, "Pad1", base.id(), tall.id()).id();
    document.addHoleFeature(body, "Hole1", pad, holes.id(), bore.id(), deep.id());

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const auto* hole =
        dynamic_cast<const HoleFeature*>(loaded.document->bodies().front()->features()[1].get());
    ASSERT_NE(hole, nullptr) << "a Hole came back as something else";
    EXPECT_EQ(hole->baseFeatureId(), pad);
    EXPECT_EQ(hole->sketchId(), holes.id());
    EXPECT_EQ(hole->diameterParameterId(), bore.id());
    EXPECT_EQ(hole->depthParameterId(), deep.id());
}

TEST(SerializationV13Test, M20_SER_004_AShellNamingNOFacesIsREFUSEDAtTheDoor) {
    // A hollow with no way in weighs less than the part and looks exactly like
    // it. A file describing one is refused where the reason is near the cause.
    PartDocument document{"ShellDoc"};
    Sketch& sketch = document.addSketch("Base");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    Parameter& tall = document.addParameter("H", 20.0, UnitType::Millimeter);
    Parameter& wall = document.addParameter("W", 3.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId pad = document.addPadFeature(body, "Pad1", sketch.id(), tall.id()).id();
    FaceQuery top;
    top.extremeTowards = Vec3{0, 0, 1};
    document.addShellFeature(body, "Shell1", pad, {top}, wall.id());

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"faceSelection\"");
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t open = text.find('[', at);
    const std::size_t close = text.find(']', open);
    ASSERT_NE(close, std::string::npos);
    text.replace(open, close - open + 1, "[]");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("names no faces"), std::string::npos) << loaded.message;
}

TEST(SerializationV13Test, M20_SER_005_AFaceQueryWithNOConditionsIsREFUSED) {
    // A query with nothing set matches every face, so it names none. A file
    // holding one describes a feature that fails on every recompute from now
    // on, with nothing to fix.
    PartDocument document{"ShellDoc"};
    Sketch& sketch = document.addSketch("Base");
    sketch.addLine(Vec2{0, 0}, Vec2{40, 0});
    Parameter& tall = document.addParameter("H", 20.0, UnitType::Millimeter);
    Parameter& wall = document.addParameter("W", 3.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId pad = document.addPadFeature(body, "Pad1", sketch.id(), tall.id()).id();
    FaceQuery top;
    top.extremeTowards = Vec3{0, 0, 1};
    document.addShellFeature(body, "Shell1", pad, {top}, wall.id());

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"extremeTowards\"");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"extremeTowards\"").size(), "\"somethingElse\"");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    // Refused by the SAME reader the sketch's tracked face goes through -- one
    // rule about what a face query has to say, not one per holder of one.
    EXPECT_NE(loaded.message.find("names no face"), std::string::npos) << loaded.message;
}
