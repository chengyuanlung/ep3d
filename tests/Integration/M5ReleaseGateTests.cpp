// M5 mandatory release gates A-H (spec 21-28), executed against the REAL
// solver and REAL OCCT geometry. M5 cannot be declared complete if any fails.
//
// Every expected number here is an INDEPENDENT ANALYTICAL RESULT computed from
// the parameter values by hand (spec 20), never read back from the solver or
// the kernel and never produced by a production helper. A gate whose expected
// value came from the code under test proves only that the code agrees with
// itself.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include "Viewer/DocumentOutline.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kLengthAbsTol = 1e-6;     // mm
constexpr double kVolumeMassRelTol = 1e-9; // planar/prismatic: exact
constexpr double kCurvedRelTol = 1e-6;     // circular cross-sections

void ExpectRel(double actual, double expected, double relTol) {
    EXPECT_NEAR(actual, expected, relTol * std::max(1.0, std::fabs(expected)));
}

// Counts solve() and kernel calls, so the selectivity gate can assert
// invocations rather than infer them from unchanged numbers.
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
        return inner_.createBox(definition);
    }
    ShapeResult extrudeProfile(const PlanarProfileDefinition& profile,
                               double distanceMm) override {
        ++extrudeCallCount;
        return inner_.extrudeProfile(profile, distanceMm);
    }
    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override {
        ++massCallCount;
        return inner_.calculateMassProperties(shape);
    }
    ShapeResult subtractShape(const KernelShape& base, const KernelShape& tool) override {
        ++subtractCallCount;
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

    int extrudeCallCount = 0;
    int massCallCount = 0;
    int subtractCallCount = 0;
    int revolveCallCount = 0;
    int filletCallCount = 0;
    int chamferCallCount = 0;

private:
    OcctGeometryKernel inner_;
};

// Spec 14's reference rectangle, drawn DELIBERATELY OFF-SIZE and skewed.
//
// This matters for every gate below: if the geometry were seeded at 100 x 50,
// a solver that did nothing at all would pass Gate A, and Gate B's edit would
// be the only thing ever tested. Starting wrong makes "the numbers are right"
// evidence that the solver produced them.
//
// PLACEMENT CONVENTION (spec 21): Fix pins bottom.StartPoint where it is, at
// sketch-local (0,0). Horizontal(bottom) + Vertical(right) + the coincidences
// therefore place the rectangle in u in [0, Width], v in [0, Height], and the
// Pad extrudes +Z. The analytical COM that follows from that convention is
// (Width/2, Height/2, PadLength/2).
struct GateRectangle {
    PartDocument document{"M5GateRect"};
    CountingKernel kernel;
    CountingSolver solver;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* padLength = nullptr;
    Parameter* unrelated = nullptr;
    Sketch* sketch = nullptr;
    PadFeature* pad = nullptr;
    SketchEntityId bottom{}, right{}, top{}, left{};

    GateRectangle() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);
        width = &document.addParameter("Width", 100.0, UnitType::Millimeter);
        height = &document.addParameter("Height", 50.0, UnitType::Millimeter);
        padLength = &document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        unrelated = &document.addParameter("Unrelated", 3.0, UnitType::Millimeter);

        Sketch& s = document.addSketch("Sketch001");
        sketch = &s;
        bottom = s.addLine(Vec2{0, 0}, Vec2{112, 3});
        right = s.addLine(Vec2{112, 3}, Vec2{115, 58});
        top = s.addLine(Vec2{115, 58}, Vec2{2, 61});
        left = s.addLine(Vec2{2, 61}, Vec2{0, 0});

        const auto sp = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::StartPoint};
        };
        const auto ep = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::EndPoint};
        };
        const auto add = [&](SketchConstraintData data) {
            return document.addSketchConstraint(s.id(), std::move(data));
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

        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", s.id(), padLength->id());
    }

    const Sketch& solved() const { return *document.findSketch(sketch->id()); }
};

struct GateCircle {
    PartDocument document{"M5GateCircle"};
    CountingKernel kernel;
    CountingSolver solver;
    Parameter* radius = nullptr;
    Parameter* padLength = nullptr;
    Sketch* sketch = nullptr;
    PadFeature* pad = nullptr;
    SketchEntityId circle{};

    GateCircle() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);
        radius = &document.addParameter("Radius", 10.0, UnitType::Millimeter);
        padLength = &document.addParameter("PadLength", 30.0, UnitType::Millimeter);

        Sketch& s = document.addSketch("Sketch001");
        sketch = &s;
        circle = s.addCircle(Vec2{0, 0}, 7.0); // off-target on purpose
        document.addSketchConstraint(
            s.id(), FixConstraint{SketchElementRef{circle, SketchSubElement::CenterPoint}});
        document.addSketchConstraint(s.id(), RadiusConstraint{circle, radius->id()});

        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", s.id(), padLength->id());
    }

    const Sketch& solved() const { return *document.findSketch(sketch->id()); }
};

// --- Gate A: fully constrained rectangle (spec 21) ---------------------------

TEST(M5ReleaseGate, GATE_A_FullyConstrainedRectangle) {
    GateRectangle fx;
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.solved().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.solved().degreesOfFreedom(), 0);

    // Profile valid.
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);

    const MassProperties& mp = fx.document.massProperties();
    ASSERT_TRUE(mp.valid);
    // 100 x 50 x 20 = 100000 mm^3; 1e-4 m^3 x 2700 kg/m^3 = 0.27 kg.
    ExpectRel(mp.volumeMm3, 100000.0, kVolumeMassRelTol);
    ExpectRel(mp.massKg, 0.27, kVolumeMassRelTol);
    // COM follows the documented Fix convention above: (50, 25, 10) mm.
    EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, 1e-6);
    EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, 1e-6);
    EXPECT_NEAR(mp.centerOfMassMm.z, 10.0, 1e-6);
}

// --- Gate B: the release-critical proof (spec 22) ----------------------------

TEST(M5ReleaseGate, GATE_B_WidthEditRebuildsTheSolid) {
    GateRectangle fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int solves = fx.solver.solveCallCount;
    const int extrudes = fx.kernel.extrudeCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.solver.solveCallCount, solves + 1);   // the sketch re-solved
    EXPECT_EQ(fx.kernel.extrudeCallCount, extrudes + 1); // the pad rebuilt
    EXPECT_EQ(fx.solved().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.solved().degreesOfFreedom(), 0);

    // The Width constraint is actually satisfied in the geometry, not merely
    // reported as solved.
    const SketchLine& b = std::get<SketchLine>(fx.solved().findEntity(fx.bottom)->geometry);
    EXPECT_NEAR(std::hypot(b.end.x - b.start.x, b.end.y - b.start.y), 120.0, kLengthAbsTol);

    const MassProperties& mp = fx.document.massProperties();
    ASSERT_TRUE(mp.valid);
    // 120 x 50 x 20 = 120000 mm^3; 1.2e-4 m^3 x 2700 = 0.324 kg.
    ExpectRel(mp.volumeMm3, 120000.0, kVolumeMassRelTol);
    ExpectRel(mp.massKg, 0.324, kVolumeMassRelTol);
    EXPECT_NEAR(mp.centerOfMassMm.x, 60.0, 1e-6);
}

// --- Gate C: height edit (spec 23) -------------------------------------------

TEST(M5ReleaseGate, GATE_C_HeightEdit) {
    GateRectangle fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.setParameterValue(fx.height->id(), 80.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.solved().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid); // profile remains valid

    const MassProperties& mp = fx.document.massProperties();
    // 120 x 80 x 20 = 192000 mm^3; 1.92e-4 m^3 x 2700 = 0.5184 kg.
    ExpectRel(mp.volumeMm3, 192000.0, kVolumeMassRelTol);
    ExpectRel(mp.massKg, 0.5184, kVolumeMassRelTol);
}

// --- Gate D: selective recompute (spec 24) -----------------------------------

TEST(M5ReleaseGate, GATE_D_SelectiveRecompute) {
    GateRectangle fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // PadLength 20 -> 30: the sketch must NOT re-solve.
    int solves = fx.solver.solveCallCount;
    int extrudes = fx.kernel.extrudeCallCount;
    int masses = fx.kernel.massCallCount;
    ASSERT_TRUE(fx.document.setParameterValue(fx.padLength->id(), 30.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.solver.solveCallCount, solves);
    EXPECT_EQ(fx.kernel.extrudeCallCount, extrudes + 1);
    EXPECT_EQ(fx.kernel.massCallCount, masses + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, 150000.0, kVolumeMassRelTol);

    // Density 2700 -> 7850: neither the sketch nor the pad may run.
    solves = fx.solver.solveCallCount;
    extrudes = fx.kernel.extrudeCallCount;
    masses = fx.kernel.massCallCount;
    ASSERT_TRUE(fx.document.setMaterialDensity(7850.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.solver.solveCallCount, solves);
    EXPECT_EQ(fx.kernel.extrudeCallCount, extrudes);
    EXPECT_EQ(fx.kernel.massCallCount, masses + 1);
    // 100 x 50 x 30 = 1.5e-4 m^3 x 7850 = 1.1775 kg.
    ExpectRel(fx.document.massProperties().massKg, 1.1775, kVolumeMassRelTol);

    // An unrelated Parameter touches nothing at all.
    solves = fx.solver.solveCallCount;
    extrudes = fx.kernel.extrudeCallCount;
    masses = fx.kernel.massCallCount;
    ASSERT_TRUE(fx.document.setParameterValue(fx.unrelated->id(), 9.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.solver.solveCallCount, solves);
    EXPECT_EQ(fx.kernel.extrudeCallCount, extrudes);
    EXPECT_EQ(fx.kernel.massCallCount, masses);
}

// --- Gate E: conflict and recovery (spec 25) ---------------------------------

TEST(M5ReleaseGate, GATE_E_ConflictThenFullRecovery) {
    GateRectangle fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const double goodVolume = fx.document.massProperties().volumeMm3;
    const SketchLine goodBottom =
        std::get<SketchLine>(fx.solved().findEntity(fx.bottom)->geometry);

    // A second, disagreeing Length on the same line.
    Parameter& other = fx.document.addParameter("WidthAlt", 70.0, UnitType::Millimeter);
    const SketchConstraintId conflicting =
        fx.document.addSketchConstraint(fx.sketch->id(), LengthConstraint{fx.bottom, other.id()});
    ASSERT_NE(conflicting, kInvalidSketchConstraintId);
    EXPECT_FALSE(fx.document.recompute().success);

    // A documented conflict status, not a generic failure.
    const SketchSolveStatus status = fx.solved().solveStatus();
    EXPECT_TRUE(status == SketchSolveStatus::Conflicting ||
                status == SketchSolveStatus::OverConstrained ||
                status == SketchSolveStatus::NumericalFailure)
        << "conflict reported as " << SolveStatusName(status);
    EXPECT_NE(status, SketchSolveStatus::Solved);

    // No corrupt geometry commit: the sketch is byte-for-byte the last good one.
    const SketchLine& afterFail =
        std::get<SketchLine>(fx.solved().findEntity(fx.bottom)->geometry);
    EXPECT_DOUBLE_EQ(afterFail.start.x, goodBottom.start.x);
    EXPECT_DOUBLE_EQ(afterFail.start.y, goodBottom.start.y);
    EXPECT_DOUBLE_EQ(afterFail.end.x, goodBottom.end.x);
    EXPECT_DOUBLE_EQ(afterFail.end.y, goodBottom.end.y);

    // Downstream is BLOCKED, and the retained result is the last valid one --
    // retention and currency are separate properties (ADR-M3-006).
    EXPECT_NE(fx.pad->state(), ComputeState::Valid);
    ExpectRel(fx.document.massProperties().volumeMm3, goodVolume, kVolumeMassRelTol);

    // A useful diagnostic: the offending constraint is NAMED.
    EXPECT_FALSE(fx.solved().offendingConstraints().empty());
    EXPECT_FALSE(fx.solved().solveMessage().empty());
    const DocumentOutline outline(fx.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(fx.sketch->id());
    bool namesOffender = false;
    for (const PropertyRow& row : rows)
        if (row.label == "Offending constraint IDs" && !row.value.empty()) namesOffender = true;
    EXPECT_TRUE(namesOffender);

    // --- Recovery: remove the conflict and prove FULL recovery. --------------
    ASSERT_TRUE(fx.document.removeSketchConstraint(fx.sketch->id(), conflicting));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.solved().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.solved().degreesOfFreedom(), 0);
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);
    ExpectRel(fx.document.massProperties().volumeMm3, 100000.0, kVolumeMassRelTol);

    // And it still responds to edits afterwards -- recovery means the model is
    // usable again, not merely that one recompute stopped failing.
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectRel(fx.document.massProperties().volumeMm3, 120000.0, kVolumeMassRelTol);
}

// --- Gate F: constrained circle (spec 26) ------------------------------------

TEST(M5ReleaseGate, GATE_F_CircleRadiusEdit) {
    GateCircle fx;
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.solved().solveStatus(), SketchSolveStatus::Solved);

    // pi * 10^2 * 30 = 9424.777960769379 mm^3.
    ExpectRel(fx.document.massProperties().volumeMm3, kPi * 100.0 * 30.0, kCurvedRelTol);
    const double volumeR10 = fx.document.massProperties().volumeMm3;

    const int extrudes = fx.kernel.extrudeCallCount;
    const double padBefore = fx.padLength->value();

    ASSERT_TRUE(fx.document.setParameterValue(fx.radius->id(), 20.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.solved().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.kernel.extrudeCallCount, extrudes + 1);
    EXPECT_DOUBLE_EQ(fx.padLength->value(), padBefore); // pad length unchanged
    // pi * 20^2 * 30 -- and exactly 4x the R=10 volume.
    ExpectRel(fx.document.massProperties().volumeMm3, kPi * 400.0 * 30.0, kCurvedRelTol);
    ExpectRel(fx.document.massProperties().volumeMm3, 4.0 * volumeR10, kCurvedRelTol);

    // The solved radius really is 20, not merely a volume that happens to match.
    const SketchCircle& c = std::get<SketchCircle>(fx.solved().findEntity(fx.circle)->geometry);
    EXPECT_NEAR(c.radiusMm, 20.0, kLengthAbsTol);
}

// --- Gate G: save / load / re-solve (spec 27) --------------------------------

namespace {

std::string Save(const PartDocument& document) {
    std::ostringstream out;
    const SaveResult result = savePartDocument(document, out);
    EXPECT_TRUE(result) << result.message;
    return out.str();
}

LoadResult Load(const std::string& text) {
    std::istringstream in(text);
    return loadPartDocument(in);
}

} // namespace

TEST(M5ReleaseGate, GATE_G_RectangleSurvivesSaveLoadAndResolves) {
    GateRectangle fx;
    ASSERT_TRUE(fx.document.recompute().success);

    const std::string saved = Save(fx.document);
    const LoadResult loaded = Load(saved);
    ASSERT_TRUE(loaded) << loaded.message;

    // Identity survived: sketch, entities, constraints, sub-element refs,
    // parameter bindings, frame, and the Pad's references.
    const Sketch& before = fx.solved();
    const std::vector<const Sketch*> sketches = loaded.document->sketches();
    ASSERT_EQ(sketches.size(), 1u);
    const Sketch& after = *sketches.front();
    EXPECT_EQ(after.id(), before.id());
    ASSERT_EQ(after.entities().size(), before.entities().size());
    for (const SketchEntity& entity : before.entities())
        EXPECT_NE(after.findEntity(entity.id), nullptr);
    ASSERT_EQ(after.constraints().size(), before.constraints().size());
    for (const SketchConstraint& constraint : before.constraints()) {
        const SketchConstraint* restored = after.findConstraint(constraint.id);
        ASSERT_NE(restored, nullptr);
        EXPECT_STREQ(ConstraintKindName(restored->data), ConstraintKindName(constraint.data));
        EXPECT_EQ(BoundParameterId(restored->data), BoundParameterId(constraint.data));
        EXPECT_EQ(ReferencedEntities(restored->data), ReferencedEntities(constraint.data));
    }
    // Sub-element refs specifically: a Fix that came back on the wrong
    // sub-element would still solve, just to the wrong answer.
    for (const SketchConstraint& constraint : after.constraints()) {
        const auto* fix = std::get_if<FixConstraint>(&constraint.data);
        if (fix == nullptr) continue;
        EXPECT_EQ(fix->target.subElement, SketchSubElement::StartPoint);
    }

    // Re-solve with a FRESH backend: no runtime identity crossed the file.
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);
    ASSERT_TRUE(loaded.document->recompute().success);

    EXPECT_EQ(after.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(after.degreesOfFreedom(), 0);
    const MassProperties& mp = loaded.document->massProperties();
    ExpectRel(mp.volumeMm3, 100000.0, kVolumeMassRelTol);
    ExpectRel(mp.massKg, 0.27, kVolumeMassRelTol);
    EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, 1e-6);
    EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, 1e-6);
    EXPECT_NEAR(mp.centerOfMassMm.z, 10.0, 1e-6);

    // And the reloaded document is still PARAMETRIC, which is the property that
    // would be silently lost if the Parameter edges were not re-derived.
    for (const auto& parameter : loaded.document->parameters().items()) {
        if (parameter->name() != "Width") continue;
        ASSERT_TRUE(loaded.document->setParameterValue(parameter->id(), 120.0));
    }
    ASSERT_TRUE(loaded.document->recompute().success);
    ExpectRel(loaded.document->massProperties().volumeMm3, 120000.0, kVolumeMassRelTol);
}

TEST(M5ReleaseGate, GATE_G_CircleSurvivesSaveLoadAndResolves) {
    GateCircle fx;
    ASSERT_TRUE(fx.document.recompute().success);

    const LoadResult loaded = Load(Save(fx.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& after = *loaded.document->sketches().front();
    ASSERT_EQ(after.constraints().size(), 2u);
    for (const SketchConstraint& constraint : after.constraints()) {
        const auto* fix = std::get_if<FixConstraint>(&constraint.data);
        if (fix == nullptr) continue;
        // The circle's Fix is on its CENTRE, and must not come back as Whole.
        EXPECT_EQ(fix->target.subElement, SketchSubElement::CenterPoint);
    }

    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);
    ASSERT_TRUE(loaded.document->recompute().success);
    ExpectRel(loaded.document->massProperties().volumeMm3, kPi * 100.0 * 30.0, kCurvedRelTol);

    for (const auto& parameter : loaded.document->parameters().items()) {
        if (parameter->name() != "Radius") continue;
        ASSERT_TRUE(loaded.document->setParameterValue(parameter->id(), 20.0));
    }
    ASSERT_TRUE(loaded.document->recompute().success);
    ExpectRel(loaded.document->massProperties().volumeMm3, kPi * 400.0 * 30.0, kCurvedRelTol);
}

TEST(M5ReleaseGate, GATE_G_NoBackendRuntimeIdentityInTheFile) {
    GateRectangle fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::string saved = Save(fx.document);
    // Solver variables, residual indexes, OCCT topology names and Qt types
    // must not appear under any name (spec 17/19).
    for (const char* forbidden : {"TopoDS", "TShape", "BRep", "gp_", "QObject", "QString",
                                  "variableIndex", "residual", "jacobian", "solveStatus",
                                  "degreesOfFreedom"}) {
        EXPECT_EQ(saved.find(forbidden), std::string::npos)
            << "'" << forbidden << "' reached the file";
    }
}

// --- Gate H: under-constrained, then fully constrained (spec 28) -------------

TEST(M5ReleaseGate, GATE_H_UnderConstrainedThenFullyConstrained) {
    PartDocument document{"M5GateH"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Aluminium", 2700.0);
    Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = document.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& padLength = document.addParameter("PadLength", 20.0, UnitType::Millimeter);

    Sketch& s = document.addSketch("Sketch001");
    const SketchEntityId bottom = s.addLine(Vec2{0, 0}, Vec2{112, 3});
    const SketchEntityId right = s.addLine(Vec2{112, 3}, Vec2{115, 58});
    const SketchEntityId top = s.addLine(Vec2{115, 58}, Vec2{2, 61});
    const SketchEntityId left = s.addLine(Vec2{2, 61}, Vec2{0, 0});
    const auto sp = [](SketchEntityId id) {
        return SketchElementRef{id, SketchSubElement::StartPoint};
    };
    const auto ep = [](SketchEntityId id) {
        return SketchElementRef{id, SketchSubElement::EndPoint};
    };
    const auto add = [&](SketchConstraintData data) {
        return document.addSketchConstraint(s.id(), std::move(data));
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

    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", s.id(), padLength.id());

    // Under-constrained is a LEGAL state, not a failure: the recompute
    // succeeds, the geometry is finite and usable, and the UI says DOF > 0.
    ASSERT_TRUE(document.recompute().success);
    const Sketch& solved = *document.findSketch(s.id());
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::UnderConstrained);
    EXPECT_GT(solved.degreesOfFreedom(), 0);
    EXPECT_EQ(pad.state(), ComputeState::Valid);
    ASSERT_TRUE(document.massProperties().valid);
    EXPECT_GT(document.massProperties().volumeMm3, 0.0);
    for (const SketchEntity& entity : solved.entities()) {
        const SketchLine& line = std::get<SketchLine>(entity.geometry);
        EXPECT_TRUE(std::isfinite(line.start.x) && std::isfinite(line.start.y));
        EXPECT_TRUE(std::isfinite(line.end.x) && std::isfinite(line.end.y));
    }
    const DocumentOutline outline(document);
    const std::vector<PropertyRow> before = outline.propertiesOf(s.id());
    const auto rowValue = [](const std::vector<PropertyRow>& rows, const char* label) {
        for (const PropertyRow& row : rows)
            if (row.label == label) return row.value;
        return std::string{};
    };
    EXPECT_EQ(rowValue(before, "Solve status"), "Under-constrained");
    EXPECT_NE(rowValue(before, "Degrees of freedom"), "0");
    EXPECT_NE(rowValue(before, "Degrees of freedom"), "");

    // Add constraints until DOF reaches 0.
    add(LengthConstraint{bottom, width.id()});
    add(LengthConstraint{right, height.id()});
    ASSERT_TRUE(document.recompute().success);

    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(solved.degreesOfFreedom(), 0);
    const std::vector<PropertyRow> after = outline.propertiesOf(s.id());
    EXPECT_EQ(rowValue(after, "Solve status"), "Solved");
    EXPECT_EQ(rowValue(after, "Degrees of freedom"), "0");
    ExpectRel(document.massProperties().volumeMm3, 100000.0, kVolumeMassRelTol);
}

} // namespace
