#include "Viewer/DocumentOutline.h"

#include "Core/Body/Body.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Material/Material.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"

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

    const auto stateOf = [&graph](ObjectId id) {
        if (!graph.hasNode(id)) return OutlineState::Normal;
        return FromComputeState(graph.state(id));
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
        // A sketch has no recompute of its own, so the useful thing to surface
        // is whether its profile is usable -- which is the actual cause a user
        // needs when a Pad fails (UI spec 12: name the object AND the reason).
        const ProfileResult profile = BuildProfile(*sketch);
        if (!profile) {
            node.state = OutlineState::Failed;
            node.diagnostic = profile.message;
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
        const ProfileResult profile = BuildProfile(*sketch);
        rows.push_back(PropertyRow{"Profile", "Status",
                                   profile ? "Closed loop" : "Invalid", "", false,
                                   kInvalidObjectId, 0.0});
        if (!profile)
            rows.push_back(PropertyRow{"Profile", "Diagnostic", profile.message, "", false,
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
