// M5-M: what the UI decides about constraints, checked against a REAL solve.
//
// These live in the integration target rather than beside the other
// DocumentOutline tests because every assertion here is about solver output --
// status, DOF, which constraint was blamed. Faking those values would make the
// tests agree with a stub instead of with the solver the user actually runs.
//
// DocumentOutline is free of Qt and of OCCT, so all of this is testable without
// a display. What genuinely cannot be checked here -- pixels, focus order,
// contrast, DPI scaling -- is validated by running the program and by the owner
// (ADR-M4-015/016), never asserted from here.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include "Viewer/DocumentOutline.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <functional>
#include "Core/Serialization/PartDocumentSerializer.h"
#include <cmath>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

struct UiDoc {
    PartDocument document{"UiDoc"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* padLength = nullptr;
    Sketch* sketch = nullptr;
    PadFeature* pad = nullptr;
    SketchEntityId bottom{}, right{}, top{}, left{};
    SketchConstraintId widthLength{kInvalidSketchConstraintId};

    // `dimensioned = false` leaves the rectangle under-constrained.
    explicit UiDoc(bool dimensioned = true) {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);
        width = &document.addParameter("Width", 100.0, UnitType::Millimeter);
        height = &document.addParameter("Height", 50.0, UnitType::Millimeter);
        padLength = &document.addParameter("PadLength", 20.0, UnitType::Millimeter);

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
        if (dimensioned) {
            widthLength = add(LengthConstraint{bottom, width->id()});
            add(LengthConstraint{right, height->id()});
        }

        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", s.id(), padLength->id());
    }

    const Sketch& solved() const { return *document.findSketch(sketch->id()); }
};

std::string RowValue(const std::vector<PropertyRow>& rows, const std::string& label) {
    for (const PropertyRow& row : rows)
        if (row.label == label) return row.value;
    return {};
}

// Both helpers return BY VALUE on purpose. The first drafts returned a
// PropertyRow* into the argument vector and OutlineNode*s into the tree, and
// every call site passed a temporary -- `FindRow(outline.propertiesOf(id), ...)`
// -- so the pointers dangled the moment the full expression ended. Three tests
// then read MSVC's 0xDD fill and one crashed outright. Returning copies makes
// the mistake unrepresentable instead of leaving it to each call site to
// remember to bind a local.
std::optional<PropertyRow> FindRow(const std::vector<PropertyRow>& rows,
                                   const std::string& label) {
    for (const PropertyRow& row : rows)
        if (row.label == label) return row;
    return std::nullopt;
}

std::vector<OutlineNode> NodesOfKind(const DocumentOutline& outline, OutlineKind kind) {
    std::vector<OutlineNode> found;
    const std::function<void(const OutlineNode&)> visit = [&](const OutlineNode& node) {
        if (node.kind == kind) found.push_back(node);
        for (const OutlineNode& child : node.children) visit(child);
    };
    visit(outline.build());
    return found;
}

// --- Solve status and DOF (spec 18) ------------------------------------------

TEST(M5Ui, M5_UI_001_SolvedSketchShowsStatusAndZeroDof) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.sketch->id());
    EXPECT_EQ(RowValue(rows, "Solve status"), "Solved");
    EXPECT_EQ(RowValue(rows, "Degrees of freedom"), "0");
    EXPECT_EQ(RowValue(rows, "Count"), "11");
}

TEST(M5Ui, M5_UI_002_UnderConstrainedSketchReportsItsFreeDegrees) {
    UiDoc doc(/*dimensioned=*/false);
    ASSERT_TRUE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.sketch->id());
    EXPECT_EQ(RowValue(rows, "Solve status"), "Under-constrained");
    // The exact count is the solver's; what the UI must not do is report 0,
    // which reads as "finished" for a sketch that is not.
    EXPECT_NE(RowValue(rows, "Degrees of freedom"), "0");
    EXPECT_NE(RowValue(rows, "Degrees of freedom"), "");
}

TEST(M5Ui, M5_UI_003_StatusIsTextNotOnlyAColour) {
    // Every status must have a distinct, non-empty English label, because a
    // state carried only by a row colour is unreadable in greyscale, to a
    // colour-blind user, and in a screenshot (spec 18).
    std::vector<std::string> labels;
    for (SketchSolveStatus status :
         {SketchSolveStatus::Solved, SketchSolveStatus::UnderConstrained,
          SketchSolveStatus::OverConstrained, SketchSolveStatus::Conflicting,
          SketchSolveStatus::InvalidInput, SketchSolveStatus::NumericalFailure}) {
        const std::string label = DocumentOutline::solveStatusLabel(status);
        EXPECT_FALSE(label.empty());
        EXPECT_NE(label, "Unknown");
        labels.push_back(label);
    }
    std::sort(labels.begin(), labels.end());
    EXPECT_EQ(std::unique(labels.begin(), labels.end()), labels.end())
        << "two solve statuses share a label, so the UI cannot tell them apart";
}

// --- The constraint list (spec 18) -------------------------------------------

TEST(M5Ui, M5_UI_004_EveryConstraintAppearsInTheTreeUnderItsSketch) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<OutlineNode> sketches = NodesOfKind(outline, OutlineKind::Sketch);
    ASSERT_EQ(sketches.size(), 1u);

    // Under ITS SKETCH, not at document level: a constraint is a sub-object of
    // one sketch, and a flat list would lose that the moment a second sketch
    // exists.
    EXPECT_EQ(sketches.front().children.size(), doc.solved().constraints().size());
    for (const OutlineNode& child : sketches.front().children)
        EXPECT_EQ(child.kind, OutlineKind::Constraint);

    // Every constraint id is reachable, so none is silently missing.
    for (const SketchConstraint& constraint : doc.solved().constraints()) {
        const bool present =
            std::any_of(sketches.front().children.begin(), sketches.front().children.end(),
                        [&](const OutlineNode& n) { return n.id == ToObjectId(constraint.id); });
        EXPECT_TRUE(present) << "constraint " << ToObjectId(constraint.id) << " is not listed";
    }
}

TEST(M5Ui, M5_UI_005_DimensionalConstraintRowsNameTheirParameterAndValue) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<OutlineNode> constraints =
        NodesOfKind(outline, OutlineKind::Constraint);

    const auto rowFor = [&](SketchConstraintId id) -> std::optional<OutlineNode> {
        for (const OutlineNode& node : constraints)
            if (node.id == ToObjectId(id)) return node;
        return std::nullopt;
    };
    const std::optional<OutlineNode> lengthRow = rowFor(doc.widthLength);
    ASSERT_TRUE(lengthRow.has_value());

    // "Length" alone is useless in a list of two Lengths -- the parameter name
    // is what a user recognises.
    EXPECT_NE(lengthRow->name.find("Width"), std::string::npos) << lengthRow->name;
    EXPECT_NE(lengthRow->name.find("100.000"), std::string::npos) << lengthRow->name;
    EXPECT_NE(lengthRow->name.find("mm"), std::string::npos) << lengthRow->name;
}

TEST(M5Ui, M5_UI_006_SelectingAConstraintDescribesIt) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(ToObjectId(doc.widthLength));
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(RowValue(rows, "Type"), "Length");
    EXPECT_EQ(RowValue(rows, "Sketch"), "Sketch001");
    EXPECT_EQ(RowValue(rows, "Parameter"), "Width");
    EXPECT_FALSE(RowValue(rows, "Entities").empty());
}

TEST(M5Ui, M5_UI_007_DimensionValueIsEditableAndCarriesItsUnit) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::optional<PropertyRow> value =
        FindRow(outline.propertiesOf(ToObjectId(doc.widthLength)), "Value");
    ASSERT_TRUE(value.has_value());
    EXPECT_TRUE(value->editable);
    // The row points at the PARAMETER, so the panel writes through
    // PartDocument's facade and never into the constraint.
    EXPECT_EQ(value->parameterId, doc.width->id());
    EXPECT_EQ(value->unitLabel, "mm"); // a missing unit is a Critical defect
    EXPECT_DOUBLE_EQ(value->numericValue, 100.0);
}

// --- Editing a dimension updates the 3D result (spec 18) ----------------------

TEST(M5Ui, M5_UI_008_EditingTheDimensionRowUpdatesTheSolidAndThePanel) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    const double volumeBefore = doc.document.massProperties().volumeMm3;
    EXPECT_NEAR(volumeBefore, 100.0 * 50.0 * 20.0, 1e-3);

    // Exactly what the property panel does: write through the id on the row.
    const DocumentOutline outline(doc.document);
    const std::optional<PropertyRow> value =
        FindRow(outline.propertiesOf(ToObjectId(doc.widthLength)), "Value");
    ASSERT_TRUE(value.has_value());
    ASSERT_TRUE(doc.document.setParameterValue(value->parameterId, 130.0));
    ASSERT_TRUE(doc.document.recompute().success);

    // The 3D result moved, which is the requirement -- not merely that the
    // number in the panel changed.
    EXPECT_NEAR(doc.document.massProperties().volumeMm3, 130.0 * 50.0 * 20.0, 1e-3);
    const std::optional<PropertyRow> after =
        FindRow(outline.propertiesOf(ToObjectId(doc.widthLength)), "Value");
    ASSERT_TRUE(after.has_value());
    EXPECT_DOUBLE_EQ(after->numericValue, 130.0);
    EXPECT_EQ(RowValue(outline.propertiesOf(doc.sketch->id()), "Solve status"), "Solved");
}

// --- Failure reporting (spec 18) ---------------------------------------------

TEST(M5Ui, M5_UI_009_ConflictNamesTheOffendingConstraintIds) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);

    Parameter& other = doc.document.addParameter("WidthAlt", 70.0, UnitType::Millimeter);
    const SketchConstraintId conflicting = doc.document.addSketchConstraint(
        doc.sketch->id(), LengthConstraint{doc.bottom, other.id()});
    ASSERT_NE(conflicting, kInvalidSketchConstraintId);
    EXPECT_FALSE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.sketch->id());
    EXPECT_NE(RowValue(rows, "Solve status"), "Solved");
    EXPECT_FALSE(RowValue(rows, "Solver diagnostic").empty());

    // The ids, listed. A status with no id leaves the user nothing to act on.
    const std::string ids = RowValue(rows, "Offending constraint IDs");
    ASSERT_FALSE(ids.empty());
    bool namesSomeOffender = false;
    for (SketchConstraintId offender : doc.solved().offendingConstraints())
        if (ids.find(std::to_string(ToObjectId(offender))) != std::string::npos)
            namesSomeOffender = true;
    EXPECT_TRUE(namesSomeOffender) << ids;
}

TEST(M5Ui, M5_UI_010_BlamedConstraintRowsAreMarkedIndividually) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    Parameter& other = doc.document.addParameter("WidthAlt", 70.0, UnitType::Millimeter);
    ASSERT_NE(doc.document.addSketchConstraint(doc.sketch->id(),
                                               LengthConstraint{doc.bottom, other.id()}),
              kInvalidSketchConstraintId);
    EXPECT_FALSE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<OutlineNode> constraints =
        NodesOfKind(outline, OutlineKind::Constraint);
    const std::vector<SketchConstraintId>& offenders = doc.solved().offendingConstraints();
    ASSERT_FALSE(offenders.empty());

    for (const OutlineNode& node : constraints) {
        const bool blamed =
            std::any_of(offenders.begin(), offenders.end(),
                        [&](SketchConstraintId id) { return ToObjectId(id) == node.id; });
        EXPECT_EQ(node.state == OutlineState::Failed, blamed)
            << "constraint " << node.id << " is marked inconsistently with the solver's blame";
        // Marked by TEXT as well as state, never by colour alone.
        if (blamed) {
            EXPECT_STRNE(DocumentOutline::stateMarker(node.state), "");
            EXPECT_FALSE(node.diagnostic.empty());
        }
    }
}

TEST(M5Ui, M5_UI_011_SolveFailureIsReportedAheadOfAProfileComplaint) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    Parameter& other = doc.document.addParameter("WidthAlt", 70.0, UnitType::Millimeter);
    ASSERT_NE(doc.document.addSketchConstraint(doc.sketch->id(),
                                               LengthConstraint{doc.bottom, other.id()}),
              kInvalidSketchConstraintId);
    EXPECT_FALSE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<OutlineNode> sketches =
        NodesOfKind(outline, OutlineKind::Sketch);
    ASSERT_EQ(sketches.size(), 1u);
    EXPECT_EQ(sketches.front().state, OutlineState::Failed);
    // The reason must be the SOLVE, not the profile: an unsolved sketch's
    // profile is meaningless, and "not a closed loop" sends the user to look
    // at geometry when the real cause is a dimension they can fix.
    EXPECT_EQ(sketches.front().diagnostic, doc.solved().solveMessage());
    EXPECT_FALSE(sketches.front().diagnostic.empty());
}

TEST(M5Ui, M5_UI_012_ConstraintFreeSketchIsUnchangedFromM4) {
    // Every M4 document is a sketch with no constraints. It must not suddenly
    // grow a constraint group, a failure, or a misleading status.
    PartDocument document{"M4Doc"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Aluminium", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    ASSERT_TRUE(document.recompute().success);
    const DocumentOutline outline(document);
    const std::vector<OutlineNode> sketches =
        NodesOfKind(outline, OutlineKind::Sketch);
    ASSERT_EQ(sketches.size(), 1u);
    EXPECT_TRUE(sketches.front().children.empty());
    EXPECT_NE(sketches.front().state, OutlineState::Failed);
    EXPECT_EQ(RowValue(outline.propertiesOf(sketch.id()), "Count"), "0");
    EXPECT_NEAR(document.massProperties().volumeMm3, 100.0 * 50.0 * 20.0, 1e-3);
}

// --- Regressions for the findings of the M5 independent review ---------------

TEST(M5Ui, M5_REV_001_ConstraintFreeSketchDoesNotReportZeroDegreesOfFreedom) {
    // "Under-constrained, DOF 0" is self-contradictory: 0 is what a FULLY
    // constrained sketch reports (spec 10, Gate A). Every M4 document is a
    // constraint-free sketch, so this was on screen for all of them.
    PartDocument document{"M4Doc"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    ASSERT_TRUE(document.recompute().success);

    const Sketch& solved = *document.findSketch(sketch.id());
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::UnderConstrained);
    EXPECT_GT(solved.degreesOfFreedom(), 0) << "free geometry reported no free degrees";
    EXPECT_NE(RowValue(DocumentOutline(document).propertiesOf(sketch.id()),
                       "Degrees of freedom"),
              "0");
}

TEST(M5Ui, M5_REV_002_FirstSolveFailureReportsAnUnmeasuredDofNotZero) {
    UiDoc doc;
    doc.document.setSketchSolver(nullptr); // fails before any DOF is measured
    EXPECT_FALSE(doc.document.recompute().success);

    EXPECT_EQ(doc.solved().degreesOfFreedom(), kUnknownDegreesOfFreedom);
    EXPECT_EQ(RowValue(DocumentOutline(doc.document).propertiesOf(doc.sketch->id()),
                       "Degrees of freedom"),
              "not measured");
}

TEST(M5Ui, M5_REV_003_RedundancyDoesNotHideFreeDegrees) {
    // A duplicate Horizontal on a sketch that is still short of constrained
    // used to report "Over-constrained (redundant)" -- telling the user there
    // are too many constraints on a sketch that needs more, and hiding the
    // free degrees behind a status that implies none remain.
    UiDoc doc(/*dimensioned=*/false);
    ASSERT_NE(doc.document.addSketchConstraint(doc.sketch->id(),
                                               HorizontalConstraint{doc.bottom}),
              kInvalidSketchConstraintId);
    ASSERT_TRUE(doc.document.recompute().success);

    EXPECT_EQ(doc.solved().solveStatus(), SketchSolveStatus::UnderConstrained);
    EXPECT_GT(doc.solved().degreesOfFreedom(), 0);
}

TEST(M5Ui, M5_REV_004_InjectingASolverClearsTheFailureItCaused) {
    // ADR-M5-004 promises that correcting the input and recomputing recovers.
    // A bare assignment dirtied nothing, and the graph never re-invokes a
    // Failed node -- so the document stayed blocked forever.
    UiDoc doc;
    doc.document.setSketchSolver(nullptr);
    EXPECT_FALSE(doc.document.recompute().success);
    EXPECT_NE(doc.pad->state(), ComputeState::Valid);

    GaussNewtonSketchSolver solver;
    doc.document.setSketchSolver(&solver);
    ASSERT_TRUE(doc.document.recompute().success) << "injecting a solver did not recover";
    EXPECT_EQ(doc.solved().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(doc.pad->state(), ComputeState::Valid);
    EXPECT_NEAR(doc.document.massProperties().volumeMm3, 100.0 * 50.0 * 20.0, 1e-3);
}

TEST(M5Ui, M5_REV_005_ConstraintsAddedThroughEditSketchStillWireTheirEdge) {
    // Sketch::addConstraint is reachable through editSketch, which the shipped
    // viewer uses. A constraint added that way wired no edge, so the document
    // behaved DIFFERENTLY before and after a save/load -- the loader re-derives
    // edges from the constraints, so one appeared out of nowhere.
    UiDoc doc(/*dimensioned=*/false);
    ASSERT_TRUE(doc.document.editSketch(doc.sketch->id(), [&](Sketch& s) {
        s.addConstraint(LengthConstraint{doc.bottom, doc.width->id()});
    }));

    const std::vector<ObjectId> dependents =
        doc.document.dependencyGraph().dependentsOf(doc.width->id());
    EXPECT_NE(std::find(dependents.begin(), dependents.end(), doc.sketch->id()),
              dependents.end())
        << "a constraint added through editSketch left the graph unaware of it";
}

TEST(M5Ui, M5_REV_006_CascadingAnEntityThroughEditSketchLeavesNoPhantomEdge) {
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    ASSERT_TRUE(doc.document.editSketch(doc.sketch->id(),
                                        [&](Sketch& s) { s.removeEntity(doc.bottom); }));

    // Length(bottom, Width) cascaded away with the entity, so Width must no
    // longer drive this sketch. It used to, wasting a solve on every edit and
    // leaving the graph disagreeing with the constraint set.
    const std::vector<ObjectId> dependents =
        doc.document.dependencyGraph().dependentsOf(doc.width->id());
    EXPECT_EQ(std::find(dependents.begin(), dependents.end(), doc.sketch->id()),
              dependents.end())
        << "a phantom Parameter edge survived the cascade";
}

TEST(M5Ui, M5_REV_007_ADimensionMustBindARealParameter) {
    UiDoc doc;
    Body& body = doc.document.addBody("Body002");
    // A Body id is not a Parameter. Accepting it produced a document that
    // looked fine and could never be saved again.
    EXPECT_EQ(doc.document.addSketchConstraint(doc.sketch->id(),
                                               LengthConstraint{doc.top, body.id()}),
              kInvalidSketchConstraintId);
    EXPECT_EQ(doc.document.addSketchConstraint(doc.sketch->id(),
                                               LengthConstraint{doc.top, 999999}),
              kInvalidSketchConstraintId);
}

TEST(M5Ui, M5_REV_008_DeletingASketchKeepsTheAcceptedM4Contract) {
    // Independent review proposed refusing this deletion. M4's own review
    // already settled the case the other way (MAJOR2/MAJOR3): deletion is
    // allowed, the Pad fails LOUDLY, and save refuses to write a dangling
    // reference so a broken document cannot overwrite a good file. This test
    // pins that contract so a future change cannot drift away from it silently
    // -- and shows the recovery path exists.
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    ASSERT_FALSE(doc.document.featuresReferencingSketch(doc.sketch->id()).empty());

    ASSERT_TRUE(doc.document.removeObject(doc.sketch->id()));
    EXPECT_FALSE(doc.document.recompute().success) << "the Pad did not fail loudly";
    EXPECT_NE(doc.pad->state(), ComputeState::Valid);

    std::ostringstream blocked;
    EXPECT_FALSE(savePartDocument(doc.document, blocked))
        << "a document with a dangling Pad reference was savable";

    // Recovery: remove the Pad, and the document is writable again.
    ASSERT_TRUE(doc.document.removeObject(doc.pad->id()));
    std::ostringstream recovered;
    EXPECT_TRUE(savePartDocument(doc.document, recovered));
}

TEST(M5Ui, M5_REV_009_CoincidentPointsAtZeroSeparationAreNotCalledContradictory) {
    // sqrt(du^2+dv^2)'s central difference at du=dv=0 is exactly zero, so the
    // whole Jacobian row vanished and the rank test reported "no configuration
    // satisfies them" for a system with an INFINITE solution set.
    PartDocument document{"Degenerate"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    Parameter& distance = document.addParameter("Gap", 10.0, UnitType::Millimeter);

    Sketch& s = document.addSketch("Sketch001");
    const SketchEntityId p1 = s.addPoint(Vec2{0, 0});
    const SketchEntityId p2 = s.addPoint(Vec2{0, 0}); // exactly coincident
    document.addSketchConstraint(s.id(),
                                 FixConstraint{SketchElementRef{p1, SketchSubElement::Whole}});
    document.addSketchConstraint(
        s.id(), DistanceConstraint{SketchElementRef{p1, SketchSubElement::Whole},
                                   SketchElementRef{p2, SketchSubElement::Whole},
                                   distance.id()});

    ASSERT_TRUE(document.recompute().success) << "a solvable system was rejected";
    const Sketch& solved = *document.findSketch(s.id());
    EXPECT_NE(solved.solveStatus(), SketchSolveStatus::Conflicting);
    const SketchPoint& a = std::get<SketchPoint>(solved.findEntity(p1)->geometry);
    const SketchPoint& b = std::get<SketchPoint>(solved.findEntity(p2)->geometry);
    EXPECT_NEAR(std::hypot(b.position.x - a.position.x, b.position.y - a.position.y), 10.0,
                1e-6);
}

// --- Regressions for the M5 RE-review ----------------------------------------

TEST(M5Ui, M5_REV2_010_AnUnboundDimensionIsRejected) {
    // Rejecting "bound to something that is not a Parameter" left "bound to
    // NOTHING" open, and that half was worse: the solve problem got target 0.0
    // with no complaint, the document SAVED CLEANLY, and the loader then
    // refused it forever -- a file that saves and can never be loaded back.
    UiDoc doc;
    EXPECT_EQ(doc.document.addSketchConstraint(doc.sketch->id(),
                                               LengthConstraint{doc.top, kInvalidObjectId}),
              kInvalidSketchConstraintId);
    EXPECT_EQ(doc.document.addSketchConstraint(doc.sketch->id(),
                                               RadiusConstraint{doc.top, kInvalidObjectId}),
              kInvalidSketchConstraintId);
    // A non-dimensional constraint binds no Parameter and must stay accepted.
    EXPECT_NE(doc.document.addSketchConstraint(doc.sketch->id(),
                                               HorizontalConstraint{doc.top}),
              kInvalidSketchConstraintId);
}

TEST(M5Ui, M5_REV2_011_AnUnboundDimensionCannotBeSavedIntoAnUnloadableFile) {
    // The facade now refuses it, so this reaches the sketch the only other way
    // -- through editSketch -- and checks the save-side guard mirrors the
    // loader, which REQUIRES parameterId for all five dimensional kinds.
    UiDoc doc;
    ASSERT_TRUE(doc.document.editSketch(doc.sketch->id(), [&](Sketch& s) {
        s.addConstraint(LengthConstraint{doc.top, kInvalidObjectId});
    }));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(doc.document, out);
    EXPECT_FALSE(saved) << "a document that can never be loaded back was savable";

    // And the solver names it rather than silently driving the line to zero.
    EXPECT_FALSE(doc.document.recompute().success);
    EXPECT_EQ(doc.solved().solveStatus(), SketchSolveStatus::InvalidInput);
    EXPECT_FALSE(doc.solved().offendingConstraints().empty());
}

TEST(M5Ui, M5_REV2_012_ConstraintsAddedThroughTheRawSketchReferenceStillPropagate) {
    // addSketch hands back a mutable Sketch&, so Sketch::addConstraint is
    // reachable without any facade -- a fifth mutation path ADR-M5-013 did not
    // list. Through it a dimension edit silently did nothing, and the document
    // behaved differently before and after a save/load.
    PartDocument document{"RawRef"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    document.addMaterial("Alu", 2700.0);
    Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = document.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& padLength = document.addParameter("PadLength", 20.0, UnitType::Millimeter);

    Sketch& s = document.addSketch("Sketch001"); // the raw reference
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
    s.addConstraint(CoincidentConstraint{ep(bottom), sp(right)});
    s.addConstraint(CoincidentConstraint{ep(right), sp(top)});
    s.addConstraint(CoincidentConstraint{ep(top), sp(left)});
    s.addConstraint(CoincidentConstraint{ep(left), sp(bottom)});
    s.addConstraint(HorizontalConstraint{bottom});
    s.addConstraint(HorizontalConstraint{top});
    s.addConstraint(VerticalConstraint{right});
    s.addConstraint(VerticalConstraint{left});
    s.addConstraint(FixConstraint{sp(bottom)});
    s.addConstraint(LengthConstraint{bottom, width.id()});
    s.addConstraint(LengthConstraint{right, height.id()});

    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", s.id(), padLength.id());

    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(document.massProperties().volumeMm3, 100.0 * 50.0 * 20.0, 1e-3);

    // The edit that used to do nothing at all.
    ASSERT_TRUE(document.setParameterValue(width.id(), 120.0));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(document.massProperties().volumeMm3, 120.0 * 50.0 * 20.0, 1e-3)
        << "a dimension edit did not reach geometry added through the raw reference";
}

TEST(M5Ui, M5_REV2_013_InjectingAKernelClearsTheFailureItCaused) {
    // The solver half of this had a test; the KERNEL half had none, and a
    // reviewer proved it by reverting the fix and watching the suite stay green.
    UiDoc doc;
    doc.document.setGeometryKernel(nullptr);
    EXPECT_FALSE(doc.document.recompute().success);
    EXPECT_NE(doc.pad->state(), ComputeState::Valid);

    OcctGeometryKernel kernel;
    doc.document.setGeometryKernel(&kernel);
    ASSERT_TRUE(doc.document.recompute().success) << "injecting a kernel did not recover";
    EXPECT_EQ(doc.pad->state(), ComputeState::Valid);
    EXPECT_NEAR(doc.document.massProperties().volumeMm3, 100.0 * 50.0 * 20.0, 1e-3);
}

TEST(M5Ui, M5_REV2_014_AnEmptySketchIsNotUnderConstrainedWithZeroFreedom) {
    PartDocument document{"Empty"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    Sketch& s = document.addSketch("Sketch001"); // no entities at all
    ASSERT_TRUE(document.recompute().success);

    const Sketch& solved = *document.findSketch(s.id());
    // Nothing to be free, so it is solved -- trivially, but genuinely.
    // "Under-constrained, DOF 0" is the self-contradictory reading
    // ADR-M5-012 exists to remove, and an empty sketch still produced it.
    EXPECT_EQ(solved.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(solved.degreesOfFreedom(), 0);
}

TEST(M5Ui, M5_REV2_015_DiameterFloorFollowsTheRadiusItDrives) {
    // The floor was checked against the diameter and committed as the radius,
    // so Diameter = 1e-6 wrote a radius the sketch's own validator rejects.
    // The fix had no test.
    PartDocument document{"DiameterFloor"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    // Expressed RELATIVE to the constant, not as a hard-coded 1e-6. Pinning the
    // literal made this test fail the moment the floor moved, for a reason that
    // had nothing to do with what it checks.
    Parameter& diameter =
        document.addParameter("D", kMinSketchDimensionMm, UnitType::Millimeter);
    Sketch& s = document.addSketch("Sketch001");
    const SketchEntityId circle = s.addCircle(Vec2{0, 0}, 5.0);
    document.addSketchConstraint(
        s.id(), FixConstraint{SketchElementRef{circle, SketchSubElement::CenterPoint}});
    document.addSketchConstraint(s.id(), DiameterConstraint{circle, diameter.id()});

    EXPECT_FALSE(document.recompute().success);
    EXPECT_EQ(document.findSketch(s.id())->solveStatus(), SketchSolveStatus::InvalidInput);
    // Geometry untouched.
    EXPECT_NEAR(
        std::get<SketchCircle>(document.findSketch(s.id())->findEntity(circle)->geometry).radiusMm,
        5.0, 1e-9);

    // Twice the floor is accepted and commits exactly the floor as the radius.
    ASSERT_TRUE(document.setParameterValue(diameter.id(), 2.0 * kMinSketchDimensionMm));
    ASSERT_TRUE(document.recompute().success);
}

// --- Previously-deferred findings, now closed --------------------------------

TEST(M5Ui, M5_DEF_010_ASolveMayNotCommitGeometryTheSketchWouldRefuse) {
    // replaceGeometry does not re-validate, so a Coincident between a line's
    // OWN endpoints solved happily to a zero-length line -- geometry addEntity
    // has always refused -- and committed it with status Solved. The sketch
    // then held state its own invariant forbids.
    PartDocument document{"Collapse"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);

    Sketch& s = document.addSketch("Sketch001");
    const SketchEntityId line = s.addLine(Vec2{0, 0}, Vec2{50, 0});
    document.addSketchConstraint(
        s.id(), FixConstraint{SketchElementRef{line, SketchSubElement::StartPoint}});
    document.addSketchConstraint(
        s.id(), CoincidentConstraint{SketchElementRef{line, SketchSubElement::StartPoint},
                                     SketchElementRef{line, SketchSubElement::EndPoint}});

    EXPECT_FALSE(document.recompute().success);
    const Sketch& solved = *document.findSketch(s.id());
    EXPECT_NE(solved.solveStatus(), SketchSolveStatus::Solved);
    // And the geometry is the last good one, not a collapsed line.
    const SketchLine& l = std::get<SketchLine>(solved.findEntity(line)->geometry);
    EXPECT_NEAR(std::hypot(l.end.x - l.start.x, l.end.y - l.start.y), 50.0, 1e-9);
}

TEST(M5Ui, M5_DEF_011_AUnitlessParameterCannotDriveADimension) {
    // Unitless satisfied BOTH the length and the angle check, so one unitless
    // Parameter could drive a Length and an Angle interchangeably -- the door
    // the unit rule exists to close, left ajar.
    UiDoc doc;
    Parameter& unitless = doc.document.addParameter("Raw", 40.0, UnitType::Unitless);
    ASSERT_NE(doc.document.addSketchConstraint(doc.sketch->id(),
                                               LengthConstraint{doc.top, unitless.id()}),
              kInvalidSketchConstraintId)
        << "the facade checks existence, not units -- units are the solver's check";

    EXPECT_FALSE(doc.document.recompute().success);
    EXPECT_EQ(doc.solved().solveStatus(), SketchSolveStatus::InvalidInput);
    EXPECT_FALSE(doc.solved().offendingConstraints().empty());
}

TEST(M5Ui, M5_DEF_012_APadBlockedByItsSketchSaysSoAndNamesTheCause) {
    // The graph stores Failed for both "this failed" and "a prerequisite
    // failed", and the display discarded the distinction: a Pad blocked by a
    // conflicting sketch read exactly like a Pad that broke on its own, with no
    // diagnostic -- pointing the user at the wrong object.
    UiDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    Parameter& other = doc.document.addParameter("WidthAlt", 70.0, UnitType::Millimeter);
    ASSERT_NE(doc.document.addSketchConstraint(doc.sketch->id(),
                                               LengthConstraint{doc.bottom, other.id()}),
              kInvalidSketchConstraintId);
    EXPECT_FALSE(doc.document.recompute().success);

    const DocumentOutline outline(doc.document);
    const std::vector<OutlineNode> sketches = NodesOfKind(outline, OutlineKind::Sketch);
    const std::vector<OutlineNode> solids = NodesOfKind(outline, OutlineKind::Solid);
    ASSERT_EQ(sketches.size(), 1u);
    ASSERT_EQ(solids.size(), 1u);

    // The sketch is the one that actually failed.
    EXPECT_EQ(sketches.front().state, OutlineState::Failed);
    // The Pad is BLOCKED, and says by what.
    EXPECT_EQ(solids.front().state, OutlineState::Blocked);
    EXPECT_NE(solids.front().diagnostic.find("Sketch001"), std::string::npos)
        << solids.front().diagnostic;
    // Blocked and Failed are distinguishable without colour.
    EXPECT_STRNE(DocumentOutline::stateMarker(OutlineState::Blocked),
                 DocumentOutline::stateMarker(OutlineState::Failed));
}

// --- Round-4 review regressions ----------------------------------------------

TEST(M5Ui, M5_REV4_010_APreExistingDegenerateEntityDoesNotPoisonTheSketch) {
    // CommitSolvedGeometry validated EVERY entity, not just the ones it wrote.
    // restoreEntity deliberately does not validate -- a hand-edited file must
    // round-trip -- so one degenerate entity anywhere in a loaded document made
    // the whole sketch permanently unsolvable, with a diagnostic naming no
    // constraint and a stale DOF of 0 reading as "fully constrained".
    //
    // No Pad here on purpose: a stray entity breaks the PROFILE too, and this
    // test is about the SOLVE. Asserting document-level success would fail for
    // the profile's reason and prove nothing about the commit.
    PartDocument document{"Poisoned"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);

    Sketch& s = document.addSketch("Sketch001");
    const SketchEntityId line = s.addLine(Vec2{0, 0}, Vec2{80.0, 0});
    document.addSketchConstraint(
        s.id(), FixConstraint{SketchElementRef{line, SketchSubElement::StartPoint}});
    document.addSketchConstraint(s.id(), HorizontalConstraint{line});
    document.addSketchConstraint(s.id(), LengthConstraint{line, width.id()});
    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(document.findSketch(s.id())->solveStatus(), SketchSolveStatus::Solved);

    // A degenerate entity the solve does not touch, arriving the way a loaded
    // file delivers one.
    ASSERT_TRUE(document.editSketch(s.id(), [](Sketch& sketch) {
        EXPECT_TRUE(sketch.restoreEntity(NextSketchEntityId(),
                                         SketchLine{Vec2{500, 500}, Vec2{500, 500}}));
    }));

    ASSERT_TRUE(document.recompute().success)
        << "one untouched degenerate entity made the whole sketch unsolvable";
    const Sketch& solved = *document.findSketch(s.id());
    EXPECT_NE(solved.solveStatus(), SketchSolveStatus::NumericalFailure);

    // And the Parameter still drives it -- the failure mode was permanent.
    ASSERT_TRUE(document.setParameterValue(width.id(), 130.0));
    ASSERT_TRUE(document.recompute().success);
    const SketchLine& l = std::get<SketchLine>(solved.findEntity(line)->geometry);
    EXPECT_NEAR(std::hypot(l.end.x - l.start.x, l.end.y - l.start.y), 130.0, 1e-6);
    // The degenerate entity was left exactly as it was, not "repaired".
    EXPECT_EQ(solved.entities().size(), 2u);
}

TEST(M5Ui, M5_REV4_011_ADimensionAtTheFloorStillCommits) {
    // kMinSketchDimensionMm and kSketchToleranceMm were the same number with
    // inclusive bounds from opposite sides, so the smallest ACCEPTED dimension
    // was the largest REJECTED geometry and a converged solve was refused.
    PartDocument document{"Floor"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);
    Parameter& length =
        document.addParameter("L", kMinSketchDimensionMm, UnitType::Millimeter);

    Sketch& s = document.addSketch("Sketch001");
    const SketchEntityId line = s.addLine(Vec2{0, 0}, Vec2{1.0, 0});
    document.addSketchConstraint(
        s.id(), FixConstraint{SketchElementRef{line, SketchSubElement::StartPoint}});
    document.addSketchConstraint(s.id(), LengthConstraint{line, length.id()});

    ASSERT_TRUE(document.recompute().success)
        << "a dimension at the documented minimum could not be committed";
    const SketchLine& l =
        std::get<SketchLine>(document.findSketch(s.id())->findEntity(line)->geometry);
    EXPECT_NEAR(std::hypot(l.end.x - l.start.x, l.end.y - l.start.y),
                kMinSketchDimensionMm, kMinSketchDimensionMm * 1e-3);
}

TEST(M5Ui, M5_REV4_012_ReplacingTheMaterialUnhooksTheOneItReplaces) {
    // The round-3 reorder made the THROW path safe and left the SUCCESS path
    // dangling one line down: replacing material_ destroyed the previous
    // Material while the registry still resolved its id to the freed address.
    // In Release the freed memory still read the old density, so the document
    // reported a plausible but WRONG mass as current, with Success and no
    // diagnostic.
    PartDocument document{"MatSwap"};
    OcctGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Parameter& w = document.addParameter("W", 10.0, UnitType::Millimeter);
    Parameter& h = document.addParameter("H", 10.0, UnitType::Millimeter);
    Parameter& d = document.addParameter("D", 10.0, UnitType::Millimeter);
    Material& first = document.addMaterial("Alu", 2700.0);
    const ObjectId firstId = first.id();
    Body& body = document.addBody("Body001");
    document.addBoxFeature(body, "Box001", w.id(), h.id(), d.id());
    ASSERT_TRUE(document.recompute().success);

    Material& second = document.addMaterial("Steel", 7850.0);
    EXPECT_NE(second.id(), firstId);
    // The replaced material must be gone from BOTH the registry and the graph.
    EXPECT_FALSE(document.objectRegistry().contains(firstId))
        << "a destroyed Material is still resolvable";
    EXPECT_FALSE(document.dependencyGraph().hasNode(firstId))
        << "a destroyed Material still has a graph node";

    // And the mass really is the new material's, not freed memory that happens
    // to still read the old value.
    ASSERT_TRUE(document.recompute().success);
    ASSERT_TRUE(document.massProperties().valid);
    EXPECT_NEAR(document.massProperties().massKg, 1e-6 * 7850.0, 1e-9);
}

TEST(M5Ui, M5_REV4_013_RestoringADuplicatePadFeatureIdIsRefused) {
    // M5_REV3_012 covered restoreBody and restoreBoxFeature; deleting the
    // restorePadFeature check left the suite green.
    UiDoc doc;
    Body& body = doc.document.addBody("Body002");
    EXPECT_THROW(doc.document.restorePadFeature(body, doc.pad->id(), "Clash",
                                                ComputeState::Valid, doc.sketch->id(),
                                                doc.padLength->id(), kInvalidObjectId),
                 std::runtime_error);
    // Refused means unchanged: the Body did not gain a duplicate.
    EXPECT_TRUE(body.features().empty());
}

} // namespace

