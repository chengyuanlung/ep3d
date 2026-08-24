// Schema v11 (M13): the seven geometric sketch constraints, persisted.
//
// A constraint that solves correctly and cannot be saved is a constraint the
// user loses on close. These tests assert the round trip carries the constraint
// KIND, its references, and -- for Tangent -- the branch flag that decides
// which of the two tangencies the model means.

#include "Core/Document/PartDocument.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <variant>
#include <vector>

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

// One sketch carrying one of everything M13 added, plus the entities they need.
struct GeometricDoc {
    PartDocument document{"V11Doc"};
    ObjectId sketchId = kInvalidObjectId;
    SketchEntityId lineA{}, lineB{}, circleA{}, circleB{}, point{};

    GeometricDoc() {
        Sketch& sketch = document.addSketch("Sketch001");
        sketchId = sketch.id();
        lineA = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
        lineB = sketch.addLine(Vec2{0, 40}, Vec2{90, 70});
        circleA = sketch.addCircle(Vec2{200, 0}, 25.0);
        circleB = sketch.addCircle(Vec2{260, 0}, 10.0);
        point = sketch.addPoint(Vec2{50, 90});

        document.addSketchConstraint(sketchId, ParallelConstraint{lineA, lineB});
        document.addSketchConstraint(sketchId, PerpendicularConstraint{lineB, lineA});
        document.addSketchConstraint(sketchId, EqualConstraint{lineA, lineB});
        document.addSketchConstraint(sketchId, ConcentricConstraint{circleA, circleB});
        document.addSketchConstraint(
            sketchId, MidpointConstraint{{point, SketchSubElement::Whole}, lineA});
        document.addSketchConstraint(
            sketchId, PointOnObjectConstraint{{point, SketchSubElement::Whole}, circleA});
        document.addSketchConstraint(sketchId, TangentConstraint{lineA, circleA, false});
        document.addSketchConstraint(sketchId, TangentConstraint{circleA, circleB, true});
    }
};

const Sketch& OnlySketch(const PartDocument& document) {
    return *document.sketches().front();
}

template <typename T>
const T* FindConstraintOfKind(const Sketch& sketch, std::size_t skip = 0) {
    for (const SketchConstraint& constraint : sketch.constraints()) {
        if (const auto* value = std::get_if<T>(&constraint.data)) {
            if (skip == 0) return value;
            --skip;
        }
    }
    return nullptr;
}

} // namespace

TEST(SerializationV11Test, M13_SER_001_TheGeometricKindsAreWrittenByName) {
    GeometricDoc source;
    const std::string text = SaveToString(source.document);
    // The v11 CONTENT, not the version number.
    //
    // This pinned `"schemaVersion": 11` and went red the moment M16 bumped to
    // 12 -- a failure that said nothing about M13. The version stamp is worth
    // ONE test (the current-version one), and every other file should assert
    // what it actually contributed.
    for (const char* kind : {"\"Parallel\"", "\"Perpendicular\"", "\"Equal\"",
                             "\"Concentric\"", "\"Midpoint\"", "\"PointOnObject\"",
                             "\"Tangent\""})
        EXPECT_NE(text.find(kind), std::string::npos) << kind;
}

TEST(SerializationV11Test, M13_SER_002_EveryGeometricConstraintSurvivesTheRoundTrip) {
    GeometricDoc source;
    const std::size_t before = OnlySketch(source.document).constraints().size();
    ASSERT_EQ(before, 8u);

    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& sketch = OnlySketch(*loaded.document);
    ASSERT_EQ(sketch.constraints().size(), before);

    // Kind-by-kind, because a loader that mapped every unknown kind onto the
    // same fallback would still produce the right COUNT.
    EXPECT_NE(FindConstraintOfKind<ParallelConstraint>(sketch), nullptr);
    EXPECT_NE(FindConstraintOfKind<PerpendicularConstraint>(sketch), nullptr);
    EXPECT_NE(FindConstraintOfKind<EqualConstraint>(sketch), nullptr);
    EXPECT_NE(FindConstraintOfKind<ConcentricConstraint>(sketch), nullptr);
    EXPECT_NE(FindConstraintOfKind<MidpointConstraint>(sketch), nullptr);
    EXPECT_NE(FindConstraintOfKind<PointOnObjectConstraint>(sketch), nullptr);
    EXPECT_NE(FindConstraintOfKind<TangentConstraint>(sketch), nullptr);
}

TEST(SerializationV11Test, M13_SER_003_ReferencesAndSubElementsAreCarried) {
    GeometricDoc source;
    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& sketch = OnlySketch(*loaded.document);

    const auto* parallel = FindConstraintOfKind<ParallelConstraint>(sketch);
    ASSERT_NE(parallel, nullptr);
    EXPECT_EQ(parallel->lineA, source.lineA);
    EXPECT_EQ(parallel->lineB, source.lineB);

    // ORDER MATTERS for Perpendicular's siblings only cosmetically, but for
    // Midpoint and PointOnObject the point and the host are different roles:
    // swapping them would be a different constraint that still loads.
    const auto* midpoint = FindConstraintOfKind<MidpointConstraint>(sketch);
    ASSERT_NE(midpoint, nullptr);
    EXPECT_EQ(midpoint->point.entityId, source.point);
    EXPECT_EQ(midpoint->point.subElement, SketchSubElement::Whole);
    EXPECT_EQ(midpoint->line, source.lineA);

    const auto* onObject = FindConstraintOfKind<PointOnObjectConstraint>(sketch);
    ASSERT_NE(onObject, nullptr);
    EXPECT_EQ(onObject->point.entityId, source.point);
    EXPECT_EQ(onObject->target, source.circleA);

    const auto* concentric = FindConstraintOfKind<ConcentricConstraint>(sketch);
    ASSERT_NE(concentric, nullptr);
    EXPECT_EQ(concentric->curveA, source.circleA);
    EXPECT_EQ(concentric->curveB, source.circleB);
}

TEST(SerializationV11Test, M13_SER_004_TangentCarriesItsBranchBothWays) {
    GeometricDoc source;
    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& sketch = OnlySketch(*loaded.document);

    const auto* outer = FindConstraintOfKind<TangentConstraint>(sketch, 0);
    const auto* inner = FindConstraintOfKind<TangentConstraint>(sketch, 1);
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);
    // THE POINT OF THIS TEST: `internal` is not a cosmetic flag. Losing it
    // turns "these two circles nest and touch" into "these two circles sit
    // beside each other and touch" -- a different model that loads cleanly and
    // solves to a different shape.
    EXPECT_FALSE(outer->internal);
    EXPECT_TRUE(inner->internal);
}

TEST(SerializationV11Test, M13_SER_005_AFileWithoutTangentsBranchIsRefusedNotDefaulted) {
    GeometricDoc source;
    std::string text = SaveToString(source.document);
    // The key is RENAMED rather than deleted, so the JSON stays well formed and
    // the loader has to fail on the missing FIELD. Deleting the line instead
    // left a dangling comma, and the load was then refused by the JSON parser
    // -- the right outcome for the wrong reason, which would have passed while
    // proving nothing about the field check.
    const std::string key = "\"internal\"";
    std::size_t at = text.find(key);
    ASSERT_NE(at, std::string::npos);
    while (at != std::string::npos) {
        text.replace(at, key.size(), "\"notInternal\"");
        at = text.find(key);
    }

    const LoadResult loaded = LoadFromString(text);
    // Refused, not silently defaulted to `false`: a default would open a
    // document that describes the OTHER tangency without saying so.
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("internal"), std::string::npos) << loaded.message;
}

TEST(SerializationV11Test, M13_SER_006_AnUnknownConstraintKindIsStillRefused) {
    GeometricDoc source;
    std::string text = SaveToString(source.document);
    const std::size_t at = text.find("\"Parallel\"");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::char_traits<char>::length("\"Parallel\""), "\"Antiparallel\"");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("Antiparallel"), std::string::npos) << loaded.message;
}

TEST(SerializationV11Test, M13_SER_007_ADocumentWithNoGeometricConstraintsStillRoundTrips) {
    // The v10 shape, saved by the v11 writer. Nothing about the new kinds may
    // leak into a document that has none of them.
    PartDocument document{"Plain"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    document.addSketchConstraint(sketch.id(), HorizontalConstraint{line});

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& reloaded = OnlySketch(*loaded.document);
    ASSERT_EQ(reloaded.constraints().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<HorizontalConstraint>(reloaded.constraints().front().data));
}

// =============================================================================
// M26.3 -- point-pair Horizontal and Vertical (schema v33)
// =============================================================================

TEST(SerializationV11Test, M26_SER_010_PointPairHorizontalAndVerticalRoundTrip) {
    PartDocument document{"PointPairs"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId lineA = sketch.addLine(Vec2{0, 0}, Vec2{40, 30});
    const SketchEntityId lineB = sketch.addLine(Vec2{90, 5}, Vec2{130, 55});
    const SketchEntityId loose = sketch.addPoint(Vec2{200, 200});
    const SketchEntityId circle = sketch.addCircle(Vec2{300, 40}, 9.0);

    document.addSketchConstraint(
        sketch.id(),
        PointsHorizontalConstraint{SketchElementRef{lineA, SketchSubElement::EndPoint},
                                   SketchElementRef{lineB, SketchSubElement::StartPoint}});
    document.addSketchConstraint(
        sketch.id(),
        PointsVerticalConstraint{SketchElementRef{loose},
                                 SketchElementRef{circle, SketchSubElement::CenterPoint}});

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& reloaded = OnlySketch(*loaded.document);
    ASSERT_EQ(reloaded.constraints().size(), 2u);

    // THE SUB-ELEMENTS ARE THE POINT. A round trip that kept the entity ids and
    // dropped which END was named would reload as a constraint about the
    // lines' start points -- a different model that still loads cleanly.
    const auto* h = std::get_if<PointsHorizontalConstraint>(&reloaded.constraints()[0].data);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->a.entityId, lineA);
    EXPECT_EQ(h->a.subElement, SketchSubElement::EndPoint);
    EXPECT_EQ(h->b.entityId, lineB);
    EXPECT_EQ(h->b.subElement, SketchSubElement::StartPoint);

    const auto* v = std::get_if<PointsVerticalConstraint>(&reloaded.constraints()[1].data);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->a.entityId, loose);
    EXPECT_EQ(v->b.entityId, circle);
    EXPECT_EQ(v->b.subElement, SketchSubElement::CenterPoint);
}

TEST(SerializationV11Test, M26_SER_011_APointPairNamingAMissingEntityIsRefused) {
    // The same door every other constraint reference goes through. A file that
    // saved cleanly and reloads pointing at nothing is ADR-M3-008's worst case.
    PartDocument document{"PointPairs"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId a = sketch.addPoint(Vec2{0, 0});
    const SketchEntityId b = sketch.addPoint(Vec2{50, 50});
    document.addSketchConstraint(
        sketch.id(), PointsHorizontalConstraint{SketchElementRef{a}, SketchElementRef{b}});

    std::string text = SaveToString(document);
    const std::string wanted = std::to_string(static_cast<unsigned long long>(ToObjectId(b)));
    const std::size_t at = text.rfind(wanted);
    ASSERT_NE(at, std::string::npos);
    text.replace(at, wanted.size(), "999999");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("not in this sketch"), std::string::npos) << loaded.message;
}
