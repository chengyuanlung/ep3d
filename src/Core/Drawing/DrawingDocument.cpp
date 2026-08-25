#include "Core/Drawing/DrawingDocument.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Undo/UndoRecord.h"

#include <stdexcept>
#include <utility>

namespace paramcad {

namespace {

// The sheet as a delta's worth of fields, so `before` and `after` are built
// the same way and cannot drift apart.
struct SheetSnapshot {
    int size;
    int orientation;
    int numerator;
    int denominator;
    double widthMm;
    double heightMm;
};

SheetSnapshot SnapshotOf(const Sheet& sheet) {
    return SheetSnapshot{static_cast<int>(sheet.size()),
                         static_cast<int>(sheet.orientation()),
                         sheet.scale().numerator,
                         sheet.scale().denominator,
                         sheet.widthMm(),
                         sheet.heightMm()};
}

} // namespace

DrawingDocument::DrawingDocument(std::string name) : DocumentBase(std::move(name)) {
    createOriginFrame();
    seedTables();
}

DrawingDocument::DrawingDocument(ObjectId id, std::string name)
    : DocumentBase(id, std::move(name)) {
    createOriginFrame();
    seedTables();
}

void DrawingDocument::seedTables() {
    // CONSTRUCTION, NOT AN EDIT (ADR-M9-001). A freshly opened drawing must
    // have nothing to undo -- otherwise "Undo" on a new file deletes the layer
    // everything is about to be drawn on.
    auto continuous = std::make_unique<Linetype>(kContinuousLinetypeName,
                                                 "Solid line", std::vector<double>{});
    registry_.registerObject(continuous->id(), continuous.get());
    linetypes_.push_back(std::move(continuous));

    auto zero = std::make_unique<Layer>(kDefaultLayerName, 7, kContinuousLinetypeName);
    currentLayerId_ = zero->id();
    registry_.registerObject(zero->id(), zero.get());
    layers_.push_back(std::move(zero));
}

// =============================================================================
// The paper
// =============================================================================

namespace {
// One recorder for every sheet change, so a size edit and a scale edit record
// the same shape of delta and undo cannot half-restore one of them.
} // namespace

bool DrawingDocument::setSheetSize(SheetSize size) {
    const SheetSnapshot before = SnapshotOf(sheet_);
    sheet_.setSize(size);
    const SheetSnapshot after = SnapshotOf(sheet_);
    SheetEdit edit;
    edit.beforeSize = before.size;
    edit.afterSize = after.size;
    edit.beforeOrientation = before.orientation;
    edit.afterOrientation = after.orientation;
    edit.beforeScaleNumerator = before.numerator;
    edit.beforeScaleDenominator = before.denominator;
    edit.afterScaleNumerator = after.numerator;
    edit.afterScaleDenominator = after.denominator;
    edit.beforeWidthMm = before.widthMm;
    edit.beforeHeightMm = before.heightMm;
    edit.afterWidthMm = after.widthMm;
    edit.afterHeightMm = after.heightMm;
    recordDelta(edit, "Sheet size");
    return true;
}

bool DrawingDocument::setSheetOrientation(SheetOrientation orientation) {
    const SheetSnapshot before = SnapshotOf(sheet_);
    sheet_.setOrientation(orientation);
    const SheetSnapshot after = SnapshotOf(sheet_);
    SheetEdit edit;
    edit.beforeSize = before.size;
    edit.afterSize = after.size;
    edit.beforeOrientation = before.orientation;
    edit.afterOrientation = after.orientation;
    edit.beforeScaleNumerator = before.numerator;
    edit.beforeScaleDenominator = before.denominator;
    edit.afterScaleNumerator = after.numerator;
    edit.afterScaleDenominator = after.denominator;
    edit.beforeWidthMm = before.widthMm;
    edit.beforeHeightMm = before.heightMm;
    edit.afterWidthMm = after.widthMm;
    edit.afterHeightMm = after.heightMm;
    recordDelta(edit, "Sheet orientation");
    return true;
}

bool DrawingDocument::setSheetScale(const DrawingScale& scale) {
    if (!scale.valid()) return false;
    const SheetSnapshot before = SnapshotOf(sheet_);
    sheet_.setScale(scale);
    const SheetSnapshot after = SnapshotOf(sheet_);
    SheetEdit edit;
    edit.beforeSize = before.size;
    edit.afterSize = after.size;
    edit.beforeOrientation = before.orientation;
    edit.afterOrientation = after.orientation;
    edit.beforeScaleNumerator = before.numerator;
    edit.beforeScaleDenominator = before.denominator;
    edit.afterScaleNumerator = after.numerator;
    edit.afterScaleDenominator = after.denominator;
    edit.beforeWidthMm = before.widthMm;
    edit.beforeHeightMm = before.heightMm;
    edit.afterWidthMm = after.widthMm;
    edit.afterHeightMm = after.heightMm;
    recordDelta(edit, "Sheet scale");
    // EVERY VIEW IS DRAWN AT A SCALE, and the ones with no opinion of their
    // own just changed. Dirtied so the projection is redone at the new size
    // rather than left as a picture at the old one.
    for (const std::unique_ptr<DrawingView>& view : views_)
        if (!view->hasOwnScale()) graph_.markDirty(view->id());
    return true;
}

bool DrawingDocument::setSheetCustomSize(double widthMm, double heightMm) {
    const SheetSnapshot before = SnapshotOf(sheet_);
    if (!sheet_.setCustomSize(widthMm, heightMm)) return false;
    const SheetSnapshot after = SnapshotOf(sheet_);
    SheetEdit edit;
    edit.beforeSize = before.size;
    edit.afterSize = after.size;
    edit.beforeOrientation = before.orientation;
    edit.afterOrientation = after.orientation;
    edit.beforeScaleNumerator = before.numerator;
    edit.beforeScaleDenominator = before.denominator;
    edit.afterScaleNumerator = after.numerator;
    edit.afterScaleDenominator = after.denominator;
    edit.beforeWidthMm = before.widthMm;
    edit.beforeHeightMm = before.heightMm;
    edit.afterWidthMm = after.widthMm;
    edit.afterHeightMm = after.heightMm;
    recordDelta(edit, "Sheet size");
    return true;
}

void DrawingDocument::restoreSheet(SheetSize size, SheetOrientation orientation,
                                   DrawingScale scale, double customWidthMm,
                                   double customHeightMm) {
    // ORDER MATTERS: setCustomSize sets the size to Custom as a side effect,
    // so it runs FIRST and the explicit size wins afterwards. Written down
    // because the other order silently turns every A3 sheet into a custom one.
    if (size == SheetSize::Custom) sheet_.setCustomSize(customWidthMm, customHeightMm);
    sheet_.setSize(size);
    sheet_.setOrientation(orientation);
    sheet_.setScale(scale);
}

// =============================================================================
// Layers
// =============================================================================

Layer& DrawingDocument::addLayer(std::string name, int color, std::string linetype) {
    if (name.empty()) throw std::invalid_argument("addLayer: a layer needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addLayer: '" + name + "' is already taken");
    // A LINETYPE THAT IS NOT IN THE TABLE is a layer that cannot be written to
    // DXF and cannot be drawn. Refused here, where the name is still in the
    // caller's hand.
    if (findLinetypeNamed(linetype) == nullptr)
        throw std::invalid_argument("addLayer: there is no linetype called '" + linetype + "'");

    auto item = std::make_unique<Layer>(std::move(name), color, std::move(linetype));
    auto& ref = *item;
    layers_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    LayerExistenceEdit edit;
    edit.layerId = ref.id();
    edit.name = ref.name();
    edit.color = ref.color();
    edit.linetype = ref.linetype();
    edit.on = ref.isOn();
    edit.frozen = ref.isFrozen();
    edit.locked = ref.isLocked();
    edit.lineweight = ref.lineweight();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add layer " + ref.name());
    return ref;
}

Layer& DrawingDocument::restoreLayer(ObjectId id, std::string name, int color,
                                     std::string linetype, bool on, bool frozen, bool locked,
                                     int lineweight) {
    requireUnusedId(id, "restoreLayer");
    auto item = std::make_unique<Layer>(id, std::move(name), color, std::move(linetype), on,
                                        frozen, locked, lineweight);
    auto& ref = *item;
    layers_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const Layer*> DrawingDocument::layers() const {
    std::vector<const Layer*> all;
    all.reserve(layers_.size());
    for (const std::unique_ptr<Layer>& one : layers_) all.push_back(one.get());
    return all;
}

const Layer* DrawingDocument::findLayer(ObjectId id) const noexcept {
    for (const std::unique_ptr<Layer>& one : layers_)
        if (one->id() == id) return one.get();
    return nullptr;
}

Layer* DrawingDocument::findLayerForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<Layer>& one : layers_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const Layer* DrawingDocument::findLayerNamed(const std::string& name) const noexcept {
    for (const std::unique_ptr<Layer>& one : layers_)
        if (one->name() == name) return one.get();
    return nullptr;
}

bool DrawingDocument::setCurrentLayer(ObjectId layerId) {
    const Layer* layer = findLayer(layerId);
    if (layer == nullptr) return false;
    // A FROZEN OR LOCKED LAYER CANNOT BE THE CURRENT ONE. New geometry would
    // land somewhere invisible or unselectable, and the user would draw a line
    // that appears not to have been drawn.
    if (!layer->isVisible() || layer->isLocked()) return false;
    if (currentLayerId_ == layerId) return true;

    CurrentLayerEdit edit;
    edit.before = currentLayerId_;
    edit.after = layerId;
    currentLayerId_ = layerId;
    recordDelta(edit, "Current layer " + layer->name());
    return true;
}

void DrawingDocument::restoreCurrentLayer(ObjectId layerId) {
    if (findLayer(layerId) != nullptr) currentLayerId_ = layerId;
}

namespace {
// Fills the before-half from what the layer currently is. One place, so six
// setters cannot each forget a different field.
void SnapshotLayerInto(const Layer& layer, LayerPropertyEdit& edit) {
    edit.layerId = layer.id();
    edit.beforeColor = layer.color();
    edit.afterColor = layer.color();
    edit.beforeLinetype = layer.linetype();
    edit.afterLinetype = layer.linetype();
    edit.beforeOn = layer.isOn();
    edit.afterOn = layer.isOn();
    edit.beforeFrozen = layer.isFrozen();
    edit.afterFrozen = layer.isFrozen();
    edit.beforeLocked = layer.isLocked();
    edit.afterLocked = layer.isLocked();
    edit.beforeLineweight = layer.lineweight();
    edit.afterLineweight = layer.lineweight();
}
} // namespace

bool DrawingDocument::setLayerColor(ObjectId layerId, int color) {
    Layer* layer = findLayerForEdit(layerId);
    if (layer == nullptr) return false;
    LayerPropertyEdit edit;
    SnapshotLayerInto(*layer, edit);
    edit.afterColor = color;
    layer->setColor(color);
    recordDelta(edit, "Colour of " + layer->name());
    return true;
}

bool DrawingDocument::setLayerLinetype(ObjectId layerId, std::string linetype) {
    Layer* layer = findLayerForEdit(layerId);
    if (layer == nullptr) return false;
    if (findLinetypeNamed(linetype) == nullptr) return false;
    LayerPropertyEdit edit;
    SnapshotLayerInto(*layer, edit);
    edit.afterLinetype = linetype;
    layer->setLinetype(std::move(linetype));
    recordDelta(edit, "Linetype of " + layer->name());
    return true;
}

bool DrawingDocument::setLayerOn(ObjectId layerId, bool on) {
    Layer* layer = findLayerForEdit(layerId);
    if (layer == nullptr) return false;
    LayerPropertyEdit edit;
    SnapshotLayerInto(*layer, edit);
    edit.afterOn = on;
    layer->setOn(on);
    recordDelta(edit, (on ? "Show " : "Hide ") + layer->name());
    return true;
}

bool DrawingDocument::setLayerFrozen(ObjectId layerId, bool frozen) {
    Layer* layer = findLayerForEdit(layerId);
    if (layer == nullptr) return false;
    LayerPropertyEdit edit;
    SnapshotLayerInto(*layer, edit);
    edit.afterFrozen = frozen;
    layer->setFrozen(frozen);
    recordDelta(edit, (frozen ? "Freeze " : "Thaw ") + layer->name());
    return true;
}

bool DrawingDocument::setLayerLocked(ObjectId layerId, bool locked) {
    Layer* layer = findLayerForEdit(layerId);
    if (layer == nullptr) return false;
    LayerPropertyEdit edit;
    SnapshotLayerInto(*layer, edit);
    edit.afterLocked = locked;
    layer->setLocked(locked);
    recordDelta(edit, (locked ? "Lock " : "Unlock ") + layer->name());
    return true;
}

bool DrawingDocument::setLayerLineweight(ObjectId layerId, int lineweight) {
    Layer* layer = findLayerForEdit(layerId);
    if (layer == nullptr) return false;
    LayerPropertyEdit edit;
    SnapshotLayerInto(*layer, edit);
    edit.afterLineweight = lineweight;
    layer->setLineweight(lineweight);
    recordDelta(edit, "Lineweight of " + layer->name());
    return true;
}

// =============================================================================
// Linetypes
// =============================================================================

Linetype& DrawingDocument::addLinetype(std::string name, std::string description,
                                       std::vector<double> pattern) {
    if (name.empty()) throw std::invalid_argument("addLinetype: a linetype needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addLinetype: '" + name + "' is already taken");

    auto item = std::make_unique<Linetype>(std::move(name), std::move(description),
                                           std::move(pattern));
    auto& ref = *item;
    linetypes_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    LinetypeExistenceEdit edit;
    edit.linetypeId = ref.id();
    edit.name = ref.name();
    edit.description = ref.description();
    edit.pattern = ref.pattern();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add linetype " + ref.name());
    return ref;
}

Linetype& DrawingDocument::restoreLinetype(ObjectId id, std::string name,
                                           std::string description,
                                           std::vector<double> pattern) {
    requireUnusedId(id, "restoreLinetype");
    auto item = std::make_unique<Linetype>(id, std::move(name), std::move(description),
                                           std::move(pattern));
    auto& ref = *item;
    linetypes_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const Linetype*> DrawingDocument::linetypes() const {
    std::vector<const Linetype*> all;
    all.reserve(linetypes_.size());
    for (const std::unique_ptr<Linetype>& one : linetypes_) all.push_back(one.get());
    return all;
}

const Linetype* DrawingDocument::findLinetype(ObjectId id) const noexcept {
    for (const std::unique_ptr<Linetype>& one : linetypes_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const Linetype* DrawingDocument::findLinetypeNamed(const std::string& name) const noexcept {
    for (const std::unique_ptr<Linetype>& one : linetypes_)
        if (one->name() == name) return one.get();
    return nullptr;
}

// =============================================================================
// Views
// =============================================================================

std::string DrawingDocument::whyViewCannotSitAt(Vec2 positionMm) const {
    if (positionMm.x < 0.0 || positionMm.y < 0.0)
        return "a view cannot sit off the left or bottom edge of the sheet";
    if (positionMm.x > sheet_.widthMm() || positionMm.y > sheet_.heightMm())
        return "a view cannot sit off the right or top edge of the sheet";
    return {};
}

DrawingView& DrawingDocument::addView(std::string name, std::string sourcePath,
                                      std::string bodyName, ViewDirection direction,
                                      Vec2 positionMm) {
    if (name.empty()) throw std::invalid_argument("addView: a view needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addView: '" + name + "' is already taken");
    // THE SAME REFUSAL AN INSTANCE GETS (ADR-M22-003): a reference that names
    // no file can never resolve, so it is refused at the door rather than at
    // recompute time, a long way from the cause.
    if (sourcePath.empty())
        throw std::invalid_argument("addView: a view names no model file");
    if (const std::string why = whyViewCannotSitAt(positionMm); !why.empty())
        throw std::invalid_argument("addView: " + why);

    auto item = std::make_unique<DrawingView>(std::move(name), std::move(sourcePath),
                                              std::move(bodyName), direction, positionMm);
    auto& ref = *item;
    views_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    addRecomputableNode(ref);

    DrawingViewExistenceEdit edit;
    edit.viewId = ref.id();
    edit.name = ref.name();
    edit.sourcePath = ref.sourcePath();
    edit.bodyName = ref.bodyName();
    edit.direction = static_cast<int>(ref.direction());
    edit.positionXMm = ref.positionMm().x;
    edit.positionYMm = ref.positionMm().y;
    edit.scaleNumerator = ref.scale().numerator;
    edit.scaleDenominator = ref.scale().denominator;
    edit.ownScale = ref.hasOwnScale();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add view " + ref.name());
    return ref;
}

DrawingView& DrawingDocument::restoreView(ObjectId id, std::string name, ComputeState state,
                                          std::string sourcePath, std::string bodyName,
                                          ViewDirection direction, Vec2 positionMm,
                                          DrawingScale scale, bool ownScale) {
    requireUnusedId(id, "restoreView");
    auto item = std::make_unique<DrawingView>(id, std::move(name), state, std::move(sourcePath),
                                              std::move(bodyName), direction, positionMm, scale,
                                              ownScale);
    auto& ref = *item;
    views_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    addRecomputableNode(ref);
    return ref;
}

std::vector<const DrawingView*> DrawingDocument::views() const {
    std::vector<const DrawingView*> all;
    all.reserve(views_.size());
    for (const std::unique_ptr<DrawingView>& one : views_) all.push_back(one.get());
    return all;
}

const DrawingView* DrawingDocument::findView(ObjectId id) const noexcept {
    for (const std::unique_ptr<DrawingView>& one : views_)
        if (one->id() == id) return one.get();
    return nullptr;
}

DrawingView* DrawingDocument::findViewForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<DrawingView>& one : views_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const DrawingView* DrawingDocument::findViewNamed(const std::string& name) const noexcept {
    for (const std::unique_ptr<DrawingView>& one : views_)
        if (one->name() == name) return one.get();
    return nullptr;
}

namespace {
void SnapshotViewInto(const DrawingView& view, DrawingViewPlacementEdit& edit) {
    edit.viewId = view.id();
    edit.beforeXMm = view.positionMm().x;
    edit.afterXMm = view.positionMm().x;
    edit.beforeYMm = view.positionMm().y;
    edit.afterYMm = view.positionMm().y;
    edit.beforeDirection = static_cast<int>(view.direction());
    edit.afterDirection = static_cast<int>(view.direction());
    edit.beforeScaleNumerator = view.scale().numerator;
    edit.afterScaleNumerator = view.scale().numerator;
    edit.beforeScaleDenominator = view.scale().denominator;
    edit.afterScaleDenominator = view.scale().denominator;
    edit.beforeOwnScale = view.hasOwnScale();
    edit.afterOwnScale = view.hasOwnScale();
}
} // namespace

bool DrawingDocument::setViewPosition(ObjectId viewId, Vec2 positionMm) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    if (!whyViewCannotSitAt(positionMm).empty()) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterXMm = positionMm.x;
    edit.afterYMm = positionMm.y;
    view->setPositionMm(positionMm);
    recordDelta(edit, "Move " + view->name());
    // MOVING A VIEW DOES NOT REPROJECT IT. Where it sits on the paper and
    // what it looks like are two different questions, and dirtying here would
    // make a drag re-run the hidden-line solve on every mouse move.
    return true;
}

bool DrawingDocument::setViewDirection(ObjectId viewId, ViewDirection direction) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterDirection = static_cast<int>(direction);
    view->setDirection(direction);
    recordDelta(edit, "Turn " + view->name());
    graph_.markDirty(viewId);
    return true;
}

bool DrawingDocument::setViewScale(ObjectId viewId, const DrawingScale& scale) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr || !scale.valid()) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterScaleNumerator = scale.numerator;
    edit.afterScaleDenominator = scale.denominator;
    edit.afterOwnScale = true;
    view->setScale(scale);
    recordDelta(edit, "Scale " + view->name());
    graph_.markDirty(viewId);
    return true;
}

bool DrawingDocument::clearViewScale(ObjectId viewId) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterOwnScale = false;
    view->clearScale();
    recordDelta(edit, view->name() + " follows the sheet");
    graph_.markDirty(viewId);
    return true;
}

// =============================================================================
// What this document owes DocumentBase
// =============================================================================

DocumentRecomputeReport DrawingDocument::recompute() { return DocumentBase::recompute(); }

void DrawingDocument::forEachOwnNamed(const std::function<void(const NamedSlot&)>& visit) {
    // ONE walk -- see DocumentBase::NamedSlot. Layers and linetypes are named
    // objects like any other: a drawing's layer list IS its table of contents,
    // and a layer nobody could rename would be a table nobody could correct.
    for (const std::unique_ptr<Layer>& one : layers_) {
        Layer* layer = one.get();
        visit(NamedSlot{layer->id(), layer->name(),
                        [layer](const std::string& n) { layer->setName(n); }});
    }
    for (const std::unique_ptr<Linetype>& one : linetypes_) {
        Linetype* linetype = one.get();
        visit(NamedSlot{linetype->id(), linetype->name(),
                        [linetype](const std::string& n) { linetype->setName(n); }});
    }
    for (const std::unique_ptr<DrawingView>& one : views_) {
        DrawingView* view = one.get();
        visit(NamedSlot{view->id(), view->name(),
                        [view](const std::string& n) { view->setName(n); }});
    }
}

bool DrawingDocument::removeObject(ObjectId id) {
    ObjectRegistry::ObjectRef* found = registry_.find(id);
    if (found == nullptr) return false;
    const ObjectRegistry::ObjectRef handle = *found;

    // A VIEW: nothing owns it and nothing else reads it yet. Annotation that
    // measures it arrives in M34, and that is when this grows a cascade.
    if (const DrawingView* view = findView(id)) {
        if (!applyingHistory()) {
            DrawingViewExistenceEdit edit;
            edit.viewId = id;
            edit.name = view->name();
            edit.sourcePath = view->sourcePath();
            edit.bodyName = view->bodyName();
            edit.direction = static_cast<int>(view->direction());
            edit.positionXMm = view->positionMm().x;
            edit.positionYMm = view->positionMm().y;
            edit.scaleNumerator = view->scale().numerator;
            edit.scaleDenominator = view->scale().denominator;
            edit.ownScale = view->hasOwnScale();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete " + view->name());
        }
        graph_.removeNode(id);
        registry_.unregisterObject(id);
        for (auto it = views_.begin(); it != views_.end(); ++it)
            if ((*it)->id() == id) {
                views_.erase(it);
                break;
            }
        return true;
    }

    // A LAYER, except the two a drawing may never be without.
    //
    // THE GUARDS PROTECT THE USER, NOT THE LOADER. A file carries its own
    // layer 0, so opening one has to clear the seeded tables first -- and a
    // guard that refused would leave the document with TWO layers called "0",
    // one of them with an id nothing in the file points at. That is what
    // M32_SER_001 caught: the round trip grew a duplicate table every time.
    //
    // `applyingHistory()` is true on the restore path and during undo, and
    // neither is a user pressing Delete. Undo cannot reach here for layer 0
    // anyway, because the facade never records a delta deleting it.
    if (const Layer* layer = findLayer(id)) {
        if (!applyingHistory()) {
            if (layer->name() == kDefaultLayerName) return false;
            // THE CURRENT LAYER CANNOT GO. Deleting it would leave new
            // geometry with nowhere to land, and silently moving the current
            // layer elsewhere is a change the user did not ask for.
            if (id == currentLayerId_) return false;
        }
        if (!applyingHistory()) {
            LayerExistenceEdit edit;
            edit.layerId = id;
            edit.name = layer->name();
            edit.color = layer->color();
            edit.linetype = layer->linetype();
            edit.on = layer->isOn();
            edit.frozen = layer->isFrozen();
            edit.locked = layer->isLocked();
            edit.lineweight = layer->lineweight();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete layer " + layer->name());
        }
        registry_.unregisterObject(id);
        for (auto it = layers_.begin(); it != layers_.end(); ++it)
            if ((*it)->id() == id) {
                layers_.erase(it);
                break;
            }
        return true;
    }

    if (const Linetype* linetype = findLinetype(id)) {
        if (!applyingHistory()) {
            if (linetype->name() == kContinuousLinetypeName) return false;
            // A LINETYPE A LAYER IS USING cannot go either -- the layer would
            // name a linetype that is not in the table, which is a file other
            // programs refuse to open.
            for (const std::unique_ptr<Layer>& one : layers_)
                if (one->linetype() == linetype->name()) return false;
        }
        if (!applyingHistory()) {
            LinetypeExistenceEdit edit;
            edit.linetypeId = id;
            edit.name = linetype->name();
            edit.description = linetype->description();
            edit.pattern = linetype->pattern();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete linetype " + linetype->name());
        }
        registry_.unregisterObject(id);
        for (auto it = linetypes_.begin(); it != linetypes_.end(); ++it)
            if ((*it)->id() == id) {
                linetypes_.erase(it);
                break;
            }
        return true;
    }

    recordBaseRemoval(handle, id);
    graph_.removeNode(id);
    registry_.unregisterObject(id);
    return eraseBaseOwned(handle, id) || true;
}

void DrawingDocument::applyOwnDelta(const UndoDelta& delta, bool forward) {
    if (const auto* edit = std::get_if<SheetEdit>(&delta)) {
        const int size = forward ? edit->afterSize : edit->beforeSize;
        const int orientation = forward ? edit->afterOrientation : edit->beforeOrientation;
        sheet_.setOrientation(static_cast<SheetOrientation>(orientation));
        sheet_.setScale(DrawingScale{
            forward ? edit->afterScaleNumerator : edit->beforeScaleNumerator,
            forward ? edit->afterScaleDenominator : edit->beforeScaleDenominator});
        // A CUSTOM SHEET carries its own millimetres, and setSize alone cannot
        // put them back -- the size table has no entry for Custom.
        //
        // NO SWAP. A custom sheet ignores orientation (see Sheet::widthMm), so
        // what the delta holds is what the user typed. The first draft swapped
        // them for a landscape sheet and M32_UNDO_002 came back 250 x 500.
        if (static_cast<SheetSize>(size) == SheetSize::Custom)
            sheet_.setCustomSize(forward ? edit->afterWidthMm : edit->beforeWidthMm,
                                 forward ? edit->afterHeightMm : edit->beforeHeightMm);
        // AFTER setCustomSize, which forces the size to Custom as a side
        // effect -- the same ordering restoreSheet has to observe.
        sheet_.setSize(static_cast<SheetSize>(size));
        for (const std::unique_ptr<DrawingView>& view : views_)
            if (!view->hasOwnScale()) graph_.markDirty(view->id());
        return;
    }
    if (const auto* edit = std::get_if<LayerExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findLayer(edit->layerId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreLayer(edit->layerId, edit->name, edit->color, edit->linetype, edit->on,
                         edit->frozen, edit->locked, edit->lineweight);
        else
            removeObject(edit->layerId);
        return;
    }
    if (const auto* edit = std::get_if<LayerPropertyEdit>(&delta)) {
        Layer* layer = findLayerForEdit(edit->layerId);
        if (layer == nullptr) return;
        layer->setColor(forward ? edit->afterColor : edit->beforeColor);
        layer->setLinetype(forward ? edit->afterLinetype : edit->beforeLinetype);
        layer->setOn(forward ? edit->afterOn : edit->beforeOn);
        layer->setFrozen(forward ? edit->afterFrozen : edit->beforeFrozen);
        layer->setLocked(forward ? edit->afterLocked : edit->beforeLocked);
        layer->setLineweight(forward ? edit->afterLineweight : edit->beforeLineweight);
        return;
    }
    if (const auto* edit = std::get_if<CurrentLayerEdit>(&delta)) {
        currentLayerId_ = forward ? edit->after : edit->before;
        return;
    }
    if (const auto* edit = std::get_if<LinetypeExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findLinetype(edit->linetypeId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreLinetype(edit->linetypeId, edit->name, edit->description, edit->pattern);
        else
            removeObject(edit->linetypeId);
        return;
    }
    if (const auto* edit = std::get_if<DrawingViewExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findView(edit->viewId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreView(edit->viewId, edit->name, ComputeState::Dirty, edit->sourcePath,
                        edit->bodyName, static_cast<ViewDirection>(edit->direction),
                        Vec2{edit->positionXMm, edit->positionYMm},
                        DrawingScale{edit->scaleNumerator, edit->scaleDenominator},
                        edit->ownScale);
        else
            removeObject(edit->viewId);
        return;
    }
    if (const auto* edit = std::get_if<DrawingViewPlacementEdit>(&delta)) {
        DrawingView* view = findViewForEdit(edit->viewId);
        if (view == nullptr) return;
        view->setPositionMm(Vec2{forward ? edit->afterXMm : edit->beforeXMm,
                                 forward ? edit->afterYMm : edit->beforeYMm});
        view->setDirection(static_cast<ViewDirection>(forward ? edit->afterDirection
                                                              : edit->beforeDirection));
        if (forward ? edit->afterOwnScale : edit->beforeOwnScale)
            view->setScale(DrawingScale{
                forward ? edit->afterScaleNumerator : edit->beforeScaleNumerator,
                forward ? edit->afterScaleDenominator : edit->beforeScaleDenominator});
        else
            view->clearScale();
        graph_.markDirty(edit->viewId);
        return;
    }

    // A DELTA THIS DOCUMENT DOES NOT KNOW IS AN ERROR, not something to skip:
    // a silently ignored delta is an undo that half-happened, with nothing
    // said. The Part and Assembly sides refuse the same way.
    throw std::runtime_error("this drawing cannot undo a change of that kind");
}

} // namespace paramcad
