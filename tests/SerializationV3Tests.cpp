#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

std::string saveToString(const PartDocument& document) {
    std::ostringstream out;
    const SaveResult result = savePartDocument(document, out);
    EXPECT_TRUE(result) << result.message;
    return out.str();
}

LoadResult loadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadPartDocument(in);
}

TEST(SerializationV3Test, M3_SER_001_StableIdsSurviveRoundTrip) {
    PartDocument original("V3Doc");
    Material& material = original.addMaterial("Aluminum", 2700.0);
    Parameter& width = original.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = original.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& depth = original.addParameter("Depth", 20.0, UnitType::Millimeter);
    Body& body = original.addBody("Body001");
    BoxFeature& box = original.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());

    const LoadResult loaded = loadFromString(saveToString(original));
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->id(), original.id());
    ASSERT_TRUE(loaded.document->material());
    EXPECT_EQ(loaded.document->material()->id(), material.id());
    ASSERT_EQ(loaded.document->parameters().items().size(), 3u);
    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    EXPECT_EQ(loaded.document->bodies()[0]->id(), body.id());
    ASSERT_EQ(loaded.document->bodies()[0]->features().size(), 1u);
    EXPECT_EQ(loaded.document->bodies()[0]->features()[0]->id(), box.id());
}

TEST(SerializationV3Test, M3_SER_002_ParameterAndMaterialReferencesSurvive) {
    PartDocument original("V3Refs");
    Material& material = original.addMaterial("Steel", 7850.0);
    material.elasticModulusPa = 2.0e11;
    material.poissonRatio = 0.3;
    material.yieldStrengthPa = 2.5e8;
    material.contact.staticFriction = 0.4;
    material.contact.dynamicFriction = 0.3;
    material.contact.restitution = 0.1;
    Parameter& width = original.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = original.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& depth = original.addParameter("Depth", 20.0, UnitType::Millimeter);
    Body& body = original.addBody("Body001");
    BoxFeature& box = original.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());

    const LoadResult loaded = loadFromString(saveToString(original));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->bodies()[0]->features().size(), 1u);
    const auto* loadedBox =
        dynamic_cast<const BoxFeature*>(loaded.document->bodies()[0]->features()[0].get());
    ASSERT_NE(loadedBox, nullptr);
    EXPECT_EQ(loadedBox->widthParameterId(), width.id());
    EXPECT_EQ(loadedBox->heightParameterId(), height.id());
    EXPECT_EQ(loadedBox->depthParameterId(), depth.id());
    EXPECT_EQ(loadedBox->materialId(), material.id());
    EXPECT_EQ(loadedBox->typeName(), "Box");

    ASSERT_TRUE(loaded.document->material());
    const Material& loadedMaterial = *loaded.document->material();
    EXPECT_EQ(loadedMaterial.name(), "Steel");
    EXPECT_EQ(loadedMaterial.density(), 7850.0);
    EXPECT_EQ(loadedMaterial.elasticModulusPa, 2.0e11);
    EXPECT_EQ(loadedMaterial.poissonRatio, 0.3);
    EXPECT_EQ(loadedMaterial.yieldStrengthPa, 2.5e8);
    EXPECT_EQ(loadedMaterial.contact.staticFriction, 0.4);
    EXPECT_EQ(loadedMaterial.contact.dynamicFriction, 0.3);
    EXPECT_EQ(loadedMaterial.contact.restitution, 0.1);
}

TEST(SerializationV3Test, M3_SER_003_NoRuntimeStatePersisted_LoadedDocumentRecomputesEquivalent) {
    // "recompute after load" architecture assertion (spec 15/17): the file
    // carries only semantic ids; runtime OCCT/KernelShape state is never
    // written, and re-running recompute() against a freshly injected fake
    // kernel reproduces equivalent geometry results from the restored
    // Parameters/Material alone.
    PartDocument original("V3Recompute");
    FakeGeometryKernel originalKernel;
    original.setGeometryKernel(&originalKernel);
    Material& material = original.addMaterial("Aluminum", 2700.0);
    Parameter& width = original.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = original.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& depth = original.addParameter("Depth", 20.0, UnitType::Millimeter);
    Body& body = original.addBody("Body001");
    original.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());
    ASSERT_TRUE(original.recompute().success);
    const MassProperties originalProperties = original.massProperties();
    (void)material;

    const std::string saved = saveToString(original);
    // No OCCT/KernelShape/IShapeHandle state appears in the file -- only
    // semantic ids and numeric fields.
    EXPECT_EQ(saved.find("TopoDS"), std::string::npos);
    EXPECT_EQ(saved.find("handle"), std::string::npos);

    const LoadResult loaded = loadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;

    // Graph states are not persisted: every node starts Dirty, and nothing
    // has been computed yet (BoxFeature has no shape, MassProperties invalid).
    EXPECT_FALSE(loaded.document->massProperties().valid);

    FakeGeometryKernel loadedKernel; // a DIFFERENT kernel instance/state
    loaded.document->setGeometryKernel(&loadedKernel);
    const DocumentRecomputeReport report = loaded.document->recompute();
    ASSERT_TRUE(report.success);
    const MassProperties& reloadedProperties = loaded.document->massProperties();
    EXPECT_TRUE(reloadedProperties.valid);
    EXPECT_EQ(reloadedProperties.volumeMm3, originalProperties.volumeMm3);
    EXPECT_EQ(reloadedProperties.massKg, originalProperties.massKg);
    EXPECT_EQ(reloadedProperties.centerOfMassMm.x, originalProperties.centerOfMassMm.x);
    EXPECT_EQ(reloadedProperties.centerOfMassMm.y, originalProperties.centerOfMassMm.y);
    EXPECT_EQ(reloadedProperties.centerOfMassMm.z, originalProperties.centerOfMassMm.z);
}

TEST(SerializationV3Test, M3_SER_004_ByteIdenticalRoundTrip) {
    PartDocument original("V3Bytes");
    original.addMaterial("Aluminum", 2700.0);
    Parameter& width = original.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = original.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& depth = original.addParameter("Depth", 20.0, UnitType::Millimeter);
    Body& body = original.addBody("Body001");
    original.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());

    const std::string firstSave = saveToString(original);
    EXPECT_NE(firstSave.find("\"schemaVersion\": 4"), std::string::npos);
    const LoadResult loaded = loadFromString(firstSave);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(saveToString(*loaded.document), firstSave);
}

TEST(SerializationV3Test, M3_SER_005_NoMaterialAssignedSerializesAsNull) {
    PartDocument original("V3NoMaterial");
    original.addParameter("Width", 1.0, UnitType::Millimeter);
    const std::string saved = saveToString(original);
    EXPECT_NE(saved.find("\"material\": null"), std::string::npos) << saved;

    const LoadResult loaded = loadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_FALSE(loaded.document->material());
}

TEST(SerializationV3Test, M3_SER_006_V1FileStillLoads) {
    constexpr const char* kV1Document = R"({
      "format": "ParametricCAD",
      "schemaVersion": 1,
      "documentType": "Part",
      "id": "9001",
      "name": "LegacyV1",
      "parameters": [],
      "bodies": []
    })";
    const LoadResult loaded = loadFromString(kV1Document);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->id(), 9001u);
    EXPECT_FALSE(loaded.document->material());
}

TEST(SerializationV3Test, M3_SER_007_V2FileStillLoads) {
    constexpr const char* kV2Document = R"({
      "format": "ParametricCAD",
      "schemaVersion": 2,
      "documentType": "Part",
      "id": "9002",
      "name": "LegacyV2",
      "parameters": [ {"id": "9003", "name": "W", "value": 1.0, "unit": "Millimeter",
                       "expression": "", "state": "Valid"} ],
      "bodies": [],
      "dependencies": []
    })";
    const LoadResult loaded = loadFromString(kV2Document);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->id(), 9002u);
    ASSERT_EQ(loaded.document->parameters().items().size(), 1u);
    EXPECT_FALSE(loaded.document->material());
}

TEST(SerializationV3Test, M3_SER_008_BoxFeatureEdgesNotInGenericDependenciesArray) {
    // Option B (ADR-M3-005): BoxFeature's Width/Height/Depth edges are
    // re-derived from semantic id fields, never written to "dependencies".
    PartDocument original("V3EdgeSplit");
    Parameter& width = original.addParameter("Width", 10.0, UnitType::Millimeter);
    Parameter& height = original.addParameter("Height", 10.0, UnitType::Millimeter);
    Parameter& depth = original.addParameter("Depth", 10.0, UnitType::Millimeter);
    Body& body = original.addBody("Body001");
    original.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());

    const std::string saved = saveToString(original);
    const LoadResult loaded = loadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    // "dependencies" is present but empty -- the graph shape is still fully
    // reconstructed via restoreBoxFeature's semantic wiring, not the array.
    EXPECT_NE(saved.find("\"dependencies\": []"), std::string::npos) << saved;
    const auto& features = loaded.document->bodies()[0]->features();
    ASSERT_EQ(features.size(), 1u);
    const DependencyGraph& graph = loaded.document->dependencyGraph();
    EXPECT_EQ(graph.dependentsOf(width.id()), std::vector<ObjectId>{features[0]->id()});
}

TEST(SerializationV3Test, M3_SER_009_UnknownBoxParameterIdRejected) {
    constexpr const char* kBadDocument = R"({
      "format": "ParametricCAD", "schemaVersion": 3, "documentType": "Part",
      "id": "9101", "name": "Bad", "parameters": [], "material": null,
      "bodies": [ {"id": "9102", "name": "Body001",
                   "features": [ {"id": "9103", "name": "Box001", "type": "Box", "state": "Dirty",
                                  "widthParameterId": "9999", "heightParameterId": "9999",
                                  "depthParameterId": "9999", "materialId": "0"} ]} ],
      "dependencies": []
    })";
    const LoadResult result = loadFromString(kBadDocument);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::UnknownDependencyId);
    EXPECT_EQ(result.document, nullptr);
}

TEST(SerializationV3Test, M3_SER_010_MismatchedBoxMaterialIdRejected) {
    constexpr const char* kBadDocument = R"({
      "format": "ParametricCAD", "schemaVersion": 3, "documentType": "Part",
      "id": "9201", "name": "Bad", "material": null,
      "parameters": [ {"id": "9202", "name": "W", "value": 1.0, "unit": "Millimeter",
                       "expression": "", "state": "Valid"} ],
      "bodies": [ {"id": "9203", "name": "Body001",
                   "features": [ {"id": "9204", "name": "Box001", "type": "Box", "state": "Dirty",
                                  "widthParameterId": "9202", "heightParameterId": "9202",
                                  "depthParameterId": "9202", "materialId": "555"} ]} ],
      "dependencies": []
    })";
    const LoadResult result = loadFromString(kBadDocument);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::UnknownDependencyId);
    EXPECT_EQ(result.document, nullptr);
}

TEST(SerializationV3Test, M3_SER_011_UnknownFeatureTypeFallsBackToPlaceholderLosslessly) {
    // A "type" string this build does not recognize round-trips losslessly
    // via PlaceholderFeature rather than being rejected or silently coerced.
    constexpr const char* kDocument = R"({
      "format": "ParametricCAD", "schemaVersion": 3, "documentType": "Part",
      "id": "9301", "name": "Future", "material": null, "parameters": [],
      "bodies": [ {"id": "9302", "name": "Body001",
                   "features": [ {"id": "9303", "name": "Fillet001", "type": "Fillet",
                                  "state": "Dirty"} ]} ],
      "dependencies": []
    })";
    const LoadResult loaded = loadFromString(kDocument);
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->bodies()[0]->features().size(), 1u);
    const Feature& feature = *loaded.document->bodies()[0]->features()[0];
    EXPECT_EQ(feature.typeName(), "Fillet");
    EXPECT_NE(dynamic_cast<const PlaceholderFeature*>(&feature), nullptr);
}

} // namespace
