#include "Viewer/DocumentOutline.h"

#include "Core/Body/Body.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Material/Material.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"

#include "Core/Sketch/SketchConstraint.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>

namespace paramcad {

namespace {

// Fixed-precision formatting so numeric columns line up and a value never
// silently changes width as it changes magnitude (UI spec 4: tabular digits).
std::string Number(double value, int decimals = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

const char* UnitLabel(UnitType unit) noexcept {
    switch (unit) {
        case UnitType::Millimeter: return "mm";
        case UnitType::Radian: return "rad";
        case UnitType::Kilogram: return "kg";
        case UnitType::Second: return "s";
        case UnitType::KilogramPerCubicMeter: return "kg/m^3";
        case UnitType::Unitless: return "";
    }
}

// The row text for one constraint. A dimensional constraint shows its
// PARAMETER NAME and current value, because "Length" alone in a list of four
// Lengths tells a user nothing about which one they are looking at -- and the
// parameter name is the thing they recognise from the parameter list.
std::string ConstraintLabel(const PartDocument& document, const SketchConstraint& constraint) {
    std::string label = ConstraintKindName(constraint.data);
    const ObjectId parameterId = BoundParameterId(constraint.data);
    if (parameterId == kInvalidObjectId) return label;

    const Parameter* parameter = document.parameters().findById(parameterId);
    if (parameter == nullptr) return label + " (unresolved parameter)";

    label += " = " + parameter->name() + " (" + Number(parameter->value());
    const char* unit = UnitLabel(parameter->unit());
    if (unit[0] != '\0') label += std::string(" ") + unit; // unitless -> no trailing space
    return label + ")";
}

OutlineState FromComputeState(ComputeState state) noexcept {
    switch (state) {
        case ComputeState::Valid: return OutlineState::Valid;
        case ComputeState::Failed: return OutlineState::Failed;
        case ComputeState::Suppressed: return OutlineState::Suppressed;
        default: return OutlineState::Dirty;
    }
}

} // namespace

const char* DocumentOutline::stateMarker(OutlineState state) noexcept {
    // Text markers so state survives greyscale, colour-blindness and a
    // screenshot (UI spec 11: never rely on colour alone).
    switch (state) {
        case OutlineState::Valid: return "";
        case OutlineState::Dirty: return "*";
        case OutlineState::Failed: return "!";
        case OutlineState::Blocked: return "-";
        case OutlineState::Suppressed: return "x";
        case OutlineState::Hidden: return "h";
        default: return "";
    }
}

const char* DocumentOutline::stateLabel(OutlineState state) noexcept {
    switch (state) {
        case OutlineState::Valid: return "Up to date";
        case OutlineState::Dirty: return "Needs recompute";
        case OutlineState::Failed: return "Failed";
        case OutlineState::Blocked: return "Blocked by a failed input";
        case OutlineState::Suppressed: return "Suppressed";
        case OutlineState::Hidden: return "Hidden";
        default: return "Not computed";
    }
}

const char* DocumentOutline::solveStatusLabel(SketchSolveStatus status) noexcept {
    // Deliberately NOT SolveStatusName(): that returns the enumerator spelling
    // for diagnostics and files, and it must stay stable. This is display text,
    // free to read like English and to change without breaking a format.
    switch (status) {
        case SketchSolveStatus::Solved: return "Solved";
        case SketchSolveStatus::UnderConstrained: return "Under-constrained";
        case SketchSolveStatus::OverConstrained: return "Over-constrained (redundant)";
        case SketchSolveStatus::Conflicting: return "Conflicting constraints";
        case SketchSolveStatus::InvalidInput: return "Invalid constraint input";
        case SketchSolveStatus::NumericalFailure: return "Solver did not converge";
    }
    return "Unknown";
}

namespace {

// A container row reports the worst state among its children, so a Part whose
// Pad has failed does not sit there reading "Not computed" while red rows hang
// beneath it. Order is severity, not enum order.
OutlineState WorstOf(OutlineState a, OutlineState b) noexcept {
    const auto rank = [](OutlineState s) {
        switch (s) {
            case OutlineState::Failed: return 5;
            case OutlineState::Blocked: return 4;
            case OutlineState::Dirty: return 3;
            case OutlineState::Suppressed: return 2;
            case OutlineState::Hidden: return 1;
            case OutlineState::Valid: return 1;
            default: return 0;
        }
    };
    return rank(a) >= rank(b) ? a : b;
}

} // namespace

OutlineNode DocumentOutline::build(const std::set<ObjectId>& hiddenIds) const {
    const PartDocument& document = *document_;
    const DependencyGraph& graph = document.dependencyGraph();

    // Blocked is DERIVED here rather than plumbed through.
    //
    // The graph stores Failed for both "this object's own recompute failed" and
    // "a prerequisite failed so it never ran" -- the recompute engine
    // distinguishes them in its report, and that distinction was discarded at
    // the display boundary. The result: in the conflicting-sketch sample the
    // Pad row read "Failed !" with no diagnostic, identical to a Pad that broke
    // on its own, pointing the user at the wrong object entirely.
    //
    // The rule is the engine's own: a node whose prerequisite failed was never
    // invoked. Applied to direct prerequisites it is transitively correct,
    // because a blocked prerequisite is itself Failed in the graph.
    const auto stateOf = [&graph](ObjectId id) {
        if (!graph.hasNode(id)) return OutlineState::Normal;
        const OutlineState own = FromComputeState(graph.state(id));
        if (own != OutlineState::Failed) return own;
        for (ObjectId prerequisite : graph.prerequisitesOf(id)) {
            if (!graph.hasNode(prerequisite)) continue;
            if (graph.state(prerequisite) == ComputeState::Failed) return OutlineState::Blocked;
        }
        return own;
    };

    // Names the prerequisite that actually failed, so "blocked" says by WHAT.
    // "Blocked" without a culprit moves the user's search rather than ending it.
    const auto blockedBy = [&graph, &document](ObjectId id) -> std::string {
        for (ObjectId prerequisite : graph.prerequisitesOf(id)) {
            if (!graph.hasNode(prerequisite)) continue;
            if (graph.state(prerequisite) != ComputeState::Failed) continue;
            for (const Sketch* sketch : document.sketches())
                if (sketch->id() == prerequisite)
                    return "waiting on sketch '" + sketch->name() + "'";
            for (const auto& body : document.bodies())
                for (const auto& feature : body->features())
                    if (feature->id() == prerequisite)
                        return "waiting on '" + feature->name() + "'";
            return "waiting on a prerequisite that failed";
        }
        return {};
    };

    OutlineNode root;
    root.id = document.id();
    root.name = document.name();
    root.typeLabel = "Part";
    root.kind = OutlineKind::Document;
    root.state = OutlineState::Normal;

    OutlineNode parameters;
    parameters.name = "Parameters";
    parameters.typeLabel = "Group";
    parameters.kind = OutlineKind::Other;
    for (const auto& parameter : document.parameters().items()) {
        OutlineNode node;
        node.id = parameter->id();
        node.name = parameter->name();
        node.typeLabel = "Parameter";
        node.kind = OutlineKind::Parameter;
        node.state = stateOf(parameter->id());
        parameters.children.push_back(std::move(node));
    }
    if (!parameters.children.empty()) root.children.push_back(std::move(parameters));

    for (const Sketch* sketch : document.sketches()) {
        OutlineNode node;
        node.id = sketch->id();
        node.name = sketch->name();
        node.typeLabel = "Sketch";
        node.kind = OutlineKind::Sketch;
        node.state = stateOf(sketch->id());

        // Since M5 a sketch DOES recompute (it solves), so the graph state is
        // the primary truth and the solve message is the reason. A failing
        // solve must win over a profile complaint: an unsolved sketch's profile
        // is meaningless, and reporting "not a closed loop" when the real cause
        // is a conflicting dimension sends the user to the wrong place.
        if (node.state == OutlineState::Failed && !sketch->solveMessage().empty()) {
            node.diagnostic = sketch->solveMessage();
        } else {
            const ProfileResult profile = BuildProfile(*sketch);
            if (!profile) {
                // The REASON is always offered. Whether it is a FAILURE depends
                // on whether anything is waiting for that profile.
                //
                // "Not a closed loop" describes a sketch that cannot be padded
                // yet. That is a failure when a Pad is asking for it -- UI spec
                // 12, UI_TREE_005 -- and it is simply the state of the drawing
                // when nothing is. Marking every open sketch "Failed" was
                // invisible until M6, because before DXF import every sketch in
                // the viewer was built by a flow that closed its profile.
                //
                // The owner's first manual import of `line.dxf` showed it at
                // once: one line imported correctly, drawn correctly, and the
                // tree said `[Skt]! line`. Nothing had failed. A user who sees
                // "Failed" after a successful import learns to distrust the
                // marker, which costs more than the marker is worth.
                node.diagnostic = profile.message;
                const bool somethingNeedsTheProfile = !graph.dependentsOf(sketch->id()).empty();
                if (somethingNeedsTheProfile) node.state = OutlineState::Failed;
            }
        }

        // The constraint list (spec 18) lives in the tree under its sketch,
        // because that is where its identity lives: a constraint is a
        // sub-object of one sketch, not a document-level peer.
        const std::vector<SketchConstraintId>& offenders = sketch->offendingConstraints();
        for (const SketchConstraint& constraint : sketch->constraints()) {
            OutlineNode child;
            child.id = ToObjectId(constraint.id);
            child.name = ConstraintLabel(document, constraint);
            child.typeLabel = ConstraintKindName(constraint.data);
            child.kind = OutlineKind::Constraint;
            // Constraints are not graph nodes, so they have no ComputeState of
            // their own. What a user needs is whether THIS constraint is one
            // the solver blamed -- named individually, because "the sketch
            // failed" does not tell anyone which constraint to remove.
            const bool blamed =
                std::find(offenders.begin(), offenders.end(), constraint.id) != offenders.end();
            child.state = blamed ? OutlineState::Failed : OutlineState::Normal;
            if (blamed) child.diagnostic = sketch->solveMessage();
            node.children.push_back(std::move(child));
        }

        root.children.push_back(std::move(node));
    }

    for (const auto& body : document.bodies()) {
        for (const auto& feature : body->features()) {
            OutlineNode node;
            node.id = feature->id();
            node.name = feature->name();
            node.typeLabel = std::string(feature->typeName());
            node.kind = dynamic_cast<const ISolidFeature*>(feature.get()) != nullptr
                            ? OutlineKind::Solid
                            : OutlineKind::Other;
            node.state = stateOf(feature->id());
            if (node.state == OutlineState::Blocked) node.diagnostic = blockedBy(feature->id());
            // Hidden is reported only when the object is otherwise fine:
            // "hidden" must never mask "failed" (UI spec 11).
            if (node.state == OutlineState::Valid && hiddenIds.count(feature->id()) != 0)
                node.state = OutlineState::Hidden;
            root.children.push_back(std::move(node));
        }
    }

    if (document.material()) {
        OutlineNode node;
        node.id = document.material()->id();
        node.name = document.material()->name();
        node.typeLabel = "Material";
        node.kind = OutlineKind::Material;
        node.state = stateOf(document.material()->id());
        root.children.push_back(std::move(node));
    }

    OutlineNode mass;
    mass.name = "MassProperties";
    mass.typeLabel = "Derived";
    mass.kind = OutlineKind::MassProperties;
    // Derived results carry their own currency flag (ADR-M3-006); showing them
    // as up to date when that flag is false is the display-layer version of the
    // defect that flag exists to prevent (UI spec 13).
    mass.state = document.massProperties().valid ? OutlineState::Valid : OutlineState::Dirty;
    root.children.push_back(std::move(mass));

    // Roll child states up so the root row summarises the document instead of
    // permanently reading "Not computed" (raised as a Major by UI review).
    const std::function<OutlineState(OutlineNode&)> rollUp =
        [&](OutlineNode& node) -> OutlineState {
        OutlineState worst = node.state;
        for (OutlineNode& child : node.children) worst = WorstOf(worst, rollUp(child));
        if (!node.children.empty()) node.state = worst;
        return worst;
    };
    rollUp(root);

    return root;
}

std::vector<PropertyRow> DocumentOutline::propertiesOf(ObjectId id) const {
    const PartDocument& document = *document_;
    std::vector<PropertyRow> rows;

    for (const auto& parameter : document.parameters().items()) {
        if (parameter->id() != id) continue;
        rows.push_back(PropertyRow{"General", "Name", parameter->name(), "", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"Value", "Value", Number(parameter->value()),
                                   UnitLabel(parameter->unit()), true, parameter->id(),
                                   parameter->value()});
        return rows;
    }

    for (const Sketch* sketch : document.sketches()) {
        if (sketch->id() != id) continue;
        rows.push_back(PropertyRow{"General", "Name", sketch->name(), "", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"General", "Entities",
                                   std::to_string(sketch->entities().size()), "", false,
                                   kInvalidObjectId, 0.0});
        const Vec3 origin = sketch->frame().toWorld(Vec2{0.0, 0.0});
        rows.push_back(
            PropertyRow{"Placement", "Origin X", Number(origin.x), "mm", false, kInvalidObjectId, 0.0});
        rows.push_back(
            PropertyRow{"Placement", "Origin Y", Number(origin.y), "mm", false, kInvalidObjectId, 0.0});
        rows.push_back(
            PropertyRow{"Placement", "Origin Z", Number(origin.z), "mm", false, kInvalidObjectId, 0.0});
        // "Solve status", not "Status": the Profile group already has a row
        // called "Status", and a panel showing two rows with the same label is
        // ambiguous to a user reading it, not just to a test looking it up.
        rows.push_back(PropertyRow{"Constraints", "Solve status",
                                   solveStatusLabel(sketch->solveStatus()), "", false,
                                   kInvalidObjectId, 0.0});
        // "not measured", never "0". Zero is this project's signal for FULLY
        // CONSTRAINED, so a sketch whose solve failed before any DOF was
        // measured must not read as finished work.
        rows.push_back(PropertyRow{
            "Constraints", "Degrees of freedom",
            sketch->degreesOfFreedom() == kUnknownDegreesOfFreedom
                ? std::string("not measured")
                : std::to_string(sketch->degreesOfFreedom()),
            "", false, kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"Constraints", "Count",
                                   std::to_string(sketch->constraints().size()), "", false,
                                   kInvalidObjectId, 0.0});
        if (!sketch->solveMessage().empty())
            rows.push_back(PropertyRow{"Constraints", "Solver diagnostic",
                                       sketch->solveMessage(), "", false, kInvalidObjectId,
                                       0.0});
        // The offending ids, listed explicitly (spec 18). A status alone does
        // not tell a user which constraint to remove, which is the one thing
        // they need in order to act.
        if (!sketch->offendingConstraints().empty()) {
            std::string ids;
            for (SketchConstraintId offender : sketch->offendingConstraints()) {
                if (!ids.empty()) ids += ", ";
                ids += std::to_string(ToObjectId(offender));
            }
            rows.push_back(PropertyRow{"Constraints", "Offending constraint IDs", ids, "", false,
                                       kInvalidObjectId, 0.0});
        }

        const ProfileResult profile = BuildProfile(*sketch);
        rows.push_back(PropertyRow{"Profile", "Status",
                                   profile ? "Closed loop" : "Invalid", "", false,
                                   kInvalidObjectId, 0.0});
        if (!profile)
            rows.push_back(PropertyRow{"Profile", "Diagnostic", profile.message, "", false,
                                       kInvalidObjectId, 0.0});
        return rows;
    }

    // A selected constraint. Looked up across every sketch by id, because a
    // constraint id is document-unique and the panel is given only an id.
    for (const Sketch* sketch : document.sketches()) {
        const auto constraintId = static_cast<SketchConstraintId>(id);
        const SketchConstraint* constraint = sketch->findConstraint(constraintId);
        if (constraint == nullptr) continue;

        rows.push_back(PropertyRow{"General", "Type", ConstraintKindName(constraint->data), "",
                                   false, kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"General", "ID", std::to_string(id), "", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"General", "Sketch", sketch->name(), "", false,
                                   kInvalidObjectId, 0.0});

        // The dimensional value, EDITABLE, pointing at the Parameter's id --
        // exactly like a Pad's Length above. Editing it goes through
        // PartDocument's facade, the sketch re-solves, and the 3D result
        // updates through the ordinary recompute path; nothing here writes
        // into the constraint.
        const ObjectId parameterId = BoundParameterId(constraint->data);
        if (parameterId != kInvalidObjectId) {
            const Parameter* parameter = document.parameters().findById(parameterId);
            if (parameter != nullptr) {
                rows.push_back(PropertyRow{"Dimension", "Value", Number(parameter->value()),
                                           UnitLabel(parameter->unit()), true, parameter->id(),
                                           parameter->value()});
                rows.push_back(PropertyRow{"Dimension", "Parameter", parameter->name(), "", false,
                                           kInvalidObjectId, 0.0});
            } else {
                // A bound parameter that does not resolve is a real fault, not
                // a blank row: silence here is how a broken reference reaches a
                // user as "the dimension does nothing" with no explanation.
                rows.push_back(PropertyRow{"Dimension", "Value", "(unresolved parameter)", "",
                                           false, kInvalidObjectId, 0.0});
            }
        }

        std::string references;
        for (SketchEntityId referenced : ReferencedEntities(constraint->data)) {
            if (!references.empty()) references += ", ";
            references += std::to_string(ToObjectId(referenced));
        }
        rows.push_back(PropertyRow{"References", "Entities", references, "", false,
                                   kInvalidObjectId, 0.0});

        const std::vector<SketchConstraintId>& offenders = sketch->offendingConstraints();
        if (std::find(offenders.begin(), offenders.end(), constraintId) != offenders.end())
            rows.push_back(PropertyRow{"Diagnostic", "Solver", sketch->solveMessage(), "", false,
                                       kInvalidObjectId, 0.0});
        return rows;
    }

    for (const auto& body : document.bodies()) {
        for (const auto& feature : body->features()) {
            if (feature->id() != id) continue;
            rows.push_back(PropertyRow{"General", "Name", feature->name(), "", false,
                                       kInvalidObjectId, 0.0});
            rows.push_back(PropertyRow{"General", "Type", std::string(feature->typeName()), "",
                                       false, kInvalidObjectId, 0.0});

            // A Pad's editable length is its Parameter, so the row points at the
            // PARAMETER's id: the panel writes through PartDocument's facade,
            // never into the feature (UI spec 20).
            if (const auto* pad = dynamic_cast<const PadFeature*>(feature.get())) {
                for (const auto& parameter : document.parameters().items()) {
                    if (parameter->id() != pad->lengthParameterId()) continue;
                    rows.push_back(PropertyRow{"Geometry", "Length", Number(parameter->value()),
                                               UnitLabel(parameter->unit()), true,
                                               parameter->id(), parameter->value()});
                }
            }

            // Pocket (M8): the editable Depth, exactly as Pad's Length, plus
            // the base it consumes -- read-only, because the chain reference is
            // structure, not a value to type over.
            if (const auto* pocket = dynamic_cast<const PocketFeature*>(feature.get())) {
                for (const auto& parameter : document.parameters().items()) {
                    if (parameter->id() != pocket->depthParameterId()) continue;
                    rows.push_back(PropertyRow{"Geometry", "Depth", Number(parameter->value()),
                                               UnitLabel(parameter->unit()), true,
                                               parameter->id(), parameter->value()});
                }
                rows.push_back(PropertyRow{"Chain", "Base feature",
                                           std::to_string(pocket->baseFeatureId()), "", false,
                                           kInvalidObjectId, 0.0});
            }

            if (const auto* referencing =
                    dynamic_cast<const IMaterialReferencing*>(feature.get())) {
                const bool assigned = referencing->materialId() != kInvalidObjectId &&
                                      document.material() &&
                                      document.material()->id() == referencing->materialId();
                rows.push_back(PropertyRow{
                    "Material", "Material",
                    assigned ? document.material()->name() : std::string("(none)"), "", false,
                    kInvalidObjectId, 0.0});
                if (assigned)
                    rows.push_back(PropertyRow{"Material", "Density",
                                               Number(document.material()->density(), 1),
                                               "kg/m^3", false, kInvalidObjectId, 0.0});
            }
            return rows;
        }
    }

    if (document.material() && document.material()->id() == id) {
        rows.push_back(PropertyRow{"General", "Name", document.material()->name(), "", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"Physical", "Density",
                                   Number(document.material()->density(), 1), "kg/m^3", false,
                                   kInvalidObjectId, 0.0});
        return rows;
    }

    return rows;
}

} // namespace paramcad
