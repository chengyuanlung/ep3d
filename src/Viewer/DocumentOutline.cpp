#include "Viewer/DocumentOutline.h"
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <variant>
#include "Core/Feature/IParameterisedFeature.h"
#include "Core/Feature/ISketchConsuming.h"

#include "Core/Body/Body.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Document/PartDocument.h"
#include "Core/Reference/ReferenceFrame.h"
#include "Core/Connector/Connector.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
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
std::string_view RoleName(ConnectorRole role) {
    switch (role) {
        case ConnectorRole::Generic: return "Generic";
        case ConnectorRole::Mount: return "Mount";
        case ConnectorRole::Shaft: return "Shaft";
        case ConnectorRole::LinearGuide: return "Linear guide";
        case ConnectorRole::ToolFlange: return "Tool flange";
        case ConnectorRole::Electrical: return "Electrical";
        case ConnectorRole::Pneumatic: return "Pneumatic";
    }
    return "Generic";
}

std::string Number(double value, int decimals = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text = buffer;
    // NO NEGATIVE ZERO. A coordinate of -1e-17 -- which is what a solver
    // returns for "on the axis" about half the time -- printed as "-0.000",
    // and a panel showing "-0.000 mm" next to "0.000 mm" is showing two
    // numbers where the model has one. The minus survives for anything that
    // actually rounds to a non-zero digit.
    if (text.size() > 1 && text[0] == '-' &&
        text.find_first_of("123456789") == std::string::npos)
        text.erase(0, 1);
    return text;
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
    // Unreachable for a valid UnitType. Present because MSVC cannot see that
    // the switch is exhaustive, and a warning left standing is one nobody
    // reads the next time.
    return "";
}

// A row that WRITES a parameter.
//
// EVERY editable value row is built here, and that is the point. The five
// call sites below used to spell the aggregate out by hand with seven
// initialisers, which left `field` at its default of PropertyField::None --
// and an editable row with no field is a SILENT no-op: the cell accepts the
// typing, ApplyPropertyEdit answers "that row is not editable", and the model
// never changes. A Pad whose Length could be retyped without the solid ever
// getting taller shipped that way. A factory cannot forget an argument it
// does not take.
// The direction row: a checkbox over the SIGN of an extrusion parameter.
//
// `numericValue` carries the current value so the widget can tick the box
// without going back to the document, and the displayed text says the same
// thing in words -- a checkbox with no label reads as nothing in a screenshot,
// and this project does not let one channel carry a fact on its own (A06).
PropertyRow ReversedRow(std::string group, const Parameter& parameter) {
    return PropertyRow{std::move(group),
                       "Reversed",
                       parameter.value() < 0.0 ? "yes" : "no",
                       "",
                       true,
                       parameter.id(),
                       parameter.value(),
                       PropertyField::Reversed};
}

// The object's own name, editable (M17.16).
//
// Through a factory for the same reason EditableValueRow exists: the seven-
// argument aggregate leaves `field` at None, and an editable row with no field
// is a cell that accepts typing and changes nothing (ADR-M17-027).
PropertyRow NameRow(ObjectId objectId, const std::string& name) {
    return PropertyRow{"General", "Name", name, "", true, objectId, 0.0, PropertyField::Name};
}

PropertyRow EditableValueRow(std::string group, std::string label, const Parameter& parameter) {
    return PropertyRow{std::move(group),  std::move(label), Number(parameter.value()),
                       UnitLabel(parameter.unit()), true, parameter.id(), parameter.value(),
                       PropertyField::Value};
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

    // Frames, in HIERARCHY order (M10.5): a frame's children nest under it, so
    // the tree shows the parent chain the world transform is composed from
    // rather than a flat list that hides it. Connectors nest under the frame
    // they are on, because a connector IS that frame plus meaning
    // (ADR-M10-004) and listing them apart would invite the reader to think
    // they have a position of their own.
    const std::function<OutlineNode(const ReferenceFrame*)> buildFrame =
        [&](const ReferenceFrame* frame) {
            OutlineNode node;
            node.id = frame->id();
            node.name = frame->name();
            node.typeLabel = "Frame";
            node.kind = OutlineKind::Frame;
            node.state = stateOf(frame->id());
            for (const Connector* connector : document.connectors()) {
                if (connector->frameId() != frame->id()) continue;
                OutlineNode child;
                child.id = connector->id();
                child.name = connector->name();
                child.typeLabel = "Connector";
                child.kind = OutlineKind::Connector;
                node.children.push_back(std::move(child));
            }
            for (const ReferenceFrame* candidate : document.frames())
                if (candidate->parentFrameId() == frame->id())
                    node.children.push_back(buildFrame(candidate));
            return node;
        };
    OutlineNode frames;
    frames.name = "Frames";
    frames.typeLabel = "Group";
    frames.kind = OutlineKind::Other;
    for (const ReferenceFrame* frame : document.frames())
        if (frame->parentFrameId() == kInvalidObjectId)
            frames.children.push_back(buildFrame(frame));
    if (!frames.children.empty()) root.children.push_back(std::move(frames));

    // --- The history, as ONE chronological spine (M17.10, ADR-M17-033) ------
    //
    // Sketches used to be listed together and then every feature after them, so
    // a user who sketched on a face and padded it three times got three
    // sketches in a row followed by three pads: the ORDER was gone, and nothing
    // said which sketch made which pad. Both facts are in the model; the tree
    // simply was not showing them.
    //
    // The two builders below make the nodes; the emit loop decides where they
    // go.
    // One feature by id, across every body. The dependency graph deals in
    // ObjectIds and the capability question below has to be put to the FEATURE,
    // so something has to turn one into the other.
    const auto featureById = [&document](ObjectId id) -> const Feature* {
        for (const auto& body : document.bodies())
            for (const auto& feature : body->features())
                if (feature->id() == id) return feature.get();
        return nullptr;
    };

    const auto buildSketchNode = [&](const Sketch* sketch) {
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
                // ...AND WHETHER ANYTHING WANTED A PROFILE OUT OF IT (M26.8).
                //
                // This asked only whether ANYTHING depended on the sketch,
                // which is a different question. A Hole depends on its sketch
                // and reads POINTS from it: four dots that never join is
                // exactly the drawing it wants. `examples/stepper-motor.ep3ds`
                // therefore showed `[Skt] ! Mounts  Failed` beside a hole
                // feature that had drilled four correct bores -- a red marker
                // on a part with nothing wrong with it, which is how a marker
                // stops being believed.
                //
                // Asked of the FEATURE through its capability, so a new
                // sketch-consuming feature answers for itself instead of
                // needing a branch here (ADR-M3-007).
                bool somethingNeedsTheProfile = false;
                for (const ObjectId dependentId : graph.dependentsOf(sketch->id())) {
                    const auto* consumer =
                        dynamic_cast<const ISketchConsuming*>(featureById(dependentId));
                    if (consumer == nullptr) continue;
                    if (consumer->needsClosedProfileOf(sketch->id()))
                        somethingNeedsTheProfile = true;
                }
                if (somethingNeedsTheProfile) node.state = OutlineState::Failed;
            }
        }

        // THE CONSTRAINT LIST IS NOT IN THE TREE (M26.10).
        //
        // It used to be, one row per constraint under its sketch. A rectangle
        // drawn with the tool carries eight of them before a single dimension
        // is added, so the tree that is meant to show a PART's structure was
        // mostly a list of Coincidents, each row saying "Not computed" because
        // a constraint is not a graph node and has no state of its own.
        //
        // Spec 18's requirement -- that the solver's blame is readable PER
        // CONSTRAINT, never just "the sketch failed" -- has not gone anywhere.
        // It is met by the Constraints panel, which says it better: kind, what
        // it is ON, its value, and OK or AT FAULT per row, with a Delete
        // Constraint button beside it. That panel is open exactly when a user
        // can act on a constraint, which is while the sketch is.
        //
        // What the tree keeps is the SKETCH's own verdict: its state, its
        // solve message, and -- in the property panel -- its constraint count
        // and the ids of any the solver blamed. That is what belongs in a
        // structure view; twelve rows of "Coincident" is not.
        return node;
    };

    const auto buildFeatureNode = [&](const Feature* feature) {
        OutlineNode node;
        node.id = feature->id();
        node.name = feature->name();
        node.typeLabel = std::string(feature->typeName());
        node.kind = dynamic_cast<const ISolidFeature*>(feature) != nullptr ? OutlineKind::Solid
                                                                          : OutlineKind::Other;
        node.state = stateOf(feature->id());
        if (node.state == OutlineState::Blocked) node.diagnostic = blockedBy(feature->id());
        // Hidden is reported only when the object is otherwise fine: "hidden"
        // must never mask "failed" (UI spec 11).
        if (node.state == OutlineState::Valid && hiddenIds.count(feature->id()) != 0)
            node.state = OutlineState::Hidden;
        return node;
    };

    // WHICH SKETCH each feature is built from, asked by capability so a fourth
    // sketch-consuming feature is absorbed without editing this (ADR-M17-033).
    //
    // A sketch with MORE THAN ONE consumer is deliberately not absorbed. Nesting
    // it under one of them would say it belongs to that feature, which is false
    // for the other -- and the user would have no way to reach the relationship
    // the tree chose to hide. It stays on the spine, visibly belonging to
    // neither.
    std::map<ObjectId, ObjectId> absorbedBy; // sketch -> its single consumer
    std::set<ObjectId> sharedSketches;
    for (const auto& body : document.bodies())
        for (const auto& feature : body->features()) {
            const auto* consumer = dynamic_cast<const ISketchConsuming*>(feature.get());
            if (consumer == nullptr) continue;
            const ObjectId sketchId = consumer->consumedSketchId();
            if (sketchId == kInvalidObjectId) continue;
            if (!absorbedBy.emplace(sketchId, feature->id()).second)
                sharedSketches.insert(sketchId);
        }
    for (ObjectId shared : sharedSketches) absorbedBy.erase(shared);

    // CHRONOLOGICAL, by ObjectId. Every id in this document came from the one
    // ObjectIdGenerator, so id order IS creation order -- including after a
    // reload, because restore keeps each id and advances the generator past it.
    // Sorting by id therefore needs no timestamp stored anywhere, and cannot
    // disagree with the order things were actually made in.
    std::vector<ObjectId> history;
    for (const Sketch* sketch : document.sketches()) history.push_back(sketch->id());
    for (const auto& body : document.bodies())
        for (const auto& feature : body->features()) history.push_back(feature->id());
    std::sort(history.begin(), history.end());

    for (ObjectId id : history) {
        if (const Sketch* sketch = document.findSketch(id)) {
            if (absorbedBy.count(id) != 0) continue; // shown under its feature below
            root.children.push_back(buildSketchNode(sketch));
            continue;
        }
        for (const auto& body : document.bodies()) {
            const Feature* found = nullptr;
            for (const auto& feature : body->features())
                if (feature->id() == id) found = feature.get();
            if (found == nullptr) continue;
            OutlineNode node = buildFeatureNode(found);
            // ABSORBED: the sketch is drawn INSIDE the feature that consumed
            // it, which is the one place a user can read the lineage without
            // opening a dependency dialog.
            const auto* consumer = dynamic_cast<const ISketchConsuming*>(found);
            if (consumer != nullptr) {
                const auto absorbed = absorbedBy.find(consumer->consumedSketchId());
                if (absorbed != absorbedBy.end() && absorbed->second == id)
                    if (const Sketch* sketch = document.findSketch(absorbed->first))
                        node.children.push_back(buildSketchNode(sketch));
            }
            root.children.push_back(std::move(node));
            break;
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
    //
    // ONLY INTO ROWS THAT HAVE NO STATE OF THEIR OWN. A container -- the root,
    // Parameters, Frames -- is Normal because there is nothing to compute about
    // it, and a summary is the only thing it can usefully say. A feature is
    // different: its state is a FACT about that feature, and a summary of what
    // is underneath it must not overwrite that fact.
    //
    // This became load-bearing when sketches were absorbed into the features
    // that consume them (M17.10, ADR-M17-033). A Pad whose sketch has a
    // conflicting dimension is BLOCKED -- it never ran, and the thing that
    // broke is the sketch. With the sketch now a child, the old rule rolled
    // Failed upward and the Pad reported that IT had failed: the exact
    // distinction M5_DEF_012 exists to protect, destroyed for exactly the case
    // it was written for, and pointing the user at the wrong object.
    const std::function<OutlineState(OutlineNode&)> rollUp =
        [&](OutlineNode& node) -> OutlineState {
        OutlineState worst = node.state;
        for (OutlineNode& child : node.children) worst = WorstOf(worst, rollUp(child));
        if (!node.children.empty() && node.state == OutlineState::Normal) node.state = worst;
        // The WORST is still what travels upward, whether or not this row took
        // it: a container above a blocked feature must still summarise it.
        return worst;
    };
    rollUp(root);

    return root;
}

namespace {

// A read-only row. Every row a picked sketch element produces is one of these:
// the geometry belongs to the solver, and a cell that took a typed coordinate
// would be overwritten by the next solve that disagreed with it.
PropertyRow ReadOnlyRow(std::string group, std::string label, std::string value,
                        std::string unit = std::string()) {
    return PropertyRow{std::move(group), std::move(label), std::move(value), std::move(unit),
                       false, kInvalidObjectId, 0.0, PropertyField::None};
}

std::string Degrees(double radians) {
    return Number(radians * 180.0 / 3.14159265358979323846, 2);
}

const char* SubElementLabel(SketchSubElement part) noexcept {
    switch (part) {
        case SketchSubElement::Whole: return "the whole thing";
        case SketchSubElement::StartPoint: return "its start point";
        case SketchSubElement::EndPoint: return "its end point";
        case SketchSubElement::CenterPoint: return "its centre";
        case SketchSubElement::SplinePoint: return "one of its points";
        case SketchSubElement::SplineHandle: return "a tangent handle";
    }
    return "part of it";
}

} // namespace

std::vector<PropertyRow> DocumentOutline::propertiesOfSketchElement(
    ObjectId sketchId, const SketchElementRef& ref) const {
    std::vector<PropertyRow> rows;
    const Sketch* sketch = document_->findSketch(sketchId);
    if (sketch == nullptr) return rows;
    const SketchEntity* entity = sketch->findEntity(ref.entityId);
    if (entity == nullptr) return rows;

    const auto point = [&rows](const char* label, Vec2 at) {
        rows.push_back(ReadOnlyRow("Geometry", std::string(label) + " u", Number(at.x), "mm"));
        rows.push_back(ReadOnlyRow("Geometry", std::string(label) + " v", Number(at.y), "mm"));
    };

    // WHAT IT IS, first, because that is the question a click asks.
    const char* kind = "Geometry";
    std::visit(
        [&kind](const auto& geometry) {
            using T = std::decay_t<decltype(geometry)>;
            if constexpr (std::is_same_v<T, SketchPoint>) kind = "Point";
            else if constexpr (std::is_same_v<T, SketchLine>) kind = "Line";
            else if constexpr (std::is_same_v<T, SketchCircle>) kind = "Circle";
            else if constexpr (std::is_same_v<T, SketchArc>) kind = "Arc";
            else if constexpr (std::is_same_v<T, SketchEllipse>) kind = "Ellipse";
            else if constexpr (std::is_same_v<T, SketchEllipticalArc>) kind = "Elliptical arc";
            else kind = "Spline";
        },
        entity->geometry);
    rows.push_back(ReadOnlyRow("General", "Type", kind));
    rows.push_back(ReadOnlyRow(
        "General", "Id",
        "#" + std::to_string(static_cast<unsigned long long>(ToObjectId(entity->id)))));
    // WHAT WAS PICKED. A line and one end of that line are different answers,
    // and the panel is the only place that can say which one the click meant.
    if (ref.subElement != SketchSubElement::Whole)
        rows.push_back(ReadOnlyRow("General", "Picked", SubElementLabel(ref.subElement)));
    // CONSTRUCTION IS A PROPERTY OF THE ENTITY, and the one that decides
    // whether a pad will sweep it -- so a user staring at a profile that will
    // not extrude needs to be able to read it here.
    rows.push_back(ReadOnlyRow("General", "Construction", entity->construction ? "yes" : "no"));

    std::visit(
        [&](const auto& geometry) {
            using T = std::decay_t<decltype(geometry)>;
            if constexpr (std::is_same_v<T, SketchPoint>) {
                point("Position", geometry.position);
            } else if constexpr (std::is_same_v<T, SketchLine>) {
                point("Start", geometry.start);
                point("End", geometry.end);
                const double du = geometry.end.x - geometry.start.x;
                const double dv = geometry.end.y - geometry.start.y;
                rows.push_back(
                    ReadOnlyRow("Geometry", "Length", Number(std::sqrt(du * du + dv * dv)), "mm"));
                // MEASURED, not stored. A line has no angle of its own -- this
                // is what its two ends currently say, and it moves when they do.
                rows.push_back(
                    ReadOnlyRow("Geometry", "Angle", Degrees(std::atan2(dv, du)), "deg"));
            } else if constexpr (std::is_same_v<T, SketchCircle>) {
                point("Centre", geometry.center);
                rows.push_back(ReadOnlyRow("Geometry", "Radius", Number(geometry.radiusMm), "mm"));
                rows.push_back(
                    ReadOnlyRow("Geometry", "Diameter", Number(geometry.radiusMm * 2.0), "mm"));
            } else if constexpr (std::is_same_v<T, SketchArc>) {
                point("Centre", geometry.center);
                rows.push_back(ReadOnlyRow("Geometry", "Radius", Number(geometry.radiusMm), "mm"));
                rows.push_back(
                    ReadOnlyRow("Geometry", "Start angle", Degrees(geometry.startAngleRad), "deg"));
                rows.push_back(
                    ReadOnlyRow("Geometry", "End angle", Degrees(geometry.endAngleRad), "deg"));
                // WHICH OF THE TWO ARCS between those angles is meant. Without
                // it the two rows above describe two different shapes equally
                // well, which is the ambiguity SketchArc stores this flag to
                // avoid in the first place.
                rows.push_back(ReadOnlyRow("Geometry", "Direction",
                                           geometry.counterClockwise ? "counter-clockwise"
                                                                     : "clockwise"));
            } else if constexpr (std::is_same_v<T, SketchEllipse> ||
                                 std::is_same_v<T, SketchEllipticalArc>) {
                point("Centre", geometry.center);
                rows.push_back(
                    ReadOnlyRow("Geometry", "Major radius", Number(geometry.majorRadiusMm), "mm"));
                rows.push_back(
                    ReadOnlyRow("Geometry", "Minor radius", Number(geometry.minorRadiusMm), "mm"));
                rows.push_back(
                    ReadOnlyRow("Geometry", "Rotation", Degrees(geometry.rotationRad), "deg"));
            } else {
                rows.push_back(
                    ReadOnlyRow("Geometry", "Points", std::to_string(geometry.points.size())));
                rows.push_back(ReadOnlyRow("Geometry", "Closed", geometry.closed ? "yes" : "no"));
                rows.push_back(
                    ReadOnlyRow("Geometry", "Handles", std::to_string(geometry.handles.size())));
            }
        },
        entity->geometry);

    // WHERE THE PICKED POINT ACTUALLY IS, when a sub-element was picked. The
    // rows above describe the whole entity; this one answers the click.
    if (ref.subElement != SketchSubElement::Whole) {
        const std::optional<Vec2> at =
            PointOfSubElement(entity->geometry, ref.subElement, ref.index);
        if (at) point("Picked point", *at);
    }

    // WHAT IS HOLDING IT. This is the row a user actually needs when geometry
    // will not move: an entity that refuses to drag is over-constrained, and
    // counting what holds it is the first thing anyone asks.
    int holding = 0;
    for (const SketchConstraint& constraint : sketch->constraints())
        for (const SketchEntityId named : ReferencedEntities(constraint.data))
            if (named == entity->id) {
                ++holding;
                break;
            }
    rows.push_back(ReadOnlyRow("Constraints", "On this", std::to_string(holding)));
    return rows;
}

std::vector<PropertyRow> DocumentOutline::propertiesOf(ObjectId id) const {
    const PartDocument& document = *document_;
    std::vector<PropertyRow> rows;

    for (const auto& parameter : document.parameters().items()) {
        if (parameter->id() != id) continue;
        const bool driven = !parameter->expression().empty();

        rows.push_back(NameRow(parameter->id(), parameter->name()));

        // TWO ROWS, NOT ONE FIELD WITH TWO MODES (M11.3).
        //
        // The reference model puts the expression and its result in a single
        // box that shows one or the other depending on whether it has focus.
        // A table cell has nowhere to put a mode indicator, so the same trick
        // here would mean a number that silently is not the thing you can edit.
        // Two rows keep both facts visible at all times, and each says plainly
        // whether it can be typed into.
        //
        // The value row is READ-ONLY while an expression drives it: typing a
        // number there would clear the expression (ADR-M11-006), and a cell
        // that quietly deletes a formula is not an edit a user asked for. The
        // Expression row is where that is done, by clearing it.
        rows.push_back(PropertyRow{"Value", "Value", Number(parameter->value()),
                                   UnitLabel(parameter->unit()), !driven, parameter->id(),
                                   parameter->value(),
                                   driven ? PropertyField::None : PropertyField::Value});

        // Always present, even when empty -- an editable row that only appears
        // once you already know it exists is not discoverable (roadmap 42.3.1).
        // Only for units an expression can produce; for the others the row
        // would be an invitation the facade refuses.
        if (ExpressionDimensionOf(parameter->unit()).has_value()) {
            rows.push_back(PropertyRow{"Value", "Expression", parameter->expression(), "",
                                       true, parameter->id(), 0.0,
                                       PropertyField::Expression});
        }
        return rows;
    }

    for (const Sketch* sketch : document.sketches()) {
        if (sketch->id() != id) continue;
        rows.push_back(NameRow(sketch->id(), sketch->name()));
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
        // Which plane this sketch actually lives on (M10.2). Named, not an id:
        // "(its own plane)" is the honest answer for a sketch with no support
        // frame, and it is a different answer from "Origin".
        if (sketch->supportFrameId() == kInvalidObjectId) {
            rows.push_back(PropertyRow{"Placement", "Support", "(its own plane)", "", false,
                                       kInvalidObjectId, 0.0});
        } else {
            const ReferenceFrame* support = document.findFrame(sketch->supportFrameId());
            rows.push_back(PropertyRow{"Placement", "Support",
                                       support != nullptr ? support->name()
                                                          : std::string("(missing frame)"),
                                       "", false, kInvalidObjectId, 0.0});
        }
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
                rows.push_back(EditableValueRow("Dimension", "Value", *parameter));
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

    // A frame's rows: its own local transform, EDITABLE where it is a plain
    // number, plus the composed world position, which is read-only because it
    // is derived (ADR-M10-002 -- a world transform is never stored, so it is
    // never typed either).
    for (const ReferenceFrame* frame : document.frames()) {
        if (frame->id() != id) continue;
        rows.push_back(PropertyRow{"General", "Name", frame->name(), "", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"General", "Type", "Frame", "", false, kInvalidObjectId, 0.0});
        const Transform3D& local = frame->localTransform();
        rows.push_back(PropertyRow{"Local", "X", Number(local.translation.x), "mm", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"Local", "Y", Number(local.translation.y), "mm", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"Local", "Z", Number(local.translation.z), "mm", false,
                                   kInvalidObjectId, 0.0});
        const Transform3D world = document.worldTransform(frame->id());
        rows.push_back(PropertyRow{"World", "X", Number(world.translation.x), "mm", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"World", "Y", Number(world.translation.y), "mm", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"World", "Z", Number(world.translation.z), "mm", false,
                                   kInvalidObjectId, 0.0});
        if (frame->parentFrameId() != kInvalidObjectId) {
            const ReferenceFrame* parent = document.findFrame(frame->parentFrameId());
            rows.push_back(PropertyRow{"Hierarchy", "Parent",
                                       parent != nullptr ? parent->name() : std::string("(gone)"),
                                       "", false, kInvalidObjectId, 0.0});
        }
        return rows;
    }

    for (const Connector* connector : document.connectors()) {
        if (connector->id() != id) continue;
        rows.push_back(PropertyRow{"General", "Name", connector->name(), "", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"General", "Type", "Connector", "", false, kInvalidObjectId,
                                   0.0});
        rows.push_back(PropertyRow{"General", "Role", std::string(RoleName(connector->role())),
                                   "", false, kInvalidObjectId, 0.0});
        const ReferenceFrame* frame = document.findFrame(connector->frameId());
        rows.push_back(PropertyRow{"Frame", "On frame",
                                   frame != nullptr ? frame->name() : std::string("(gone)"), "",
                                   false, kInvalidObjectId, 0.0});
        const Transform3D world = document.connectorWorldTransform(connector->id());
        rows.push_back(PropertyRow{"World", "X", Number(world.translation.x), "mm", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"World", "Y", Number(world.translation.y), "mm", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(PropertyRow{"World", "Z", Number(world.translation.z), "mm", false,
                                   kInvalidObjectId, 0.0});
        return rows;
    }

    for (const auto& body : document.bodies()) {
        for (const auto& feature : body->features()) {
            if (feature->id() != id) continue;
            rows.push_back(NameRow(feature->id(), feature->name()));
            rows.push_back(PropertyRow{"General", "Type", std::string(feature->typeName()), "",
                                       false, kInvalidObjectId, 0.0});

            // EVERY editable number the feature exposes, ASKED OF THE FEATURE
            // (M26.9).
            //
            // This was four hand-written dynamic_casts -- Pad, Pocket,
            // Fillet/Chamfer, Revolve -- and every feature added after them
            // arrived without one. By M26 that was Hole, Shell, Draft and all
            // three patterns: their numbers were stored, solved, saved and
            // reloaded correctly, and there was nowhere in the shell to read or
            // change them. Nothing failed; the rows simply were not there.
            //
            // The row points at the PARAMETER's id, because the panel writes
            // through PartDocument's facade and never into the feature (UI
            // spec 20).
            if (const auto* parameterised =
                    dynamic_cast<const IParameterisedFeature*>(feature.get())) {
                for (const FeatureParameter& exposed : parameterised->featureParameters()) {
                    const Parameter* parameter =
                        document.parameters().findById(exposed.parameterId);
                    if (parameter == nullptr) continue;
                    rows.push_back(EditableValueRow("Geometry", exposed.label, *parameter));
                    // Which way it grows, for the numbers that HAVE another way.
                    // A pad from a sketch made ON A FACE grows away from the
                    // part, because that sketch's normal points out of it
                    // (ADR-M17-028), and a pocket from a face sketch MUST be
                    // reversed to cut into the material at all (ADR-M17-031) --
                    // so "the other way" is ordinary, not an edge case. A
                    // diameter and a count have no other way, and the box would
                    // mean nothing on them.
                    if (exposed.reversible) rows.push_back(ReversedRow("Geometry", *parameter));
                }
            }

            // The consumed base, for the features that chain off another --
            // read-only, because a chain reference is structure, not a value to
            // type over.
            if (const auto* pocket = dynamic_cast<const PocketFeature*>(feature.get())) {
                rows.push_back(PropertyRow{"Chain", "Base feature",
                                           std::to_string(pocket->baseFeatureId()), "", false,
                                           kInvalidObjectId, 0.0});
            }
            if (const auto* dress = dynamic_cast<const EdgeDressFeature*>(feature.get())) {
                rows.push_back(PropertyRow{"Chain", "Base feature",
                                           std::to_string(dress->baseFeatureId()), "", false,
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
        rows.push_back(NameRow(document.material()->id(), document.material()->name()));
        rows.push_back(PropertyRow{"Physical", "Density",
                                   Number(document.material()->density(), 1), "kg/m^3", false,
                                   kInvalidObjectId, 0.0});
        return rows;
    }

    return rows;
}

} // namespace paramcad
