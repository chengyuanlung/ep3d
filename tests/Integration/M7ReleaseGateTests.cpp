// M7.1 release gate (spec 37), executed against the REAL solver and REAL OCCT
// geometry:
//
//     DXF rectangle -> M6 import -> M7 reconstruct Width/Height
//     -> Solved, DOF 0 -> save -> fresh load -> Width 100 -> 120
//     -> Pad volume = 120000 mm^3
//
// Every expected number is computed by hand from the parameter values, never
// read back from the solver, the reconstructor or the kernel. A gate whose
// expectation came from the code under test proves only self-consistency.
//
// THE GEOMETRY STARTS WRONG, ON PURPOSE. The imported rectangle is 99.5 x 49.7,
// its corners miss by half a micron and its sides are half a milliradian off
// axis. If it were imported at 100 x 50 then an implementation that
// reconstructed nothing at all would pass every assertion below, and the gate
// would be measuring the fixture instead of the code.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Import/SketchImporter.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kVolumeRelTol = 1e-9; // prismatic: exact
constexpr double kGeometryTolMm = 1e-6;

void ExpectRel(double actual, double expected, double relTol) {
    EXPECT_NEAR(actual, expected, relTol * std::max(1.0, std::fabs(expected)));
}

// The drawn (wrong) geometry. Named constants because several tests assert that
// the solved result is NOT these numbers.
constexpr double kDrawnWidth = 99.5;
constexpr double kDrawnHeight = 49.7;
constexpr double kCornerGap = 5e-4;  // mm, inside the coincidence tolerance
constexpr double kSideSkew = 5e-4;   // rad, inside the axis tolerance

// What the drawing SAYS, which is what reconstruction must produce.
constexpr double kStatedWidth = 100.0;
constexpr double kStatedHeight = 50.0;
constexpr double kPadLength = 20.0;

ImportedSketchGeometry DimensionedRectangle() {
    ImportedSketchGeometry geometry;
    geometry.unit = ImportedLengthUnit::Millimeter;
    geometry.unitWasDefaulted = false;
    geometry.millimetresPerUnit = 1.0;

    // Corners, with the deliberate imperfections.
    const Vec2 bottomLeft{0.0, 0.0};
    const Vec2 bottomRight{kDrawnWidth, kDrawnWidth * std::sin(kSideSkew)};
    const Vec2 topRight{kDrawnWidth, kDrawnHeight};
    const Vec2 topLeft{0.0, kDrawnHeight};

    geometry.lines.push_back(ImportedLine2D{bottomLeft, bottomRight});
    geometry.lines.push_back(ImportedLine2D{bottomRight, topRight});
    geometry.lines.push_back(ImportedLine2D{topRight, topLeft});
    // The closing side misses by kCornerGap -- a real export's rounding.
    geometry.lines.push_back(ImportedLine2D{topLeft, Vec2{kCornerGap, 0.0}});

    // The dimensions. Definition points sit ON the drawn geometry, which is how
    // a real dimensioned DXF is authored; the stated value is what the designer
    // meant, and differs from the drawn size by 0.5% and 0.6% respectively --
    // inside the agreement band, so reconstruction believes the statement and
    // the SOLVER is what moves the geometry (ADR-M7-009).
    ImportedDimension2D width;
    width.kind = ImportedDimensionKind::Linear;
    width.measureFrom = bottomLeft;
    width.measureTo = bottomRight;
    width.directionRad = 0.0;
    width.statedValueMm = kStatedWidth;
    width.sourceHandle = "2A";

    ImportedDimension2D height;
    height.kind = ImportedDimensionKind::Linear;
    height.measureFrom = bottomRight;
    height.measureTo = topRight;
    height.directionRad = kPi / 2.0;
    height.statedValueMm = kStatedHeight;
    height.sourceHandle = "2B";

    geometry.dimensions.push_back(width);
    geometry.dimensions.push_back(height);
    return geometry;
}

// import -> reconstruct -> pad, with the real backends attached.
struct GateFixture {
    PartDocument document{"M7Gate"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    ObjectId sketchId{kInvalidObjectId};
    PadFeature* pad = nullptr;
    ReconstructionResult reconstruction;

    GateFixture() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);

        const ImportedSketchGeometry geometry = DimensionedRectangle();
        const SketchImportResult imported =
            ImportGeometryIntoNewSketch(document, "rectangle", geometry);
        sketchId = imported.sketchId;

        reconstruction = ReconstructSketch(document, sketchId, geometry.dimensions);

        Parameter& padLength = document.addParameter("PadLength", kPadLength, UnitType::Millimeter);
        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", sketchId, padLength.id());
    }

    const Sketch& sketch() const { return *document.findSketch(sketchId); }

    ObjectId parameterNamed(const std::string& name) const {
        const Parameter* parameter = document.parameters().findByName(name);
        return parameter == nullptr ? kInvalidObjectId : parameter->id();
    }

    // Measured from the SOLVED entities, independently of any constraint: the
    // widest u extent and the tallest v extent of the sketch's line endpoints.
    // This is the independent geometric oracle spec 31 requires -- it does not
    // ask the constraint what it thinks the length is.
    double measuredWidth() const { return extent(true); }
    double measuredHeight() const { return extent(false); }

private:
    double extent(bool horizontal) const {
        double low = 0.0;
        double high = 0.0;
        bool first = true;
        for (const SketchEntity& entity : sketch().entities()) {
            const auto* line = std::get_if<SketchLine>(&entity.geometry);
            if (line == nullptr) continue;
            for (Vec2 point : {line->start, line->end}) {
                const double value = horizontal ? point.x : point.y;
                if (first) {
                    low = high = value;
                    first = false;
                } else {
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
            }
        }
        return high - low;
    }
};

// --- Gate A: an explicit source dimension becomes a native Parameter --------

TEST(M7ReleaseGate, GATE_A_ExplicitDimensionBecomesNativeParameterAndConstraint) {
    GateFixture fx;
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;

    const ObjectId widthId = fx.parameterNamed("Width");
    const ObjectId heightId = fx.parameterNamed("Height");
    ASSERT_NE(widthId, kInvalidObjectId);
    ASSERT_NE(heightId, kInvalidObjectId);

    // The source value was interpreted, not the drawn one.
    EXPECT_DOUBLE_EQ(fx.document.parameters().findById(widthId)->value(), kStatedWidth);
    EXPECT_DOUBLE_EQ(fx.document.parameters().findById(heightId)->value(), kStatedHeight);

    // The binding is real: a constraint in this document reads that Parameter.
    EXPECT_FALSE(fx.document.constraintsBindingParameter(widthId).empty());
    EXPECT_FALSE(fx.document.constraintsBindingParameter(heightId).empty());

    // Spec 23: PASS requires measuring native geometry, not inspecting
    // metadata. So the sketch is solved and the SHAPE is measured.
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_NEAR(fx.measuredWidth(), kStatedWidth, kGeometryTolMm);
    EXPECT_NEAR(fx.measuredHeight(), kStatedHeight, kGeometryTolMm);

    // And it is not merely the geometry that was drawn.
    EXPECT_GT(std::abs(fx.measuredWidth() - kDrawnWidth), 0.4);
}

// --- Gate B: the rectangle is fully constrained -----------------------------

TEST(M7ReleaseGate, GATE_B_RectangleIsFullyConstrained) {
    GateFixture fx;
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 0);

    // Spec 13's minimum design intent, on the NATIVE sketch.
    int coincident = 0, horizontal = 0, vertical = 0, fix = 0, length = 0;
    for (const SketchConstraint& constraint : fx.sketch().constraints()) {
        const std::string kind = ConstraintKindName(constraint.data);
        if (kind == "Coincident") ++coincident;
        if (kind == "Horizontal") ++horizontal;
        if (kind == "Vertical") ++vertical;
        if (kind == "Fix") ++fix;
        if (kind == "Length") ++length;
    }
    EXPECT_EQ(coincident, 4);
    EXPECT_EQ(horizontal, 2);
    EXPECT_EQ(vertical, 2);
    EXPECT_EQ(fix, 1);
    EXPECT_EQ(length, 2);

    // Geometry independently calculated: 100 x 50 mm.
    EXPECT_NEAR(fx.measuredWidth(), 100.0, kGeometryTolMm);
    EXPECT_NEAR(fx.measuredHeight(), 50.0, kGeometryTolMm);

    // 100 x 50 x 20 = 100000 mm^3.
    const MassProperties& mp = fx.document.massProperties();
    ASSERT_TRUE(mp.valid);
    ExpectRel(mp.volumeMm3, 100000.0, kVolumeRelTol);
}

// --- Gate C: the central M7 release gate ------------------------------------

TEST(M7ReleaseGate, GATE_C_WidthEditRebuildsTheSolid) {
    GateFixture fx;
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
    ASSERT_TRUE(fx.document.recompute().success);

    const ObjectId widthId = fx.parameterNamed("Width");
    ASSERT_NE(widthId, kInvalidObjectId);
    ASSERT_TRUE(fx.document.setParameterValue(widthId, 120.0));
    ASSERT_TRUE(fx.document.recomputeFrom(widthId).success);

    EXPECT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 0);
    EXPECT_NEAR(fx.measuredWidth(), 120.0, kGeometryTolMm);
    EXPECT_NEAR(fx.measuredHeight(), 50.0, kGeometryTolMm);

    // V = 120 x 50 x 20 = 120000 mm^3.
    const MassProperties& mp = fx.document.massProperties();
    ASSERT_TRUE(mp.valid);
    ExpectRel(mp.volumeMm3, 120000.0, kVolumeRelTol);
}

// --- Gate D: the height edit on top of it -----------------------------------

TEST(M7ReleaseGate, GATE_D_HeightEditAfterWidthEdit) {
    GateFixture fx;
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.setParameterValue(fx.parameterNamed("Width"), 120.0));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.parameterNamed("Width")).success);
    ASSERT_TRUE(fx.document.setParameterValue(fx.parameterNamed("Height"), 80.0));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.parameterNamed("Height")).success);

    EXPECT_NEAR(fx.measuredWidth(), 120.0, kGeometryTolMm);
    EXPECT_NEAR(fx.measuredHeight(), 80.0, kGeometryTolMm);

    // V = 120 x 80 x 20 = 192000 mm^3.
    ExpectRel(fx.document.massProperties().volumeMm3, 192000.0, kVolumeRelTol);
}

// --- The M7.1 gate in full, including save / fresh load ---------------------

TEST(M7ReleaseGate, M7_1_GATE_ImportReconstructSaveReloadEditRebuild) {
    std::string saved;
    ObjectId savedSketchId = kInvalidObjectId;
    {
        GateFixture fx;
        ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
        ASSERT_TRUE(fx.document.recompute().success);
        ASSERT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::Solved);
        ASSERT_EQ(fx.sketch().degreesOfFreedom(), 0);
        savedSketchId = fx.sketchId;

        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(fx.document, out));
        saved = out.str();
    }

    // Nothing about the original geometry source survives into the new process
    // -- and nothing needs to. The saved text must not name a DXF file at all.
    EXPECT_EQ(saved.find(".dxf"), std::string::npos);

    // A FRESH document and FRESH backends: reconstruction is not re-run, and
    // this is where a result that depended on the reconstructor still being
    // around would come apart.
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    std::istringstream in(saved);
    LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);

    // Identity survived.
    const Sketch* sketch = loaded.document->findSketch(savedSketchId);
    ASSERT_NE(sketch, nullptr);
    const Parameter* width = loaded.document->parameters().findByName("Width");
    const Parameter* height = loaded.document->parameters().findByName("Height");
    ASSERT_NE(width, nullptr);
    ASSERT_NE(height, nullptr);
    EXPECT_DOUBLE_EQ(width->value(), 100.0);
    EXPECT_DOUBLE_EQ(height->value(), 50.0);

    // The bindings survived: a constraint still reads each Parameter.
    EXPECT_FALSE(loaded.document->constraintsBindingParameter(width->id()).empty());
    EXPECT_FALSE(loaded.document->constraintsBindingParameter(height->id()).empty());

    ASSERT_TRUE(loaded.document->recompute().success);
    EXPECT_EQ(sketch->solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(sketch->degreesOfFreedom(), 0);

    // Width 100 -> 120, through the ordinary editing path.
    ASSERT_TRUE(loaded.document->setParameterValue(width->id(), 120.0));
    ASSERT_TRUE(loaded.document->recomputeFrom(width->id()).success);

    EXPECT_EQ(sketch->solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(sketch->degreesOfFreedom(), 0);

    // V = 120 x 50 x 20 = 120000 mm^3, in a document that has never seen a DXF
    // file, a parser, or the reconstruction code.
    const MassProperties& mp = loaded.document->massProperties();
    ASSERT_TRUE(mp.valid);
    ExpectRel(mp.volumeMm3, 120000.0, kVolumeRelTol);
}

// --- Gate F's mutation controls (spec 23) -----------------------------------
//
// Each recogniser is disabled in turn and the DOF-0 result must be lost. This
// is the evidence that each one is load-bearing; without it, "the rectangle
// solves" could be true for reasons none of them contribute to.

struct ToggleFixture {
    PartDocument document{"M7Toggle"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    ObjectId sketchId{kInvalidObjectId};

    explicit ToggleFixture(const ReconstructionOptions& options) {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        const ImportedSketchGeometry geometry = DimensionedRectangle();
        sketchId = ImportGeometryIntoNewSketch(document, "rectangle", geometry).sketchId;
        ReconstructSketch(document, sketchId, geometry.dimensions, options);
        document.recompute();
    }

    const Sketch& sketch() const { return *document.findSketch(sketchId); }
};

TEST(M7ReleaseGate, GATE_F_DisablingHorizontalRecognitionLosesDof0) {
    ReconstructionOptions options;
    options.recognizeHorizontal = false;
    ToggleFixture fx(options);
    EXPECT_NE(fx.sketch().degreesOfFreedom(), 0);
}

TEST(M7ReleaseGate, GATE_F_DisablingVerticalRecognitionLosesDof0) {
    ReconstructionOptions options;
    options.recognizeVertical = false;
    ToggleFixture fx(options);
    EXPECT_NE(fx.sketch().degreesOfFreedom(), 0);
}

TEST(M7ReleaseGate, GATE_F_DisablingCoincidentRecognitionLosesDof0) {
    ReconstructionOptions options;
    options.recognizeCoincident = false;
    ToggleFixture fx(options);
    EXPECT_NE(fx.sketch().degreesOfFreedom(), 0);
}

TEST(M7ReleaseGate, GATE_F_DisablingTheFixPlacementLosesDof0) {
    ReconstructionOptions options;
    options.placeFix = false;
    ToggleFixture fx(options);
    // Without a Fix the shape is right and its POSITION is free: exactly two
    // remaining degrees of freedom, which is a fact about the plane and not
    // about this fixture.
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 2);
}

TEST(M7ReleaseGate, GATE_F_DisablingExplicitDimensionsLosesDof0) {
    ReconstructionOptions options;
    options.reconstructExplicitDimensions = false;
    ToggleFixture fx(options);
    // The shape and placement are fixed but the SIZE is free: Width and Height.
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 2);
    EXPECT_EQ(fx.document.parameters().findByName("Width"), nullptr);
}

// --- M7.2: general recognition, measured through the real solver ------------
//
// The Core tests count planned constraints. These check what the SOLVER makes
// of them, which is the only place an over-constrained plan actually shows up.

struct SolvedSketch {
    PartDocument document{"M7General"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    ObjectId sketchId{kInvalidObjectId};

    SolvedSketch() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
    }

    Sketch& begin() {
        Sketch& sketch = document.addSketch("Imported");
        sketchId = sketch.id();
        return sketch;
    }

    void reconstructAndSolve() {
        ReconstructSketch(document, sketchId, {});
        document.recompute();
    }

    const Sketch& sketch() const { return *document.findSketch(sketchId); }
};

TEST(M7ReleaseGate, GATE_F2_AThreeWayJunctionIsNotOverConstrained) {
    SolvedSketch fx;
    Sketch& sketch = fx.begin();
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addLine(Vec2{0, 0}, Vec2{0, 40});
    sketch.addLine(Vec2{0, 0}, Vec2{-30, 0});
    fx.reconstructAndSolve();

    EXPECT_NE(fx.sketch().solveStatus(), SketchSolveStatus::OverConstrained);
    EXPECT_NE(fx.sketch().solveStatus(), SketchSolveStatus::Conflicting);

    // 3 lines x 4 = 12 variables; 2 Coincident x 2 + 2 Horizontal + 1 Vertical
    // + 1 Fix x 2 = 9 residuals. DOF = 3, computed by hand.
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 3);

    // NOTE, because it limits what this test proves: it does NOT discriminate
    // between a spanning set and all pairs. M5 gives DOF priority over
    // redundancy, so while DOF > 0 the extra rows are invisible here and this
    // test passes either way -- verified by running it against that mutation.
    // The test below is the one that sees the difference.
}

TEST(M7ReleaseGate, GATE_F2_AFullyDimensionedJunctionSolvesRatherThanOverConstrains) {
    SolvedSketch fx;
    Sketch& sketch = fx.begin();
    const SketchEntityId right = sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    const SketchEntityId up = sketch.addLine(Vec2{0, 0}, Vec2{0, 40});
    const SketchEntityId left = sketch.addLine(Vec2{0, 0}, Vec2{-30, 0});
    ReconstructSketch(fx.document, fx.sketchId, {});

    // Take the junction the rest of the way to DOF 0 by hand. This is where a
    // redundant Coincident stops being masked: with no freedom left, M5 makes
    // redundancy the headline, and an ordinary T-junction would report
    // OverConstrained -- which blocks downstream rebuild under M5's contracts.
    //
    // The harm is DEFERRED, appearing only once the user finishes dimensioning
    // rather than when the junction is drawn, so it has to be provoked
    // deliberately. It cannot be seen on the release rectangle at all, whose
    // clusters all have two members.
    const auto length = [&](SketchEntityId line, double mm) {
        Parameter& parameter = fx.document.addParameter(
            "L" + std::to_string(static_cast<unsigned long long>(line)), mm,
            UnitType::Millimeter);
        fx.document.addSketchConstraint(fx.sketchId, LengthConstraint{line, parameter.id()});
    };
    length(right, 50.0);
    length(up, 40.0);
    length(left, 30.0);
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 0);
    EXPECT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::Solved)
        << fx.sketch().solveMessage();
}

TEST(M7ReleaseGate, GATE_F2_AnLShapeSolvesToItsHandCountedDof) {
    SolvedSketch fx;
    Sketch& sketch = fx.begin();
    sketch.addLine(Vec2{0, 0}, Vec2{60, 0});
    sketch.addLine(Vec2{60, 0}, Vec2{60, 20});
    sketch.addLine(Vec2{60, 20}, Vec2{20, 20});
    sketch.addLine(Vec2{20, 20}, Vec2{20, 50});
    sketch.addLine(Vec2{20, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
    fx.reconstructAndSolve();

    EXPECT_NE(fx.sketch().solveStatus(), SketchSolveStatus::OverConstrained);
    EXPECT_NE(fx.sketch().solveStatus(), SketchSolveStatus::Conflicting);

    // 6 lines x 4 = 24 variables; 6 Coincident x 2 + 3 Horizontal + 3 Vertical
    // + 1 Fix x 2 = 20 residuals. DOF = 4 -- the four free lengths of the L.
    // A shape M7.1 refused entirely, now fully recognised except its sizes.
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 4);
}

TEST(M7ReleaseGate, GATE_F2_ASkewedRectangleIsStraightenedByTheSolverNotTheReader) {
    SolvedSketch fx;
    Sketch& sketch = fx.begin();
    // Drawn with every corner missing and every side off axis, all inside the
    // recognition tolerances.
    const double gap = 0.4 * kReconstructionCoincidenceToleranceMm;
    const double skew = 0.4 * kReconstructionAxisAngleToleranceRad;
    sketch.addLine(Vec2{0, 0}, Vec2{60, 60 * std::sin(skew)});
    sketch.addLine(Vec2{60, 60 * std::sin(skew) + gap}, Vec2{60 + gap, 40});
    sketch.addLine(Vec2{60, 40}, Vec2{gap, 40 + gap});
    sketch.addLine(Vec2{0, 40}, Vec2{gap, 0});
    fx.reconstructAndSolve();

    // No dimensions here, so the SHAPE is determined and the two SIZES are not:
    // 16 variables - (4 Coincident x 2 + 2 Horizontal + 2 Vertical + 1 Fix x 2)
    // = 2. Under-constrained is the correct outcome, not a failure, and M5
    // still commits the solved geometry -- which is what the check below reads.
    EXPECT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::UnderConstrained);
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 2);

    // Tolerance is a RECOGNITION rule, not permission to rewrite geometry
    // (spec 11, ADR-M7-011). Nothing snapped the coordinates: the corners are
    // closed and the sides are on-axis because the SOLVER moved them, and the
    // proof is that they are now exact rather than merely within tolerance.
    for (const SketchEntity& entity : fx.sketch().entities()) {
        const auto* line = std::get_if<SketchLine>(&entity.geometry);
        ASSERT_NE(line, nullptr);
        const double du = std::abs(line->end.x - line->start.x);
        const double dv = std::abs(line->end.y - line->start.y);
        // Exactly axis-aligned now -- far tighter than the 1e-3 rad that let it
        // be recognised in the first place.
        EXPECT_TRUE(du < 1e-9 || dv < 1e-9)
            << "a side is still off axis: du=" << du << " dv=" << dv;
    }
}

// --- Selective recompute (spec 21, Gate K) ----------------------------------

TEST(M7ReleaseGate, GATE_K_DensityDoesNotResolveTheSketch) {
    class CountingSolver final : public ISketchSolver {
    public:
        SketchSolveResult solve(const SketchSolveProblem& problem) override {
            ++calls;
            return inner.solve(problem);
        }
        int calls = 0;
        GaussNewtonSketchSolver inner;
    };

    PartDocument document{"M7Selective"};
    OcctGeometryKernel kernel;
    CountingSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Aluminium", 2700.0);

    const ImportedSketchGeometry geometry = DimensionedRectangle();
    const ObjectId sketchId = ImportGeometryIntoNewSketch(document, "rectangle", geometry).sketchId;
    ASSERT_TRUE(ReconstructSketch(document, sketchId, geometry.dimensions));
    Parameter& padLength = document.addParameter("PadLength", kPadLength, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketchId, padLength.id());
    ASSERT_TRUE(document.recompute().success);

    const int afterFirstSolve = solver.calls;
    ASSERT_GT(afterFirstSolve, 0);

    // Density touches mass properties and nothing else. Counted, not inferred
    // from an unchanged number -- spec 21 is explicit that equal final values
    // are not evidence.
    ASSERT_TRUE(document.setMaterialDensity(7800.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(solver.calls, afterFirstSolve);

    // A Width edit, by contrast, must solve.
    ASSERT_TRUE(document.setParameterValue(document.parameters().findByName("Width")->id(), 120.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_GT(solver.calls, afterFirstSolve);
}

} // namespace
