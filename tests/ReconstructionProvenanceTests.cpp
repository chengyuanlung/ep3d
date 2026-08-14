// M7.4 — provenance and ambiguity reporting (spec 18, 26).
//
// The five questions spec 26 requires the diagnostic layer to answer, each with
// a test: was this explicit or inferred, which source item produced it, which
// entities does it target, which Parameter controls it, and was anything
// skipped.
//
// These matter because after application the answers are UNRECOVERABLE. A
// Length the source stated and a Length M7 chose are the same native object,
// and no inspection of the finished document can separate them.

#include "Core/Document/PartDocument.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

struct RectangleFixture {
    PartDocument document{"M7Prov"};
    Sketch* sketch = nullptr;

    RectangleFixture() {
        Sketch& s = document.addSketch("Imported");
        sketch = &s;
        s.addLine(Vec2{0, 0}, Vec2{100, 0});
        s.addLine(Vec2{100, 0}, Vec2{100, 50});
        s.addLine(Vec2{100, 50}, Vec2{0, 50});
        s.addLine(Vec2{0, 50}, Vec2{0, 0});
    }
};

ImportedDimension2D Linear(Vec2 from, Vec2 to, double directionRad, const char* handle) {
    ImportedDimension2D dimension;
    dimension.kind = ImportedDimensionKind::Linear;
    dimension.measureFrom = from;
    dimension.measureTo = to;
    dimension.directionRad = directionRad;
    dimension.sourceHandle = handle;
    return dimension;
}

// --- Question 1: explicit, or inferred? -------------------------------------

TEST(M7Provenance, ExplicitAndInferredConstraintsAreDistinguishable) {
    RectangleFixture fx;
    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(),
        {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A"),
         Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2, "2B")});
    ASSERT_TRUE(result) << result.message;

    // 2 Length from the source; 4 Coincident + 2 Horizontal + 2 Vertical
    // inferred from geometry; 1 Fix inferred placement. Spec 3: these three
    // categories must not be silently mixed, so each is counted separately.
    EXPECT_EQ(result.report.countByOrigin(ReconstructionOrigin::ExplicitSource), 2u);
    EXPECT_EQ(result.report.countByOrigin(ReconstructionOrigin::InferredGeometric), 8u);
    EXPECT_EQ(result.report.countByOrigin(ReconstructionOrigin::InferredPlacement), 1u);
    EXPECT_EQ(result.report.entries.size(), 11u);
}

TEST(M7Provenance, TheFixIsPlacementNotAMeasurement) {
    RectangleFixture fx;
    const ReconstructionResult result = ReconstructSketch(fx.document, fx.sketch->id(), {});
    ASSERT_TRUE(result) << result.message;

    // A Fix is M7's own choice of where to pin the sketch. Reporting it as
    // something the source asked for would be the clearest possible case of
    // presenting an inference as a measurement.
    for (const ProvenanceEntry& entry : result.report.entries)
        if (entry.constraintKind == "Fix")
            EXPECT_EQ(entry.origin, ReconstructionOrigin::InferredPlacement);
}

// --- Question 2: which source item produced it? -----------------------------

TEST(M7Provenance, EachExplicitConstraintNamesTheSourceItemItCameFrom) {
    RectangleFixture fx;
    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(),
        {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A"),
         Linear(Vec2{100, 0}, Vec2{100, 50}, kPi / 2, "2B")});
    ASSERT_TRUE(result) << result.message;

    bool sawA = false;
    bool sawB = false;
    for (const ProvenanceEntry& entry : result.report.entries) {
        if (entry.origin != ReconstructionOrigin::ExplicitSource) continue;
        if (entry.sourceRef.find("2A") != std::string::npos) sawA = true;
        if (entry.sourceRef.find("2B") != std::string::npos) sawB = true;
    }
    // "Which of my dimensions became this?" is unanswerable without it, and a
    // user looking at a wrong Width has no other way back to the drawing.
    EXPECT_TRUE(sawA);
    EXPECT_TRUE(sawB);
}

// --- Questions 3 and 4: which entities, which Parameter? --------------------

TEST(M7Provenance, EveryEntryNamesItsTargetsAndItsControllingParameter) {
    RectangleFixture fx;
    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A")});
    ASSERT_TRUE(result) << result.message;

    for (const ProvenanceEntry& entry : result.report.entries) {
        EXPECT_FALSE(entry.targets.empty()) << entry.constraintKind << " names no geometry";
        for (SketchEntityId target : entry.targets)
            EXPECT_NE(fx.document.findSketch(fx.sketch->id())->findEntity(target), nullptr)
                << "a provenance entry names an entity the sketch does not have";

        if (entry.constraintKind == "Length") {
            // The dimensional kinds must name a Parameter that actually
            // resolves -- an entry claiming "controlled by Width" while Width
            // does not exist is worse than no report.
            EXPECT_NE(entry.parameterId, kInvalidObjectId);
            EXPECT_NE(fx.document.parameters().findById(entry.parameterId), nullptr);
            EXPECT_EQ(entry.parameterName, "Width");
        } else {
            EXPECT_EQ(entry.parameterId, kInvalidObjectId)
                << entry.constraintKind << " reports a Parameter it cannot use";
        }
    }
}

TEST(M7Provenance, AnEntryIsFindableByItsNativeConstraintId) {
    RectangleFixture fx;
    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A")});
    ASSERT_TRUE(result) << result.message;
    ASSERT_FALSE(result.createdConstraints.empty());

    // Native id in, provenance out. This is the direction the UI needs: the
    // user selects a constraint in the tree and asks where it came from.
    for (SketchConstraintId id : result.createdConstraints)
        EXPECT_NE(result.report.find(id), nullptr);

    // And an id that was not reconstructed has no entry, rather than a
    // plausible-looking default.
    EXPECT_EQ(result.report.find(static_cast<SketchConstraintId>(999999)), nullptr);
}

// --- Question 5: was anything skipped? --------------------------------------

TEST(M7Provenance, SkippedItemsSurviveIntoTheReportWithTheirReasons) {
    RectangleFixture fx;
    fx.sketch->addLine(Vec2{0, 0}, Vec2{100, 0}); // a duplicate: ambiguous

    ImportedDimension2D override = Linear(Vec2{100, 50}, Vec2{0, 50}, 0.0, "2C");
    override.textOverride = "SEE NOTE 4";

    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(),
        {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A"), override});
    ASSERT_TRUE(result) << result.message;

    ASSERT_EQ(result.report.skipped.size(), 2u);
    const bool ambiguous =
        std::any_of(result.report.skipped.begin(), result.report.skipped.end(),
                    [](const ReconstructionSkip& s) {
                        return s.reason == ReconstructionSkipReason::AmbiguousTarget;
                    });
    const bool textOverride =
        std::any_of(result.report.skipped.begin(), result.report.skipped.end(),
                    [](const ReconstructionSkip& s) {
                        return s.reason == ReconstructionSkipReason::TextOverride;
                    });
    EXPECT_TRUE(ambiguous);
    EXPECT_TRUE(textOverride);

    // Each names its source, so the user can find the offending dimension in
    // their own drawing rather than being told only that "something" was
    // dropped.
    for (const ReconstructionSkip& skip : result.report.skipped) {
        EXPECT_FALSE(skip.detail.empty());
        EXPECT_FALSE(skip.sourceRef.empty());
    }

    // And the valid candidates still reconstructed alongside them (spec 18:
    // "continue with other unambiguous reconstruction candidates").
    EXPECT_GT(result.report.entries.size(), 0u);
}

TEST(M7Provenance, DiagnosticsSurviveAFailedApply) {
    RectangleFixture fx;
    ImportedDimension2D override = Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A");
    override.textOverride = "APPROX";

    ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {override});
    ASSERT_EQ(plan.skipped.size(), 1u);
    // Break the plan so apply must refuse it outright.
    plan.constraints.push_back(PlannedConstraint{LengthConstraint{kInvalidSketchEntityId,
                                                                 kInvalidObjectId},
                                                 kInvalidPlanParameterSlot,
                                                 ReconstructionOrigin::ExplicitSource, {}});

    const ReconstructionResult result = ApplyReconstruction(fx.document, plan);

    ASSERT_FALSE(result);
    // "It did not work" with no diagnostics is the least useful failure there
    // is. What the analysis declined is the half that matters most when the
    // rest goes wrong, so it must survive the failure path.
    EXPECT_EQ(result.report.skipped.size(), 1u);
    EXPECT_EQ(result.report.skipped.front().reason, ReconstructionSkipReason::TextOverride);
}

// --- The rendered report ----------------------------------------------------

TEST(M7Provenance, TheFormattedReportStatesTheSkippedCountEvenWhenItIsZero) {
    RectangleFixture fx;
    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A")});
    ASSERT_TRUE(result) << result.message;

    const std::string text = FormatReconstructionReport(result.report);

    // A report that omits the section when nothing was skipped cannot be told
    // from one where the question was never asked -- and "nothing was skipped"
    // is exactly the claim a user must be able to trust before relying on the
    // result.
    EXPECT_NE(text.find("skipped: 0"), std::string::npos) << text;
    EXPECT_NE(text.find("explicit in source: 1"), std::string::npos) << text;
    EXPECT_NE(text.find("Width"), std::string::npos) << text;
}

TEST(M7Provenance, ProvenanceIsSessionScopedAndSaysSoOutLoud) {
    RectangleFixture fx;
    const ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A")});
    ASSERT_TRUE(result) << result.message;
    // 4 Coincident + 2 Horizontal + 2 Vertical + 1 Fix + 1 Length = 10; this
    // fixture carries ONE dimension, not two.
    ASSERT_EQ(result.report.entries.size(), 10u);

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.document, out));
    const std::string saved = out.str();

    // DOCUMENTED LIMITATION, pinned so it cannot become an accidental claim
    // (ADR-M7-017): provenance is NOT persisted. The saved document carries no
    // DXF handle, no origin classification and no skip list.
    EXPECT_EQ(saved.find("2A"), std::string::npos);
    EXPECT_EQ(saved.find("explicit in source"), std::string::npos);

    // What DOES survive is everything correctness depends on. After reload the
    // constraints and their bindings are intact and the model still rebuilds --
    // which is exactly the line spec 26 draws when it says provenance must not
    // become runtime identity.
    std::istringstream in(saved);
    LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const Parameter* width = loaded.document->parameters().findByName("Width");
    ASSERT_NE(width, nullptr);
    EXPECT_FALSE(loaded.document->constraintsBindingParameter(width->id()).empty());
}

TEST(M7Provenance, ProvenanceIsNotIdentity) {
    RectangleFixture fx;
    ReconstructionResult result = ReconstructSketch(
        fx.document, fx.sketch->id(), {Linear(Vec2{0, 0}, Vec2{100, 0}, 0.0, "2A")});
    ASSERT_TRUE(result) << result.message;

    const ObjectId widthId = fx.document.parameters().findByName("Width")->id();

    // Throw the entire report away. Spec 26: provenance must not become runtime
    // identity, so the document has to be exactly as correct without it.
    result.report = ReconstructionReport{};

    EXPECT_NE(fx.document.parameters().findById(widthId), nullptr);
    EXPECT_FALSE(fx.document.constraintsBindingParameter(widthId).empty());
    ASSERT_TRUE(fx.document.setParameterValue(widthId, 120.0));
    EXPECT_DOUBLE_EQ(fx.document.parameters().findById(widthId)->value(), 120.0);
}

} // namespace
