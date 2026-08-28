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

// --- Gate E: circle dimension drives real 3D volume (spec 23) ---------------

struct GateCircleFixture {
    PartDocument document{"M7GateCircle"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    ObjectId sketchId{kInvalidObjectId};
    ReconstructionResult reconstruction;

    // `drawnRadius` is deliberately NOT the dimensioned one, so the first solve
    // cannot be a no-op -- but only by 2%, inside the agreement band.
    //
    // A curve dimension is associated by CENTRE and cross-checked on SIZE
    // (ADR-M7-014), which is what tells a hole from the counterbore around it.
    // So unlike the rectangle, a circle drawn at 7 and dimensioned 10 is not a
    // fixture that proves the solver works -- it is a drawing that contradicts
    // itself, and reconstruction correctly refuses it. The real proof that the
    // solver drives this geometry is Gate E's edit, which cannot be a no-op
    // under any implementation.
    explicit GateCircleFixture(ImportedDimensionKind kind, double statedValue,
                               double drawnRadius = 9.8) {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);

        ImportedSketchGeometry geometry;
        geometry.unit = ImportedLengthUnit::Millimeter;
        geometry.millimetresPerUnit = 1.0;
        geometry.circles.push_back(ImportedCircle2D{Vec2{25, 30}, drawnRadius});

        ImportedDimension2D dimension;
        dimension.kind = kind;
        const double half = statedValue * 0.5 / std::sqrt(2.0);
        const double leg = statedValue / std::sqrt(2.0);
        if (kind == ImportedDimensionKind::Diameter) {
            dimension.measureFrom = Vec2{25 - half, 30 - half};
            dimension.measureTo = Vec2{25 + half, 30 + half};
        } else {
            dimension.measureFrom = Vec2{25, 30};
            dimension.measureTo = Vec2{25 + leg, 30 + leg};
        }
        dimension.sourceHandle = "3C";
        geometry.dimensions.push_back(dimension);

        sketchId = ImportGeometryIntoNewSketch(document, "circle", geometry).sketchId;
        reconstruction = ReconstructSketch(document, sketchId, geometry.dimensions);

        Parameter& padLength = document.addParameter("PadLength", 30.0, UnitType::Millimeter);
        Body& body = document.addBody("Body001");
        document.addPadFeature(body, "Pad001", sketchId, padLength.id());
    }

    const Sketch& sketch() const { return *document.findSketch(sketchId); }

    double solvedRadius() const {
        for (const SketchEntity& entity : sketch().entities())
            if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry))
                return circle->radiusMm;
        return -1.0;
    }
};

TEST(M7ReleaseGate, GATE_E_RadiusReconstructsSolvesAndDrivesVolume) {
    GateCircleFixture fx(ImportedDimensionKind::Radius, 10.0);
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 0);

    // Drawn at 7, dimensioned 10: the solver moved it.
    EXPECT_NEAR(fx.solvedRadius(), 10.0, 1e-9);

    // V = pi r^2 h = pi x 100 x 30, computed here from pi and the parameters,
    // never read back from the kernel.
    const MassProperties& mp = fx.document.massProperties();
    ASSERT_TRUE(mp.valid);
    ExpectRel(mp.volumeMm3, kPi * 100.0 * 30.0, 1e-6);
}

TEST(M7ReleaseGate, GATE_E_DoublingTheRadiusQuadruplesTheVolume) {
    GateCircleFixture fx(ImportedDimensionKind::Radius, 10.0);
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
    ASSERT_TRUE(fx.document.recompute().success);
    const double before = fx.document.massProperties().volumeMm3;

    const Parameter* radius = fx.document.parameters().findByName("Radius");
    ASSERT_NE(radius, nullptr);
    ASSERT_TRUE(fx.document.setParameterValue(radius->id(), 20.0));
    ASSERT_TRUE(fx.document.recomputeFrom(radius->id()).success);

    EXPECT_NEAR(fx.solvedRadius(), 20.0, 1e-9);

    // Spec 23 Gate E, stated as a RATIO: with the extrusion length unchanged,
    // doubling the radius must give exactly 4x. A ratio cannot be satisfied by
    // a scale error that an absolute value might hide -- a unit mistake would
    // move both numbers and leave the ratio intact only if it were genuinely a
    // scale, which is the point.
    const double after = fx.document.massProperties().volumeMm3;
    ExpectRel(after / before, 4.0, 1e-9);
    ExpectRel(after, kPi * 400.0 * 30.0, 1e-6);
}

TEST(M7ReleaseGate, GATE_E_DiameterDrivesTheSameGeometryAsRadiusWould) {
    // The same hole, dimensioned the other way: diameter 20 instead of
    // radius 10. The Parameter reads 20, the geometry is radius 10, and the
    // volume is identical to the radial fixture above.
    GateCircleFixture fx(ImportedDimensionKind::Diameter, 20.0);
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
    ASSERT_TRUE(fx.document.recompute().success);

    const Parameter* diameter = fx.document.parameters().findByName("Diameter");
    ASSERT_NE(diameter, nullptr);
    EXPECT_DOUBLE_EQ(diameter->value(), 20.0);

    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 0);
    EXPECT_NEAR(fx.solvedRadius(), 10.0, 1e-9);
    ExpectRel(fx.document.massProperties().volumeMm3, kPi * 100.0 * 30.0, 1e-6);

    // Editing the DIAMETER halves into the radius, which is what "one quantity,
    // two views" has to mean in practice (spec 12).
    ASSERT_TRUE(fx.document.setParameterValue(diameter->id(), 40.0));
    ASSERT_TRUE(fx.document.recomputeFrom(diameter->id()).success);
    EXPECT_NEAR(fx.solvedRadius(), 20.0, 1e-9);
    ExpectRel(fx.document.massProperties().volumeMm3, kPi * 400.0 * 30.0, 1e-6);
}

// --- Gate H: conflict and recovery (spec 23) --------------------------------

TEST(M7ReleaseGate, GATE_H_ConflictIsExplicitCommitsNothingCorruptAndRecovers) {
    GateFixture fx;
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
    ASSERT_TRUE(fx.document.recompute().success);
    const double goodVolume = fx.document.massProperties().volumeMm3;
    ExpectRel(goodVolume, 100000.0, kVolumeRelTol);

    // A second Length on the bottom edge that disagrees with Width: 100 and 60
    // cannot both hold. Added directly rather than through reconstruction,
    // because a second run is refused (ADR-M7-007) and this is about the
    // solver's response to conflicting intent, not about the analysis.
    SketchEntityId bottom = kInvalidSketchEntityId;
    for (const SketchConstraint& constraint : fx.sketch().constraints())
        if (const auto* length = std::get_if<LengthConstraint>(&constraint.data)) {
            const ObjectId bound = BoundParameterId(constraint.data);
            const Parameter* parameter = fx.document.parameters().findById(bound);
            if (parameter != nullptr && parameter->name() == "Width") bottom = length->line;
        }
    ASSERT_NE(bottom, kInvalidSketchEntityId);

    Parameter& rival = fx.document.addParameter("Rival", 60.0, UnitType::Millimeter);
    const SketchConstraintId offender =
        fx.document.addSketchConstraint(fx.sketchId, LengthConstraint{bottom, rival.id()});
    ASSERT_NE(offender, kInvalidSketchConstraintId);
    fx.document.recompute();

    // Explicit conflict, not a quiet wrong answer.
    EXPECT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::Conflicting)
        << SolveStatusName(fx.sketch().solveStatus()) << ": " << fx.sketch().solveMessage();

    // DOWNSTREAM STALE STATE, which spec 23 Gate H names and this gate did not
    // check. Mass properties must stop being current while the sketch is
    // conflicting -- otherwise the status bar reports a volume for geometry the
    // solver has refused, which is spec 34's "stale 3D geometry displayed as
    // current".
    EXPECT_FALSE(fx.document.massProperties().valid)
        << "mass properties are still current while the sketch is Conflicting";
    // Spec 17: offending constraints identified where possible.
    EXPECT_FALSE(fx.sketch().offendingConstraints().empty());

    // No corrupt geometry was committed: the sketch still measures what it did
    // before the conflict, rather than some half-solved compromise between 100
    // and 60. This is the "solver reports a state while geometry is wrong"
    // Critical of spec 34, checked by measurement rather than by status.
    EXPECT_NEAR(fx.measuredWidth(), 100.0, kGeometryTolMm);

    // Recovery: remove the offender, and the model comes back.
    ASSERT_TRUE(fx.document.removeSketchConstraint(fx.sketchId, offender));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fx.sketch().degreesOfFreedom(), 0);
    EXPECT_TRUE(fx.document.massProperties().valid)
        << "mass properties did not become current again after recovery";
    ExpectRel(fx.document.massProperties().volumeMm3, goodVolume, kVolumeRelTol);

    // And a Parameter edit still drives geometry afterwards -- the part that
    // proves recovery was real rather than merely quiet.
    const ObjectId widthId = fx.parameterNamed("Width");
    ASSERT_TRUE(fx.document.setParameterValue(widthId, 120.0));
    ASSERT_TRUE(fx.document.recomputeFrom(widthId).success);
    EXPECT_EQ(fx.sketch().solveStatus(), SketchSolveStatus::Solved);
    ExpectRel(fx.document.massProperties().volumeMm3, 120000.0, kVolumeRelTol);
}

// --- Selective recompute (spec 21, Gate K) ----------------------------------

TEST(M7ReleaseGate, GATE_K_NoParameterEditEverReRunsReconstruction) {
    GateFixture fx;
    ASSERT_TRUE(fx.reconstruction) << fx.reconstruction.message;
    ASSERT_TRUE(fx.document.recompute().success);

    const std::size_t parametersAfterReconstruction = fx.document.parameters().items().size();
    const std::size_t constraintsAfterReconstruction = fx.sketch().constraints().size();

    // Reconstruction is an EXPLICIT ONE-SHOT OPERATION, not a graph node. It is
    // not scheduled, cannot be dirtied, and no recompute can invoke it -- which
    // is the architectural reason spec 23's "PadLength does not re-run
    // reconstruction" holds.
    //
    // NOTE ON WHAT THIS PROVES, because independent review showed the earlier
    // version proved less than it claimed. Counting Parameters and constraints
    // cannot distinguish "reconstruction never ran" from "it ran and the
    // idempotence guard refused it" -- a reviewer inserted a REAL second
    // ReconstructSketch call here and the test still passed. The counts below
    // are therefore a necessary check, not a sufficient one; the KERNEL and
    // SOLVER counters added afterwards are what carry the selectivity claim.
    const ObjectId padLength = fx.parameterNamed("PadLength");
    ASSERT_NE(padLength, kInvalidObjectId);
    ASSERT_TRUE(fx.document.setParameterValue(padLength, 35.0));
    ASSERT_TRUE(fx.document.recomputeFrom(padLength).success);
    EXPECT_EQ(fx.document.parameters().items().size(), parametersAfterReconstruction);
    EXPECT_EQ(fx.sketch().constraints().size(), constraintsAfterReconstruction);

    // ...and the pad length DID take effect: 100 x 50 x 35.
    ExpectRel(fx.document.massProperties().volumeMm3, 175000.0, kVolumeRelTol);

    // An unrelated Parameter touches nothing at all.
    Parameter& unrelated = fx.document.addParameter("Unrelated", 3.0, UnitType::Millimeter);
    const double before = fx.document.massProperties().volumeMm3;
    ASSERT_TRUE(fx.document.setParameterValue(unrelated.id(), 9.0));
    ASSERT_TRUE(fx.document.recomputeFrom(unrelated.id()).success);
    EXPECT_EQ(fx.sketch().constraints().size(), constraintsAfterReconstruction);
    ExpectRel(fx.document.massProperties().volumeMm3, before, kVolumeRelTol);
    EXPECT_NEAR(fx.measuredWidth(), 100.0, kGeometryTolMm);
}

// Counting backends, so selectivity is measured rather than inferred from equal
// final values -- which spec 21 says explicitly is not evidence.
class CountingSolverBackend final : public ISketchSolver {
public:
    SketchSolveResult solve(const SketchSolveProblem& problem) override {
        ++calls;
        return inner.solve(problem);
    }
    int calls = 0;
    GaussNewtonSketchSolver inner;
};

class CountingKernelBackend final : public IGeometryKernel {
public:
    ShapeResult createBox(const BoxDefinition& definition) override {
        return inner.createBox(definition);
    }
    ShapeResult extrudeProfile(const PlanarProfileDefinition& profile, double distanceMm) override {
        ++extrudes;
        return inner.extrudeProfile(profile, distanceMm);
    }
    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override {
        ++massCalls;
        return inner.calculateMassProperties(shape);
    }
    ShapeResult subtractShape(const KernelShape& base, const KernelShape& tool) override {
        ++subtracts;
        return inner.subtractShape(base, tool);
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
        return inner.rotateShape(shape, axisOriginMm, axisDirection, angleRad);
    }
    KernelInterferenceResult measureInterference(const KernelShape& a,
                                                 const KernelShape& b) override {
        (void)a;
        (void)b;
        return KernelInterferenceResult{false, "this kernel does not measure interference", 0.0};
    }
    ShapeResult intersectShapes(const KernelShape& a, const KernelShape& b) override {
        return inner.intersectShapes(a, b);
    }
    ShapeResult shellSolid(const KernelShape& base, const FaceSelection& openFaces,
                           double thicknessMm) override {
        return inner.shellSolid(base, openFaces, thicknessMm);
    }
    ShapeResult draftFaces(const KernelShape& base, const FaceSelection& faces,
                           const FaceQuery& neutral, double angleRad) override {
        return inner.draftFaces(base, faces, neutral, angleRad);
    }
    IoResult exportStep(const KernelShape& shape, const std::string& path) override {
        return inner.exportStep(shape, path);
    }
    ShapeResult importStep(const std::string& path) override {
        return inner.importStep(path);
    }
    IoResult exportIges(const KernelShape& shape, const std::string& path) override {
        return inner.exportIges(shape, path);
    }
    ShapeResult importIges(const std::string& path) override {
        return inner.importIges(path);
    }
    ShapeKind kindOfShape(const KernelShape& shape) override {
        return inner.kindOfShape(shape);
    }
    ShapeResult importSurfaces(const std::string& path) override {
        return inner.importSurfaces(path);
    }
    ShapeResult thickenSurface(const KernelShape& shape, double thicknessMm) override {
        return inner.thickenSurface(shape, thicknessMm);
    }
    ShapeResult solidFromSkin(const KernelShape& shape) override {
        return inner.solidFromSkin(shape);
    }
    IoResult exportStl(const KernelShape& shape, const std::string& path,
                       double deflectionMm) override {
        return inner.exportStl(shape, path, deflectionMm);
    }
    KernelBoundsResult boundsOfShape(const KernelShape& shape) override {
        return inner.boundsOfShape(shape);
    }
    ShapeResult sweepProfile(const PlanarProfileDefinition& profile,
                             const PlanarPathDefinition& path) override {
        return inner.sweepProfile(profile, path);
    }
    ShapeResult loftProfiles(const std::vector<PlanarProfileDefinition>& profiles)
        override {
        return inner.loftProfiles(profiles);
    }
    ShapeResult revolveProfile(const PlanarProfileDefinition& profile, const Vec3& axisOriginMm,
                               const Vec3& axisDirection, double angleRad) override {
        ++revolveCallCount;
        return inner.revolveProfile(profile, axisOriginMm, axisDirection, angleRad);
    }
    ShapeResult filletEdges(const KernelShape& shape, const EdgeSelection& selection,
                            double radiusMm) override {
        ++filletCallCount;
        return inner.filletEdges(shape, AllEdgesSelection(), radiusMm);
    }
    ShapeResult chamferEdges(const KernelShape& shape, const EdgeSelection& selection,
                             double distanceMm) override {
        ++chamferCallCount;
        return inner.chamferEdges(shape, AllEdgesSelection(), distanceMm);
    }
    // M10.6 verbs. Forwarded, uncounted: these suites predate them and none
    // of their gates is about mirroring, so counting would add a member every
    // fixture has to ignore.
    ShapeResult mirrorShape(const KernelShape& shape, const Vec3& planeOriginMm,
                            const Vec3& planeNormal) override {
        return inner.mirrorShape(shape, planeOriginMm, planeNormal);
    }
    ShapeResult translateShape(const KernelShape& shape, const Vec3& offsetMm) override {
        return inner.translateShape(shape, offsetMm);
    }
    int countSolids(const KernelShape& shape) override {
        (void)shape;
        return 0;
    }
    ShapeResult compoundOf(const std::vector<KernelShape>& shapes) override {
        (void)shapes;
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "this kernel does not build compounds"};
    }
    ShapeResult fuseShapes(const KernelShape& a, const KernelShape& b) override {
        return inner.fuseShapes(a, b);
    }
    int extrudes = 0;
    int massCalls = 0;
    int subtracts = 0;
    int revolveCallCount = 0;
    int filletCallCount = 0;
    int chamferCallCount = 0;
    OcctGeometryKernel inner;
};

TEST(M7ReleaseGate, GATE_K_CountersProveWhichStagesEachEditRuns) {
    PartDocument document{"M7GateK"};
    CountingKernelBackend kernel;
    CountingSolverBackend solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Aluminium", 2700.0);

    const ImportedSketchGeometry geometry = DimensionedRectangle();
    const ObjectId sketchId = ImportGeometryIntoNewSketch(document, "rectangle", geometry).sketchId;
    ASSERT_TRUE(ReconstructSketch(document, sketchId, geometry.dimensions));
    Parameter& padLength = document.addParameter("PadLength", kPadLength, UnitType::Millimeter);
    Parameter& unrelated = document.addParameter("Unrelated", 3.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketchId, padLength.id());
    ASSERT_TRUE(document.recompute().success);

    // WIDTH: solves the sketch AND rebuilds downstream.
    int solves = solver.calls;
    int extrudes = kernel.extrudes;
    ASSERT_TRUE(document.setParameterValue(document.parameters().findByName("Width")->id(), 120.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_GT(solver.calls, solves) << "a Width edit did not re-solve the sketch";
    EXPECT_GT(kernel.extrudes, extrudes) << "a Width edit did not rebuild the Pad";

    // PADLENGTH: rebuilds the Pad, does NOT re-solve the sketch.
    solves = solver.calls;
    extrudes = kernel.extrudes;
    ASSERT_TRUE(document.setParameterValue(padLength.id(), 35.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(solver.calls, solves) << "a PadLength edit re-solved the sketch";
    EXPECT_GT(kernel.extrudes, extrudes) << "a PadLength edit did not rebuild the Pad";

    // UNRELATED: touches neither.
    solves = solver.calls;
    extrudes = kernel.extrudes;
    const int massCalls = kernel.massCalls;
    ASSERT_TRUE(document.setParameterValue(unrelated.id(), 9.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(solver.calls, solves) << "an unrelated Parameter re-solved the sketch";
    EXPECT_EQ(kernel.extrudes, extrudes) << "an unrelated Parameter rebuilt the Pad";
    EXPECT_EQ(kernel.massCalls, massCalls) << "an unrelated Parameter recomputed mass properties";
}

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
