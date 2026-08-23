#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <cmath>
#include <string>

// M5-J/K integration: the constraint solver, the M2 recompute graph, the M4
// profile/Pad path and REAL OCCT geometry, in one process.
//
// This is the only suite that links Core + Solver + KernelOcct together, and it
// is the only place spec 13's selective-recompute requirements can actually be
// checked: the counters that prove "PadLength changed and the sketch did NOT
// re-solve" need a real solver on one side and a real kernel on the other.
// Neither ParametricCADSolverTests (no kernel) nor ParametricCADKernelOcctTests
// (no solver) can see it.

namespace {

using namespace paramcad;

// Counts solve() calls around the real solver. "The sketch did not re-solve" is
// a claim about invocations, so it has to be measured at the invocation, not
// inferred from unchanged coordinates -- a sketch that re-solved to the same
// answer is exactly the failure this must catch.
class CountingSolver final : public ISketchSolver {
public:
    SketchSolveResult solve(const SketchSolveProblem& problem) override {
        ++solveCallCount;
        return inner_.solve(problem);
    }

    int solveCallCount = 0;

private:
    GaussNewtonSketchSolver inner_;
};

class CountingKernel final : public IGeometryKernel {
public:
    ShapeResult createBox(const BoxDefinition& definition) override {
        ++createBoxCallCount;
        return inner_.createBox(definition);
    }
    ShapeResult extrudeProfile(const PlanarProfileDefinition& profile,
                               double distanceMm) override {
        ++extrudeProfileCallCount;
        return inner_.extrudeProfile(profile, distanceMm);
    }
    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override {
        ++calculateMassPropertiesCallCount;
        return inner_.calculateMassProperties(shape);
    }
    ShapeResult subtractShape(const KernelShape& base, const KernelShape& tool) override {
        ++subtractCallCount;
        return inner_.subtractShape(base, tool);
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
        ++revolveCallCount;
        return inner_.revolveProfile(profile, axisOriginMm, axisDirection, angleRad);
    }
    ShapeResult filletEdges(const KernelShape& shape, const EdgeSelection& selection,
                            double radiusMm) override {
        ++filletCallCount;
        return inner_.filletEdges(shape, selection, radiusMm);
    }
    ShapeResult chamferEdges(const KernelShape& shape, const EdgeSelection& selection,
                             double distanceMm) override {
        ++chamferCallCount;
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


    int createBoxCallCount = 0;
    int extrudeProfileCallCount = 0;
    int calculateMassPropertiesCallCount = 0;
    int subtractCallCount = 0;
    int revolveCallCount = 0;
    int filletCallCount = 0;
    int chamferCallCount = 0;

private:
    OcctGeometryKernel inner_;
};

constexpr double kVolumeRelTol = 1e-9;
constexpr double kCoordAbsTol = 1e-6; // mm

void ExpectRel(double actual, double expected, double relTol = kVolumeRelTol) {
    EXPECT_NEAR(actual, expected, relTol * std::max(1.0, std::fabs(expected)));
}

const SketchLine& LineOf(const Sketch& sketch, SketchEntityId id) {
    const SketchEntity* entity = sketch.findEntity(id);
    EXPECT_NE(entity, nullptr);
    return std::get<SketchLine>(entity->geometry);
}

// --- Spec 14: the fully constrained rectangle -------------------------------
//
// Built deliberately OFF the target size (110 x 60 instead of Width x Height)
// and slightly skewed, so a passing test proves the solver moved the geometry
// to the constrained answer. Seeding it at the answer would let a solver that
// does nothing at all pass every dimension assertion here.
struct RectangleFixture {
    PartDocument document{"M5Doc"};
    CountingKernel kernel;
    CountingSolver solver;

    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* padLength = nullptr;
    Parameter* unrelated = nullptr;
    Material* material = nullptr;
    Sketch* sketch = nullptr;
    PadFeature* pad = nullptr;

    SketchEntityId bottom{}, right{}, top{}, left{};

    RectangleFixture(double w = 100.0, double h = 50.0, double length = 10.0,
                     double density = 2700.0) {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);

        width = &document.addParameter("Width", w, UnitType::Millimeter);
        height = &document.addParameter("Height", h, UnitType::Millimeter);
        padLength = &document.addParameter("PadLength", length, UnitType::Millimeter);
        unrelated = &document.addParameter("Unrelated", 7.0, UnitType::Millimeter);
        material = &document.addMaterial("Alu", density);

        Sketch& s = document.addSketch("Sketch001");
        sketch = &s;
        // Off-target and skewed on purpose (see above).
        bottom = s.addLine(Vec2{0.0, 0.0}, Vec2{110.0, 2.0});
        right = s.addLine(Vec2{110.0, 2.0}, Vec2{112.0, 62.0});
        top = s.addLine(Vec2{112.0, 62.0}, Vec2{1.0, 60.0});
        left = s.addLine(Vec2{1.0, 60.0}, Vec2{0.0, 0.0});

        const auto endPoint = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::EndPoint};
        };
        const auto startPoint = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::StartPoint};
        };

        document.addSketchConstraint(s.id(), CoincidentConstraint{endPoint(bottom),
                                                                  startPoint(right)});
        document.addSketchConstraint(s.id(), CoincidentConstraint{endPoint(right),
                                                                  startPoint(top)});
        document.addSketchConstraint(s.id(), CoincidentConstraint{endPoint(top),
                                                                  startPoint(left)});
        document.addSketchConstraint(s.id(), CoincidentConstraint{endPoint(left),
                                                                  startPoint(bottom)});
        document.addSketchConstraint(s.id(), HorizontalConstraint{bottom});
        document.addSketchConstraint(s.id(), HorizontalConstraint{top});
        document.addSketchConstraint(s.id(), VerticalConstraint{right});
        document.addSketchConstraint(s.id(), VerticalConstraint{left});
        document.addSketchConstraint(s.id(), FixConstraint{startPoint(bottom)});
        document.addSketchConstraint(s.id(), LengthConstraint{bottom, width->id()});
        document.addSketchConstraint(s.id(), LengthConstraint{right, height->id()});

        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", s.id(), padLength->id());
    }

    const Sketch& solved() const {
        const Sketch* s = document.findSketch(sketch->id());
        EXPECT_NE(s, nullptr);
        return *s;
    }
};

// --- Spec 14 ----------------------------------------------------------------

TEST(M5RecomputeIntegration, M5_RECT_001_FullyConstrainedRectangleSolvesToDofZero) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    const Sketch& s = fx.solved();
    EXPECT_EQ(s.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(s.degreesOfFreedom(), 0);

    // The rectangle the constraints describe, not the skewed one it started as.
    const SketchLine& b = LineOf(s, fx.bottom);
    const SketchLine& r = LineOf(s, fx.right);
    EXPECT_NEAR(b.start.x, 0.0, kCoordAbsTol);
    EXPECT_NEAR(b.start.y, 0.0, kCoordAbsTol);
    EXPECT_NEAR(b.end.y, b.start.y, kCoordAbsTol);        // horizontal
    EXPECT_NEAR(std::fabs(b.end.x - b.start.x), 100.0, kCoordAbsTol);
    EXPECT_NEAR(r.end.x, r.start.x, kCoordAbsTol);        // vertical
    EXPECT_NEAR(std::fabs(r.end.y - r.start.y), 50.0, kCoordAbsTol);
}

TEST(M5RecomputeIntegration, M5_RECT_002_SolvedRectangleFeedsM4PadWithRealVolume) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);
    ExpectRel(fx.document.massProperties().volumeMm3, 100.0 * 50.0 * 10.0);
    // 100 x 50 x 10 mm^3 = 5e-5 m^3 at 2700 kg/m^3.
    ExpectRel(fx.document.massProperties().massKg, 5.0e-5 * 2700.0);
}

TEST(M5RecomputeIntegration, M5_RECT_003_WidthEditResolvesSketchAndRebuildsDownstream) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int solves = fx.solver.solveCallCount;
    const int extrudes = fx.kernel.extrudeProfileCallCount;
    const int masses = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.solver.solveCallCount, solves + 1);
    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudes + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, masses + 1);

    const SketchLine& b = LineOf(fx.solved(), fx.bottom);
    EXPECT_NEAR(std::fabs(b.end.x - b.start.x), 120.0, kCoordAbsTol);
    ExpectRel(fx.document.massProperties().volumeMm3, 120.0 * 50.0 * 10.0);
}

// --- Spec 13: selective recompute -------------------------------------------

TEST(M5RecomputeIntegration, M5_SELECTIVE_001_PadLengthDoesNotResolveTheSketch) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int solves = fx.solver.solveCallCount;
    const int extrudes = fx.kernel.extrudeProfileCallCount;
    const int masses = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.padLength->id(), 25.0));
    ASSERT_TRUE(fx.document.recompute().success);

    // The requirement in one line: PadLength is not a prerequisite of the
    // sketch, so the solver must not run at all.
    EXPECT_EQ(fx.solver.solveCallCount, solves);
    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudes + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, masses + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, 100.0 * 50.0 * 25.0);
}

TEST(M5RecomputeIntegration, M5_SELECTIVE_002_DensityRecomputesMassOnly) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int solves = fx.solver.solveCallCount;
    const int extrudes = fx.kernel.extrudeProfileCallCount;
    const int masses = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setMaterialDensity(7800.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.solver.solveCallCount, solves);
    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudes);
    // Mass is recomputed; density is not a geometric input, so the volume is
    // unchanged and only the mass moves.
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, masses + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, 100.0 * 50.0 * 10.0);
    ExpectRel(fx.document.massProperties().massKg, 5.0e-5 * 7800.0);
}

TEST(M5RecomputeIntegration, M5_SELECTIVE_003_UnrelatedParameterTouchesNothing) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int solves = fx.solver.solveCallCount;
    const int extrudes = fx.kernel.extrudeProfileCallCount;
    const int masses = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.unrelated->id(), 9.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.solver.solveCallCount, solves);
    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudes);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, masses);
}

TEST(M5RecomputeIntegration, M5_SELECTIVE_004_SecondRecomputeWithNoEditSolvesNothing) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int solves = fx.solver.solveCallCount;

    ASSERT_TRUE(fx.document.recompute().success);
    // A clean sketch is not re-solved. Without this, "PadLength does not
    // re-solve" above could pass while every recompute solved everything.
    EXPECT_EQ(fx.solver.solveCallCount, solves);
    EXPECT_EQ(fx.solved().solveStatus(), SketchSolveStatus::Solved);
}

// --- Spec 15: the circle ----------------------------------------------------

TEST(M5RecomputeIntegration, M5_CIRCLE_001_RadiusEditRebuildsTheCylindricalPad) {
    PartDocument document{"CircleDoc"};
    CountingKernel kernel;
    CountingSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);

    Parameter& radius = document.addParameter("Radius", 20.0, UnitType::Millimeter);
    Parameter& length = document.addParameter("PadLength", 10.0, UnitType::Millimeter);
    document.addMaterial("Alu", 2700.0);

    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{5.0, -3.0}, 12.0); // off-target
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{circle, SketchSubElement::CenterPoint}});
    document.addSketchConstraint(sketch.id(), RadiusConstraint{circle, radius.id()});

    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(pad.state(), ComputeState::Valid);
    ExpectRel(document.massProperties().volumeMm3, 3.14159265358979323846 * 20.0 * 20.0 * 10.0,
              1e-4); // OCCT tessellation-free exact volume, loose for pi rounding

    const int extrudes = kernel.extrudeProfileCallCount;
    ASSERT_TRUE(document.setParameterValue(radius.id(), 30.0));
    ASSERT_TRUE(document.recompute().success);

    EXPECT_EQ(kernel.extrudeProfileCallCount, extrudes + 1);
    ExpectRel(document.massProperties().volumeMm3, 3.14159265358979323846 * 30.0 * 30.0 * 10.0,
              1e-4);
}

// --- Angle, end to end (the gap that let a Critical ship) --------------------

TEST(M5RecomputeIntegration, M5_ANGLE_E2E_001_AngleParameterDrivesRealSketchGeometry) {
    // Before this test existed, NO test anywhere built an AngleConstraint on a
    // real sketch and measured the resulting geometry. The residual compared
    // millimetres to radians, reported Solved, and 444 tests stayed green.
    PartDocument document{"AngleDoc"};
    CountingKernel kernel;
    CountingSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);

    Parameter& angle = document.addParameter("Corner", 60.0 * 3.14159265358979323846 / 180.0,
                                             UnitType::Radian);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId base = sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    // The arm starts near the SUPPLEMENTARY root (60 + 180 = 240 degrees), not
     // merely "wrong". Started at ~9 degrees it was close enough to the correct
     // root that Gauss-Newton picked it even under the broken sin() residual --
     // so this test, added specifically so C1 could not ship again, could not
     // fail against C1's formulation. Same weakness M5_ANGLE_002 had.
    const SketchEntityId arm =
        sketch.addLine(Vec2{0, 0}, Vec2{30.0 * std::cos(4.1), 30.0 * std::sin(4.1)});

    const auto sp = [](SketchEntityId id) {
        return SketchElementRef{id, SketchSubElement::StartPoint};
    };
    Parameter& armLength = document.addParameter("ArmLength", 40.0, UnitType::Millimeter);
    document.addSketchConstraint(sketch.id(), FixConstraint{sp(base)});
    document.addSketchConstraint(sketch.id(),
                                 FixConstraint{SketchElementRef{base,
                                                                SketchSubElement::EndPoint}});
    document.addSketchConstraint(sketch.id(), FixConstraint{sp(arm)});
    document.addSketchConstraint(sketch.id(), LengthConstraint{arm, armLength.id()});
    document.addSketchConstraint(sketch.id(), AngleConstraint{base, arm, angle.id()});

    ASSERT_TRUE(document.recompute().success);
    const Sketch& solved = *document.findSketch(sketch.id());
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::Solved);

    // Measured from the SOLVED COORDINATES with atan2 -- never by re-evaluating
    // the solver's own residual formula.
    const auto measure = [&] {
        const SketchLine& b = std::get<SketchLine>(solved.findEntity(base)->geometry);
        const SketchLine& a = std::get<SketchLine>(solved.findEntity(arm)->geometry);
        double d = std::atan2(a.end.y - a.start.y, a.end.x - a.start.x) -
                   std::atan2(b.end.y - b.start.y, b.end.x - b.start.x);
        while (d < 0.0) d += 2.0 * 3.14159265358979323846;
        return d * 180.0 / 3.14159265358979323846;
    };
    EXPECT_NEAR(measure(), 60.0, 1e-4);
    // The arm kept its length -- the angle did not achieve itself by stretching.
    const SketchLine& a = std::get<SketchLine>(solved.findEntity(arm)->geometry);
    EXPECT_NEAR(std::hypot(a.end.x - a.start.x, a.end.y - a.start.y), 40.0, kCoordAbsTol);

    // And the Parameter really drives it.
    ASSERT_TRUE(document.setParameterValue(angle.id(),
                                           135.0 * 3.14159265358979323846 / 180.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_NEAR(measure(), 135.0, 1e-4);
}

// --- Failure propagation ----------------------------------------------------

TEST(M5RecomputeIntegration, M5_FAILURE_001_ConflictingSketchBlocksPadAndRetainsLastGood) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const double goodVolume = fx.document.massProperties().volumeMm3;

    // Bottom already has Length = Width. A second, disagreeing length on the
    // same line is the textbook conflict.
    Parameter& other = fx.document.addParameter("Other", 80.0, UnitType::Millimeter);
    ASSERT_NE(fx.document.addSketchConstraint(fx.sketch->id(),
                                              LengthConstraint{fx.bottom, other.id()}),
              kInvalidSketchConstraintId);

    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);

    const Sketch& s = fx.solved();
    EXPECT_NE(s.solveStatus(), SketchSolveStatus::Solved);
    // Transactional: the failed solve committed nothing, so the geometry is
    // still the last good rectangle (ADR-M5-004).
    const SketchLine& b = LineOf(s, fx.bottom);
    EXPECT_NEAR(std::fabs(b.end.x - b.start.x), 100.0, kCoordAbsTol);
    // And the last valid downstream result is retained, not zeroed
    // (ADR-M3-006: retention and currency are separate properties).
    ExpectRel(fx.document.massProperties().volumeMm3, goodVolume);
}

TEST(M5RecomputeIntegration, M5_FAILURE_002_NoSolverConfiguredFailsCleanly) {
    RectangleFixture fx;
    fx.document.setSketchSolver(nullptr);

    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.solved().solveStatus(), SketchSolveStatus::InvalidInput);
    EXPECT_NE(fx.pad->state(), ComputeState::Valid);
}

// --- The constraint facade's graph edge --------------------------------------

TEST(M5RecomputeIntegration, M5_FACADE_001_RemovingTheLastConstraintDropsTheParameterEdge) {
    RectangleFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // Find the Length(bottom, Width) constraint by content, never by position.
    SketchConstraintId lengthOnBottom = kInvalidSketchConstraintId;
    for (const SketchConstraint& c : fx.solved().constraints())
        if (const auto* length = std::get_if<LengthConstraint>(&c.data))
            if (length->line == fx.bottom) lengthOnBottom = c.id;
    ASSERT_NE(lengthOnBottom, kInvalidSketchConstraintId);

    ASSERT_TRUE(fx.document.removeSketchConstraint(fx.sketch->id(), lengthOnBottom));
    ASSERT_TRUE(fx.document.recompute().success);
    const int solves = fx.solver.solveCallCount;

    // Width no longer drives anything in this sketch, so editing it must not
    // re-solve. This is the assertion that fails if the edge is never removed.
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 130.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.solver.solveCallCount, solves);
}

} // namespace
