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
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
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

constexpr double kPi = 3.14159265358979323846;
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
    ShapeResult revolveProfile(const PlanarProfileDefinition& profile, const Vec3& axisOriginMm,
                               const Vec3& axisDirection, double angleRad) override {
        ++revolveCallCount;
        return inner_.revolveProfile(profile, axisOriginMm, axisDirection, angleRad);
    }
    ShapeResult filletAllEdges(const KernelShape& shape, double radiusMm) override {
        ++filletCallCount;
        return inner_.filletAllEdges(shape, radiusMm);
    }
    ShapeResult chamferAllEdges(const KernelShape& shape, double distanceMm) override {
        ++chamferCallCount;
        return inner_.chamferAllEdges(shape, distanceMm);
    }
    int extrudes = 0;
    int subtracts = 0;
    int revolveCallCount = 0;
    int filletCallCount = 0;
    int chamferCallCount = 0;

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



// --- M8.2: Revolve gates -----------------------------------------------------
//
// The revolve fixture: rectangle u in [10,30], v in [0,40] on WorldXY, revolved
// about the sketch's own AXIS LINE at u=0 (a separate line entity, NOT a loop
// member). Full turn: annular cylinder, V = pi*(30^2-10^2)*50 = pi*40000.
//
// The axis line is added FIRST, before the profile, precisely so that any
// implementation that grabbed "the first line in the sketch" as the axis would
// pick... the axis, correctly, by accident. So the fixture ALSO builds a
// variant with the axis added LAST, and the two must agree -- entity order is
// not identity (ADR-M6-004), for features as much as for imports.

namespace {

// pi*(30^2-10^2)*50 = 40000*pi. The v extent is 50, NOT 40, and the reason is
// a mutation that slipped a weak gate: with v=40, revolving about the axis
// (annulus, pi*800*40) and revolving about the profile's own bottom edge --
// what an axis-resolved-by-position bug produces -- give the SAME 32000*pi by
// pure arithmetic coincidence (pi*1600*20). GATE_RB2 could not tell them
// apart; only GATE_RG caught the mutation, one step removed. At v=50 the two
// differ (40000*pi vs 50000*pi) and GATE_RB2 discriminates directly.
constexpr double kAnnulusVolume = kPi * 40000.0;

struct RevolveFixture {
    PartDocument document{"M8Revolve"};
    CountingKernel kernel;
    CountingSolver solver;
    Parameter* angle = nullptr;
    Sketch* sketch = nullptr;
    RevolveFeature* revolve = nullptr;
    SketchEntityId axis{};

    explicit RevolveFixture(bool axisFirst = true) {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);
        angle = &document.addParameter("RevolveAngle", 2.0 * kPi, UnitType::Radian);

        Sketch& s = document.addSketch("RevolveSketch");
        sketch = &s;
        if (axisFirst) axis = s.addLine(Vec2{0, -5}, Vec2{0, 55});
        s.addLine(Vec2{10, 0}, Vec2{30, 0});
        s.addLine(Vec2{30, 0}, Vec2{30, 50});
        s.addLine(Vec2{30, 50}, Vec2{10, 50});
        s.addLine(Vec2{10, 50}, Vec2{10, 0});
        if (!axisFirst) axis = s.addLine(Vec2{0, -5}, Vec2{0, 55});

        Body& body = document.addBody("Body001");
        revolve = &document.addRevolveFeature(body, "Revolve001", s.id(), axis, angle->id());
    }

    double volume() const { return document.massProperties().volumeMm3; }
};

} // namespace

TEST(M8ReleaseGate, GATE_RB_FullRevolveMatchesTheAnnulusOracle) {
    RevolveFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.revolve->state(), ComputeState::Valid);
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), kAnnulusVolume, 1e-6);
}

TEST(M8ReleaseGate, GATE_RB2_AxisEntityOrderIsNotIdentity) {
    RevolveFixture first(true);
    RevolveFixture last(false);
    ASSERT_TRUE(first.document.recompute().success);
    ASSERT_TRUE(last.document.recompute().success);

    // Same drawing, axis line stored first vs last: identical solid. An
    // implementation resolving the axis by position instead of by
    // SketchEntityId would revolve about a profile edge in one of the two.
    ExpectRel(first.volume(), last.volume(), 1e-9);
    ExpectRel(first.volume(), kAnnulusVolume, 1e-6);
}

TEST(M8ReleaseGate, GATE_RC_HalfAngleHalvesTheVolumeSelectively) {
    RevolveFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const double full = fx.volume();

    const int solvesBefore = fx.solver.calls;
    const int revolvesBefore = fx.kernel.revolveCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.angle->id(), kPi));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.angle->id()).success);

    // The ratio (a scale error cannot hide), and the counters (equal final
    // values are not evidence -- spec 21).
    ExpectRel(fx.volume() / full, 0.5, 1e-9);
    EXPECT_EQ(fx.solver.calls, solvesBefore) << "an angle edit re-solved a sketch";
    EXPECT_EQ(fx.kernel.revolveCallCount, revolvesBefore + 1);
}

TEST(M8ReleaseGate, GATE_RD_ARevolveIsALegalChainBase) {
    RevolveFixture fx;
    // A pocket cut into the revolved solid: prism u in [2,7], v in [5,15],
    // depth 5 (+Z from the XY plane). The annulus about the world-Y axis spans
    // x^2+z^2 in [100,900] for y in [0,40]; every prism point has
    // x^2+z^2 <= 49+25 = 74 < 100 -- the prism sits INSIDE THE HOLE of the
    // annulus, so the cut legally removes NOTHING and the volume is unchanged.
    //
    // A second pocket at u in [12,17] (x^2+z^2 up to 289+25, straddling the
    // inner wall) would be a partial cut with no closed-form oracle -- which is
    // exactly why THIS fixture uses the hole: the oracle stays analytical.
    Sketch& ps = fx.document.addSketch("PocketSketch");
    ps.addLine(Vec2{2, 5}, Vec2{7, 5});
    ps.addLine(Vec2{7, 5}, Vec2{7, 15});
    ps.addLine(Vec2{7, 15}, Vec2{2, 15});
    ps.addLine(Vec2{2, 15}, Vec2{2, 5});
    Parameter& depth = fx.document.addParameter("PocketDepth", 5.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    PocketFeature& pocket =
        fx.document.addPocketFeature(body, "Pocket001", fx.revolve->id(), ps.id(), depth.id());

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(pocket.state(), ComputeState::Valid);

    // The CAPABILITY claim, proved: PocketFeature consumes a Revolve with no
    // change to either type, because bases resolve through ISolidFeature
    // (ADR-M8-001). And the disjoint cut is LEGAL (ADR-M8-002): volume
    // unchanged, not an error.
    ExpectRel(fx.volume(), kAnnulusVolume, 1e-6);
}

TEST(M8ReleaseGate, GATE_RE_RevolveFailureModesAndRecovery) {
    RevolveFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // Angle out of range -> Failed, mass not current.
    ASSERT_TRUE(fx.document.setParameterValue(fx.angle->id(), 7.0)); // > 2*pi
    fx.document.recompute();
    EXPECT_EQ(fx.revolve->state(), ComputeState::Failed);
    EXPECT_FALSE(fx.document.massProperties().valid);

    // Recovery.
    ASSERT_TRUE(fx.document.setParameterValue(fx.angle->id(), 2.0 * kPi));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.revolve->state(), ComputeState::Valid);
    ExpectRel(fx.volume(), kAnnulusVolume, 1e-6);
}

TEST(M8ReleaseGate, GATE_RE2_AWrongUnitAngleParameterIsRefusedWithADiagnostic) {
    RevolveFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // A MILLIMETRE parameter wired as the angle. 3.1 "millimetres" would read
    // as a perfectly plausible 3.1-radian sweep -- the silent unit confusion
    // the Radian check exists to refuse.
    Parameter& wrong = fx.document.addParameter("NotAnAngle", 3.1, UnitType::Millimeter);
    Sketch& s2 = fx.document.addSketch("Sketch2");
    const SketchEntityId axis2 = s2.addLine(Vec2{0, 0}, Vec2{0, 40});
    s2.addLine(Vec2{10, 0}, Vec2{30, 0});
    s2.addLine(Vec2{30, 0}, Vec2{30, 40});
    s2.addLine(Vec2{30, 40}, Vec2{10, 40});
    s2.addLine(Vec2{10, 40}, Vec2{10, 0});
    Body& body = *fx.document.bodies().front();
    RevolveFeature& bad =
        fx.document.addRevolveFeature(body, "Revolve002", s2.id(), axis2, wrong.id());

    fx.document.recompute();
    EXPECT_EQ(bad.state(), ComputeState::Failed);
}

TEST(M8ReleaseGate, GATE_RF_RevolveSurvivesSaveLoadAndStillRebuilds) {
    std::string saved;
    ObjectId angleId = kInvalidObjectId;
    {
        RevolveFixture fx;
        ASSERT_TRUE(fx.document.recompute().success);
        angleId = fx.angle->id();
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
    ExpectRel(loaded.document->massProperties().volumeMm3, kAnnulusVolume, 1e-6);

    // The axis reference survived SEMANTICALLY: the half-angle edit still
    // produces exactly half, in a fresh document with fresh backends.
    ASSERT_TRUE(loaded.document->setParameterValue(angleId, kPi));
    ASSERT_TRUE(loaded.document->recomputeFrom(angleId).success);
    ExpectRel(loaded.document->massProperties().volumeMm3, kAnnulusVolume / 2.0, 1e-6);
}

TEST(M8ReleaseGate, GATE_RG_DeletingTheAxisLineFailsTheRevolveLoudly) {
    RevolveFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // The axis is an ordinary entity; deleting it must fail the revolve with a
    // diagnostic, never crash, never fall back to some other line.
    ASSERT_TRUE(fx.document.removeSketchEntity(fx.sketch->id(), fx.axis));
    fx.document.recompute();
    EXPECT_EQ(fx.revolve->state(), ComputeState::Failed);
    EXPECT_FALSE(fx.document.massProperties().valid);
}



// --- M8.3: Fillet / Chamfer gates --------------------------------------------

TEST(M8ReleaseGate, GATE_FB_PadThenFilletMatchesTheMinkowskiOracle) {
    ChainFixture fx; // 100 x 50 x 20 pad (constrained sketch)
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    // The ChainFixture already chains a pocket onto the pad, and consumption
    // is UNIQUE (round 1's R1-C1): adding the fillet while the pocket still
    // consumes the pad is a diamond and is refused. The pocket is removed
    // FIRST -- the original order here was itself a transient diamond.
    ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fillet.state(), ComputeState::Valid);

    // The Minkowski rounded box (see OcctFilletChamferTests for the derivation):
    // 70656 + 26752 + 632*pi + (4/3)*pi*8.
    const double expected = 70656.0 + 26752.0 + 632.0 * kPi + (4.0 / 3.0) * kPi * 8.0;
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), expected, 1e-6);

    // Selectivity: a radius edit runs ONE fillet call, no solve, no extrude.
    const int solves = fx.solver.calls;
    const int extrudes = fx.kernel.extrudes;
    const int fillets = fx.kernel.filletCallCount;
    ASSERT_TRUE(fx.document.setParameterValue(radius.id(), 1.0));
    ASSERT_TRUE(fx.document.recomputeFrom(radius.id()).success);
    EXPECT_EQ(fx.solver.calls, solves);
    EXPECT_EQ(fx.kernel.extrudes, extrudes);
    EXPECT_EQ(fx.kernel.filletCallCount, fillets + 1);
}

TEST(M8ReleaseGate, GATE_CB_RevolveThenChamferIsAThreeKindChain) {
    RevolveFixture fx; // annulus about the axis line, pi*40000
    // Chamfering the annulus's FOUR rim edges (outer r=30 and inner r=10, both
    // ends): Pappus per rim with the triangle centroid a third of the way in
    // from each rim toward the material:
    //   outer rims: 2 * 2*pi*(30 - 2/3)*2 = 8*pi*(88/3)
    //   inner rims: 2 * 2*pi*(10 + 2/3)*2 = 8*pi*(32/3)
    // total removed = 8*pi*40 = 320*pi.
    //
    // The oracle also RELIES on OCCT treating the full revolution's SEAM edges
    // (the degenerate u=0 boundary the revolve leaves on each face) as
    // un-chamferable no-ops when the dedup map hands them to Add -- if a
    // future OCCT started beveling the seam, this gate would (correctly) go
    // red with a volume below 320*pi. Stated because the reliance is implicit
    // in the arithmetic (review round 1, R1 minor).
    Parameter& distance = fx.document.addParameter("ChamferDistance", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    ChamferFeature& chamfer =
        fx.document.addChamferFeature(body, "Chamfer001", fx.revolve->id(), distance.id());

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(chamfer.state(), ComputeState::Valid);

    const double expected = kAnnulusVolume - 320.0 * kPi;
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.volume(), expected, 1e-6);

    // Sketch -> Revolve -> Chamfer: a three-kind chain where the middle link
    // was created two slices after the chain machinery -- the capability
    // resolution paying out a second time.
}

TEST(M8ReleaseGate, GATE_FC_AnImpossibleRadiusFailsTheFilletOnlyAndRecovers) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
    ASSERT_TRUE(fx.document.recompute().success);
    const double good = fx.volume();

    // Radius 15 > half the 20mm thickness: the rounds collide, OCCT refuses.
    ASSERT_TRUE(fx.document.setParameterValue(radius.id(), 15.0));
    fx.document.recompute();
    EXPECT_EQ(fillet.state(), ComputeState::Failed);
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid) << "failure travelled upstream";
    EXPECT_FALSE(fx.document.massProperties().valid);

    ASSERT_TRUE(fx.document.setParameterValue(radius.id(), 2.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fillet.state(), ComputeState::Valid);
    ExpectRel(fx.volume(), good, 1e-9);
}

TEST(M8ReleaseGate, GATE_FD_FilletSurvivesSaveLoadAndStillRebuilds) {
    std::string saved;
    ObjectId radiusId = kInvalidObjectId;
    const double expected = 70656.0 + 26752.0 + 632.0 * kPi + (4.0 / 3.0) * kPi * 8.0;
    {
        ChainFixture fx;
        ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
        Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
        radiusId = radius.id();
        Body& body = *fx.document.bodies().front();
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
        ASSERT_TRUE(fx.document.recompute().success);
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
    ExpectRel(loaded.document->massProperties().volumeMm3, expected, 1e-6);

    // The radius still drives geometry after a fresh load: r=1 has its own
    // Minkowski value, computed the same way with r=1:
    // 98*48*18 + 2*1*(98*48+98*18+48*18) + pi*1*(98+48+18) + (4/3)*pi.
    ASSERT_TRUE(loaded.document->setParameterValue(radiusId, 1.0));
    ASSERT_TRUE(loaded.document->recomputeFrom(radiusId).success);
    const double expectedR1 =
        84672.0 + 2.0 * (4704.0 + 1764.0 + 864.0) + kPi * 164.0 + (4.0 / 3.0) * kPi;
    ExpectRel(loaded.document->massProperties().volumeMm3, expectedR1, 1e-6);
}

// --- M8.4: the Hole deferral, DEMONSTRATED rather than asserted --------------

TEST(M8ReleaseGate, GATE_HOLE_AHoleIsExpressibleTodayAsACirclePocket) {
    // ADR-M8-007 defers a dedicated Hole feature on the grounds that the
    // CAPABILITY already exists: a hole is a Pocket whose sketch is one
    // circle. A deferral resting on "it is expressible" owes a demonstration,
    // and this is it -- a 6mm-radius hole through the 20mm pad:
    // 100000 - pi*36*20.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
    Sketch& hole = fx.document.addSketch("HoleSketch");
    hole.addCircle(Vec2{50, 25}, 6.0);
    Parameter& depth = fx.document.addParameter("HoleDepth", 20.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    PocketFeature& pocket =
        fx.document.addPocketFeature(body, "Hole001", fx.pad->id(), hole.id(), depth.id());

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(pocket.state(), ComputeState::Valid);
    ExpectRel(fx.volume(), 100000.0 - kPi * 36.0 * 20.0, 1e-6);
}

// --- Round-1 regression gates (M8 independent review) ------------------------
// One per finding. The review's own mutations name what each must kill.

TEST(M8ReleaseGate, GATE_RH_RevolvingARectangleAboutItsOwnEdgeIsTheCanonicalCylinder) {
    // R1-M1: ADR-M8-005 and RevolveFeature.h claimed the axis MAY be a member
    // of the profile loop; the unconditional exclusion made that false (the
    // loop broke and the revolve was refused). The fallback in
    // RevolveFeature::recompute makes the claim true, and this gate is why
    // the claim is allowed to stand: rectangle x in [10,30], y in [0,50],
    // revolved about its OWN left edge -> solid cylinder r=20, h=50,
    // V = pi*400*50 = 20000*pi.
    PartDocument document{"M8AxisMember"};
    CountingKernel kernel;
    CountingSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Aluminium", 2700.0);
    Parameter& angle = document.addParameter("Angle", 2.0 * kPi, UnitType::Radian);

    Sketch& s = document.addSketch("RectSketch");
    const SketchEntityId leftEdge = s.addLine(Vec2{10, 0}, Vec2{10, 50});
    s.addLine(Vec2{10, 50}, Vec2{30, 50});
    s.addLine(Vec2{30, 50}, Vec2{30, 0});
    s.addLine(Vec2{30, 0}, Vec2{10, 0});

    Body& body = document.addBody("Body001");
    RevolveFeature& revolve =
        document.addRevolveFeature(body, "Revolve001", s.id(), leftEdge, angle.id());

    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(revolve.state(), ComputeState::Valid);
    ASSERT_TRUE(document.massProperties().valid);
    ExpectRel(document.massProperties().volumeMm3, kPi * 400.0 * 50.0, 1e-6);
}

TEST(M8ReleaseGate, GATE_RC2_AnAngleEditTouchesNothingOutsideTheRevolve) {
    // R3-M1: GATE_RC's fixture has no constraints and no second
    // counter-bearing node, so "no sketch re-solved" could never fire and
    // "one revolve call" was equally true of a global rebuild -- under an
    // engine degraded to recompute-everything, GATE_RC stayed green while
    // GATE_D and GATE_FB went red. This gate puts a CONSTRAINED pad+pocket
    // chain in the same document (built first, so mass still follows the
    // revolve, added last): now a degraded engine re-solves the pad sketch
    // and re-runs extrudes/subtracts, and every counter below notices.
    ChainFixture fx;
    Parameter& angle = fx.document.addParameter("Angle", 2.0 * kPi, UnitType::Radian);
    Sketch& rs = fx.document.addSketch("RevolveSketch");
    const SketchEntityId axis = rs.addLine(Vec2{0, -5}, Vec2{0, 55});
    rs.addLine(Vec2{10, 0}, Vec2{30, 0});
    rs.addLine(Vec2{30, 0}, Vec2{30, 50});
    rs.addLine(Vec2{30, 50}, Vec2{10, 50});
    rs.addLine(Vec2{10, 50}, Vec2{10, 0});
    Body& body2 = fx.document.addBody("Body002");
    RevolveFeature& revolve =
        fx.document.addRevolveFeature(body2, "Revolve001", rs.id(), axis, angle.id());

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(revolve.state(), ComputeState::Valid);
    const double full = fx.document.massProperties().volumeMm3; // 40000*pi
    ExpectRel(full, kPi * 40000.0, 1e-6);

    const int solves = fx.solver.calls;
    const int extrudes = fx.kernel.extrudes;
    const int subtracts = fx.kernel.subtracts;
    const int revolves = fx.kernel.revolveCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(angle.id(), kPi));
    ASSERT_TRUE(fx.document.recomputeFrom(angle.id()).success);

    ExpectRel(fx.document.massProperties().volumeMm3 / full, 0.5, 1e-9);
    EXPECT_EQ(fx.solver.calls, solves) << "an angle edit re-solved a sketch";
    EXPECT_EQ(fx.kernel.extrudes, extrudes) << "an angle edit re-ran an extrude";
    EXPECT_EQ(fx.kernel.subtracts, subtracts) << "an angle edit re-ran a boolean";
    EXPECT_EQ(fx.kernel.revolveCallCount, revolves + 1);
}

TEST(M8ReleaseGate, GATE_E3_APersistedFailureStillBlocksThePocketOnALaterEdit) {
    // R3-m1, wording corrected by round 2 (R3R2-M2): this gate exercises the
    // persisted-Failed seam -- pad already Failed in a previous pass, then a
    // depth edit dirties ONLY the pocket -- but it pins the TWO-LAYER SYSTEM
    // (engine barrier + PocketFeature's own base-state check), not the
    // barrier alone. Delete only the barrier and this stays green (the
    // feature-level check masks it); delete both layers and it goes red. The
    // barrier's only direct pins are DependencyGraphTests.StaleFailureGates*
    // and EdgeRewireAcrossFailedPrerequisite (list completed in round 3).
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.setParameterValue(fx.padLength->id(), -1.0));
    EXPECT_FALSE(fx.document.recompute().success);
    ASSERT_EQ(fx.pad->state(), ComputeState::Failed);

    const int subtracts = fx.kernel.subtracts;
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 12.0));
    const DocumentRecomputeReport report = fx.document.recomputeFrom(fx.depth->id());
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.document.dependencyGraph().state(fx.pocket->id()), ComputeState::Failed)
        << "the pocket was not blocked by its persistently failed base";
    EXPECT_EQ(fx.kernel.subtracts, subtracts)
        << "a boolean ran against a base that failed in a previous pass";
    EXPECT_FALSE(fx.document.massProperties().valid);
}

TEST(M8ReleaseGate, GATE_FE_AWidthEditRebuildsTheDressChain) {
    // R1-M3: with the base->dress edge deleted, every test stayed green --
    // no test drove an UPSTREAM edit through a dress feature. Width 100->120:
    // the pad rebuilds and the fillet must follow, or mass reports the
    // 100-wide Minkowski value while looking current.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fillet.state(), ComputeState::Valid);

    const int extrudes = fx.kernel.extrudes;
    const int fillets = fx.kernel.filletCallCount;
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.width->id()).success);

    // Minkowski rounded box for 120 x 50 x 20, r=2:
    // 116*46*16 + 2*2*(116*46 + 46*16 + 116*16) + pi*4*(116+46+16) + (4/3)*pi*8.
    const double expected =
        85376.0 + 31712.0 + kPi * 712.0 + (4.0 / 3.0) * kPi * 8.0;
    ASSERT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.document.massProperties().volumeMm3, expected, 1e-6);
    EXPECT_EQ(fx.kernel.extrudes, extrudes + 1);
    EXPECT_EQ(fx.kernel.filletCallCount, fillets + 1)
        << "the width edit rebuilt the pad but not the fillet consuming it";
}

TEST(M8ReleaseGate, GATE_FF_PresenterShowsOnlyTheDressChainTail) {
    // R1-M3's display half: Gate H covered Pocket only, so a dress feature
    // whose consumedSolidId() regressed to invalid would resurrect the
    // overlapping-solids defect with every test green.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
    Parameter& radius = fx.document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    FilletFeature& fillet =
        fx.document.addFilletFeature(body, "Fillet001", fx.pad->id(), radius.id());
    DocumentPresenter presenter(fx.document);

    ASSERT_TRUE(fx.document.recompute().success);
    const std::vector<ObjectId> solids = presenter.displayableSolids();
    ASSERT_EQ(solids.size(), 1u) << "the dress chain must display exactly its tail";
    EXPECT_EQ(solids.front(), fillet.id());

    // And consumption is STRUCTURAL (Gate H's lesson, applied to the dress
    // twin): an impossible radius fails the fillet, and NOTHING displays --
    // never the bare pad underneath a broken successor.
    ASSERT_TRUE(fx.document.setParameterValue(radius.id(), 15.0));
    EXPECT_FALSE(fx.document.recompute().success);
    EXPECT_TRUE(presenter.displayableSolids().empty())
        << "a failed fillet un-consumed its base in the viewer";
}

TEST(M8ReleaseGate, GATE_KC_APocketSketchEditReachesThePocket) {
    // R1-M5: the Sketch->Pocket edge was deleted and every test stayed green;
    // no test edited a pocket's sketch. Removing one line of the pocket
    // sketch opens its profile: the edit must REACH the pocket (loud failure)
    // while the pad stays untouched.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int extrudes = fx.kernel.extrudes;
    const int subtracts = fx.kernel.subtracts;

    const SketchEntityId line = fx.pocketSketch->entities().front().id;
    ASSERT_TRUE(fx.document.removeSketchEntity(fx.pocketSketch->id(), line));
    EXPECT_FALSE(fx.document.recompute().success);

    EXPECT_EQ(fx.pocket->state(), ComputeState::Failed)
        << "the pocket-sketch edit never reached the pocket";
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);
    EXPECT_EQ(fx.kernel.extrudes, extrudes) << "the pad was rebuilt by a pocket-sketch edit";
    EXPECT_EQ(fx.kernel.subtracts, subtracts) << "a cut ran with an open pocket profile";
}

TEST(M8ReleaseGate, GATE_BB_ABoxIsALegalChainBaseEndToEnd) {
    // Round 2 (R2-R1-M1): Box is the one member of the solid-type frontier no
    // test exercised as a chain base -- correct by reviewer probe, gated
    // nowhere. Box 100x50x20 minus pocket 20x30x10 = 94000; width edit 120
    // -> 114000; the viewer shows the tail only; the chain survives a save/
    // load round trip.
    PartDocument document{"M8BoxBase"};
    CountingKernel kernel;
    CountingSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Aluminium", 2700.0);
    Parameter& w = document.addParameter("W", 100.0, UnitType::Millimeter);
    Parameter& h = document.addParameter("H", 50.0, UnitType::Millimeter);
    Parameter& d = document.addParameter("D", 20.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("Depth", 10.0, UnitType::Millimeter);
    Sketch& ks = document.addSketch("PocketSketch");
    ks.addLine(Vec2{10, 10}, Vec2{30, 10});
    ks.addLine(Vec2{30, 10}, Vec2{30, 40});
    ks.addLine(Vec2{30, 40}, Vec2{10, 40});
    ks.addLine(Vec2{10, 40}, Vec2{10, 10});
    Body& body = document.addBody("Body001");
    BoxFeature& box = document.addBoxFeature(body, "Box001", w.id(), h.id(), d.id());
    PocketFeature& pocket =
        document.addPocketFeature(body, "Pocket001", box.id(), ks.id(), depth.id());

    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(pocket.state(), ComputeState::Valid);
    ASSERT_TRUE(document.massProperties().valid);
    ExpectRel(document.massProperties().volumeMm3, 94000.0, 1e-6);

    DocumentPresenter presenter(document);
    const std::vector<ObjectId> solids = presenter.displayableSolids();
    ASSERT_EQ(solids.size(), 1u);
    EXPECT_EQ(solids.front(), pocket.id());

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    OcctGeometryKernel freshKernel;
    loaded.document->setGeometryKernel(&freshKernel);
    ASSERT_TRUE(loaded.document->recompute().success);
    ExpectRel(loaded.document->massProperties().volumeMm3, 94000.0, 1e-6);

    ASSERT_TRUE(loaded.document->setParameterValue(w.id(), 120.0));
    ASSERT_TRUE(loaded.document->recomputeFrom(w.id()).success);
    ExpectRel(loaded.document->massProperties().volumeMm3, 114000.0, 1e-6);
}

// --- Spec 8 adversarial rows (R1-M6: correct by probe, previously untested) --

TEST(M8ReleaseGate, ADV_A_ThePocketMayShareThePadsOwnSketch) {
    // Same sketch drives base and tool: tool = the full 100x50 footprint,
    // 10 deep -> 100000 - 50000 = 50000. Legal, exact, and previously only a
    // reviewer's probe.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
    Parameter& depth = fx.document.addParameter("SharedDepth", 10.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    PocketFeature& pocket = fx.document.addPocketFeature(body, "Pocket002", fx.pad->id(),
                                                         fx.padSketch->id(), depth.id());

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(pocket.state(), ComputeState::Valid);
    ExpectRel(fx.volume(), 50000.0, 1e-6);
}

TEST(M8ReleaseGate, ADV_B_DepthAtTheFloorWorksAndBelowItFailsLoudly) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // AT the floor (kMinExtrusionDistanceMm = 1e-6): legal, and the removed
    // sliver is 6e-4 mm^3 -- indistinguishable from 100000 at 1e-6 relative,
    // which is exactly what the assertion states.
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 1e-6));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.depth->id()).success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Valid);
    ExpectRel(fx.volume(), 100000.0, 1e-6);

    // BELOW the floor: refused loudly, never a zero-thickness boolean.
    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 5e-7));
    EXPECT_FALSE(fx.document.recomputeFrom(fx.depth->id()).success);
    EXPECT_EQ(fx.pocket->state(), ComputeState::Failed);
}

TEST(M8ReleaseGate, ADV_C_AnOpenPocketProfileFailsThePocketNotThePad) {
    ChainFixture fx;
    ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
    Sketch& open = fx.document.addSketch("OpenSketch");
    open.addLine(Vec2{10, 10}, Vec2{30, 10});
    open.addLine(Vec2{30, 10}, Vec2{30, 40});
    open.addLine(Vec2{30, 40}, Vec2{10, 40}); // no closing line
    Parameter& depth = fx.document.addParameter("OpenDepth", 5.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    PocketFeature& pocket =
        fx.document.addPocketFeature(body, "Pocket002", fx.pad->id(), open.id(), depth.id());

    EXPECT_FALSE(fx.document.recompute().success);
    EXPECT_EQ(pocket.state(), ComputeState::Failed);
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);
}

TEST(M8ReleaseGate, ADV_D_ACutThatSwallowsTheBaseIsLegalAndPinned) {
    // ADR-M8-002's empty-result rule, previously unpinned (R1 minor): a tool
    // covering the whole pad yields volume zero, VALID mass, and one
    // displayable (empty) solid -- a legal modeling state, not an error.
    ChainFixture fx;
    ASSERT_TRUE(fx.document.removeObject(fx.pocket->id()));
    Sketch& big = fx.document.addSketch("SwallowSketch");
    big.addLine(Vec2{-10, -10}, Vec2{110, -10});
    big.addLine(Vec2{110, -10}, Vec2{110, 60});
    big.addLine(Vec2{110, 60}, Vec2{-10, 60});
    big.addLine(Vec2{-10, 60}, Vec2{-10, -10});
    Parameter& depth = fx.document.addParameter("SwallowDepth", 20.0, UnitType::Millimeter);
    Body& body = *fx.document.bodies().front();
    PocketFeature& pocket =
        fx.document.addPocketFeature(body, "Pocket002", fx.pad->id(), big.id(), depth.id());
    DocumentPresenter presenter(fx.document);

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(pocket.state(), ComputeState::Valid);
    ASSERT_TRUE(fx.document.massProperties().valid);
    EXPECT_NEAR(fx.document.massProperties().volumeMm3, 0.0, 1e-9);
    EXPECT_EQ(presenter.displayableSolids().size(), 1u);
}

} // namespace
