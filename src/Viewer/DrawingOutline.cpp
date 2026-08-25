#include "Viewer/DrawingOutline.h"

#include <cstdio>
#include <string>

namespace paramcad {

namespace {

std::string Number(double value, int decimals = 1) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text = buffer;
    // The same no-negative-zero rule the other two panels follow: "-0.0" and
    // "0.0" beside each other are two numbers where the model has one.
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

OutlineNode Group(std::string name) {
    OutlineNode node;
    node.name = std::move(name);
    node.typeLabel = "Group";
    node.kind = OutlineKind::Other;
    node.state = OutlineState::Valid;
    return node;
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

OutlineNode DrawingOutline::build(const std::set<ObjectId>& hiddenIds) const {
    const DrawingDocument& document = *document_;

    OutlineNode root;
    root.id = document.id();
    root.name = document.name();
    root.typeLabel = "Drawing";
    root.kind = OutlineKind::Drawing;
    root.state = OutlineState::Valid;
    // THE PAPER, ON THE ROOT ROW. It is the one fact about a drawing a reader
    // needs before anything else -- what size is this and what scale is it at.
    root.diagnostic = std::string(toString(document.sheet().size())) + " " +
                      std::string(toString(document.sheet().orientation())) + ", " +
                      document.sheet().scale().toString() + ", " +
                      std::string(toString(document.sheet().projectionAngle())) + " angle";

    // --- Views ---------------------------------------------------------------
    //
    // NESTED, because a projected view IS under the one it came from: its
    // place is composed from its parent's, and a flat list would hide the
    // relationship that decides where half the drawing sits.
    OutlineNode views = Group("Views");
    const std::vector<ObjectId> stale = document.staleViews();
    const std::function<OutlineNode(const DrawingView&)> makeView =
        [&](const DrawingView& view) {
            OutlineNode node;
            node.id = view.id();
            node.name = view.name();
            node.typeLabel = std::string(toString(view.direction()));
            node.kind = OutlineKind::DrawingView;
            node.state = FromComputeState(view.currentState());
            if (node.state == OutlineState::Valid && hiddenIds.count(view.id()) != 0)
                node.state = OutlineState::Hidden;

            if (view.currentState() == ComputeState::Failed) {
                node.diagnostic = view.diagnostic();
            } else {
                bool behind = false;
                for (const ObjectId one : stale)
                    if (one == view.id()) behind = true;
                // "OUT OF DATE" IS NOT "FAILED". The view draws correctly; it
                // draws a part that has since changed, and the user's move is
                // to update rather than to fix something.
                node.diagnostic =
                    behind ? "out of date -- the model changed"
                           : std::to_string(view.projected().curves.size()) + " curves";
                if (view.hasOwnScale())
                    node.diagnostic += ", " + view.scale().toString();
            }

            for (const DrawingView* child : document.views())
                if (child->parentViewId() == view.id())
                    node.children.push_back(makeView(*child));
            return node;
        };
    for (const DrawingView* view : document.views())
        if (view->parentViewId() == kInvalidObjectId) views.children.push_back(makeView(*view));
    root.children.push_back(std::move(views));

    // --- Layers --------------------------------------------------------------
    OutlineNode layers = Group("Layers");
    for (const Layer* layer : document.layers()) {
        OutlineNode node;
        node.id = layer->id();
        node.name = layer->name();
        node.typeLabel = "Layer";
        node.kind = OutlineKind::Layer;
        node.state = layer->isVisible() ? OutlineState::Normal : OutlineState::Hidden;
        // WHICH ONE NEW GEOMETRY LANDS ON, and the three flags that do three
        // different things -- said in the row, because a layer manager the
        // user has to open to answer "where am I drawing" is a dialog that
        // should not have been needed.
        std::string what = layer->id() == document.currentLayerId() ? "current" : "";
        if (!layer->isOn()) what += (what.empty() ? "" : ", ") + std::string("off");
        if (layer->isFrozen()) what += (what.empty() ? "" : ", ") + std::string("frozen");
        if (layer->isLocked()) what += (what.empty() ? "" : ", ") + std::string("locked");
        if (layer->linetype() != kContinuousLinetypeName)
            what += (what.empty() ? "" : ", ") + layer->linetype();
        node.diagnostic = what;
        layers.children.push_back(std::move(node));
    }
    root.children.push_back(std::move(layers));

    // --- Linetypes -----------------------------------------------------------
    //
    // ONLY WHEN THERE IS MORE THAN THE ONE every drawing has. A group holding
    // CONTINUOUS and nothing else teaches a reader to skip it.
    if (document.linetypes().size() > 1) {
        OutlineNode linetypes = Group("Linetypes");
        for (const Linetype* linetype : document.linetypes()) {
            OutlineNode node;
            node.id = linetype->id();
            node.name = linetype->name();
            node.typeLabel = "Linetype";
            node.kind = OutlineKind::Linetype;
            node.state = OutlineState::Normal;
            node.diagnostic = linetype->isContinuous()
                                  ? "solid"
                                  : std::to_string(linetype->pattern().size()) + " segments";
            linetypes.children.push_back(std::move(node));
        }
        root.children.push_back(std::move(linetypes));
    }

    // THE VERDICT, once, on the drawing it is about -- the same rule the
    // assembly root follows rather than smearing it over every row.
    if (!stale.empty())
        root.state = OutlineState::Dirty;
    for (const DrawingView* view : document.views())
        if (view->currentState() == ComputeState::Failed) root.state = OutlineState::Failed;
    return root;
}

std::vector<PropertyRow> DrawingOutline::propertiesOf(ObjectId id) const {
    const DrawingDocument& document = *document_;
    std::vector<PropertyRow> rows;

    if (id == document.id()) {
        rows.push_back(ReadOnlyRow("Sheet", "Size", std::string(toString(document.sheet().size()))));
        rows.push_back(ReadOnlyRow("Sheet", "Orientation",
                                   std::string(toString(document.sheet().orientation()))));
        rows.push_back(ReadOnlyRow("Sheet", "Width", Number(document.sheet().widthMm()), "mm"));
        rows.push_back(ReadOnlyRow("Sheet", "Height", Number(document.sheet().heightMm()), "mm"));
        rows.push_back(ReadOnlyRow("Sheet", "Scale", document.sheet().scale().toString()));
        rows.push_back(ReadOnlyRow("Sheet", "Projection",
                                   std::string(toString(document.sheet().projectionAngle())) +
                                       " angle"));
        return rows;
    }

    if (const DrawingView* view = document.findView(id)) {
        rows.push_back(PropertyRow{"General", "Name", view->name(), "", true, view->id(), 0.0,
                                   PropertyField::Name});
        rows.push_back(ReadOnlyRow("General", "Direction",
                                   std::string(toString(view->direction()))));
        // THE SENTENCE, not the geometry (ADR-M22-003). What a view stores is
        // a path re-read every rebuild, so the path is what a user needs to
        // see when it stops resolving.
        rows.push_back(ReadOnlyRow("Source", "File", view->sourcePath()));
        if (!view->bodyName().empty())
            rows.push_back(ReadOnlyRow("Source", "Body", view->bodyName()));

        const Vec2 at = document.viewPositionMm(view->id());
        rows.push_back(ReadOnlyRow("Placement", "X", Number(at.x), "mm"));
        rows.push_back(ReadOnlyRow("Placement", "Y", Number(at.y), "mm"));
        if (view->parentViewId() != kInvalidObjectId) {
            const DrawingView* parent = document.findView(view->parentViewId());
            rows.push_back(ReadOnlyRow("Placement", "Projected from",
                                       parent != nullptr ? parent->name() : "(missing)"));
            rows.push_back(ReadOnlyRow("Placement", "Offset",
                                       Number(view->alignmentOffsetMm()), "mm"));
        }

        rows.push_back(ReadOnlyRow("Drawing", "Scale",
                                   view->effectiveScale(document.sheet().scale()).toString() +
                                       (view->hasOwnScale() ? "" : " (sheet)")));
        rows.push_back(ReadOnlyRow("Drawing", "Hidden lines",
                                   view->showsHiddenLines() ? "shown" : "off"));
        rows.push_back(ReadOnlyRow("Drawing", "Tangent edges",
                                   view->showsTangentEdges() ? "shown" : "off"));

        // THE MEASURED SIZE OF THE PART, in model millimetres -- what a
        // dimension will read. Beside the paper footprint, so the difference
        // between the two is visible rather than something to work out.
        rows.push_back(ReadOnlyRow("Extent", "Model width",
                                   Number(view->projected().extent.widthMm()), "mm"));
        rows.push_back(ReadOnlyRow("Extent", "Model height",
                                   Number(view->projected().extent.heightMm()), "mm"));
        rows.push_back(ReadOnlyRow("Extent", "On paper",
                                   Number(view->paperWidthMm(document.sheet().scale())) + " x " +
                                       Number(view->paperHeightMm(document.sheet().scale())),
                                   "mm"));
        return rows;
    }

    if (const Layer* layer = document.findLayer(id)) {
        rows.push_back(PropertyRow{"General", "Name", layer->name(), "", true, layer->id(), 0.0,
                                   PropertyField::Name});
        rows.push_back(ReadOnlyRow("General", "Colour", std::to_string(layer->color()) + " (ACI)"));
        rows.push_back(ReadOnlyRow("General", "Linetype", layer->linetype()));
        rows.push_back(ReadOnlyRow("State", "On", layer->isOn() ? "yes" : "no"));
        rows.push_back(ReadOnlyRow("State", "Frozen", layer->isFrozen() ? "yes" : "no"));
        rows.push_back(ReadOnlyRow("State", "Locked", layer->isLocked() ? "yes" : "no"));
        rows.push_back(ReadOnlyRow("State", "Current",
                                   layer->id() == document.currentLayerId() ? "yes" : "no"));
        rows.push_back(ReadOnlyRow("Plot", "Lineweight",
                                   layer->lineweight() < 0
                                       ? std::string("by default")
                                       : Number(layer->lineweight() / 100.0, 2) + " mm"));
        return rows;
    }

    if (const Linetype* linetype = document.findLinetype(id)) {
        rows.push_back(PropertyRow{"General", "Name", linetype->name(), "", true, linetype->id(),
                                   0.0, PropertyField::Name});
        rows.push_back(ReadOnlyRow("General", "Description", linetype->description()));
        std::string pattern;
        for (const double segment : linetype->pattern())
            pattern += (pattern.empty() ? "" : ", ") + Number(segment, 2);
        rows.push_back(ReadOnlyRow("Pattern", "Segments",
                                   pattern.empty() ? "solid" : pattern));
        return rows;
    }

    return rows;
}

} // namespace paramcad
