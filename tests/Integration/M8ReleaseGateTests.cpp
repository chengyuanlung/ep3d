// M8.1 release gates (M8 spec 7), executed against the REAL solver and REAL
// OCCT geometry.
//
// Every expected number is computed by hand from the parameter values, never
// read back from the kernel, and the selectivity gates use COUNTERS, not equal
// final values -- the lesson M7's review carved into Gate K.
//
//   pad   : 100 x 50 x 20               = 100000 mm^3
//   pocket: 20 x 30 rectangle, 10 deep  = -6000  mm^3   -> 94000
//   width 100 -> 120                    -> 120x50x20 - 6000 = 114000
//   depth 10 -> 20 (through)           -> 120000 - 12000    = 108000
//
// The pocket profile sits at (10,10)..(30,40) on the SAME plane as the pad
// sketch, wholly inside the pad's footprint, so the tool prism is entirely
// contained in the base and the analytical volumes are exact.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Viewer/DocumentPresenter.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>

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

// Pad + Pocket on one Body, dimensions driven by Parameters. The PAD sketch is
// constraint-free (its geometry is exact); Width drives it indirectly through
// nothing -- so to make the Width edit REAL, the pad sketch is constrained the
// M5 way: Fix + Horizontal/Vertical/Coincident + Width/Height Lengths. The
// POCKET sketch is plain free geometry: what M8.1 is proving is the chain, not
// a second constraint system.
struct ChainFixture {
    PartDocument document{"M8Chain"};
    CountingKernel kernel;
    CountingSolver solver;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* padLength = nullptr;
    Parameter* depth = nullptr;
    Sketch* padSketch = nullptr;
    Sketch* pocketSketch = nullptr;
    PadFeature* pad = nullptr;
    PocketFeature* pocket = nullptr;

    ChainFixture() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);
        width = &document.addParameter("Width", 100.0, UnitType::Millimeter);
        height = &document.addParameter("Height", 50.0, UnitType::Millimeter);
        padLength = &document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        depth = &document.addParameter("PocketDepth", 10.0, UnitType::Millimeter);

        Sketch& ps = document.addSketch("PadSketch");
        padSketch = &ps;
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
        pocketSketch = &ks;
        ks.addLine(Vec2{10, 10}, Vec2{30, 10});
        ks.addLine(Vec2{30, 10}, Vec2{30, 40});
        ks.addLine(Vec2{30, 40}, Vec2{10, 40});
        ks.addLine(Vec2{10, 40}, Vec2{10, 10});

        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", ps.id(), padLength->id());
        pocket = &document.addPocketFeature(body, "Pocket001", pad->id(), ks.id(), depth->id());
    }

    double volume() const { return document.massProperties().volumeMm3; }
};

// --- Gate B: the chain produces the analytical volume ------------------------

TEST(M8ReleaseGate, GATE_B_PadMinusPocketMatchesHandComputedVolume) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Valid);

    // 100*50*20 - 20*30*10 = 100000 - 6000 = 94000 mm^3.
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), 94000.0);
    // 94000 mm^3 * 2700 kg/m^3 = 0.2538 kg.
    ExpectRel(fx.document.massProperties().massKg, 0.2538);
}

// --- Gate C: an upstream Parameter edit rebuilds the WHOLE chain -------------

TEST(M8ReleaseGate, GATE_C_WidthEditRebuildsPadAndPocket) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.width->id()).success);

    // 120*50*20 - 6000 = 114000. If the chain edge were missing, the pocket
    // would still be cutting yesterday's 100-wide pad and mass properties
    // (which follow the pocket) would report 94000 -- current-looking,
    // analytically wrong. That is the defect ADR-M8-001's edge exists to
    // prevent, and this is its gate.
    ExpectRel(fx.volume(), 114000.0);
}

// --- Gate D: a pocket-only edit touches pocket and mass, nothing upstream ----

TEST(M8ReleaseGate, GATE_D_DepthEditIsSelective) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    const int solvesBefore = fx.solver.calls;
    const int extrudesBefore = fx.kernel.extrudes;
    const int subtractsBefore = fx.kernel.subtracts;

    // Through: depth == pad length. 100000 - 20*30*20 = 88000.
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 20.0));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.depth->id()).success);
    ExpectRel(fx.volume(), 88000.0);

    // COUNTERS, not equal values (spec 21, and M7 review's Gate K lesson):
    // no sketch re-solved; exactly ONE extrude ran (the pocket's tool -- if the
    // pad had rebuilt too there would be two); exactly one new subtract.
    EXPECT_EQ(fx.solver.calls, solvesBefore) << "a depth edit re-solved a sketch";
    EXPECT_EQ(fx.kernel.extrudes, extrudesBefore + 1)
        << "a depth edit rebuilt more than the pocket's tool";
    EXPECT_EQ(fx.kernel.subtracts, subtractsBefore + 1);
}

// --- Gate E: failure isolation and recovery ----------------------------------

TEST(M8ReleaseGate, GATE_E_InvalidDepthFailsThePocketOnlyAndRecovers) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const double good = fx.volume();

    for (double bad : {0.0, -5.0, std::numeric_limits<double>::quiet_NaN()}) {
        ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), bad));
        fx.document.recompute();
        // The pocket fails; the BASE stays valid -- failure never travels
        // upstream (M8 spec 6).
        EXPECT_EQ(fx.pocket->state(), ComputeState::Failed);
        EXPECT_EQ(fx.pad->state(), ComputeState::Valid);
        // Downstream is stale and SAYS so, exactly as M7's Gate H requires.
        EXPECT_FALSE(fx.document.massProperties().valid);
    }

    // Recovery through the ordinary edit path.
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 10.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Valid);
    EXPECT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), good);
}

// --- Gate E adjunct: a FAILED base with a retained stale shape ---------------

TEST(M8ReleaseGate, GATE_E2_APocketNeverCutsAFailedBasesRetainedShape) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int subtractsBefore = fx.kernel.subtracts;

    // Fail the PAD -- not the pocket -- while it still RETAINS its last valid
    // shape (ADR-M3-001/004 keeps it byte-for-byte). The base is present,
    // resolvable, and carrying a perfectly usable-looking stale solid: the one
    // situation where cutting would produce a current-looking wrong part.
    ASSERT_TRUE(fx.document.setParameterValue(fx.padLength->id(), -1.0));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    ASSERT_EQ(fx.pad->state(), ComputeState::Failed);
    ASSERT_TRUE(fx.pad->currentShape().isValid()) << "the retained shape IS the trap";

    // The contract, asserted at its AUTHORITATIVE sources rather than the
    // feature cache. The engine BLOCKS dependents of failed nodes -- it never
    // invokes them -- so the pocket's cached Feature::state() legitimately
    // still reads Valid while the graph, which schedules, says Failed
    // (ADR-M3-004: the graph is authoritative; the first draft of this gate
    // read the cache and learned the difference).
    EXPECT_EQ(fx.document.dependencyGraph().state(fx.pocket->id()), ComputeState::Failed)
        << "the pocket was not blocked by its failed base";
    // And by COUNTER: no cut ran against the stale base. This is the assertion
    // that would catch an engine that started invoking dependents of failed
    // nodes -- the pocket's own base-state check is the second line of defense
    // behind it (see PocketFeature::recompute).
    EXPECT_EQ(fx.kernel.subtracts, subtractsBefore)
        << "a boolean ran against a failed base's retained shape";
    EXPECT_FALSE(fx.document.massProperties().valid);

    // Recovery flows back down the chain.
    ASSERT_TRUE(fx.document.setParameterValue(fx.padLength->id(), 20.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Valid);
    ExpectRel(fx.volume(), 94000.0);
}

// --- Gate F: save / fresh load / edit ----------------------------------------

TEST(M8ReleaseGate, GATE_F_ChainSurvivesSaveLoadAndStillRebuilds) {
    std::string saved;
    ObjectId widthId = kInvalidObjectId;
    {
        ChainFixture fx;
        ASSERT_TRUE(fx.document.recompute().success);
        widthId = fx.width->id();
        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(fx.document, out));
        saved = out.str();
    }

    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    std::istringstream in(saved);
    LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);

    ASSERT_TRUE(loaded.document->recompute().success);
    ExpectRel(loaded.document->massProperties().volumeMm3, 94000.0);

    // The chain reference survived SEMANTICALLY: the edit drives both features
    // in a fresh document with fresh backends.
    ASSERT_TRUE(loaded.document->setParameterValue(widthId, 120.0));
    ASSERT_TRUE(loaded.document->recomputeFrom(widthId).success);
    ExpectRel(loaded.document->massProperties().volumeMm3, 114000.0);
}

// --- Gate G: deleting chain members ------------------------------------------

TEST(M8ReleaseGate, GATE_G_DeletingThePocketIsCleanDeletingTheBaseIsLoud) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // Removing the POCKET is an ordinary removal; the pad becomes the tail
    // again. Mass must be re-pointed by the caller (the UI's removal command
    // does this); here the observable contract is: no crash, pad still valid.
    const ObjectId pocketId = fx.pocket->id();
    ASSERT_TRUE(fx.document.removeObject(pocketId));
    fx.document.recompute();
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);

    // Removing the BASE under a pocket must fail the pocket with a diagnostic
    // naming the situation -- never a crash, never a silent cut against a
    // stale shape.
    ChainFixture fx2;
    ASSERT_TRUE(fx2.document.recompute().success);
    ASSERT_TRUE(fx2.document.removeObject(fx2.pad->id()));
    fx2.document.recompute();
    EXPECT_EQ(fx2.pocket->state(), ComputeState::Failed);
    EXPECT_FALSE(fx2.document.massProperties().valid);
}

// --- Gate H adjunct: a save with a deleted base is refused -------------------

TEST(M8ReleaseGate, GATE_H_SavingAPocketWhoseBaseIsGoneIsRefused) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.removeObject(fx.pad->id()));

    // The file would name a base feature the loader must reject, so the SAVE
    // must refuse (ADR-M3-008) -- a save that emits an unopenable document is
    // C2's defect class, and M8 must not reintroduce it through the chain.
    std::ostringstream out;
    const SaveResult saved = savePartDocument(fx.document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("base feature"), std::string::npos) << saved.message;
}

// --- Gate H: the viewer shows the chain TAIL, not tail plus intermediates ----

TEST(M8ReleaseGate, GATE_H_PresenterShowsOnlyTheChainTail) {
    ChainFixture fx;
    DocumentPresenter presenter(fx.document);
    ASSERT_TRUE(fx.document.recompute().success);

    // ONE displayable solid: the pocketed result. The pad is a chain
    // intermediate now -- drawing it underneath its own successor would
    // overlap two versions of the same material and visually erase the pocket
    // (ADR-M8-003). Without the consumed-solid rule this returns two.
    const std::vector<ObjectId> solids = presenter.displayableSolids();
    ASSERT_EQ(solids.size(), 1u);
    EXPECT_EQ(solids.front(), fx.pocket->id());

    // And when the pocket FAILS, the last valid tail result would be stale --
    // the presenter must not fall back to quietly showing the bare pad as if
    // the part had healed. The pocket keeps its retained shape but is not
    // Valid, so nothing in this body displays; stale is visible as absence,
    // never as a healthy-looking wrong solid.
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), -1.0));
    fx.document.recompute();
    EXPECT_EQ(fx.pocket->state(), ComputeState::Failed);
    const std::vector<ObjectId> afterFailure = presenter.displayableSolids();
    EXPECT_TRUE(afterFailure.empty());
}

// --- Chained pockets: the chain is not limited to depth one ------------------

TEST(M8ReleaseGate, TwoChainedPocketsCutInSequence) {
    ChainFixture fx;
    Sketch& second = fx.document.addSketch("PocketSketch2");
    second.addLine(Vec2{60, 10}, Vec2{80, 10});
    second.addLine(Vec2{80, 10}, Vec2{80, 40});
    second.addLine(Vec2{80, 40}, Vec2{60, 40});
    second.addLine(Vec2{60, 40}, Vec2{60, 10});
    Parameter& depth2 = fx.document.addParameter("PocketDepth2", 5.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    PocketFeature& pocket2 =
        fx.document.addPocketFeature(body, "Pocket002", fx.pocket->id(), second.id(), depth2.id());

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(pocket2.state(), ComputeState::Valid);

    // 100000 - 6000 - 20*30*5 = 91000 mm^3, with mass following the NEW tail.
    ExpectRel(fx.volume(), 91000.0);
}

} // namespace
