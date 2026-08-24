// M10 release gates (M10 spec §7), executed against the REAL solver and REAL
// OCCT geometry.
//
// The fixture is a 100 × 50 rectangle padded 20 mm, supported by a frame.
// **Volume is 100000 mm³ in every gate here, deliberately**, so no gate can
// pass on volume alone — the centre of mass is what discriminates. That is
// M8's GATE_RB2 lesson applied before the fact rather than after: an oracle is
// only as good as the coincidences it avoids.
//
//   frame at origin          → COM (50, 25, 10)
//   frame translated +30 Z   → COM (50, 25, 40)
//   frame rotated 90° about X→ COM (50, −10, 25)
//
// The rotation: about +X by 90° sends (x, y, z) → (x, −z, y), so the local
// centroid (50, 25, 10) lands at (50, −10, 25). Hand-computed, not read back.

#include "Core/Document/PartDocument.h"
#include "Core/Reference/ReferenceFrame.h"
#include "Core/Connector/Connector.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/TransformFeatures.h"
#include "Core/Geometry/Transform.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

void ExpectNear3(const Vec3& actual, const Vec3& expected, double tol = 1e-6) {
    EXPECT_NEAR(actual.x, expected.x, tol);
    EXPECT_NEAR(actual.y, expected.y, tol);
    EXPECT_NEAR(actual.z, expected.z, tol);
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

// A frame, an unconstrained 100 x 50 rectangle on it, and a 20 mm pad.
// Unconstrained on purpose: this milestone is about WHERE the geometry is, and
// a constraint system would put a second mechanism between the frame and the
// answer.
struct FrameFixture {
    PartDocument document{"M10Frame"};
    CountingKernel kernel;
    CountingSolver solver;
    ObjectId frameId = kInvalidObjectId;
    Sketch* sketch = nullptr;
    PadFeature* pad = nullptr;

    FrameFixture() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);
        Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        frameId = document.addFrame("Frame001").id();

        Sketch& s = document.addSketch("Sketch001");
        sketch = &s;
        s.addLine(Vec2{0, 0}, Vec2{100, 0});
        s.addLine(Vec2{100, 0}, Vec2{100, 50});
        s.addLine(Vec2{100, 50}, Vec2{0, 50});
        s.addLine(Vec2{0, 50}, Vec2{0, 0});

        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", s.id(), length.id());
    }

    double volume() const { return document.massProperties().volumeMm3; }
    Vec3 com() const { return document.massProperties().centerOfMassMm; }
};

// --- Gate A: a frame is a document object -------------------------------------

TEST(M10ReleaseGate, GATE_A_AFrameIsRegisteredResolvableAndAGraphNode) {
    FrameFixture fx;
    // Before M10 `addFrame` pushed into a vector and stopped: the id resolved
    // to nothing and `removeObject` could not see it.
    EXPECT_TRUE(fx.document.objectRegistry().contains(fx.frameId));
    ASSERT_NE(fx.document.findFrame(fx.frameId), nullptr);
    EXPECT_EQ(fx.document.findFrame(fx.frameId)->name(), "Frame001");
    EXPECT_TRUE(fx.document.dependencyGraph().hasNode(fx.frameId));

    // The Origin frame the constructor makes is one too, and it is NOT an undo
    // step -- constructing a document is not something the user did.
    ASSERT_EQ(fx.document.frames().size(), 2u);
    EXPECT_EQ(fx.document.frames().front()->name(), "Origin");

    ASSERT_TRUE(fx.document.removeObject(fx.frameId));
    EXPECT_EQ(fx.document.findFrame(fx.frameId), nullptr);
    EXPECT_FALSE(fx.document.objectRegistry().contains(fx.frameId));
    EXPECT_FALSE(fx.document.dependencyGraph().hasNode(fx.frameId));
}

// --- Gate B: hierarchy and cycles ---------------------------------------------

TEST(M10ReleaseGate, GATE_B_WorldTransformIsComposedFromTheParentChain) {
    PartDocument document{"M10Hierarchy"};
    ReferenceFrame& parent = document.addFrame("Parent");
    ReferenceFrame& child = document.addFrame("Child", parent.id());

    // Parent: rotate 90 degrees about X, then translate +10 in X.
    // Child: offset +0/+0/+7 in the PARENT's frame.
    Transform3D parentLocal;
    parentLocal.translation = Vec3{10.0, 0.0, 0.0};
    parentLocal.rotation = Quaternion{std::cos(kPi / 4.0), std::sin(kPi / 4.0), 0.0, 0.0};
    ASSERT_TRUE(document.setFrameTransform(parent.id(), parentLocal));

    Transform3D childLocal;
    childLocal.translation = Vec3{0.0, 0.0, 7.0};
    ASSERT_TRUE(document.setFrameTransform(child.id(), childLocal));

    // Hand-composed: the parent's rotation sends the child's +7 Z offset to
    // -7 Y, then the parent's own translation adds +10 X.
    // This is the term that is easy to omit -- adding the translations without
    // rotating the child's would give (10, 0, 7), which is why the fixture
    // rotates AND offsets rather than doing one of them.
    const Transform3D world = document.worldTransform(child.id());
    ExpectNear3(world.translation, Vec3{10.0, -7.0, 0.0});

    // And the root's world transform is its own local transform.
    ExpectNear3(document.worldTransform(parent.id()).translation, Vec3{10.0, 0.0, 0.0});
}

TEST(M10ReleaseGate, GATE_B2_ACyclicParentIsRefusedAtTheDoor) {
    PartDocument document{"M10Cycle"};
    ReferenceFrame& a = document.addFrame("A");
    ReferenceFrame& b = document.addFrame("B", a.id());

    // A parented to its own descendant would close a loop. Refused BEFORE it
    // exists, because a cycle discovered at recompute time is an unbounded
    // walk -- and M9.1 already paid for learning what an unbounded walk costs.
    EXPECT_THROW(document.setFrameParent(a.id(), b.id()), std::runtime_error);
    EXPECT_EQ(a.parentFrameId(), kInvalidObjectId) << "the refused re-parent was applied anyway";

    // Self-parenting is the one-frame case of the same rule.
    EXPECT_THROW(document.setFrameParent(b.id(), b.id()), std::runtime_error);

    // A parent that is not a frame at all.
    Parameter& p = document.addParameter("P", 1.0, UnitType::Millimeter);
    EXPECT_THROW(document.addFrame("C", p.id()), std::runtime_error);

    // The document is still savable: every refusal happened before anything
    // was stored.
    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(document, out));
}

// --- Gate C: frame edits are undoable -----------------------------------------

TEST(M10ReleaseGate, GATE_C_FrameEditsAreOneUndoStepEach) {
    PartDocument document{"M10Undo"};
    ReferenceFrame& frame = document.addFrame("Frame001");
    const std::size_t depthAfterAdd = document.undoDepth();
    EXPECT_EQ(document.nextUndoLabel(), "Add Frame001");

    Transform3D moved;
    moved.translation = Vec3{0.0, 0.0, 30.0};
    ASSERT_TRUE(document.setFrameTransform(frame.id(), moved));
    EXPECT_EQ(document.undoDepth(), depthAfterAdd + 1);
    EXPECT_EQ(document.nextUndoLabel(), "Move Frame001");

    ASSERT_TRUE(document.undo());
    ExpectNear3(document.findFrame(frame.id())->localTransform().translation, Vec3{0, 0, 0});
    ASSERT_TRUE(document.redo());
    ExpectNear3(document.findFrame(frame.id())->localTransform().translation, Vec3{0, 0, 30});

    // Undoing the CREATION removes the frame, with its registry entry and its
    // graph node -- and redo brings it back with the SAME ObjectId, which is
    // what every reference to it depends on.
    const ObjectId id = frame.id();
    ASSERT_TRUE(document.undo()); // the move
    ASSERT_TRUE(document.undo()); // the creation
    EXPECT_EQ(document.findFrame(id), nullptr);
    EXPECT_FALSE(document.objectRegistry().contains(id));
    ASSERT_TRUE(document.redo());
    ASSERT_NE(document.findFrame(id), nullptr);
    EXPECT_EQ(document.findFrame(id)->id(), id);
    EXPECT_TRUE(document.dependencyGraph().hasNode(id));
}


// --- Gate D: THE RELEASE PROOF -------------------------------------------------

TEST(M10ReleaseGate, GATE_D_MovingTheFrameMovesEverythingBuiltOnIt) {
    FrameFixture fx;
    ASSERT_TRUE(fx.document.setSketchSupportFrame(fx.sketch->id(), fx.frameId));
    ASSERT_TRUE(fx.document.recompute().success);

    // Volume is 100000 in ALL THREE states below. That is the point: a gate
    // that checked volume would pass no matter where the solid ended up, which
    // is the coincidence M8's GATE_RB2 was rewritten to avoid.
    ASSERT_TRUE(fx.document.massProperties().valid);
    EXPECT_NEAR(fx.volume(), 100000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 10.0});

    // Translate the FRAME. No edit to the sketch, no edit to the pad.
    Transform3D lifted;
    lifted.translation = Vec3{0.0, 0.0, 30.0};
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, lifted));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_NEAR(fx.volume(), 100000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 40.0});

    // Rotate the frame 90 degrees about +X: (x, y, z) -> (x, -z, y), so the
    // local centroid (50, 25, 10) lands at (50, -10, 25). Hand-computed.
    Transform3D turned;
    turned.rotation = Quaternion{std::cos(kPi / 4.0), std::sin(kPi / 4.0), 0.0, 0.0};
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, turned));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_NEAR(fx.volume(), 100000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{50.0, -10.0, 25.0});

    // Back to the origin, exactly.
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, Transform3D::Identity()));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 10.0});
}

TEST(M10ReleaseGate, GATE_D2_ASketchWithNoSupportFrameIsUnaffected) {
    // The negative control, and the compatibility contract: every pre-M10
    // document and every world-XY sketch takes the fallback path, so world-XY
    // stays a CASE of the general rule rather than a shortcut around it.
    FrameFixture fx; // deliberately NOT placed on the frame
    ASSERT_EQ(fx.sketch->supportFrameId(), kInvalidObjectId);
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 10.0});

    // Moving a frame the sketch does not use changes nothing.
    Transform3D lifted;
    lifted.translation = Vec3{0.0, 0.0, 30.0};
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, lifted));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 10.0});
}

// --- Gate E: selectivity -------------------------------------------------------

TEST(M10ReleaseGate, GATE_E_MovingTheFrameRebuildsTheSolidAndNotTheSketchSolve) {
    FrameFixture fx;
    ASSERT_TRUE(fx.document.setSketchSupportFrame(fx.sketch->id(), fx.frameId));
    ASSERT_TRUE(fx.document.recompute().success);

    // COUNTERS, never equal values: an engine degraded to global recompute
    // would land on the same centre of mass and fail here, which is the whole
    // reason this gate exists rather than a second COM check.
    const int solves = fx.solver.calls;
    const int extrudes = fx.kernel.extrudes;

    Transform3D lifted;
    lifted.translation = Vec3{0.0, 0.0, 30.0};
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, lifted));
    ASSERT_TRUE(fx.document.recomputeFrom(fx.frameId).success);

    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 40.0});
    EXPECT_EQ(fx.kernel.extrudes, extrudes + 1) << "the pad was not rebuilt exactly once";
    // The sketch has no constraints, so the solver never had anything to do
    // here; asserting it did NOT acquire work is what catches a frame move
    // being turned into a whole-document rebuild.
    EXPECT_EQ(fx.solver.calls, solves) << "a frame move re-solved a sketch";
}

// --- Gate F: two levels --------------------------------------------------------

TEST(M10ReleaseGate, GATE_F_AParentMoveReachesAGrandchildsSolid) {
    FrameFixture fx;
    // Re-parent the fixture's frame under a new root, then move the ROOT.
    ReferenceFrame& root = fx.document.addFrame("Root");
    ASSERT_TRUE(fx.document.setFrameParent(fx.frameId, root.id()));
    ASSERT_TRUE(fx.document.setSketchSupportFrame(fx.sketch->id(), fx.frameId));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 10.0});

    // Child offset +5 Z in the ROOT's space; root rotated 90 degrees about X.
    // The root's rotation carries the child's offset from +5 Z to -5 Y, and it
    // carries the whole solid with it: the local centroid (50, 25, 10) becomes
    // (50, -10, 25), then the child's rotated offset adds (0, -5, 0).
    Transform3D childLocal;
    childLocal.translation = Vec3{0.0, 0.0, 5.0};
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, childLocal));

    Transform3D rootLocal;
    rootLocal.rotation = Quaternion{std::cos(kPi / 4.0), std::sin(kPi / 4.0), 0.0, 0.0};
    ASSERT_TRUE(fx.document.setFrameTransform(root.id(), rootLocal));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_NEAR(fx.volume(), 100000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{50.0, -15.0, 25.0});
}

TEST(M10ReleaseGate, GATE_F2_PlacingASketchOnAFrameIsUndoable) {
    FrameFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 10.0});

    Transform3D lifted;
    lifted.translation = Vec3{0.0, 0.0, 30.0};
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, lifted));
    ASSERT_TRUE(fx.document.setSketchSupportFrame(fx.sketch->id(), fx.frameId));
    EXPECT_EQ(fx.document.nextUndoLabel(), "Place Sketch001");
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 40.0});

    // Undo takes the sketch OFF the frame and back onto its embedded plane.
    ASSERT_TRUE(fx.document.undo());
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.sketch->supportFrameId(), kInvalidObjectId);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 10.0});

    ASSERT_TRUE(fx.document.redo());
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 40.0});
}


// --- Gate G: connectors ---------------------------------------------------------

TEST(M10ReleaseGate, GATE_G_AConnectorIsAFramePlusMeaningAndFollowsItsFrame) {
    FrameFixture fx;
    Connector& connector = fx.document.addConnector("ShaftAxis", ConnectorRole::Shaft,
                                                     fx.frameId);
    const ObjectId connectorId = connector.id();

    // First-class: registered and resolvable, whichever route created it
    // (roadmap §18.1 -- an implicit connector differs in when it is made and
    // where it is listed, never in whether it can be re-resolved).
    EXPECT_TRUE(fx.document.objectRegistry().contains(connectorId));
    ASSERT_NE(fx.document.findConnector(connectorId), nullptr);
    EXPECT_EQ(fx.document.findConnector(connectorId)->role(), ConnectorRole::Shaft);
    EXPECT_EQ(fx.document.findConnector(connectorId)->frameId(), fx.frameId);
    EXPECT_EQ(fx.document.findConnector(connectorId)->owner(), ConnectorOwner::PartDefinition);

    // It holds no geometry of its own: it IS its frame, so moving the frame
    // moves the connector with nothing to keep in step (ADR-M10-004).
    ExpectNear3(fx.document.connectorWorldTransform(connectorId).translation, Vec3{0, 0, 0});
    Transform3D lifted;
    lifted.translation = Vec3{0.0, 0.0, 30.0};
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, lifted));
    ExpectNear3(fx.document.connectorWorldTransform(connectorId).translation, Vec3{0, 0, 30});

    // A connector on nothing is a mate anchor nobody can resolve, which is
    // A03's failure mode rather than a recoverable state.
    Parameter& p = fx.document.addParameter("NotAFrame", 1.0, UnitType::Millimeter);
    EXPECT_THROW(fx.document.addConnector("Bad", ConnectorRole::Generic, p.id()),
                 std::runtime_error);

    // Creation and removal are undoable, like every other document edit.
    EXPECT_EQ(fx.document.findConnector(connectorId)->name(), "ShaftAxis");
    ASSERT_TRUE(fx.document.removeObject(connectorId));
    EXPECT_EQ(fx.document.findConnector(connectorId), nullptr);
    ASSERT_TRUE(fx.document.undo());
    ASSERT_NE(fx.document.findConnector(connectorId), nullptr);
    EXPECT_EQ(fx.document.findConnector(connectorId)->id(), connectorId);
}

// --- Gate H: v10 round-trip -----------------------------------------------------

TEST(M10ReleaseGate, GATE_H_FramesConnectorsAndSketchSupportSurviveASaveAndLoad) {
    std::string saved;
    ObjectId frameId = kInvalidObjectId;
    ObjectId rootId = kInvalidObjectId;
    ObjectId connectorId = kInvalidObjectId;
    ObjectId sketchId = kInvalidObjectId;
    {
        FrameFixture fx;
        ReferenceFrame& root = fx.document.addFrame("Root");
        rootId = root.id();
        frameId = fx.frameId;
        sketchId = fx.sketch->id();
        ASSERT_TRUE(fx.document.setFrameParent(frameId, rootId));
        ASSERT_TRUE(fx.document.setSketchSupportFrame(sketchId, frameId));
        connectorId = fx.document.addConnector("Mount", ConnectorRole::Mount, frameId).id();

        Transform3D lifted;
        lifted.translation = Vec3{0.0, 0.0, 30.0};
        ASSERT_TRUE(fx.document.setFrameTransform(rootId, lifted));
        ASSERT_TRUE(fx.document.recompute().success);
        ExpectNear3(fx.com(), Vec3{50.0, 25.0, 40.0});

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

    // The hierarchy came back, and exactly ONE Origin came back -- the file's.
    // Restoring naively gave the loaded document two frames named "Origin", the
    // second with an id nothing in the file referenced.
    std::size_t origins = 0;
    for (const ReferenceFrame* frame : loaded.document->frames())
        if (frame->name() == "Origin") ++origins;
    EXPECT_EQ(origins, 1u);
    ASSERT_NE(loaded.document->findFrame(frameId), nullptr);
    EXPECT_EQ(loaded.document->findFrame(frameId)->parentFrameId(), rootId);

    // The connector came back with its role, its frame and its owner.
    ASSERT_NE(loaded.document->findConnector(connectorId), nullptr);
    EXPECT_EQ(loaded.document->findConnector(connectorId)->role(), ConnectorRole::Mount);
    EXPECT_EQ(loaded.document->findConnector(connectorId)->frameId(), frameId);

    // And the sketch is still ON its frame -- proven by the geometry landing
    // where the frame puts it, not merely by the id being present.
    const Sketch* sketch = loaded.document->findSketch(sketchId);
    ASSERT_NE(sketch, nullptr);
    EXPECT_EQ(sketch->supportFrameId(), frameId);
    ASSERT_TRUE(loaded.document->recompute().success);
    EXPECT_NEAR(loaded.document->massProperties().volumeMm3, 100000.0, 1e-6);
    ExpectNear3(loaded.document->massProperties().centerOfMassMm, Vec3{50.0, 25.0, 40.0});

    // A loaded document carries no history (ADR-M9-001), frames included.
    EXPECT_EQ(loaded.document->undoDepth(), 0u);
}

TEST(M10ReleaseGate, GATE_H2_APreV10FileStillLoadsAndKeepsItsOrigin) {
    // The compatibility half: a file with no `frames` array is every document
    // written before M10, and it must behave exactly as it did -- one Origin,
    // made by the constructor, and sketches on their own embedded planes.
    FrameFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.document, out));

    std::string text = out.str();
    const std::size_t framesAt = text.find("\"frames\"");
    ASSERT_NE(framesAt, std::string::npos);
    const std::size_t open = text.find('[', framesAt);
    const std::size_t close = text.find(']', open);
    ASSERT_NE(close, std::string::npos);
    text.erase(framesAt, close - framesAt + 2); // drop "frames": [...],

    std::istringstream in(text);
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->frames().size(), 1u);
    EXPECT_EQ(loaded.document->frames().front()->name(), "Origin");
    EXPECT_EQ(loaded.document->findSketch(fx.sketch->id())->supportFrameId(), kInvalidObjectId);
}

// --- Gate I: failure semantics --------------------------------------------------

TEST(M10ReleaseGate, GATE_I_ASketchWhoseSupportFrameIsGoneFailsRatherThanFallingBack) {
    FrameFixture fx;
    ASSERT_TRUE(fx.document.setSketchSupportFrame(fx.sketch->id(), fx.frameId));
    Transform3D lifted;
    lifted.translation = Vec3{0.0, 0.0, 30.0};
    ASSERT_TRUE(fx.document.setFrameTransform(fx.frameId, lifted));
    ASSERT_TRUE(fx.document.recompute().success);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, 40.0});

    // Delete the frame. M4's accepted precedent for deleting a sketch a Pad
    // reads applies unchanged: the dependent fails LOUDLY and save refuses the
    // dangling reference. What must NOT happen is a silent fallback to world
    // XY -- a sketch that quietly relocates to the origin is the geometric twin
    // of the stale-result defect this project has fixed three times.
    ASSERT_TRUE(fx.document.removeObject(fx.frameId));
    fx.document.recompute();

    EXPECT_NE(fx.com().z, 10.0) << "the sketch silently fell back to world XY";
    EXPECT_FALSE(fx.document.massProperties().valid)
        << "mass is current although the sketch has no plane to live on";

    std::ostringstream out;
    const SaveResult saveResult = savePartDocument(fx.document, out);
    EXPECT_FALSE(saveResult) << "a document with a dangling support frame was written";
}


// --- M10.6: Mirror and Pattern, ADR-M9-006's deferral closed --------------------

TEST(M10ReleaseGate, GATE_M_MirrorAboutAFramesPlaneDoublesTheMaterial) {
    // The pad spans z 0..20. A mirror frame at z = -10 reflects it to
    // z -40..-20 -- DISJOINT, so the fused volume is exactly twice the pad and
    // the centroid is the midpoint of the two lumps:
    //   volume = 2 x 100000
    //   COM z  = (10 x 100000 + (-30) x 100000) / 200000 = -10
    // Disjoint on purpose: touching solids would make the volume depend on how
    // OCCT merges a shared face, and the oracle would be measuring the kernel
    // instead of the feature.
    FrameFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    ReferenceFrame& plane = fx.document.addFrame("MirrorPlane");
    Transform3D at;
    at.translation = Vec3{0.0, 0.0, -10.0};
    ASSERT_TRUE(fx.document.setFrameTransform(plane.id(), at));

    Body& body = *fx.document.bodies().front();
    MirrorFeature& mirror =
        fx.document.addMirrorFeature(body, "Mirror001", fx.pad->id(), plane.id());
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(mirror.state(), ComputeState::Valid);

    ASSERT_TRUE(fx.document.massProperties().valid);
    EXPECT_NEAR(fx.volume(), 200000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, -10.0});

    // PARAMETRIC, which is the whole reason this waited for M10: move the
    // mirror PLANE and the mirrored half moves, with no edit to the feature.
    // Plane at z = -30 reflects the pad to z -80..-60; COM z becomes
    // (10 - 70) / 2 = -30.
    at.translation = Vec3{0.0, 0.0, -30.0};
    ASSERT_TRUE(fx.document.setFrameTransform(plane.id(), at));
    ASSERT_TRUE(fx.document.recomputeFrom(plane.id()).success);
    EXPECT_NEAR(fx.volume(), 200000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, -30.0});
}

TEST(M10ReleaseGate, GATE_N_APatternRepeatsAlongItsFramesAxisAndIsDrivenByParameters) {
    // Three instances at 200 mm along the frame's +X: lumps at x 0..100,
    // 200..300, 400..500. Disjoint, so volume = 3 x 100000 and the centroid is
    // the mean of the three lump centres: (50 + 250 + 450) / 3 = 250.
    FrameFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    Parameter& count = fx.document.addParameter("Count", 3.0, UnitType::Unitless);
    Parameter& spacing = fx.document.addParameter("Spacing", 200.0, UnitType::Millimeter);
    ReferenceFrame& axis = fx.document.addFrame("PatternAxis");
    Body& body = *fx.document.bodies().front();
    PatternFeature& pattern = fx.document.addPatternFeature(body, "Pattern001", fx.pad->id(),
                                                             axis.id(), count.id(), spacing.id());
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(pattern.state(), ComputeState::Valid);
    EXPECT_NEAR(fx.volume(), 300000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{250.0, 25.0, 10.0});

    // Driven by a PARAMETER, not a literal: four instances, and the centroid
    // moves to (50 + 250 + 450 + 650) / 4 = 350.
    ASSERT_TRUE(fx.document.setParameterValue(count.id(), 4.0));
    ASSERT_TRUE(fx.document.recomputeFrom(count.id()).success);
    EXPECT_NEAR(fx.volume(), 400000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{350.0, 25.0, 10.0});

    // A count of ONE is the base alone -- legal, and a state a user passes
    // through while typing rather than an error.
    ASSERT_TRUE(fx.document.setParameterValue(count.id(), 1.0));
    ASSERT_TRUE(fx.document.recomputeFrom(count.id()).success);
    EXPECT_NEAR(fx.volume(), 100000.0, 1e-6);

    // A fractional count is refused rather than truncated: a value that cannot
    // mean what it says is an error (ADR-M3-009's rule for dimensions).
    ASSERT_TRUE(fx.document.setParameterValue(count.id(), 2.5));
    fx.document.recompute();
    EXPECT_EQ(pattern.state(), ComputeState::Failed);
    EXPECT_FALSE(fx.document.massProperties().valid);
}

TEST(M10ReleaseGate, GATE_O_MirrorAndPatternInheritTheWholeChain) {
    // They are consuming features, so everything M8 and M9 built applies with
    // no new concept: the base is consumed once, the tail is the transform, a
    // suppressed transform gives the base back, and the record round-trips.
    FrameFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ReferenceFrame& plane = fx.document.addFrame("MirrorPlane");
    Transform3D at;
    at.translation = Vec3{0.0, 0.0, -10.0};
    ASSERT_TRUE(fx.document.setFrameTransform(plane.id(), at));
    Body& body = *fx.document.bodies().front();
    MirrorFeature& mirror =
        fx.document.addMirrorFeature(body, "Mirror001", fx.pad->id(), plane.id());
    ASSERT_TRUE(fx.document.recompute().success);

    // Consumed once: a second consumer of the same pad is refused (ADR-M8-008).
    EXPECT_THROW(fx.document.addMirrorFeature(body, "Mirror002", fx.pad->id(), plane.id()),
                 std::runtime_error);

    // Suppression closes the chain over it (ADR-M9-002): the pad is the tail.
    ASSERT_TRUE(fx.document.setSuppressed(mirror.id(), true));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_NEAR(fx.volume(), 100000.0, 1e-6);
    ASSERT_TRUE(fx.document.setSuppressed(mirror.id(), false));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_NEAR(fx.volume(), 200000.0, 1e-6);

    // And it round-trips, with the frame reference intact -- proven by the
    // geometry, not by the id being present.
    std::ostringstream out;
    const SaveResult saveResult = savePartDocument(fx.document, out);
    ASSERT_TRUE(saveResult) << saveResult.message;
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    std::istringstream in(out.str());
    LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);
    ASSERT_TRUE(loaded.document->recompute().success);
    EXPECT_NEAR(loaded.document->massProperties().volumeMm3, 200000.0, 1e-6);
    ExpectNear3(loaded.document->massProperties().centerOfMassMm, Vec3{50.0, 25.0, -10.0});
}


TEST(M10ReleaseGate, GATE_P_ATransformUsesItsFramesWORLDPlaneAndRotatedAXIS) {
    // WRITTEN BECAUSE TWO MUTATIONS SURVIVED (M10 battery R1 and R2). Every
    // other Mirror and Pattern gate used a ROOT frame with NO rotation, so
    // `local` and `world` were the same transform and rotating the plane normal
    // by identity changed nothing. Both mutations -- "use the local transform"
    // and "ignore the frame's rotation when building the normal" -- passed
    // every one of them.
    //
    // That is M8 GATE_RB2's lesson again (an oracle is only as good as the
    // coincidences it avoids) and M10's own Q4 again (only a two-level fixture
    // can tell local from world). One fixture kills both: a frame that is a
    // CHILD of a translating parent AND carries a rotation of its own.
    //
    //   parent: translate +150 X, no rotation
    //   child : rotate 90 degrees about Y, no translation
    //   world(child) = translation (150, 0, 0), rotation 90 about Y
    //
    // The child's local +Z is (0,0,1); rotated 90 about Y it becomes (1,0,0),
    // so the mirror plane is x = 150. The pad spans x 0..100, so its reflection
    // spans x 200..300 -- disjoint, and the fused centroid is the midpoint:
    //   volume = 200000, COM = (150, 25, 10)
    //
    // Under R1 the plane origin would be (0,0,0) and the COM x would be 0.
    // Under R2 the normal would stay (0,0,1) and the COM x would be 50.
    FrameFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    ReferenceFrame& parent = fx.document.addFrame("Carrier");
    ReferenceFrame& plane = fx.document.addFrame("MirrorPlane", parent.id());
    Transform3D carried;
    carried.translation = Vec3{150.0, 0.0, 0.0};
    ASSERT_TRUE(fx.document.setFrameTransform(parent.id(), carried));
    Transform3D turned;
    turned.rotation = Quaternion{std::cos(kPi / 4.0), 0.0, std::sin(kPi / 4.0), 0.0}; // +90 about Y
    ASSERT_TRUE(fx.document.setFrameTransform(plane.id(), turned));

    Body& body = *fx.document.bodies().front();
    fx.document.addMirrorFeature(body, "Mirror001", fx.pad->id(), plane.id());
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.massProperties().valid);
    EXPECT_NEAR(fx.volume(), 200000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{150.0, 25.0, 10.0});
}

TEST(M10ReleaseGate, GATE_P2_APatternMarchesAlongItsFramesROTATEDAxis) {
    // The pattern half of the same hole: with an unrotated frame the axis is
    // +X whether or not the code rotates it.
    //
    // The frame is rotated 90 degrees about Y, which sends its local +X (1,0,0)
    // to (0,0,-1) -- so two instances at 200 mm march DOWNWARD:
    //   lumps at z 0..20 and z -200..-180
    //   volume = 200000, COM z = (10 + (-190)) / 2 = -90
    FrameFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    Parameter& count = fx.document.addParameter("Count", 2.0, UnitType::Unitless);
    Parameter& spacing = fx.document.addParameter("Spacing", 200.0, UnitType::Millimeter);
    ReferenceFrame& axis = fx.document.addFrame("PatternAxis");
    Transform3D turned;
    turned.rotation = Quaternion{std::cos(kPi / 4.0), 0.0, std::sin(kPi / 4.0), 0.0};
    ASSERT_TRUE(fx.document.setFrameTransform(axis.id(), turned));

    Body& body = *fx.document.bodies().front();
    fx.document.addPatternFeature(body, "Pattern001", fx.pad->id(), axis.id(), count.id(),
                                  spacing.id());
    ASSERT_TRUE(fx.document.recompute().success);

    ASSERT_TRUE(fx.document.massProperties().valid);
    EXPECT_NEAR(fx.volume(), 200000.0, 1e-6);
    ExpectNear3(fx.com(), Vec3{50.0, 25.0, -90.0});
}

} // namespace
