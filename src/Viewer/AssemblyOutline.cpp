#include "Viewer/AssemblyOutline.h"

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/AssemblyStates.h"
#include "Core/Assembly/Instance.h"
#include "Core/Assembly/Mate.h"
#include "Core/Assembly/Relation.h"

#include <cstdio>
#include <string>

namespace paramcad {

namespace {

std::string Number(double value, int decimals = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text = buffer;
    // The same no-negative-zero rule the part panel follows: a panel showing
    // "-0.000 mm" beside "0.000 mm" shows two numbers where the model has one.
    if (text.size() > 1 && text[0] == '-' &&
        text.find_first_of("123456789") == std::string::npos)
        text.erase(0, 1);
    return text;
}

PropertyRow ReadOnlyRow(std::string group, std::string label, std::string value,
                        std::string unit = std::string()) {
    return PropertyRow{std::move(group), std::move(label), std::move(value), std::move(unit),
                       false, kInvalidObjectId, 0.0, PropertyField::None};
}

OutlineState FromComputeState(ComputeState state) noexcept {
    switch (state) {
        case ComputeState::Valid: return OutlineState::Valid;
        case ComputeState::Failed: return OutlineState::Failed;
        case ComputeState::Suppressed: return OutlineState::Suppressed;
        default: return OutlineState::Dirty;
    }
}

// The ratio as a sentence, in the unit the type gives it.
//
// SHARED by the tree row and the property panel, because those two saying
// different things about the same number is exactly the seam this project
// keeps closing -- and it is `Relation::valueFor` that decides the meaning,
// so the wording follows that and nothing else.
std::string RatioSentence(const Relation& relation) {
    const bool perTurn = relation.type() == RelationType::Screw ||
                         relation.type() == RelationType::RackAndPinion;
    std::string text = Number(relation.ratio());
    text += perTurn ? " mm per turn" : " : 1";
    if (relation.reversed()) text += ", reversed";
    return text;
}

OutlineNode Group(std::string name) {
    OutlineNode node;
    node.name = std::move(name);
    node.typeLabel = "Group";
    node.kind = OutlineKind::Other;
    node.state = OutlineState::Valid;
    return node;
}

} // namespace

OutlineNode AssemblyOutline::build(const std::set<ObjectId>& hiddenIds) const {
    const AssemblyDocument& document = *document_;

    OutlineNode root;
    root.id = document.id();
    root.name = document.name();
    root.typeLabel = "Assembly";
    root.kind = OutlineKind::Assembly;
    root.state = OutlineState::Valid;

    const AssemblyDocument::MateSolveReport& report = document.mateSolveReport();

    // --- Instances ----------------------------------------------------------
    OutlineNode instances = Group("Instances");
    for (const Instance* instance : document.instances()) {
        OutlineNode node;
        node.id = instance->id();
        node.name = instance->name();
        node.typeLabel = instance->isSubAssembly() ? "Sub-assembly" : "Instance";
        node.kind = OutlineKind::Instance;
        node.state = FromComputeState(instance->currentState());
        // Hidden is reported only when the instance is otherwise fine:
        // "hidden" must never mask "failed" (UI spec 11).
        if (node.state == OutlineState::Valid && hiddenIds.count(instance->id()) != 0)
            node.state = OutlineState::Hidden;

        // WHAT HOLDS IT, in the row itself. Roadmap §20.3 asks for degrees of
        // freedom PER INSTANCE and says why: "this assembly is
        // under-constrained" is not something a user can act on.
        if (document.isInstanceGrounded(instance->id())) {
            node.diagnostic = "grounded";
        } else {
            for (const auto& freedom : report.freedoms) {
                if (freedom.instanceId != instance->id()) continue;
                const int left = freedom.rotational + freedom.translational;
                node.diagnostic = left == 0
                                      ? std::string("fully constrained")
                                      : std::to_string(left) + " DOF (" +
                                            std::to_string(freedom.translational) + "T + " +
                                            std::to_string(freedom.rotational) + "R)";
                if (!freedom.describedBy.empty())
                    node.diagnostic += " -- by " + freedom.describedBy;
            }
        }
        instances.children.push_back(std::move(node));
    }
    root.children.push_back(std::move(instances));

    // --- Mates --------------------------------------------------------------
    OutlineNode mates = Group("Mates");
    for (const Mate* mate : document.mates()) {
        OutlineNode node;
        node.id = mate->id();
        node.name = mate->name();
        node.typeLabel = std::string(toString(mate->type()));
        node.kind = OutlineKind::Mate;
        // A MATE HAS NO COMPUTE STATE of its own -- it is not a graph node. What
        // a user needs is whether the SOLVE failed, and that belongs to the
        // whole assembly rather than to one mate, so it is reported once on the
        // root rather than smeared over every row (the mistake the part tree
        // made with constraints, fixed in M26.10).
        node.state = OutlineState::Normal;
        if (mate->isDriven()) node.diagnostic = "driven";
        mates.children.push_back(std::move(node));
    }
    root.children.push_back(std::move(mates));

    // --- Relations (§20.5) ----------------------------------------------------
    //
    // ONLY WHEN THERE ARE ANY, like the two groups below: an empty group in
    // every assembly ever made is a row that teaches a reader to skip it.
    if (!document.relations().empty()) {
        OutlineNode relations = Group("Relations");
        for (const Relation* relation : document.relations()) {
            OutlineNode node;
            node.id = relation->id();
            node.name = relation->name();
            node.typeLabel = std::string(toString(relation->type()));
            node.kind = OutlineKind::Relation;
            node.state = OutlineState::Normal;
            // WHAT IT DOES, in the unit the user typed it in -- the ratio
            // alone reads as a pure number, and "4" on a screw is 4 mm per
            // turn, not four of anything else.
            node.diagnostic = RatioSentence(*relation);
            relations.children.push_back(std::move(node));
        }
        root.children.push_back(std::move(relations));
    }

    // --- Named positions (§49) ----------------------------------------------
    if (!document.namedPositions().empty()) {
        OutlineNode positions = Group("Named positions");
        for (const NamedPosition* position : document.namedPositions()) {
            OutlineNode node;
            node.id = position->id();
            node.name = position->name();
            node.typeLabel = "Named position";
            node.kind = OutlineKind::NamedPosition;
            node.state = OutlineState::Normal;
            positions.children.push_back(std::move(node));
        }
        root.children.push_back(std::move(positions));
    }

    // --- Exploded views (§49) -----------------------------------------------
    if (!document.explodeViews().empty()) {
        OutlineNode views = Group("Exploded views");
        for (const ExplodeView* view : document.explodeViews()) {
            OutlineNode node;
            node.id = view->id();
            node.name = view->name();
            node.typeLabel = "Exploded view";
            node.kind = OutlineKind::ExplodeView;
            node.state = OutlineState::Normal;
            node.diagnostic = std::to_string(view->steps().size()) + " step" +
                              (view->steps().size() == 1 ? "" : "s");
            views.children.push_back(std::move(node));
        }
        root.children.push_back(std::move(views));
    }

    // THE SOLVE'S VERDICT, once, on the assembly it is about.
    if (!report.ok) {
        root.state = OutlineState::Failed;
        root.diagnostic = report.message;
    }
    return root;
}

std::vector<PropertyRow> AssemblyOutline::propertiesOf(ObjectId id) const {
    const AssemblyDocument& document = *document_;
    std::vector<PropertyRow> rows;

    if (const Instance* instance = document.findInstance(id)) {
        rows.push_back(PropertyRow{"General", "Name", instance->name(), "", true, instance->id(),
                                   0.0, PropertyField::Name});
        rows.push_back(ReadOnlyRow("General", "Type",
                                   instance->isSubAssembly() ? "Sub-assembly" : "Part instance"));
        // THE SENTENCE, not the geometry (ADR-M22-003). What an instance stores
        // is a path re-read every rebuild, so the path is what a user needs to
        // see when it stops resolving.
        rows.push_back(ReadOnlyRow("Source", "File", instance->sourcePath()));
        if (!instance->bodyName().empty())
            rows.push_back(ReadOnlyRow("Source", "Body", instance->bodyName()));

        const Transform3D placement = document.instanceWorldTransform(instance->id());
        rows.push_back(ReadOnlyRow("Placement", "X", Number(placement.translation.x), "mm"));
        rows.push_back(ReadOnlyRow("Placement", "Y", Number(placement.translation.y), "mm"));
        rows.push_back(ReadOnlyRow("Placement", "Z", Number(placement.translation.z), "mm"));

        rows.push_back(ReadOnlyRow("Mates", "Grounded",
                                   document.isInstanceGrounded(instance->id()) ? "yes" : "no"));
        // PER INSTANCE, split into rotational and translational -- §20.3 asks
        // for both, because "3 DOF" does not say whether it can turn or slide.
        for (const auto& freedom : document.mateSolveReport().freedoms) {
            if (freedom.instanceId != instance->id()) continue;
            rows.push_back(
                ReadOnlyRow("Mates", "Free to move", std::to_string(freedom.translational)));
            rows.push_back(
                ReadOnlyRow("Mates", "Free to turn", std::to_string(freedom.rotational)));
            if (!freedom.describedBy.empty())
                rows.push_back(ReadOnlyRow("Mates", "Held by", freedom.describedBy));
        }
        rows.push_back(
            ReadOnlyRow("Connectors", "On this", std::to_string(instance->connectors().size())));
        return rows;
    }

    if (const Mate* mate = document.findMate(id)) {
        rows.push_back(PropertyRow{"General", "Name", mate->name(), "", true, mate->id(), 0.0,
                                   PropertyField::Name});
        rows.push_back(ReadOnlyRow("General", "Type", std::string(toString(mate->type()))));
        rows.push_back(ReadOnlyRow("General", "Driven", mate->isDriven() ? "yes" : "no"));

        const auto describeEnd = [&](const char* label, ObjectId instanceId,
                                     const std::string& connector) {
            const Instance* end = document.findInstance(instanceId);
            rows.push_back(ReadOnlyRow(
                "Connects", label,
                (end != nullptr ? end->name() : std::string("(missing)")) + " / " + connector));
        };
        describeEnd("From", mate->leadingInstanceId(), mate->leadingConnector());
        describeEnd("To", mate->followingInstanceId(), mate->followingConnector());
        return rows;
    }

    if (const Relation* relation = document.findRelation(id)) {
        rows.push_back(PropertyRow{"General", "Name", relation->name(), "", true, relation->id(),
                                   0.0, PropertyField::Name});
        rows.push_back(ReadOnlyRow("General", "Type", std::string(toString(relation->type()))));

        // THE TWO ENDS, each named by its mate and the freedom on it. A
        // relation whose panel showed only a ratio would leave the user unable
        // to tell a gear from the gear next to it.
        const auto describeEnd = [&](const char* label, const CoupledFreedom& end) {
            const Mate* mate = document.findMate(end.mateId);
            rows.push_back(ReadOnlyRow(
                "Couples", label,
                (mate != nullptr ? mate->name() : std::string("(missing)")) + " -- " +
                    (IsRotation(end.component) ? "turn " : "slide ") +
                    std::string(toString(end.component))));
        };
        describeEnd("Driven by", relation->driver());
        describeEnd("Drives", relation->driven());

        // THE UNIT IS PART OF THE NUMBER. A gear's ratio is turns per turn and
        // a screw's is millimetres per turn; a panel that showed "4.000" for
        // both would be a panel that means two things by one row.
        const bool perTurn = relation->type() == RelationType::Screw ||
                             relation->type() == RelationType::RackAndPinion;
        rows.push_back(ReadOnlyRow("Coupling", "Ratio", Number(relation->ratio()),
                                   perTurn ? "mm per turn" : "per turn"));
        rows.push_back(ReadOnlyRow("Coupling", "Direction",
                                   relation->reversed() ? "reversed" : "same way"));
        return rows;
    }

    if (const NamedPosition* position = document.findNamedPosition(id)) {
        rows.push_back(PropertyRow{"General", "Name", position->name(), "", true, position->id(),
                                   0.0, PropertyField::Name});
        rows.push_back(ReadOnlyRow("General", "Type", "Named position"));
        return rows;
    }

    if (const ExplodeView* view = document.findExplodeView(id)) {
        rows.push_back(PropertyRow{"General", "Name", view->name(), "", true, view->id(), 0.0,
                                   PropertyField::Name});
        rows.push_back(ReadOnlyRow("General", "Type", "Exploded view"));
        rows.push_back(ReadOnlyRow("Steps", "Count", std::to_string(view->steps().size())));
        return rows;
    }

    return rows;
}

} // namespace paramcad
