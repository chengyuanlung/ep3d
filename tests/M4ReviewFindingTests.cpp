// Regression tests for the Critical and Major findings raised by the M4
// independent review (docs/reviews/M4_IndependentReview.md). Each failed
// against the reviewed code and passes against the fix.
//
// Every defect here lived one step off the happy path the release gates
// exercise, which is why 284 passing tests did not catch any of them.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Profile.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <cmath>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

// --- Critical 1: self-intersection test was orientation-dependent ----------
// The collinear-overlap check shrank its x bounds but grew its y bounds, so for
// any segment narrower than the tolerance in x -- every vertical segment -- it
// could never fire. OCCT then built a degenerate face and reported volume 0 as
// a SUCCESSFUL result: silent wrong geometry.

TEST(M4ReviewFindingTest, CRITICAL1_OverlappingVerticalSegmentsAreRejected) {
    // The reviewer's counterexample: the two vertical members overlap along x=0.
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{0, 100});
    sketch.addLine(Vec2{0, 100}, Vec2{5, 90});
    sketch.addLine(Vec2{5, 90}, Vec2{0, 80});
    sketch.addLine(Vec2{0, 80}, Vec2{0, 20});
    sketch.addLine(Vec2{0, 20}, Vec2{-5, 10});
    sketch.addLine(Vec2{-5, 10}, Vec2{0, 0});

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result) << "a self-overlapping outline was accepted";
    EXPECT_EQ(result.error, ProfileError::SelfIntersecting);
}

TEST(M4ReviewFindingTest, CRITICAL1_DetectionIsOrientationIndependent) {
    // The same figure rotated 90 degrees was already rejected before the fix.
    // Both orientations must now agree -- that agreement is the actual
    // invariant, and a bounding-box test cannot provide it.
    Sketch horizontal{"H"};
    horizontal.addLine(Vec2{0, 0}, Vec2{100, 0});
    horizontal.addLine(Vec2{100, 0}, Vec2{90, 5});
    horizontal.addLine(Vec2{90, 5}, Vec2{80, 0});
    horizontal.addLine(Vec2{80, 0}, Vec2{20, 0});
    horizontal.addLine(Vec2{20, 0}, Vec2{10, -5});
    horizontal.addLine(Vec2{10, -5}, Vec2{0, 0});

    EXPECT_FALSE(BuildProfile(horizontal));
    EXPECT_EQ(BuildProfile(horizontal).error, ProfileError::SelfIntersecting);
}

TEST(M4ReviewFindingTest, CRITICAL1_ValidAxisAlignedProfilesStillAccepted) {
    // The fix must not over-reject: a plain rectangle has two vertical members
    // that are collinear with nothing.
    Sketch rectangle{"R"};
    rectangle.addLine(Vec2{0, 0}, Vec2{100, 0});
    rectangle.addLine(Vec2{100, 0}, Vec2{100, 50});
    rectangle.addLine(Vec2{100, 50}, Vec2{0, 50});
    rectangle.addLine(Vec2{0, 50}, Vec2{0, 0});
    EXPECT_TRUE(BuildProfile(rectangle)) << "a valid rectangle was rejected";

    // A U-shape has two parallel vertical members that do NOT overlap.
    Sketch ushape{"U"};
    ushape.addLine(Vec2{0, 0}, Vec2{100, 0});
    ushape.addLine(Vec2{100, 0}, Vec2{100, 80});
    ushape.addLine(Vec2{100, 80}, Vec2{70, 80});
    ushape.addLine(Vec2{70, 80}, Vec2{70, 30});
    ushape.addLine(Vec2{70, 30}, Vec2{30, 30});
    ushape.addLine(Vec2{30, 30}, Vec2{30, 80});
    ushape.addLine(Vec2{30, 80}, Vec2{0, 80});
    ushape.addLine(Vec2{0, 80}, Vec2{0, 0});
    EXPECT_TRUE(BuildProfile(ushape)) << "a valid U-shape was rejected";
}

// --- Major 1: sketches were mutable through a const document ---------------

TEST(M4ReviewFindingTest, MAJOR1_EditSketchDirtiesDependentsAutomatically) {
    PartDocument document{"Doc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    document.addMaterial("Mat", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    const ObjectId sketchId = document.addSketch("Sketch001").id();
    document.editSketch(sketchId, [](Sketch& s) {
        s.addLine(Vec2{0, 0}, Vec2{100, 0});
        s.addLine(Vec2{100, 0}, Vec2{100, 50});
        s.addLine(Vec2{100, 50}, Vec2{0, 50});
        s.addLine(Vec2{0, 50}, Vec2{0, 0});
    });
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketchId, length.id());
    ASSERT_TRUE(document.recompute().success);
    ASSERT_DOUBLE_EQ(document.massProperties().volumeMm3, 100000.0);

    // Widen to 120 through the facade. Dirtying is not the caller's job.
    ASSERT_TRUE(document.editSketch(sketchId, [](Sketch& s) {
        std::vector<SketchEntityId> toRemove;
        for (const SketchEntity& e : s.entities()) toRemove.push_back(e.id);
        for (SketchEntityId id : toRemove) s.removeEntity(id);
        s.addLine(Vec2{0, 0}, Vec2{120, 0});
        s.addLine(Vec2{120, 0}, Vec2{120, 50});
        s.addLine(Vec2{120, 50}, Vec2{0, 50});
        s.addLine(Vec2{0, 50}, Vec2{0, 0});
    }));

    ASSERT_TRUE(document.recompute().success);
    EXPECT_DOUBLE_EQ(document.massProperties().volumeMm3, 120000.0)
        << "an edit through the facade did not reach the dependent Pad";
}

TEST(M4ReviewFindingTest, MAJOR1_EditSketchRejectsUnknownId) {
    PartDocument document{"Doc"};
    bool called = false;
    EXPECT_FALSE(document.editSketch(12345, [&](Sketch&) { called = true; }));
    EXPECT_FALSE(called) << "an edit ran against a sketch that does not exist";
}

// --- Major 2: removeObject had no owner step for Sketch or Material --------

TEST(M4ReviewFindingTest, MAJOR2_RemovingASketchDetachesItFromTheDocument) {
    PartDocument document{"Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const ObjectId sketchId = sketch.id();
    sketch.addLine(Vec2{0, 0}, Vec2{10, 0});

    ASSERT_TRUE(document.removeObject(sketchId));
    EXPECT_TRUE(document.sketches().empty())
        << "removeObject reported success but the document still owns the sketch";
    EXPECT_EQ(document.findSketch(sketchId), nullptr);
    EXPECT_EQ(document.objectRegistry().find(sketchId), nullptr);
}

TEST(M4ReviewFindingTest, MAJOR2_RemovedSketchIsNotResurrectedOnReload) {
    PartDocument document{"Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{10, 0});
    ASSERT_TRUE(document.removeObject(sketch.id()));

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_TRUE(loaded.document->sketches().empty())
        << "a removed sketch came back on load";
}

TEST(M4ReviewFindingTest, MAJOR2_PadWhoseSketchWasRemovedFailsLoudly) {
    PartDocument document{"Doc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    document.addMaterial("Mat", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    ASSERT_TRUE(document.recompute().success);

    ASSERT_TRUE(document.removeObject(sketch.id()));
    EXPECT_FALSE(document.recompute().success)
        << "a Pad kept reporting a solid built from a sketch that no longer exists";
    EXPECT_EQ(pad.state(), ComputeState::Failed);
    EXPECT_FALSE(document.massProperties().valid);
}

TEST(M4ReviewFindingTest, MAJOR2_RemovingTheMaterialDropsIt) {
    PartDocument document{"Doc"};
    Material& material = document.addMaterial("Mat", 2700.0);
    const ObjectId materialId = material.id();

    ASSERT_TRUE(document.removeObject(materialId));
    EXPECT_FALSE(document.material()) << "the document still holds the removed material";
    EXPECT_EQ(document.objectRegistry().find(materialId), nullptr);
}

// --- Major 3: save-side validation did not cover PadFeature ---------------

TEST(M4ReviewFindingTest, MAJOR3_SaveRejectsPadWithADanglingLengthParameter) {
    PartDocument document{"Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    ASSERT_TRUE(document.removeObject(length.id()));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_FALSE(saved) << "saved a document whose own loader would reject it";
    EXPECT_EQ(saved.error, SerializationError::UnknownDependencyId);
}

TEST(M4ReviewFindingTest, MAJOR3_SaveRejectsPadWithADanglingSketch) {
    PartDocument document{"Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    ASSERT_TRUE(document.removeObject(sketch.id()));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_FALSE(saved);
    EXPECT_EQ(saved.error, SerializationError::UnknownDependencyId);
}

TEST(M4ReviewFindingTest, MAJOR3_IntactPadDocumentStillSaves) {
    // Guard against the validation above rejecting healthy documents.
    PartDocument document{"Doc"};
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(document, out));
}

// --- New Major 1: removing a Material left referrers holding its id --------
// Introduced by the Major-2 fix and caught only on re-review. The owner was
// cleared but every referring feature kept writing the removed id, so the file
// saved cleanly and its own loader rejected it forever. The original
// MAJOR2_RemovingTheMaterialDropsIt never called save, which is why the suite
// stayed green -- a removal test that never round-trips proves half the rule.

TEST(M4ReviewFindingTest, NEWMAJOR1_RemovingMaterialClearsPadReference) {
    PartDocument document{"Doc"};
    Material& material = document.addMaterial("Mat", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    ASSERT_EQ(pad.materialId(), material.id());

    ASSERT_TRUE(document.removeObject(material.id()));
    EXPECT_EQ(pad.materialId(), kInvalidObjectId)
        << "the Pad still references a material this document no longer has";
}

TEST(M4ReviewFindingTest, NEWMAJOR1_RemovingMaterialClearsBoxReference) {
    // The same defect reached M3's BoxFeature, not just M4's Pad.
    PartDocument document{"Doc"};
    Material& material = document.addMaterial("Mat", 2700.0);
    Parameter& w = document.addParameter("W", 100.0, UnitType::Millimeter);
    Parameter& h = document.addParameter("H", 50.0, UnitType::Millimeter);
    Parameter& d = document.addParameter("D", 20.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    BoxFeature& box = document.addBoxFeature(body, "Box001", w.id(), h.id(), d.id());
    ASSERT_EQ(box.materialId(), material.id());

    ASSERT_TRUE(document.removeObject(material.id()));
    EXPECT_EQ(box.materialId(), kInvalidObjectId);
}

TEST(M4ReviewFindingTest, NEWMAJOR1_DocumentStillRoundTripsAfterMaterialRemoval) {
    // The property that actually matters: removing a material must leave a
    // document that still saves AND loads.
    PartDocument document{"Doc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Material& material = document.addMaterial("Mat", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    ASSERT_TRUE(document.recompute().success);

    ASSERT_TRUE(document.removeObject(material.id()));
    // No material is a legitimate state (ADR-M3-004: density 0), so recompute
    // still succeeds -- with zero mass.
    EXPECT_TRUE(document.recompute().success);

    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    ASSERT_TRUE(saved) << saved.message;
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    EXPECT_TRUE(loaded) << "saved a document that can never be loaded: " << loaded.message;
}

TEST(M4ReviewFindingTest, NEWMAJOR1_SaveRejectsAStaleMaterialReference) {
    // The save-side half of the fix, exercised on its own. Adding a material no
    // longer produces a stale reference (it reassigns every feature), so this
    // uses the RESTORE path -- which is exactly how a hand-edited or
    // externally-produced document could carry one.
    PartDocument document{"Doc"};
    Material& material = document.addMaterial("Mat", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    Body& body = document.addBody("Body001");
    document.restorePadFeature(body, 987654, "Ghost", ComputeState::Dirty, sketch.id(),
                               length.id(), material.id() + 4242); // never this document's

    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_FALSE(saved) << "saved a feature whose materialId no longer matches the document";
    EXPECT_EQ(saved.error, SerializationError::UnknownDependencyId);
}

// --- Major 1 (re-review): sketches() leaked a mutable Sketch& -------------

TEST(M4ReviewFindingTest, MAJOR1_SketchesAccessorIsGenuinelyReadOnly) {
    // const vector<unique_ptr<Sketch>>& is not read-only: constness stops at the
    // unique_ptr, so *front() was a mutable Sketch& obtained from a const
    // document. The accessor now yields const pointers, so the compiler
    // enforces what the comment claims.
    PartDocument document{"Doc"};
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});

    const PartDocument& readOnly = document;
    const std::vector<const Sketch*> sketches = readOnly.sketches();
    ASSERT_EQ(sketches.size(), 1u);
    static_assert(std::is_same_v<decltype(sketches)::value_type, const Sketch*>,
                  "sketches() must not hand out a mutable Sketch");
    EXPECT_EQ(sketches.front()->entities().size(), 1u);
}

// --- Re-review Minor: a material added after a feature was unassignable ----

TEST(M4ReviewFindingTest, MINOR_MaterialAddedAfterAFeatureIsStillAssigned) {
    // Reported as: remove the material, add a new one, and the part weighs
    // nothing. Self-consistent and round-trippable, so not a wrong answer --
    // but a document showing a material and a mass of zero is visibly wrong.
    PartDocument document{"Doc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    // The Pad exists first; the material arrives afterwards.
    Material& material = document.addMaterial("Steel", 7800.0);
    EXPECT_EQ(pad.materialId(), material.id())
        << "a material added after the feature never reached it";

    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(document.massProperties().massKg, 7800.0 * 100000.0 * 1e-9, 1e-9)
        << "the part still weighs nothing despite having a material";
}

TEST(M4ReviewFindingTest, MINOR_MaterialCanBeReassignedAfterRemoval) {
    // The exact sequence from the review: remove, then add a different one.
    PartDocument document{"Doc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Material& first = document.addMaterial("Aluminium", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    ASSERT_TRUE(document.recompute().success);

    ASSERT_TRUE(document.removeObject(first.id()));
    ASSERT_EQ(pad.materialId(), kInvalidObjectId);
    ASSERT_TRUE(document.recompute().success);
    EXPECT_DOUBLE_EQ(document.massProperties().massKg, 0.0); // no material: zero mass

    Material& second = document.addMaterial("Steel", 7800.0);
    EXPECT_EQ(pad.materialId(), second.id());
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(document.massProperties().massKg, 7800.0 * 100000.0 * 1e-9, 1e-9);

    // And the result still round-trips.
    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(document, out));
    std::istringstream in(out.str());
    EXPECT_TRUE(loadPartDocument(in));
}

// --- Minor: full-turn arc was accepted despite the stated rule -------------

TEST(M4ReviewFindingTest, MINOR_FullTurnArcRejected) {
    Sketch sketch{"Sketch001"};
    // A 2*pi sweep is a circle, not an arc; the old check compared the raw
    // angle difference, which 2*pi passes comfortably.
    EXPECT_EQ(sketch.addArc(Vec2{0, 0}, 10.0, 0.0, 2.0 * kPi), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addArc(Vec2{0, 0}, 10.0, 1.0, 1.0 + 2.0 * kPi), kInvalidSketchEntityId);
    // Ordinary arcs, including ones phrased with negative or large angles,
    // remain valid.
    EXPECT_NE(sketch.addArc(Vec2{0, 0}, 10.0, 0.0, kPi), kInvalidSketchEntityId);
    EXPECT_NE(sketch.addArc(Vec2{0, 0}, 10.0, -kPi / 2, kPi / 2), kInvalidSketchEntityId);
}

} // namespace
