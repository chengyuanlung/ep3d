// M9.1 release gates (M9 spec section 7, gates A-D), executed against the REAL
// solver and REAL OCCT geometry.
//
// The fixture is M8's chain, unchanged, because M9 has to prove that history
// works on the model M8 shipped rather than on a model built to suit it:
//
//   pad   : 100 x 50 x 20               = 100000 mm^3
//   pocket: 20 x 30 rectangle, 10 deep  =  -6000 mm^3  -> 94000
//   width 100 -> 120                    -> 120x50x20 - 6000 = 114000
//   pocket deleted                      -> the pad alone     = 100000
//
// Every number is hand-computed from the parameter values. Selectivity is
// proven by COUNTERS, never by equal final values -- the lesson M7's review
// carved into Gate K and M8's review repeated at GATE_RC.

#include "Core/Document/PartDocument.h"
#include "Core/Edit/FeatureEditSession.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Viewer/DocumentPresenter.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kVolumeRelTol = 1e-9; // prismatic booleans: exact

void ExpectRel(double actual, double expected, double relTol = kVolumeRelTol) {
    EXPECT_NEAR(actual, expected, relTol * std::max(1.0, std::fabs(expected)));
}

class CountingKernel final : public IGeometryKernel {
public:
    ShapeResult createBox(const BoxDefinition& definition) override {
        return inner_.createBox(definition);
    }
    ShapeResult extrudeProfile(const PlanarProfileDefinition& profile,
                               double distanceMm) override {
        ++extrudes;
        return inner_.extrudeProfile(profile, distanceMm);
    }
    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override {
        return inner_.calculateMassProperties(shape);
    }
    ShapeResult subtractShape(const KernelShape& base, const KernelShape& tool) override {
        ++subtracts;
        return inner_.subtractShape(base, tool);
    }
    ShapeResult placeShape(const KernelShape& shape, const Transform3D& placement) override {
        ++placeShapeCallCount;
        (void)shape;
        (void)placement;
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "this counting kernel does not model placements"};
    }
    int placeShapeCallCount = 0;

    ShapeResult rotateShape(const KernelShape& shape, const Vec3& axisOriginMm,
                            const Vec3& axisDirection, double angleRad) override {
        return inner_.rotateShape(shape, axisOriginMm, axisDirection, angleRad);
    }
    KernelInterferenceResult measureInterference(const KernelShape& a,
                                                 const KernelShape& b) override {
        (void)a;
        (void)b;
        return KernelInterferenceResult{false, "this kernel does not measure interference", 0.0};
    }
    ShapeResult intersectShapes(const KernelShape& a, const KernelShape& b) override {
        return inner_.intersectShapes(a, b);
    }
    ShapeResult shellSolid(const KernelShape& base, const FaceSelection& openFaces,
                           double thicknessMm) override {
        return inner_.shellSolid(base, openFaces, thicknessMm);
    }
    ShapeResult draftFaces(const KernelShape& base, const FaceSelection& faces,
                           const FaceQuery& neutral, double angleRad) override {
        return inner_.draftFaces(base, faces, neutral, angleRad);
    }
    IoResult exportStep(const KernelShape& shape, const std::string& path) override {
        return inner_.exportStep(shape, path);
    }
    ShapeResult importStep(const std::string& path) override {
        return inner_.importStep(path);
    }
    IoResult exportStl(const KernelShape& shape, const std::string& path,
                       double deflectionMm) override {
        return inner_.exportStl(shape, path, deflectionMm);
    }
    KernelBoundsResult boundsOfShape(const KernelShape& shape) override {
        return inner_.boundsOfShape(shape);
    }
    ShapeResult sweepProfile(const PlanarProfileDefinition& profile,
                             const PlanarPathDefinition& path) override {
        return inner_.sweepProfile(profile, path);
    }
    ShapeResult loftProfiles(const std::vector<PlanarProfileDefinition>& profiles)
        override {
        return inner_.loftProfiles(profiles);
    }
    ShapeResult revolveProfile(const PlanarProfileDefinition& profile, const Vec3& axisOriginMm,
                               const Vec3& axisDirection, double angleRad) override {
        return inner_.revolveProfile(profile, axisOriginMm, axisDirection, angleRad);
    }
    ShapeResult filletEdges(const KernelShape& shape, const EdgeSelection& selection,
                            double radiusMm) override {
        return inner_.filletEdges(shape, selection, radiusMm);
    }
    ShapeResult chamferEdges(const KernelShape& shape, const EdgeSelection& selection,
                             double distanceMm) override {
        return inner_.chamferEdges(shape, selection, distanceMm);
    }
    // M10.6 verbs. Forwarded, uncounted: these suites predate them and none
    // of their gates is about mirroring, so counting would add a member every
    // fixture has to ignore.
    ShapeResult mirrorShape(const KernelShape& shape, const Vec3& planeOriginMm,
                            const Vec3& planeNormal) override {
        return inner_.mirrorShape(shape, planeOriginMm, planeNormal);
    }
    ShapeResult translateShape(const KernelShape& shape, const Vec3& offsetMm) override {
        return inner_.translateShape(shape, offsetMm);
    }
    ShapeResult fuseShapes(const KernelShape& a, const KernelShape& b) override {
        return inner_.fuseShapes(a, b);
    }
    int extrudes = 0;
    int subtracts = 0;

private:
    OcctGeometryKernel inner_;
};

class CountingSolver final : public ISketchSolver {
public:
    SketchSolveResult solve(const SketchSolveProblem& problem) override {
        ++calls;
        return inner_.solve(problem);
    }
    int calls = 0;

private:
    GaussNewtonSketchSolver inner_;
};

// The engine's diagnostic for one node in the last pass, or empty.
std::string FailureMessageFor(PartDocument& document, ObjectId id) {
    const DocumentRecomputeReport report = document.recompute();
    for (const RecomputeItemReport& item : report.items)
        if (item.id == id) return item.message;
    return {};
}

struct ChainFixture {
    PartDocument document{"M9Chain"};
    CountingKernel kernel;
    CountingSolver solver;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* padLength = nullptr;
    Parameter* depth = nullptr;
    PadFeature* pad = nullptr;
    PocketFeature* pocket = nullptr;
    ObjectId bodyId = kInvalidObjectId;

    ChainFixture() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);
        width = &document.addParameter("Width", 100.0, UnitType::Millimeter);
        height = &document.addParameter("Height", 50.0, UnitType::Millimeter);
        padLength = &document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        depth = &document.addParameter("PocketDepth", 10.0, UnitType::Millimeter);

        Sketch& ps = document.addSketch("PadSketch");
        const SketchEntityId bottom = ps.addLine(Vec2{0, 0}, Vec2{100, 0});
        const SketchEntityId right = ps.addLine(Vec2{100, 0}, Vec2{100, 50});
        const SketchEntityId top = ps.addLine(Vec2{100, 50}, Vec2{0, 50});
        const SketchEntityId left = ps.addLine(Vec2{0, 50}, Vec2{0, 0});
        const auto sp = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::StartPoint};
        };
        const auto ep = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::EndPoint};
        };
        const auto add = [&](SketchConstraintData data) {
            document.addSketchConstraint(ps.id(), std::move(data));
        };
        add(CoincidentConstraint{ep(bottom), sp(right)});
        add(CoincidentConstraint{ep(right), sp(top)});
        add(CoincidentConstraint{ep(top), sp(left)});
        add(CoincidentConstraint{ep(left), sp(bottom)});
        add(HorizontalConstraint{bottom});
        add(HorizontalConstraint{top});
        add(VerticalConstraint{right});
        add(VerticalConstraint{left});
        add(FixConstraint{sp(bottom)});
        add(LengthConstraint{bottom, width->id()});
        add(LengthConstraint{right, height->id()});

        Sketch& ks = document.addSketch("PocketSketch");
        ks.addLine(Vec2{10, 10}, Vec2{30, 10});
        ks.addLine(Vec2{30, 10}, Vec2{30, 40});
        ks.addLine(Vec2{30, 40}, Vec2{10, 40});
        ks.addLine(Vec2{10, 40}, Vec2{10, 10});

        Body& body = document.addBody("Body001");
        bodyId = body.id();
        pad = &document.addPadFeature(body, "Pad001", ps.id(), padLength->id());
        pocket = &document.addPocketFeature(body, "Pocket001", pad->id(), ks.id(), depth->id());
    }

    double volume() const { return document.massProperties().volumeMm3; }
};

// --- Gate A: undo and redo of a parameter edit -------------------------------

TEST(M9ReleaseGate, GATE_A_UndoAndRedoOfAParameterEditRestoreTheExactVolume) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);

    // Building the fixture is itself a sequence of recorded edits (two features
    // were added), so history is measured as a DELTA from here rather than
    // assumed empty -- an assumption that would quietly stop being true the day
    // another recorded operation joins the fixture.
    const std::size_t depthBefore = fx.document.undoDepth();

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.width->id()).success);
    ExpectRel(fx.volume(), 114000.0);
    EXPECT_EQ(fx.document.undoDepth(), depthBefore + 1);
    // The label says what the undo would undo (M9 spec section 4).
    EXPECT_EQ(fx.document.nextUndoLabel(), "Change Width");

    ASSERT_TRUE(fx.document.undo());
    ASSERT_TRUE(fx.document.recompute().success);
    // EXACTLY 94000, not "close to it": undo restores the value, and the
    // geometry is re-derived from it rather than resurrected from the history.
    ExpectRel(fx.volume(), 94000.0);
    EXPECT_DOUBLE_EQ(fx.width->value(), 100.0);
    EXPECT_EQ(fx.document.undoDepth(), depthBefore);
    EXPECT_EQ(fx.document.redoDepth(), 1u);

    ASSERT_TRUE(fx.document.redo());
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 114000.0);
    EXPECT_DOUBLE_EQ(fx.width->value(), 120.0);
    EXPECT_EQ(fx.document.redoDepth(), 0u);
}

TEST(M9ReleaseGate, GATE_A2_ANewEditDiscardsTheRedoBranch) {
    // Without this, a redo could replay a change against a document that has
    // since moved -- the one way an undo system produces a state the user never
    // had. Not part of gate A's wording; it is the invariant gate A relies on.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.undo());
    ASSERT_EQ(fx.document.redoDepth(), 1u);

    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 12.0));
    EXPECT_EQ(fx.document.redoDepth(), 0u) << "the redo branch survived a new edit";
    EXPECT_FALSE(fx.document.redo());
}

// --- Gate B: undo depth ------------------------------------------------------

TEST(M9ReleaseGate, GATE_B_NUndosWalkBackThroughEveryIntermediateValueInOrder) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::size_t depthBefore = fx.document.undoDepth();

    const std::vector<double> steps = {110.0, 120.0, 130.0, 140.0, 150.0};
    for (double value : steps) ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), value));
    ASSERT_EQ(fx.document.undoDepth(), depthBefore + steps.size());
    EXPECT_DOUBLE_EQ(fx.width->value(), 150.0);

    // Back through EVERY intermediate value, in order -- not merely back to the
    // start. A stack that collapsed the five edits into one would pass an
    // end-to-end check and fail here.
    for (std::size_t i = steps.size(); i-- > 0;) {
        ASSERT_TRUE(fx.document.undo());
        const double expected = i == 0 ? 100.0 : steps[i - 1];
        EXPECT_DOUBLE_EQ(fx.width->value(), expected)
            << "undo " << (steps.size() - i) << " did not land on the previous value";
    }
    EXPECT_EQ(fx.document.undoDepth(), depthBefore);

    // The fixture's own two feature additions are still on the stack; drain
    // them, and then one more undo must be a NO-OP rather than a corruption.
    //
    // BOUNDED, and the bound is the assertion. The first version of this loop
    // was `while (undo()) {}`, and a mutation that removed the re-entrancy
    // guard in `recordDelta` made every undo push its own inverse -- so the
    // stack never emptied and the loop ran for ever. A test that HANGS when
    // the invariant breaks is worse than one that fails: CI reports a timeout
    // with no finding, and the next reader blames the machine. Found by
    // mutating the guard, which is the only way this class ever surfaces.
    const std::size_t drainLimit = fx.document.undoDepth() + 1;
    std::size_t drained = 0;
    while (fx.document.undo()) {
        ASSERT_LT(++drained, drainLimit)
            << "undo is not consuming the stack -- each undo is recording its own inverse";
    }
    EXPECT_EQ(fx.document.undoDepth(), 0u);
    EXPECT_FALSE(fx.document.undo());
    EXPECT_EQ(fx.document.undoDepth(), 0u);
    EXPECT_EQ(fx.document.nextUndoLabel(), "");
}

TEST(M9ReleaseGate, GATE_B2_ATransactionIsOneUndoStepAndAnEmptyOneIsNone) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::size_t depthBefore = fx.document.undoDepth();

    fx.document.beginTransaction("Resize plate");
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.setParameterValue(fx.height->id(), 60.0));
    ASSERT_TRUE(fx.document.commitTransaction());
    EXPECT_EQ(fx.document.undoDepth(), depthBefore + 1) << "three edits, one step";
    EXPECT_EQ(fx.document.nextUndoLabel(), "Resize plate");

    ASSERT_TRUE(fx.document.undo());
    EXPECT_DOUBLE_EQ(fx.width->value(), 100.0);
    EXPECT_DOUBLE_EQ(fx.height->value(), 50.0) << "the transaction did not undo as one";

    // An empty transaction is not a step: "opened a dialog, pressed OK, changed
    // nothing" must not cost the user an undo.
    const std::size_t depthNow = fx.document.undoDepth();
    fx.document.beginTransaction("Did nothing");
    ASSERT_TRUE(fx.document.commitTransaction());
    EXPECT_EQ(fx.document.undoDepth(), depthNow);
}

TEST(M9ReleaseGate, GATE_B3_AnAbortedTransactionLeavesNothingBehind) {
    // M9 spec section 6: a transaction that fails leaves the document exactly
    // as it was, and creates no undo record for work that did not happen.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::size_t depthBefore = fx.document.undoDepth();

    fx.document.beginTransaction("Abandoned edit");
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 15.0));
    ASSERT_TRUE(fx.document.abortTransaction());

    EXPECT_DOUBLE_EQ(fx.width->value(), 100.0);
    EXPECT_DOUBLE_EQ(fx.depth->value(), 10.0);
    EXPECT_EQ(fx.document.undoDepth(), depthBefore) << "an abort left an undo record behind";
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
}

// --- Gate C: undo of a structural change -------------------------------------

TEST(M9ReleaseGate, GATE_C_UndoOfADeleteRestoresTheFeatureWithItsOriginalIdentity) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);

    const ObjectId pocketId = fx.pocket->id();
    const ObjectId padId = fx.pad->id();

    // Delete the pocket: the pad becomes the chain tail, and mass follows it.
    ASSERT_TRUE(fx.document.removeObject(pocketId));
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.massProperties().valid)
        << "mass did not follow the new tail after the tail feature was removed";
    ExpectRel(fx.volume(), 100000.0);
    EXPECT_EQ(fx.document.nextUndoLabel(), "Delete Pocket001");

    // Undo: the pocket is back.
    ASSERT_TRUE(fx.document.undo());

    // ...with its ORIGINAL ObjectId. A lookalike carrying a fresh id would
    // leave every other reference in the document pointing at nothing, and no
    // volume check would notice.
    const Body& body = *fx.document.bodies().front();
    ASSERT_EQ(body.features().size(), 2u);
    EXPECT_EQ(body.features()[0]->id(), padId);
    EXPECT_EQ(body.features()[1]->id(), pocketId) << "the restored pocket has a new identity";
    EXPECT_EQ(body.features()[1]->typeName(), "Pocket");
    EXPECT_EQ(body.features()[1]->name(), "Pocket001");

    // ...chained, and producing the original volume again.
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), 94000.0);

    // ...and the restored document is SAVABLE, which is the check that proves
    // the chain reference and the feature ORDER both came back: the save-side
    // chain walk refuses a consumer that does not follow its base.
    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(fx.document, out));

    // Redo removes it again, symmetrically.
    ASSERT_TRUE(fx.document.redo());
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 100000.0);
    EXPECT_EQ(fx.document.bodies().front()->features().size(), 1u);
}

TEST(M9ReleaseGate, GATE_C2_UndoOfAnAddRemovesTheFeatureItAdded) {
    // The other direction of the same delta. Without it, "undo a creation"
    // would be untested while "undo a deletion" carried the whole finding.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // The fixture's LAST recorded edit is adding the pocket.
    EXPECT_EQ(fx.document.nextUndoLabel(), "Add Pocket001");
    ASSERT_TRUE(fx.document.undo());
    ASSERT_EQ(fx.document.bodies().front()->features().size(), 1u);
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 100000.0);

    ASSERT_TRUE(fx.document.redo());
    ASSERT_EQ(fx.document.bodies().front()->features().size(), 2u);
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
}

TEST(M9ReleaseGate, GATE_C3_RemovingAConsumedFeatureClearsTheHistoryRatherThanLying) {
    // M9.1's stated limit, asserted rather than left to a comment: removing a
    // feature that another feature CONSUMES cannot be replayed -- the
    // consumer's chain edge dies with the node -- so the history is dropped.
    // A UI can see undoDepth() fall to zero and say so; what it must never do
    // is offer an undo that would restore the base and leave the chain broken.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_GT(fx.document.undoDepth(), 0u);

    ASSERT_TRUE(fx.document.removeObject(fx.pad->id())); // the pocket consumes it
    EXPECT_EQ(fx.document.undoDepth(), 0u);
    EXPECT_EQ(fx.document.redoDepth(), 0u);
    EXPECT_FALSE(fx.document.undo());
}

TEST(M9ReleaseGate, GATE_C4_UndoPutsAMiddleFeatureBackWhereItWas) {
    // WRITTEN BECAUSE A MUTATION SURVIVED. Every other gate removes the LAST
    // feature in its body, so `Body::moveFeatureToIndex` was never exercised at
    // all: neutering it to `return true` left all nine gates green. The index
    // machinery existed for a reason and no test could see it -- this project's
    // most-repeated defect shape, this time in M9's own new code.
    //
    // Note what does NOT catch it: the document still SAVES with the feature at
    // the wrong index, because the save-side chain walk only requires a
    // CONSUMER to follow its base and this feature is neither a consumer nor a
    // base. The assertion therefore has to be on the order itself.
    //
    // The body is built so the removable feature is genuinely in the MIDDLE:
    // Pad001, SparePad, Pocket001 -- the pocket consumes Pad001, SparePad is
    // independent, and only an independent feature is recordable at all
    // (removing a consumed one clears the history; see GATE_C3).
    PartDocument document{"M9Order"};
    CountingKernel kernel;
    CountingSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Aluminium", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("PocketDepth", 10.0, UnitType::Millimeter);

    const auto rectangle = [](Sketch& sketch, double x0, double y0, double x1, double y1) {
        sketch.addLine(Vec2{x0, y0}, Vec2{x1, y0});
        sketch.addLine(Vec2{x1, y0}, Vec2{x1, y1});
        sketch.addLine(Vec2{x1, y1}, Vec2{x0, y1});
        sketch.addLine(Vec2{x0, y1}, Vec2{x0, y0});
    };
    Sketch& padSketch = document.addSketch("PadSketch");
    rectangle(padSketch, 0, 0, 100, 50);
    Sketch& spareSketch = document.addSketch("SpareSketch");
    rectangle(spareSketch, 200, 0, 240, 30);
    Sketch& pocketSketch = document.addSketch("PocketSketch");
    rectangle(pocketSketch, 10, 10, 30, 40);

    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", padSketch.id(), length.id());
    PadFeature& sparePad =
        document.addPadFeature(body, "SparePad", spareSketch.id(), length.id());
    document.addPocketFeature(body, "Pocket001", pad.id(), pocketSketch.id(), depth.id());

    ASSERT_EQ(body.features().size(), 3u);
    ASSERT_EQ(body.features()[1]->name(), "SparePad") << "the fixture is not testing a MIDDLE feature";
    const ObjectId spareId = sparePad.id();

    ASSERT_TRUE(document.removeObject(spareId));
    ASSERT_EQ(body.features().size(), 2u);

    ASSERT_TRUE(document.undo());
    ASSERT_EQ(body.features().size(), 3u);
    EXPECT_EQ(body.features()[0]->name(), "Pad001");
    EXPECT_EQ(body.features()[1]->name(), "SparePad")
        << "the restored feature was appended instead of being put back where it was";
    EXPECT_EQ(body.features()[2]->name(), "Pocket001");
    EXPECT_EQ(body.features()[1]->id(), spareId);

    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(document, out));
}

// --- Gate D: selectivity -----------------------------------------------------

TEST(M9ReleaseGate, GATE_D_UndoOfADepthEditReRunsThePocketOnly) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);

    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 20.0));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.depth->id()).success);
    ExpectRel(fx.volume(), 88000.0);

    // COUNTERS, not equal values: an engine degraded to global recompute would
    // reach the same volume and fail here, which is the entire point.
    const int solves = fx.solver.calls;
    const int extrudes = fx.kernel.extrudes;
    const int subtracts = fx.kernel.subtracts;

    ASSERT_TRUE(fx.document.undo());
    ASSERT_TRUE(fx.document.recomputeFrom(fx.depth->id()).success);

    ExpectRel(fx.volume(), 94000.0);
    EXPECT_EQ(fx.solver.calls, solves) << "undo re-solved a sketch that did not change";
    EXPECT_EQ(fx.kernel.extrudes, extrudes + 1)
        << "the pocket's TOOL prism is one extrude; the pad must not be re-extruded";
    EXPECT_EQ(fx.kernel.subtracts, subtracts + 1) << "exactly one subtract";
}



// --- Gate E: the feature edit transaction -------------------------------------

TEST(M9ReleaseGate, GATE_E_PreviewShowsTheNewVolumeWhileTheDocumentStillShowsTheOld) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
    const std::size_t depthBefore = fx.document.undoDepth();

    std::unique_ptr<FeatureEditSession> session =
        FeatureEditSession::open(fx.document, "Edit Pocket001");
    ASSERT_NE(session, nullptr);

    ASSERT_TRUE(session->setParameterValue(fx.depth->id(), 20.0));
    ASSERT_TRUE(session->recomputePreview());

    // The PREVIEW shows the edited part...
    ASSERT_TRUE(session->previewMassProperties().valid);
    ExpectRel(session->previewMassProperties().volumeMm3, 88000.0);

    // ...and the DOCUMENT is untouched, in every way that can be observed:
    // the value, the computed mass, and the undo history.
    EXPECT_DOUBLE_EQ(fx.depth->value(), 10.0);
    ExpectRel(fx.volume(), 94000.0);
    EXPECT_EQ(fx.document.undoDepth(), depthBefore)
        << "an open preview left an undo record behind";
}

TEST(M9ReleaseGate, GATE_E2_CancelIsIndistinguishableFromNeverHavingStarted) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::size_t depthBefore = fx.document.undoDepth();
    const std::string labelBefore = fx.document.nextUndoLabel();

    {
        std::unique_ptr<FeatureEditSession> session =
            FeatureEditSession::open(fx.document, "Edit Pocket001");
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(session->setParameterValue(fx.depth->id(), 20.0));
        ASSERT_TRUE(session->recomputePreview());
        session->cancel();
        EXPECT_FALSE(session->isOpen());
    }

    EXPECT_DOUBLE_EQ(fx.depth->value(), 10.0);
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
    EXPECT_EQ(fx.document.undoDepth(), depthBefore) << "cancel created an undo record";
    EXPECT_EQ(fx.document.nextUndoLabel(), labelBefore);
}

TEST(M9ReleaseGate, GATE_E3_AcceptCommitsExactlyOneUndoStepHoweverManyValuesMoved) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::size_t depthBefore = fx.document.undoDepth();

    std::unique_ptr<FeatureEditSession> session =
        FeatureEditSession::open(fx.document, "Resize the pocket");
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->setParameterValue(fx.depth->id(), 20.0));
    ASSERT_TRUE(session->setParameterValue(fx.width->id(), 120.0));
    // Two edits to ONE parameter collapse: the session holds the destination,
    // not the route. Without this an edit box would record one step per
    // keystroke.
    ASSERT_TRUE(session->setParameterValue(fx.depth->id(), 20.0));
    ASSERT_TRUE(session->accept());

    EXPECT_EQ(fx.document.undoDepth(), depthBefore + 1) << "accept was not one step";
    EXPECT_EQ(fx.document.nextUndoLabel(), "Resize the pocket");

    ASSERT_TRUE(fx.document.recompute().success);
    // 120 x 50 x 20 - 20 x 30 x 20 = 120000 - 12000.
    ExpectRel(fx.volume(), 108000.0);

    // And one undo puts BOTH values back.
    ASSERT_TRUE(fx.document.undo());
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
    EXPECT_DOUBLE_EQ(fx.width->value(), 100.0);
    EXPECT_DOUBLE_EQ(fx.depth->value(), 10.0);
}

TEST(M9ReleaseGate, GATE_E4_AnAbandonedSessionCancelsRatherThanCommits) {
    // A destructor that committed would turn every early return in a UI slot
    // into an accidental edit.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::size_t depthBefore = fx.document.undoDepth();
    {
        std::unique_ptr<FeatureEditSession> session =
            FeatureEditSession::open(fx.document, "Abandoned");
        ASSERT_NE(session, nullptr);
        ASSERT_TRUE(session->setParameterValue(fx.depth->id(), 20.0));
    } // destroyed without accept or cancel
    EXPECT_DOUBLE_EQ(fx.depth->value(), 10.0);
    EXPECT_EQ(fx.document.undoDepth(), depthBefore);
}

TEST(M9ReleaseGate, GATE_E5_APreviewThatDoesNotBuildIsReportedBeforeCommitting) {
    // The whole reason preview exists: the user finds out the edit breaks the
    // part while they can still say no.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    std::unique_ptr<FeatureEditSession> session =
        FeatureEditSession::open(fx.document, "Break it");
    ASSERT_NE(session, nullptr);
    // 0.0, not a negative value: since M17.8 a negative length is a
    // DIRECTION and builds a perfectly good solid on the other side of the
    // plane (ADR-M17-031). Zero is what still has no magnitude, and failing
    // the feature is what this gate needs -- the value is the lever, not the
    // subject.
    ASSERT_TRUE(session->setParameterValue(fx.depth->id(), 0.0));
    EXPECT_FALSE(session->recomputePreview());
    EXPECT_FALSE(session->previewMassProperties().valid);

    // The real document is still fine, and still building.
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
}

// --- Gate F: suppression ------------------------------------------------------

TEST(M9ReleaseGate, GATE_F_SuppressingTheTailMakesItsBaseTheTail) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);

    ASSERT_TRUE(fx.document.setSuppressed(fx.pocket->id(), true));
    ASSERT_TRUE(fx.document.recompute().success);

    // Reported through the FEATURE, not only through the graph. Until M8's
    // review round 4 the facade set the graph node alone, so the graph said
    // Suppressed while the feature -- the thing the tree and the panel read --
    // said Dirty, and no UI could tell the user anything at all.
    EXPECT_EQ(fx.pocket->state(), ComputeState::Suppressed);
    EXPECT_EQ(fx.document.dependencyGraph().state(fx.pocket->id()), ComputeState::Suppressed);
    EXPECT_FALSE(fx.document.isFeatureActive(fx.pocket->id()));

    // The pad is the tail again, and the mass followed it.
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), 100000.0);

    ASSERT_TRUE(fx.document.setSuppressed(fx.pocket->id(), false));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Valid);
    ExpectRel(fx.volume(), 94000.0);
}

TEST(M9ReleaseGate, GATE_F2_SuppressingAMiddleFeatureClosesTheChainOverIt) {
    // The rule that makes suppression more than a delete-with-undo
    // (ADR-M9-002): Sketch -> Pad -> Pocket -> Fillet, suppress the POCKET, and
    // the fillet dresses the PAD -- without either feature being edited. The
    // stored base reference still names the pocket the whole time.
    ChainFixture fx;
    Body& body = *fx.document.bodies().front();
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pocket->id(), radius.id());
    ASSERT_TRUE(fx.document.recompute().success);
    const double dressedPocket = fx.volume();

    ASSERT_TRUE(fx.document.setSuppressed(fx.pocket->id(), true));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fillet.state(), ComputeState::Valid) << "the fillet was orphaned by the suppression";
    EXPECT_EQ(fillet.baseFeatureId(), fx.pocket->id())
        << "suppression rewrote the stored reference; it is a state, not an edit";

    // The fillet now dresses the PAD: the Minkowski rounded box of 100x50x20 at
    // r = 2, hand-computed exactly as M8's GATE_FB does it.
    const double kPi = 3.14159265358979323846;
    const double roundedPad = 70656.0 + 26752.0 + 632.0 * kPi + (4.0 / 3.0) * kPi * 8.0;
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), roundedPad, 1e-6);
    EXPECT_GT(std::fabs(fx.volume() - dressedPocket), 1.0)
        << "the volume did not change, so the chain did not actually close over the pocket";

    ASSERT_TRUE(fx.document.setSuppressed(fx.pocket->id(), false));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), dressedPocket, 1e-6);
}

TEST(M9ReleaseGate, GATE_F3_SuppressingTheOnlyBaseFailsTheConsumerLoudly) {
    // The other half of gate F. Suppress the PAD, and the pocket has nothing
    // left to cut -- the walk past inactive links runs out. It must fail with a
    // diagnostic, NOT cut against the pad's retained shape: that shape is still
    // there (ADR-M3-001 keeps it byte-for-byte) and using it would produce a
    // healthy-looking wrong solid from geometry the user just switched off.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.setSuppressed(fx.pad->id(), true));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Failed);
    EXPECT_FALSE(fx.document.massProperties().valid)
        << "mass is current while the only solid in the body is switched off";

    ASSERT_TRUE(fx.document.setSuppressed(fx.pad->id(), false));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
}

TEST(M9ReleaseGate, GATE_F4_SuppressionIsUndoableAndRoundTrips) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.setSuppressed(fx.pocket->id(), true));
    EXPECT_EQ(fx.document.nextUndoLabel(), "Suppress Pocket001");
    ASSERT_TRUE(fx.document.undo());
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Valid);
    ExpectRel(fx.volume(), 94000.0);

    // And it survives a save/load: ComputeState has been persisted since M3,
    // so this needs no new format -- asserted rather than assumed, because
    // "it already round-trips" is the kind of claim this project has been
    // wrong about.
    ASSERT_TRUE(fx.document.redo());
    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const Body& body = *loaded.document->bodies().front();
    ASSERT_EQ(body.features().size(), 2u);
    EXPECT_EQ(body.features()[1]->state(), ComputeState::Suppressed);
    EXPECT_FALSE(loaded.document->isFeatureActive(body.features()[1]->id()));
}

// --- Gate G: rollback ---------------------------------------------------------

TEST(M9ReleaseGate, GATE_G_RollingBackHidesLaterFeaturesWithoutRemovingThem) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
    const ObjectId bodyId = fx.bodyId;
    const ObjectId pocketId = fx.pocket->id();

    // Evaluate only the first feature: the pad.
    ASSERT_TRUE(fx.document.setRollbackPosition(bodyId, 1));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 100000.0);
    EXPECT_FALSE(fx.document.isFeatureActive(pocketId));

    // NOT removed and NOT modified -- the feature is still there, with its id,
    // its name and its chain reference. Rollback is a position, not an edit.
    ASSERT_EQ(fx.document.bodies().front()->features().size(), 2u);
    EXPECT_EQ(fx.document.bodies().front()->features()[1]->id(), pocketId);
    EXPECT_EQ(fx.pocket->baseFeatureId(), fx.pad->id());

    // The presenter shows the tail of what is EVALUATED.
    DocumentPresenter presenter(fx.document);
    presenter.recomputeForDisplay();
    const std::vector<ObjectId> shown = presenter.displayableSolids();
    ASSERT_EQ(shown.size(), 1u);
    EXPECT_EQ(shown.front(), fx.pad->id()) << "a rolled-back feature is being displayed";

    // Back to the end.
    ASSERT_TRUE(fx.document.setRollbackPosition(bodyId, Body::kNoRollback));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);
    EXPECT_TRUE(fx.document.isFeatureActive(pocketId));
}

TEST(M9ReleaseGate, GATE_G2_ASaveAtARolledBackPositionRoundTripsTheWholeHistory) {
    std::string saved;
    ObjectId bodyId = kInvalidObjectId;
    ObjectId pocketId = kInvalidObjectId;
    {
        ChainFixture fx;
        ASSERT_TRUE(fx.document.recompute().success);
        bodyId = fx.bodyId;
        pocketId = fx.pocket->id();
        ASSERT_TRUE(fx.document.setRollbackPosition(bodyId, 1));
        ASSERT_TRUE(fx.document.recompute().success);
        ExpectRel(fx.volume(), 100000.0);
        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(fx.document, out));
        saved = out.str();
    }

    // The FULL history is in the file -- both features -- and the position came
    // back with it. A rollback implemented as truncation would pass a volume
    // check here and fail on the feature count.
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    std::istringstream in(saved);
    LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);

    ASSERT_EQ(loaded.document->bodies().front()->features().size(), 2u)
        << "the rolled-back feature was not written to the file";
    EXPECT_EQ(loaded.document->rollbackPosition(bodyId), 1u)
        << "the document reopened at a different step than it was saved at";
    // WRITTEN BECAUSE A MUTATION SURVIVED (M9 battery N7). Restoring the
    // position through the recording facade instead of the restore path left
    // M9_UNDO_402 green, because THAT fixture has no rollback position and
    // never reaches the line. A loaded document must arrive with no history
    // whatever it contains (ADR-M9-001).
    EXPECT_EQ(loaded.document->undoDepth(), 0u)
        << "loading recorded an undo step; the document arrived with a history of its own "
           "construction";
    EXPECT_FALSE(loaded.document->undo());

    ASSERT_TRUE(loaded.document->recompute().success);
    ExpectRel(loaded.document->massProperties().volumeMm3, 100000.0);

    // Rolling forward in the reloaded document produces the pocket again, from
    // the file's own record of it.
    ASSERT_TRUE(loaded.document->setRollbackPosition(bodyId, Body::kNoRollback));
    ASSERT_TRUE(loaded.document->recompute().success);
    ExpectRel(loaded.document->massProperties().volumeMm3, 94000.0);
    EXPECT_TRUE(loaded.document->isFeatureActive(pocketId));
}

TEST(M9ReleaseGate, GATE_G3_RollbackIsUndoableAndClampsAtBothEnds) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const ObjectId bodyId = fx.bodyId;

    ASSERT_TRUE(fx.document.setRollbackPosition(bodyId, 1));
    EXPECT_EQ(fx.document.nextUndoLabel(), "Roll back Body001");
    ASSERT_TRUE(fx.document.undo());
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);

    // Before the first feature: nothing is evaluated, so the body has no tail
    // and mass is not current -- an empty part is not a part with stale
    // numbers.
    ASSERT_TRUE(fx.document.setRollbackPosition(bodyId, 0));
    fx.document.recompute();
    EXPECT_FALSE(fx.document.massProperties().valid);

    // Past the end clamps rather than hiding a feature a later edit appends.
    ASSERT_TRUE(fx.document.setRollbackPosition(bodyId, 99));
    EXPECT_EQ(fx.document.rollbackPosition(bodyId), 2u);
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);

    EXPECT_FALSE(fx.document.setRollbackPosition(fx.pad->id(), 0))
        << "a feature id was accepted as a body id";
}


TEST(M9ReleaseGate, GATE_G4_RollingBackPastAFailingFeatureLeavesAHealthyDocument) {
    // WRITTEN BECAUSE A MUTATION SURVIVED (M9 battery N4). Neutering the graph's
    // skip predicate -- so rolled-back features are still COMPUTED -- left every
    // other gate green, because the tail and the presenter already filter on
    // activity and the volumes came out the same.
    //
    // What that mutation actually breaks is the reason rollback exists: rolling
    // back to before a broken feature is how a user gets a working model back
    // while they fix it. If the hidden feature is still evaluated, the document
    // keeps reporting a failure for something the user is not even looking at,
    // and `recompute().success` stays false for ever.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // Break the pocket.
    // 0.0, not a negative value: since M17.8 a negative length is a
    // DIRECTION and builds a perfectly good solid on the other side of the
    // plane (ADR-M17-031). Zero is what still has no magnitude, and failing
    // the feature is what this gate needs -- the value is the lever, not the
    // subject.
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 0.0));
    ASSERT_FALSE(fx.document.recompute().success);
    ASSERT_EQ(fx.pocket->state(), ComputeState::Failed);

    // Roll back to before it: the model is healthy again, and says so.
    ASSERT_TRUE(fx.document.setRollbackPosition(fx.bodyId, 1));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_TRUE(report.success)
        << "a rolled-back feature is still being evaluated, so its failure still counts";
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), 100000.0);

    // And the failure is still there waiting when the user rolls forward --
    // rollback hid it, it did not fix it.
    ASSERT_TRUE(fx.document.setRollbackPosition(fx.bodyId, Body::kNoRollback));
    EXPECT_FALSE(fx.document.recompute().success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Failed);
}

// --- Gate H: undo across a failure --------------------------------------------

TEST(M9ReleaseGate, GATE_H_UndoAcrossAFailureRestoresTheValidStateAndRedoRestoresTheFailure) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.volume(), 94000.0);

    // 0.0, not a negative value: since M17.8 a negative length is a
    // DIRECTION and builds a perfectly good solid on the other side of the
    // plane (ADR-M17-031). Zero is what still has no magnitude, and failing
    // the feature is what this gate needs -- the value is the lever, not the
    // subject.
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 0.0));
    fx.document.recompute();
    ASSERT_EQ(fx.pocket->state(), ComputeState::Failed);
    ASSERT_FALSE(fx.document.massProperties().valid);
    const std::string diagnostic = FailureMessageFor(fx.document, fx.pocket->id());
    EXPECT_FALSE(diagnostic.empty());

    // Undo is not a repair tool -- but here the failure IS the thing being
    // undone, so the model must come back valid.
    ASSERT_TRUE(fx.document.undo());
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Valid);
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), 94000.0);

    // Redo restores the failure FAITHFULLY, with the same diagnostic. An undo
    // system that quietly healed a model the user never had healed would be
    // presenting a state that never existed.
    ASSERT_TRUE(fx.document.redo());
    fx.document.recompute();
    EXPECT_EQ(fx.pocket->state(), ComputeState::Failed);
    EXPECT_FALSE(fx.document.massProperties().valid);
    EXPECT_EQ(FailureMessageFor(fx.document, fx.pocket->id()), diagnostic);
}


// --- Combinations: where four review rounds have found this project's defects -

TEST(M9ReleaseGate, GATE_X1_UndoOfADeletionShiftsWhatTheRollbackPositionMeans) {
    // Two mechanisms that both index the same array. Undo restores a feature AT
    // an index; the rollback position is a COUNT against that array. Restoring a
    // feature before the position moves what the position refers to, and each
    // side is tested alone (C4, G3) while the pair was not. Written because
    // "the combination has no test" was on this milestone's own
    // least-confident list, and a worry that can be turned into a test should
    // be.
    //
    // Body: Pad001, SparePad, Pocket001 -- SparePad independent, so removing it
    // is recordable.
    PartDocument document{"M9Combo"};
    CountingKernel kernel;
    CountingSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Aluminium", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("PocketDepth", 10.0, UnitType::Millimeter);
    const auto rectangle = [](Sketch& sketch, double x0, double y0, double x1, double y1) {
        sketch.addLine(Vec2{x0, y0}, Vec2{x1, y0});
        sketch.addLine(Vec2{x1, y0}, Vec2{x1, y1});
        sketch.addLine(Vec2{x1, y1}, Vec2{x0, y1});
        sketch.addLine(Vec2{x0, y1}, Vec2{x0, y0});
    };
    Sketch& padSketch = document.addSketch("PadSketch");
    rectangle(padSketch, 0, 0, 100, 50);
    Sketch& spareSketch = document.addSketch("SpareSketch");
    rectangle(spareSketch, 200, 0, 240, 30);
    Sketch& pocketSketch = document.addSketch("PocketSketch");
    rectangle(pocketSketch, 10, 10, 30, 40);

    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", padSketch.id(), length.id());
    PadFeature& spare = document.addPadFeature(body, "SparePad", spareSketch.id(), length.id());
    document.addPocketFeature(body, "Pocket001", pad.id(), pocketSketch.id(), depth.id());
    ASSERT_TRUE(document.recompute().success);
    const ObjectId spareId = spare.id();

    // Remove the middle feature, THEN roll back to two features.
    ASSERT_TRUE(document.removeObject(spareId));
    ASSERT_EQ(body.features().size(), 2u); // Pad001, Pocket001
    ASSERT_TRUE(document.setRollbackPosition(body.id(), 2));
    ASSERT_TRUE(document.recompute().success);
    ExpectRel(document.massProperties().volumeMm3, 94000.0);

    // Now undo the removal. The feature comes back at index 1, so the position
    // of 2 now cuts BEFORE the pocket -- which is the honest reading: a count
    // counts features, and there is one more of them.
    ASSERT_TRUE(document.undo());
    ASSERT_EQ(body.features().size(), 3u);
    EXPECT_EQ(body.features()[1]->id(), spareId);
    EXPECT_EQ(document.rollbackPosition(body.id()), 2u);
    ASSERT_TRUE(document.recompute().success);
    EXPECT_FALSE(document.isFeatureActive(body.features()[2]->id()))
        << "the pocket should now be beyond the position";

    // Whatever the position means, the document must still be SAVABLE and
    // reload identically -- that is the invariant, not the number.
    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->bodies().front()->features().size(), 3u);
    EXPECT_EQ(loaded.document->rollbackPosition(body.id()), 2u);
}

TEST(M9ReleaseGate, GATE_X2_ChainResolutionWalksPastSuppressedAndRolledBackTogether) {
    // `activeChainBase` handles both by construction -- both are "inactive" --
    // but the COMBINATION had no test, and combinations are where every review
    // round of this project has found its defects.
    //
    // Sketch -> Pad -> Pocket -> Fillet, with the pocket SUPPRESSED and the
    // fillet ROLLED BACK. What is left is the bare pad.
    ChainFixture fx;
    Body& body = *fx.document.bodies().front();
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    fx.document.addFilletFeature(body, "Fillet001", fx.pocket->id(), radius.id());
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(body.features().size(), 3u);

    ASSERT_TRUE(fx.document.setSuppressed(fx.pocket->id(), true));
    ASSERT_TRUE(fx.document.setRollbackPosition(fx.bodyId, 2)); // hide the fillet
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.massProperties().valid);
    // The bare pad is what is left: the pocket is suppressed and the fillet is
    // rolled back, so neither contributes.
    ExpectRel(fx.volume(), 100000.0);

    // Roll the fillet back in: it must now dress the PAD, resolving past the
    // still-suppressed pocket.
    ASSERT_TRUE(fx.document.setRollbackPosition(fx.bodyId, Body::kNoRollback));
    ASSERT_TRUE(fx.document.recompute().success);
    const double kPi = 3.14159265358979323846;
    const double roundedPad = 70656.0 + 26752.0 + 632.0 * kPi + (4.0 / 3.0) * kPi * 8.0;
    ExpectRel(fx.volume(), roundedPad, 1e-6);

    // Unsuppress: the fillet dresses the pocketed pad again, all the way back.
    ASSERT_TRUE(fx.document.setSuppressed(fx.pocket->id(), false));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_LT(fx.volume(), roundedPad) << "the pocket is not being cut again";
}

// --- The history is session state, not document state ------------------------

TEST(M9ReleaseGate, M9_UNDO_402_ALoadedDocumentStartsWithAnEmptyHistory) {
    // ADR-M9-001: undo history is NOT persisted. A document reopened later must
    // not offer to undo into somebody else's session -- and, more sharply, the
    // restore paths must not record, or every load would arrive with a history
    // of its own construction.
    std::string saved;
    {
        ChainFixture fx;
        ASSERT_TRUE(fx.document.recompute().success);
        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(fx.document, out));
        saved = out.str();
    }
    std::istringstream in(saved);
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->undoDepth(), 0u);
    EXPECT_EQ(loaded.document->redoDepth(), 0u);
    EXPECT_FALSE(loaded.document->undo());
}

} // namespace
