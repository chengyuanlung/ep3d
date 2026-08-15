// Schema v7 (M8.2): the Revolve record and its axis reference, at the Core
// level with the fake kernel -- no OCCT, no solver.
//
// The fake's revolve models FULL revolutions of a rectangle about an in-plane
// vertical axis, by Pappus -- exactly this fixture's shape, so the
// recompute-after-load check is real arithmetic: A=800, Rbar=20,
// V = 2*pi*20*800 = 32000*pi.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

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

struct RevolveDoc {
    PartDocument document{"V7Doc"};
    FakeGeometryKernel kernel;
    ObjectId revolveId = kInvalidObjectId;
    ObjectId angleId = kInvalidObjectId;
    SketchEntityId axisId{};

    RevolveDoc() {
        document.setGeometryKernel(&kernel);
        document.addMaterial("Aluminium", 2700.0);
        Parameter& angle = document.addParameter("RevolveAngle", 2.0 * kPi, UnitType::Radian);
        angleId = angle.id();

        Sketch& sketch = document.addSketch("RevolveSketch");
        axisId = sketch.addLine(Vec2{0, -5}, Vec2{0, 45});
        sketch.addLine(Vec2{10, 0}, Vec2{30, 0});
        sketch.addLine(Vec2{30, 0}, Vec2{30, 40});
        sketch.addLine(Vec2{30, 40}, Vec2{10, 40});
        sketch.addLine(Vec2{10, 40}, Vec2{10, 0});

        Body& body = document.addBody("Body001");
        RevolveFeature& revolve =
            document.addRevolveFeature(body, "Revolve001", sketch.id(), axisId, angleId);
        revolveId = revolve.id();
    }
};

TEST(SerializationV7Test, M8_SER_101_RevolveRoundTripsWithItsAxisReference) {
    RevolveDoc doc;
    const std::string saved = SaveToString(doc.document);

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;

    const RevolveFeature* revolve = nullptr;
    for (const auto& body : loaded.document->bodies())
        for (const auto& feature : body->features())
            if (auto* r = dynamic_cast<RevolveFeature*>(feature.get())) revolve = r;
    ASSERT_NE(revolve, nullptr) << "the revolve did not survive as a Revolve";
    EXPECT_EQ(revolve->id(), doc.revolveId);
    EXPECT_EQ(revolve->axisEntityId(), doc.axisId);
    EXPECT_EQ(revolve->angleParameterId(), doc.angleId);

    // And it WORKS after load: Pappus, 32000*pi.
    FakeGeometryKernel kernel;
    loaded.document->setGeometryKernel(&kernel);
    ASSERT_TRUE(loaded.document->recompute().success);
    ASSERT_TRUE(loaded.document->massProperties().valid);
    EXPECT_NEAR(loaded.document->massProperties().volumeMm3, 2.0 * kPi * 20.0 * 800.0, 1e-6);
}

TEST(SerializationV7Test, M8_SER_102_AnAxisThatIsNotInTheNamedSketchIsRefused) {
    RevolveDoc doc;
    std::string saved = SaveToString(doc.document);

    // Point the axis at an entity id that exists nowhere in the sketch.
    const std::string needle =
        "\"axisEntityId\": \"" + std::to_string(ToObjectId(doc.axisId)) + "\"";
    const std::size_t pos = saved.find(needle);
    ASSERT_NE(pos, std::string::npos);
    saved.replace(pos, needle.size(), "\"axisEntityId\": \"999999\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("axis entity"), std::string::npos) << loaded.message;
}

TEST(SerializationV7Test, M8_SER_103_AnAngleThatIsNotAParameterIsRefused) {
    RevolveDoc doc;
    std::string saved = SaveToString(doc.document);

    const std::string needle = "\"angleParameterId\": \"" + std::to_string(doc.angleId) + "\"";
    const std::size_t pos = saved.find(needle);
    ASSERT_NE(pos, std::string::npos);
    saved.replace(pos, needle.size(), "\"angleParameterId\": \"999999\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("angle parameter"), std::string::npos) << loaded.message;
}

// --- Round-1 regression tests (M8 independent review, R2-C1 probes A3-A5) ----
// Save/load symmetry for every reference the loader checks (ADR-M3-008). Each
// was demonstrated in review as save-OK -> load-refused via the public facade.

TEST(SerializationV7Test, M8_REV_311_RemovingTheAngleParameterMakesTheDocUnsavable) {
    RevolveDoc doc;
    ASSERT_TRUE(doc.document.removeObject(doc.angleId)); // probe A3

    std::ostringstream out;
    const SaveResult saved = savePartDocument(doc.document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("revolve angle parameter id"), std::string::npos)
        << saved.message;
}

TEST(SerializationV7Test, M8_REV_312_RemovingTheRevolveSketchMakesTheDocUnsavable) {
    RevolveDoc doc;
    ASSERT_TRUE(doc.document.removeObject(doc.document.sketches().front()->id())); // probe A4

    std::ostringstream out;
    const SaveResult saved = savePartDocument(doc.document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("revolve sketch id"), std::string::npos) << saved.message;
}

TEST(SerializationV7Test, M8_REV_313_RemovingTheAxisLineMakesTheDocUnsavable) {
    // The sharpest of review's six symmetry gaps (probe A5): deleting any
    // sketch line that happens to be a revolve axis used to succeed AND save
    // cleanly -- silently poisoning every future save of the document.
    RevolveDoc doc;
    ASSERT_TRUE(
        doc.document.removeSketchEntity(doc.document.sketches().front()->id(), doc.axisId));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(doc.document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("revolve axis entity id"), std::string::npos) << saved.message;
}

} // namespace
