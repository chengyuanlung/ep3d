#include "Core/Drawing/DrawingDocument.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/SourceShapeResolver.h"
#include "Core/Document/SourceShapeResolver.h"
#include "Core/Drawing/ObjectSnap.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"

#include <fstream>
#include "Core/Undo/UndoRecord.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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
    int angle;
};

SheetSnapshot SnapshotOf(const Sheet& sheet) {
    return SheetSnapshot{static_cast<int>(sheet.size()),
                         static_cast<int>(sheet.orientation()),
                         sheet.scale().numerator,
                         sheet.scale().denominator,
                         sheet.widthMm(),
                         sheet.heightMm(),
                         static_cast<int>(sheet.projectionAngle())};
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
    //
    // THE FIRST PAGE IS SEEDED THE SAME WAY (M44). A drawing always has one:
    // every accessor that asks for "the current page" would otherwise have a
    // no-page case, and a case that cannot happen is a case nobody maintains.
    auto first = std::make_unique<SheetPage>(std::string("Sheet1"),
                                             Sheet{SheetSize::A3, SheetOrientation::Landscape});
    currentPageId_ = first->id();
    registry_.registerObject(first->id(), first.get());
    pages_.push_back(std::move(first));

    auto continuous = std::make_unique<Linetype>(kContinuousLinetypeName,
                                                 "Solid line", std::vector<double>{});
    registry_.registerObject(continuous->id(), continuous.get());
    linetypes_.push_back(std::move(continuous));

    auto zero = std::make_unique<Layer>(kDefaultLayerName, 7, kContinuousLinetypeName);
    currentLayerId_ = zero->id();
    registry_.registerObject(zero->id(), zero.get());
    layers_.push_back(std::move(zero));

    // ...AND A DIMENSION STYLE, for the same reason: a dimension has to have
    // one, and a drawing that made the user create one before they could put a
    // size on anything would be a drawing nobody finishes.
    auto iso = std::make_unique<DimensionStyle>(kDefaultDimensionStyleName);
    currentStyleId_ = iso->id();
    registry_.registerObject(iso->id(), iso.get());
    dimensionStyles_.push_back(std::move(iso));
}

// =============================================================================
// The paper
// =============================================================================

namespace {
// One recorder for every sheet change, so a size edit and a scale edit record
// the same shape of delta and undo cannot half-restore one of them.
} // namespace

const SheetPage& DrawingDocument::currentPage() const noexcept {
    for (const std::unique_ptr<SheetPage>& page : pages_)
        if (page->id() == currentPageId_) return *page;
    // NEVER EMPTY: a drawing is constructed with one page and the last cannot
    // be deleted, so this is the first page rather than a special case.
    return *pages_.front();
}

SheetPage& DrawingDocument::currentPageForEdit() noexcept {
    for (const std::unique_ptr<SheetPage>& page : pages_)
        if (page->id() == currentPageId_) return *page;
    return *pages_.front();
}

SheetPage& DrawingDocument::addSheetPage(std::string name) {
    if (name.empty()) throw std::invalid_argument("addSheetPage: a page needs a name");
    for (const std::unique_ptr<SheetPage>& page : pages_)
        if (page->name() == name)
            throw std::invalid_argument("addSheetPage: this drawing already has a page "
                                        "called " + name);
    // THE SAME PAPER AS THE PAGE IT WAS ADDED FROM. A drawing set is nearly
    // always one size throughout, and a second page that came up A4 when the
    // first is A2 is a surprise nobody asked for.
    auto item = std::make_unique<SheetPage>(std::move(name), currentPage().paper());
    auto& ref = *item;
    ref.setFrameMargins(currentPage().frameMargins());
    ref.setFrameZoneTargetMm(currentPage().frameZoneTargetMm());
    ref.setFrameVisible(currentPage().isFrameVisible());
    pages_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    SheetPageExistenceEdit edit;
    edit.pageId = ref.id();
    edit.name = ref.name();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add sheet " + ref.name());
    return ref;
}

void DrawingDocument::clearSheetPagesForRestore() {
    for (const std::unique_ptr<SheetPage>& page : pages_) registry_.unregisterObject(page->id());
    pages_.clear();
    currentPageId_ = kInvalidObjectId;
}

SheetPage& DrawingDocument::restoreSheetPage(ObjectId id, std::string name, Sheet paper,
                                             FrameMargins margins, double zoneTargetMm,
                                             bool frameVisible) {
    auto item = std::make_unique<SheetPage>(id, std::move(name), std::move(paper));
    auto& ref = *item;
    ref.setFrameMargins(margins);
    ref.setFrameZoneTargetMm(zoneTargetMm);
    ref.setFrameVisible(frameVisible);
    pages_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const SheetPage*> DrawingDocument::sheetPages() const {
    std::vector<const SheetPage*> out;
    out.reserve(pages_.size());
    for (const std::unique_ptr<SheetPage>& page : pages_) out.push_back(page.get());
    return out;
}

const SheetPage* DrawingDocument::findSheetPage(ObjectId id) const noexcept {
    for (const std::unique_ptr<SheetPage>& page : pages_)
        if (page->id() == id) return page.get();
    return nullptr;
}

ObjectId DrawingDocument::currentSheetId() const noexcept { return currentPageId_; }

bool DrawingDocument::setCurrentSheet(ObjectId sheetId) {
    if (findSheetPage(sheetId) == nullptr) return false;
    currentPageId_ = sheetId;
    return true;
}

std::string DrawingDocument::sheetNumberOf(ObjectId sheetId) const {
    // "2 / 3", from WHERE THE PAGE SITS. Stored, it is the first thing to go
    // stale when a page is inserted -- and it goes stale in the one place on a
    // drawing a reader trusts absolutely.
    for (std::size_t i = 0; i < pages_.size(); ++i)
        if (pages_[i]->id() == sheetId)
            return std::to_string(i + 1) + " / " + std::to_string(pages_.size());
    return {};
}

int DrawingDocument::currentSheetNumber() const noexcept {
    for (std::size_t i = 0; i < pages_.size(); ++i)
        if (pages_[i]->id() == currentPageId_) return static_cast<int>(i) + 1;
    return 1;
}

int DrawingDocument::sheetCount() const noexcept { return static_cast<int>(pages_.size()); }

std::size_t DrawingDocument::objectsOnSheet(ObjectId sheetId) const {
    std::size_t count = 0;
    const auto tally = [&](ObjectId on) {
        // kInvalidObjectId means the FIRST page: every object made before this
        // drawing had more than one page belongs there.
        const ObjectId where = on == kInvalidObjectId ? pages_.front()->id() : on;
        if (where == sheetId) ++count;
    };
    eachPagedList(*this, [&](const auto& list, const char*) {
        for (const auto& one : list) tally(one->sheetId());
    });
    return count;
}

bool DrawingDocument::removeSheetPage(ObjectId sheetId) {
    // A DRAWING WITH NO PAPER IS NOT A DRAWING.
    if (pages_.size() <= 1) return false;
    if (findSheetPage(sheetId) == nullptr) return false;
    // ...AND A PAGE WITH THINGS ON IT TAKES THEM WITH IT. Refused instead,
    // for the reason M41's datums are: cascading throws away work nobody
    // asked to lose, and the count tells the user exactly what to move.
    if (objectsOnSheet(sheetId) > 0) return false;

    for (auto it = pages_.begin(); it != pages_.end(); ++it) {
        if ((*it)->id() != sheetId) continue;
        SheetPageExistenceEdit edit;
        edit.pageId = sheetId;
        edit.name = (*it)->name();
        edit.addedByTheEdit = false;
        registry_.unregisterObject(sheetId);
        pages_.erase(it);
        if (currentPageId_ == sheetId) currentPageId_ = pages_.front()->id();
        recordDelta(edit, "Delete sheet");
        return true;
    }
    return false;
}

bool DrawingDocument::isOnCurrentSheet(ObjectId sheetId) const noexcept {
    // kInvalidObjectId is the FIRST page: everything made before a drawing had
    // more than one page belongs there, and so does everything a caller adds
    // without saying otherwise.
    const ObjectId where = sheetId == kInvalidObjectId ? pages_.front()->id() : sheetId;
    return where == currentPageId_;
}

ObjectId DrawingDocument::sheetOfObject(ObjectId objectId) const {
    const auto resolve = [&](ObjectId on) {
        return on == kInvalidObjectId ? pages_.front()->id() : on;
    };
    ObjectId found = kInvalidObjectId;
    eachPagedList(*this, [&](const auto& list, const char*) {
        if (found != kInvalidObjectId) return;
        for (const auto& one : list)
            if (one->id() == objectId) {
                found = resolve(one->sheetId());
                return;
            }
    });
    return found;
}

bool DrawingDocument::setObjectSheet(ObjectId objectId, ObjectId sheetId) {
    if (findSheetPage(sheetId) == nullptr) return false;
    bool moved = false;
    eachPagedList(*this, [&](auto& list, const char*) {
        if (moved) return;
        for (const auto& one : list)
            if (one->id() == objectId) {
                one->setSheetId(sheetId);
                moved = true;
                return;
            }
    });
    return moved;
}

std::string DrawingDocument::whyDrawingRefused() const {
    // ONE RULE, asked by the saver and by the loader. An object on a page that
    // is not here draws nowhere and cannot be found: it is not on any tab, and
    // deleting the page it named is what put it there.
    const auto check = [&](ObjectId on, const char* what) -> std::string {
        if (on == kInvalidObjectId) return {};
        if (findSheetPage(on) != nullptr) return {};
        return std::string(what) + " is on a sheet that is not in this drawing";
    };
    std::string refused;
    eachPagedList(*this, [&](const auto& list, const char* what) {
        if (!refused.empty()) return;
        for (const auto& one : list)
            if (std::string why = check(one->sheetId(), what); !why.empty()) {
                refused = std::move(why);
                return;
            }
    });
    return refused;
}

bool DrawingDocument::setSheetSize(SheetSize size) {
    const SheetSnapshot before = SnapshotOf(paperForEdit());
    paperForEdit().setSize(size);
    const SheetSnapshot after = SnapshotOf(paperForEdit());
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
    edit.beforeAngle = before.angle;
    edit.afterAngle = after.angle;
    recordDelta(edit, "Sheet size");
    return true;
}

bool DrawingDocument::setSheetOrientation(SheetOrientation orientation) {
    const SheetSnapshot before = SnapshotOf(paperForEdit());
    paperForEdit().setOrientation(orientation);
    const SheetSnapshot after = SnapshotOf(paperForEdit());
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
    edit.beforeAngle = before.angle;
    edit.afterAngle = after.angle;
    recordDelta(edit, "Sheet orientation");
    return true;
}

bool DrawingDocument::setSheetScale(const DrawingScale& scale) {
    if (!scale.valid()) return false;
    const SheetSnapshot before = SnapshotOf(paperForEdit());
    paperForEdit().setScale(scale);
    const SheetSnapshot after = SnapshotOf(paperForEdit());
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
    edit.beforeAngle = before.angle;
    edit.afterAngle = after.angle;
    recordDelta(edit, "Sheet scale");
    // NOTHING IS REPROJECTED. The curves are in MODEL millimetres (see
    // ProjectedGeometry.h), so the scale changes how much paper a view takes
    // and not what it contains. Dirtying here would re-run hidden-line removal
    // -- the expensive operation in this whole block -- to arrive at exactly
    // the geometry already held.
    return true;
}

bool DrawingDocument::setSheetCustomSize(double widthMm, double heightMm) {
    const SheetSnapshot before = SnapshotOf(paperForEdit());
    if (!paperForEdit().setCustomSize(widthMm, heightMm)) return false;
    const SheetSnapshot after = SnapshotOf(paperForEdit());
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
    edit.beforeAngle = before.angle;
    edit.afterAngle = after.angle;
    recordDelta(edit, "Sheet size");
    return true;
}

void DrawingDocument::restoreSheet(SheetSize size, SheetOrientation orientation,
                                   DrawingScale scale, double customWidthMm,
                                   double customHeightMm, ProjectionAngle angle) {
    // ORDER MATTERS: setCustomSize sets the size to Custom as a side effect,
    // so it runs FIRST and the explicit size wins afterwards. Written down
    // because the other order silently turns every A3 sheet into a custom one.
    if (size == SheetSize::Custom) paperForEdit().setCustomSize(customWidthMm, customHeightMm);
    paperForEdit().setSize(size);
    paperForEdit().setOrientation(orientation);
    paperForEdit().setScale(scale);
    paperForEdit().setProjectionAngle(angle);
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

void DrawingDocument::restoreCurrentDimensionStyle(ObjectId styleId) {
    if (findDimensionStyle(styleId) != nullptr) currentStyleId_ = styleId;
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
// WHAT A VIEW IS, FOR THE RECORD THAT MAKES IT EXIST AGAIN (M49).
//
// Five places built this by hand: adding a base view, projecting one, cutting
// a section, taking a detail, and DELETING any of them. The four adding ones
// each filled in what they had just been given; the deleting one filled in
// what it could see -- and it could not see the section cut or the detail
// circle, because nobody had added them to it.
//
// So undoing the delete of a section restored a view with no cut line, which
// projects the WHOLE part and looks entirely reasonable. That has been true
// since M38. The header of DrawingViewExistenceEdit even says it must not be:
// "A SECTION'S CUT COMES BACK WITH IT. Without this, undoing the delete of a
// section view would restore a view with no cut line." The field existed. The
// delete path never wrote it.
//
// Nobody had deleted a section and pressed undo. M49's gate did, because it
// asked the same question about a detail.
//
// One reader now, taken FROM THE VIEW, so what comes back is what was there
// rather than what a caller remembered to mention.
void SnapshotViewExistence(const DrawingView& view, DrawingViewExistenceEdit& edit) {
    edit.viewId = view.id();
    edit.name = view.name();
    edit.sourcePath = view.sourcePath();
    edit.bodyName = view.bodyName();
    edit.direction = static_cast<int>(view.direction());
    edit.positionXMm = view.positionMm().x;
    edit.positionYMm = view.positionMm().y;
    edit.scaleNumerator = view.scale().numerator;
    edit.scaleDenominator = view.scale().denominator;
    edit.ownScale = view.hasOwnScale();
    edit.showHidden = view.showsHiddenLines();
    edit.showTangent = view.showsTangentEdges();
    edit.parentViewId = view.parentViewId();
    edit.alignmentOffsetMm = view.alignmentOffsetMm();
    edit.sectionActive = view.sectionCut().active;
    edit.sectionFromXMm = view.sectionCut().fromMm.x;
    edit.sectionFromYMm = view.sectionCut().fromMm.y;
    edit.sectionToXMm = view.sectionCut().toMm.x;
    edit.sectionToYMm = view.sectionCut().toMm.y;
    edit.sectionArrowSide = view.sectionCut().arrowSide;
    edit.detailActive = view.detailFrame().active;
    edit.detailCentreXMm = view.detailFrame().centreMm.x;
    edit.detailCentreYMm = view.detailFrame().centreMm.y;
    edit.detailRadiusMm = view.detailFrame().radiusMm;
    edit.flatPattern = view.showsFlatPattern();
    edit.breakActive = view.breakSpan().active;
    edit.breakFromMm = view.breakSpan().fromMm;
    edit.breakToMm = view.breakSpan().toMm;
    edit.breakHorizontal = view.breakSpan().horizontal;
    edit.breakGapMm = view.breakSpan().gapMm;
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
    if (positionMm.x > sheet().widthMm() || positionMm.y > sheet().heightMm())
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
    ref.setSheetId(currentPageId_);
    views_.push_back(std::move(item));
    // ONE CALL, NOT TWO. `addRecomputableNode` registers the object AND makes
    // the graph node; registering first makes the second registration fail,
    // and it fails by returning NodeAlreadyExists rather than by saying
    // anything -- so the graph node is silently never created and the engine
    // never runs the view. It reports success over blank paper, which is
    // exactly the failure this view's own diagnostic exists to prevent.
    addRecomputableNode(ref);

    DrawingViewExistenceEdit edit;
    SnapshotViewExistence(ref, edit);
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add view " + ref.name());
    return ref;
}

std::string DrawingDocument::whyViewCannotBeProjectedFrom(ObjectId parentViewId,
                                                          ViewDirection direction) const {
    const DrawingView* parent = findView(parentViewId);
    if (parent == nullptr) return "there is no view to project that one from";
    if (parent->direction() == direction)
        return "a view projected from another one has to look at it from a different side";
    const ViewAlignmentRule rule = AlignmentOf(parent->direction(), direction);
    if (rule.alignment == ViewAlignment::None)
        return "an " + std::string(toString(direction)) + " view is not square to a " +
               std::string(toString(parent->direction())) +
               " one, so there is no side of it to sit on";
    return {};
}

DrawingView& DrawingDocument::addProjectedView(std::string name, ObjectId parentViewId,
                                               ViewDirection direction, double offsetMm) {
    if (name.empty()) throw std::invalid_argument("addProjectedView: a view needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addProjectedView: '" + name + "' is already taken");
    if (const std::string why = whyViewCannotBeProjectedFrom(parentViewId, direction);
        !why.empty())
        throw std::invalid_argument("addProjectedView: " + why);

    // THE SAME MODEL AS ITS PARENT, always. A "projected view" of a different
    // file is not a projected view -- it is a second base view that happens to
    // sit beside this one, and calling it a child would make the alignment a
    // lie the moment somebody read a measurement across.
    const DrawingView* parent = findView(parentViewId);
    auto item = std::make_unique<DrawingView>(std::move(name), parent->sourcePath(),
                                              parent->bodyName(), direction, Vec2{0.0, 0.0});
    auto& ref = *item;
    ref.setParentViewId(parentViewId);
    ref.setAlignmentOffsetMm(offsetMm);
    ref.setSheetId(currentPageId_);
    views_.push_back(std::move(item));
    addRecomputableNode(ref);
    // THE ONE EDGE A CHILD VIEW HAS: its parent. It is not a geometric
    // dependency -- the child projects the same file for itself -- but it IS a
    // placement one, and the graph is how a placement change reaches whatever
    // it moves.
    addDependency(ref.id(), parentViewId);

    DrawingViewExistenceEdit edit;
    SnapshotViewExistence(ref, edit);
    edit.addedByTheEdit = true;
    recordDelta(edit, "Project " + ref.name());
    return ref;
}

Vec2 DrawingDocument::viewPositionMm(ObjectId viewId) const {
    const DrawingView* view = findView(viewId);
    if (view == nullptr) return Vec2{0.0, 0.0};
    if (view->parentViewId() == kInvalidObjectId) return view->positionMm();

    // COMPOSED, NEVER STORED. Walked up rather than cached, so moving a parent
    // moves everything under it without anything being told -- the same reason
    // a frame's world transform is composed (ADR-M10-002).
    //
    // Bounded by the view count: a parent chain that had somehow closed a loop
    // would otherwise spin here for ever, and refusing to create one is worth
    // nothing if the reader can still hang.
    Vec2 place{0.0, 0.0};
    const DrawingView* walk = view;
    for (std::size_t step = 0; step <= views_.size(); ++step) {
        const DrawingView* parent = findView(walk->parentViewId());
        if (parent == nullptr) return Vec2{place.x + walk->positionMm().x,
                                           place.y + walk->positionMm().y};
        // A SECTION IS ALIGNED ALONG ITS ARROWS, not by the six-direction table.
        //
        // Its stored direction is its PARENT'S -- the real camera is worked
        // out from the cut line at every recompute -- so AlignmentOf would
        // compare a direction with itself, find no relationship, and leave the
        // section sitting ON TOP OF the view it was cut from. Which is exactly
        // what it did: the first screenshot showed the section overlapping its
        // parent with the two labels written over each other.
        //
        // Where it belongs is off to the side the reader is looking FROM,
        // along the arrows -- the same direction the painter draws them in,
        // computed the same way, so the view and its arrows cannot disagree.
        if (walk->isSection() && walk->sectionCut().usable()) {
            const Vec2 from = walk->sectionCut().fromMm;
            const Vec2 to = walk->sectionCut().toMm;
            const double dx = to.x - from.x;
            const double dy = to.y - from.y;
            const double run = std::hypot(dx, dy);
            if (run > 1e-9) {
                const double side = walk->sectionCut().arrowSide >= 0 ? 1.0 : -1.0;
                place.x += dy / run * side * walk->alignmentOffsetMm();
                place.y += -dx / run * side * walk->alignmentOffsetMm();
            }
            walk = parent;
            continue;
        }
        // A DETAIL IS NOT ALIGNED TO ANYTHING EITHER, and for the same reason:
        // its direction IS its parent's, so AlignmentOf compares a direction
        // with itself, finds no relationship, and leaves the enlargement
        // sitting ON TOP OF the view it came from. The section above learned
        // this from a screenshot; the detail learned it from the next one.
        //
        // Where it belongs is out along the line from the middle of the parent
        // THROUGH the circle -- so the enlargement lands on the side of the
        // view it magnifies, which is where a reader's eye goes next. Derived
        // from the circle rather than asked for, so a detail cannot be placed
        // on the opposite side from the thing it is about.
        if (walk->isDetail() && walk->detailFrame().usable()) {
            const ProjectedExtent& span = parent->projected().extent;
            const Vec2 middle{0.5 * (span.min.x + span.max.x), 0.5 * (span.min.y + span.max.y)};
            double dx = walk->detailFrame().centreMm.x - middle.x;
            double dy = walk->detailFrame().centreMm.y - middle.y;
            double run = std::hypot(dx, dy);
            if (run <= 1e-9) {
                // A CIRCLE ON THE MIDDLE HAS NO SIDE. Straight out to the
                // right is arbitrary and is said to be: what it must not do is
                // stay at zero, which puts the detail back on its parent.
                dx = 1.0;
                dy = 0.0;
                run = 1.0;
            }
            place.x += dx / run * walk->alignmentOffsetMm();
            place.y += dy / run * walk->alignmentOffsetMm();
            walk = parent;
            continue;
        }
        const ViewAlignmentRule rule = AlignmentOf(parent->direction(), walk->direction());
        // FIRST ANGLE PUTS EVERY VIEW ON THE OTHER SIDE. One flip, here, for
        // exactly the reason the angle lives on the SHEET: a drawing is in one
        // convention or the other, never both.
        const double sign =
            rule.sign * (sheet().projectionAngle() == ProjectionAngle::First ? -1.0 : 1.0);
        if (rule.alignment == ViewAlignment::Horizontal)
            place.x += sign * walk->alignmentOffsetMm();
        else if (rule.alignment == ViewAlignment::Vertical)
            place.y += sign * walk->alignmentOffsetMm();
        walk = parent;
    }
    return place;
}

std::vector<ObjectId> DrawingDocument::staleViews() const {
    std::vector<ObjectId> behind;
    for (const std::unique_ptr<DrawingView>& view : views_) {
        // A VIEW THAT NEVER BUILT IS NOT "STALE", it is broken, and the tree
        // already says so. Offering to update it would send the user round a
        // loop that cannot end.
        if (view->currentState() != ComputeState::Valid) continue;
        if (SourceFileStamp(view->sourcePath()) != view->sourceStamp())
            behind.push_back(view->id());
    }
    return behind;
}

bool DrawingDocument::setSheetProjectionAngle(ProjectionAngle angle) {
    if (paperForEdit().projectionAngle() == angle) return true;
    const SheetSnapshot before = SnapshotOf(paperForEdit());
    paperForEdit().setProjectionAngle(angle);
    const SheetSnapshot after = SnapshotOf(paperForEdit());
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
    edit.beforeAngle = before.angle;
    edit.afterAngle = after.angle;
    recordDelta(edit, "Projection angle");
    // NOTHING IS REPROJECTED. Every view still looks the same way; they have
    // moved to the other side of each other, which is a placement change.
    return true;
}

DrawingView& DrawingDocument::restoreView(ObjectId id, std::string name, ComputeState state,
                                          std::string sourcePath, std::string bodyName,
                                          ViewDirection direction, Vec2 positionMm,
                                          DrawingScale scale, bool ownScale, bool showHidden,
                                          bool showTangent, ObjectId parentViewId,
                                          double alignmentOffsetMm) {
    requireUnusedId(id, "restoreView");
    auto item = std::make_unique<DrawingView>(id, std::move(name), state, std::move(sourcePath),
                                              std::move(bodyName), direction, positionMm, scale,
                                              ownScale, showHidden, showTangent, parentViewId,
                                              alignmentOffsetMm);
    auto& ref = *item;
    views_.push_back(std::move(item));
    addRecomputableNode(ref);
    if (parentViewId != kInvalidObjectId) addDependency(ref.id(), parentViewId);
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
    // The cut, both sides, so an edit that does not touch it puts it back
    // exactly -- see DrawingViewPlacementEdit.
    edit.beforeSectionActive = edit.afterSectionActive = view.sectionCut().active;
    edit.beforeSectionFromXMm = edit.afterSectionFromXMm = view.sectionCut().fromMm.x;
    edit.beforeSectionFromYMm = edit.afterSectionFromYMm = view.sectionCut().fromMm.y;
    edit.beforeSectionToXMm = edit.afterSectionToXMm = view.sectionCut().toMm.x;
    edit.beforeSectionToYMm = edit.afterSectionToYMm = view.sectionCut().toMm.y;
    edit.beforeSectionArrowSide = edit.afterSectionArrowSide = view.sectionCut().arrowSide;
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
    edit.beforeShowHidden = view.showsHiddenLines();
    edit.afterShowHidden = view.showsHiddenLines();
    edit.beforeShowTangent = view.showsTangentEdges();
    edit.afterShowTangent = view.showsTangentEdges();
    edit.beforeAlignmentOffsetMm = view.alignmentOffsetMm();
    edit.afterAlignmentOffsetMm = view.alignmentOffsetMm();
    // The circle, both sides, so an edit that does not touch it puts it back
    // exactly -- the same contract the cut above has.
    edit.beforeDetailActive = edit.afterDetailActive = view.detailFrame().active;
    edit.beforeDetailCentreXMm = edit.afterDetailCentreXMm = view.detailFrame().centreMm.x;
    edit.beforeDetailCentreYMm = edit.afterDetailCentreYMm = view.detailFrame().centreMm.y;
    edit.beforeDetailRadiusMm = edit.afterDetailRadiusMm = view.detailFrame().radiusMm;
    edit.beforeBreakActive = edit.afterBreakActive = view.breakSpan().active;
    edit.beforeBreakFromMm = edit.afterBreakFromMm = view.breakSpan().fromMm;
    edit.beforeBreakToMm = edit.afterBreakToMm = view.breakSpan().toMm;
    edit.beforeBreakHorizontal = edit.afterBreakHorizontal = view.breakSpan().horizontal;
    edit.beforeBreakGapMm = edit.afterBreakGapMm = view.breakSpan().gapMm;
}
} // namespace

// A CHILD IS MOVED BY ITS OFFSET, never by a free position -- see
// setViewPosition for why.
bool DrawingDocument::setViewAlignmentOffsetMm(ObjectId viewId, double offsetMm) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    if (view->parentViewId() == kInvalidObjectId) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterAlignmentOffsetMm = offsetMm;
    view->setAlignmentOffsetMm(offsetMm);
    recordDelta(edit, "Move " + view->name());
    return true;
}

DrawingView& DrawingDocument::addSectionView(std::string name, ObjectId parentViewId,
                                             Vec2 fromMm, Vec2 toMm, int arrowSide,
                                             double offsetMm) {
    const DrawingView* parent = findView(parentViewId);
    if (parent == nullptr)
        throw std::invalid_argument("addSectionView: a section is cut from a view, and that "
                                    "view is not in this drawing");
    if (parent->isSection())
        throw std::invalid_argument("addSectionView: a section of a section is not supported "
                                    "yet -- cut from a plain view instead");
    if (std::fabs(toMm.x - fromMm.x) < 1e-9 && std::fabs(toMm.y - fromMm.y) < 1e-9)
        throw std::invalid_argument("addSectionView: a cut line of no length cuts nothing");

    if (name.empty()) throw std::invalid_argument("addSectionView: a view needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addSectionView: '" + name + "' is already taken");

    // ONE UNDO STEP for the view and its cut together: making a section is one
    // thing the user did, and a half-made section -- a view with no cut line --
    // is a state no drawing was ever in.
    const bool ownsStep = !isTransactionOpen() && !applyingHistory();
    if (ownsStep) beginTransaction("Add section view");

    // THE SAME MODEL AS ITS PARENT, always -- the same rule a projected view
    // follows, and for the same reason: a section of a different file is not a
    // section of this one.
    //
    // The DIRECTION recorded is the parent's. The section's real camera is
    // worked out from the cut line at every recompute (see DrawingView), so
    // this is only what it falls back to before it has one -- and it is why
    // the direction is not asked of the caller.
    auto item = std::make_unique<DrawingView>(name, parent->sourcePath(), parent->bodyName(),
                                              parent->direction(), Vec2{0.0, 0.0});
    auto& ref = *item;
    ref.setParentViewId(parentViewId);
    ref.setAlignmentOffsetMm(offsetMm);
    DrawingView::SectionCut cut;
    cut.active = true;
    cut.fromMm = fromMm;
    cut.toMm = toMm;
    cut.arrowSide = arrowSide >= 0 ? 1 : -1;
    ref.setSectionCut(cut);
    ref.setSheetId(currentPageId_);
    views_.push_back(std::move(item));
    addRecomputableNode(ref);
    // THE PLACEMENT EDGE, as a projected view has: a section is positioned
    // relative to the view it was cut from.
    addDependency(ref.id(), parentViewId);

    DrawingViewExistenceEdit edit;
    SnapshotViewExistence(ref, edit);
    edit.addedByTheEdit = true;
    recordDelta(edit, "Section " + ref.name());
    if (ownsStep) commitTransaction();
    return ref;
}

bool DrawingDocument::setSectionCut(ObjectId viewId, Vec2 fromMm, Vec2 toMm, int arrowSide) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    if (std::fabs(toMm.x - fromMm.x) < 1e-9 && std::fabs(toMm.y - fromMm.y) < 1e-9)
        return false;
    DrawingView::SectionCut cut;
    cut.active = true;
    cut.fromMm = fromMm;
    cut.toMm = toMm;
    cut.arrowSide = arrowSide >= 0 ? 1 : -1;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterSectionFromXMm = fromMm.x;
    edit.afterSectionFromYMm = fromMm.y;
    edit.afterSectionToXMm = toMm.x;
    edit.afterSectionToYMm = toMm.y;
    edit.afterSectionArrowSide = cut.arrowSide;
    view->setSectionCut(cut);
    recordDelta(edit, "Move the section line");
    // THE VIEW IS NOW OUT OF DATE. Moving the knife changes what is drawn, and
    // a section that kept its old curves would be a picture of a cut nobody
    // asked for.
    markDirty(viewId);
    return true;
}

DrawingView& DrawingDocument::addFlatPatternView(std::string name, std::string sourcePath,
                                                 std::string bodyName, Vec2 positionMm) {
    if (name.empty()) throw std::invalid_argument("addFlatPatternView: a view needs a name");
    if (sourcePath.empty())
        throw std::invalid_argument("addFlatPatternView: a flat pattern is of a part, and no "
                                    "part was named");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addFlatPatternView: '" + name + "' is already taken");

    // A BASE VIEW WITH A FLAG. The direction is Front and means nothing here:
    // a blank has no camera, it has a layout. It is set rather than left at
    // whatever the enum's first value happens to be, because a reader of the
    // file should see a stated answer and not an accident.
    auto item = std::make_unique<DrawingView>(std::move(name), std::move(sourcePath),
                                              std::move(bodyName), ViewDirection::Front,
                                              positionMm);
    auto& ref = *item;
    ref.setShowsFlatPattern(true);
    // HIDDEN LINES OFF. There is nothing behind a flat sheet, and a blank
    // drawn with dashes would send a laser cutter looking for them.
    ref.setShowsHiddenLines(false);
    ref.setSheetId(currentPageId_);
    views_.push_back(std::move(item));
    addRecomputableNode(ref);

    DrawingViewExistenceEdit edit;
    SnapshotViewExistence(ref, edit);
    edit.addedByTheEdit = true;
    recordDelta(edit, "Flat pattern " + ref.name());
    return ref;
}

DrawingView& DrawingDocument::addDetailView(std::string name, ObjectId parentViewId,
                                            Vec2 centreMm, double radiusMm,
                                            DrawingScale scale, double offsetMm) {
    const DrawingView* parent = findView(parentViewId);
    if (parent == nullptr)
        throw std::invalid_argument("addDetailView: a detail is taken from a view, and that "
                                    "view is not in this drawing");
    if (parent->isDetail())
        throw std::invalid_argument("addDetailView: a detail of a detail is not supported "
                                    "yet -- take it from the view that is not already a "
                                    "detail");
    if (!(radiusMm > 1e-9))
        throw std::invalid_argument("addDetailView: a circle of no size encloses nothing to "
                                    "enlarge");
    if (name.empty()) throw std::invalid_argument("addDetailView: a view needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addDetailView: '" + name + "' is already taken");

    const bool ownsStep = !isTransactionOpen() && !applyingHistory();
    if (ownsStep) beginTransaction("Add detail view");

    // THE SAME MODEL AS ITS PARENT, and the parent's direction -- which the
    // recompute reads again every time, so this is only what it starts from.
    auto item = std::make_unique<DrawingView>(name, parent->sourcePath(), parent->bodyName(),
                                              parent->direction(), Vec2{0.0, 0.0});
    auto& ref = *item;
    ref.setParentViewId(parentViewId);
    ref.setAlignmentOffsetMm(offsetMm);
    DrawingView::DetailFrame frame;
    frame.active = true;
    frame.centreMm = centreMm;
    frame.radiusMm = radiusMm;
    ref.setDetailFrame(frame);
    // A DETAIL HAS ITS OWN SCALE, always. Following the sheet it would be the
    // same size as the thing it magnifies, which is not a detail view -- and a
    // detail that followed a later rescale would stop being an enlargement
    // without anybody touching it.
    ref.setScale(scale);
    ref.setSheetId(currentPageId_);
    views_.push_back(std::move(item));
    addRecomputableNode(ref);
    // THE PLACEMENT EDGE, as a section has: a detail is positioned relative to
    // the view it was taken from.
    addDependency(ref.id(), parentViewId);

    DrawingViewExistenceEdit edit;
    SnapshotViewExistence(ref, edit);
    edit.addedByTheEdit = true;
    recordDelta(edit, "Detail " + ref.name());
    if (ownsStep) commitTransaction();
    return ref;
}

bool DrawingDocument::setDetailFrame(ObjectId viewId, Vec2 centreMm, double radiusMm) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    // A CIRCLE OF NO SIZE ENCLOSES NOTHING, and a detail of nothing draws an
    // empty ring with a caption -- which reads as "this area is featureless".
    if (!(radiusMm > 1e-9)) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterDetailActive = true;
    edit.afterDetailCentreXMm = centreMm.x;
    edit.afterDetailCentreYMm = centreMm.y;
    edit.afterDetailRadiusMm = radiusMm;
    DrawingView::DetailFrame frame;
    frame.active = true;
    frame.centreMm = centreMm;
    frame.radiusMm = radiusMm;
    view->setDetailFrame(frame);
    recordDelta(edit, "Move the detail circle");
    // THE VIEW IS NOW OUT OF DATE. Moving the circle changes what is drawn,
    // and a detail that kept its old curves would be a picture of somewhere
    // nobody is pointing at.
    markDirty(viewId);
    return true;
}

std::string DrawingDocument::sectionLetterOf(ObjectId viewId) const {
    // A SECTION'S LETTER IS A VIEW'S LETTER -- one sequence, see viewLetterOf.
    const DrawingView* asked = findView(viewId);
    if (asked == nullptr || !asked->isSection()) return {};
    return viewLetterOf(viewId);
}

std::string DrawingDocument::viewLetterOf(ObjectId viewId) const {
    const DrawingView* asked = findView(viewId);
    // The `!isSection()` half of this is an EARLY-OUT, not a guard, and a
    // mutation deleting it survives: the walk below skips every view that is
    // not a section, so a plain view's id never matches and the answer is the
    // same empty string either way. It stays because every caption on the
    // sheet asks this question of every view, and because the contract reads
    // better stated at the top than inferred from a loop three lines down.
    // ONE SEQUENCE FOR SECTIONS AND DETAILS TOGETHER. Two pools would put a
    // "SECTION A-A" and a "DETAIL A" on the same sheet, and a reader looking
    // up A would find whichever they saw first.
    if (asked == nullptr || !(asked->isSection() || asked->isDetail())) return {};
    // IN DOCUMENT ORDER, so the first one made is A. Derived rather than
    // stored: the mark on the parent and the title under the view both ask
    // here, so they cannot end up carrying different letters.
    int index = 0;
    for (const std::unique_ptr<DrawingView>& one : views_) {
        if (!(one->isSection() || one->isDetail())) continue;
        if (one->id() == viewId) {
            std::string letter;
            int number = index + 1;
            while (number > 0) {
                const int remainder = (number - 1) % 26;
                letter.insert(letter.begin(), static_cast<char>('A' + remainder));
                number = (number - 1) / 26;
            }
            return letter;
        }
        ++index;
    }
    return {};
}

std::string DrawingDocument::viewLabelText(ObjectId viewId) const {
    const DrawingView* view = findView(viewId);
    if (view == nullptr) return {};
    // A SECTION IS TITLED BY ITS LETTER, not by its name: "A-A" is what a
    // reader looks for under it, and it has to match the line drawn on the
    // parent -- so neither is typed and both come from viewLetterOf.
    //
    // A DETAIL IS TITLED "DETAIL A", not "A-A". The two are different
    // instructions about where to look on the parent -- one says "a line
    // crosses the view here", the other "a circle is drawn round this" -- and
    // a detail captioned A-A sends the reader hunting for a cut line.
    const std::string letter = viewLetterOf(viewId);
    std::string label = view->name();
    // A FLAT PATTERN SAYS SO, always. A blank and a folded view of the same
    // part are both rectangles-with-lines at a glance, and the one that goes
    // to the laser is not the one that goes to the fitter.
    if (view->showsFlatPattern()) label = "FLAT PATTERN  " + view->name();
    if (!letter.empty())
        label = view->isDetail() ? "DETAIL " + letter : letter + "-" + letter;
    // The scale is written only when it is NOT the sheet's. Written always, it
    // is noise.
    if (view->hasOwnScale()) label += "  (" + view->scale().toString() + ")";
    return label;
}

HatchRegion DrawingDocument::sectionHatchRegionMm(ObjectId viewId) const {
    HatchRegion region;
    const DrawingView* view = findView(viewId);
    if (view == nullptr) return region;
    for (const std::vector<Vec2>& loop : view->projected().cutLoops) {
        std::vector<Vec2> onSheet;
        onSheet.reserve(loop.size());
        // SHEET MILLIMETRES. The curves never carry the scale, so the cut
        // loops do not either -- hatching them where they lie would give a
        // 1:2 section twice the pitch of a 1:1 one on the same sheet.
        for (const Vec2 point : loop) onSheet.push_back(viewPointToSheetMm(viewId, point));
        region.add(std::move(onSheet));
    }
    return region;
}

HatchStyle DrawingDocument::sectionHatchStyle(ObjectId viewId) const {
    HatchStyle style;
    const std::string letter = sectionLetterOf(viewId);
    const int which = letter.empty() ? 0 : (letter.front() - 'A');
    // 45 degrees, then 135, alternating -- the convention for telling two cut
    // parts apart where they touch. The offset walks as well, so two sections
    // at the SAME angle still do not line up.
    style.angleRad = (which % 2 == 0) ? 0.7853981633974483 : -0.7853981633974483;
    style.offsetMm = static_cast<double>(which % 3) * 1.0;
    return style;
}

bool DrawingDocument::setViewPosition(ObjectId viewId, Vec2 positionMm) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    // A CHILD HAS NO FREE POSITION. It slides along one axis and shares the
    // other with its parent -- that sharing is the whole point of an
    // orthographic layout, and a child that could be dragged anywhere would
    // silently break the ruler-across-views property a reader relies on.
    // `setViewAlignmentOffsetMm` is how a child is moved.
    if (view->parentViewId() != kInvalidObjectId) return false;
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
    // NOT DIRTIED, for the reason setSheetScale gives: the curves are in
    // model millimetres and a scale does not change them.
    return true;
}

bool DrawingDocument::setViewShowsHiddenLines(ObjectId viewId, bool show) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterShowHidden = show;
    view->setShowsHiddenLines(show);
    recordDelta(edit, (show ? "Show hidden lines on " : "Hide hidden lines on ") + view->name());
    // THIS ONE DOES REPROJECT. Unlike the scale, it changes which edges are
    // computed at all -- the request asks the projector for them, so the
    // curves have to be built again.
    graph_.markDirty(viewId);
    return true;
}

bool DrawingDocument::setViewShowsTangentEdges(ObjectId viewId, bool show) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterShowTangent = show;
    view->setShowsTangentEdges(show);
    recordDelta(edit,
                (show ? "Show tangent edges on " : "Hide tangent edges on ") + view->name());
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
    return true;
}

// =============================================================================
// Authored geometry (M33)
// =============================================================================

namespace {
// The whole property set, before and after, filled the same way -- so four
// setters cannot each forget a different field.
void SnapshotEntityInto(const DrawingEntity& entity, DrawingEntityPropertyEdit& edit) {
    edit.entityId = entity.id();
    edit.beforeLayerId = entity.layerId();
    edit.afterLayerId = entity.layerId();
    edit.beforeColor = entity.color();
    edit.afterColor = entity.color();
    edit.beforeLinetype = entity.linetype();
    edit.afterLinetype = entity.linetype();
    edit.beforeLineweight = entity.lineweight();
    edit.afterLineweight = entity.lineweight();
}
} // namespace

DrawingEntity& DrawingDocument::addEntity(DrawShape shape) {
    // THE CURRENT LAYER, always -- see the header. `setCurrentLayer` already
    // refuses a frozen or locked one, so this cannot land somewhere the user
    // would not see it.
    auto item = std::make_unique<DrawingEntity>(std::move(shape), currentLayerId_);
    auto& ref = *item;
    ref.setSheetId(currentPageId_);
    entities_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    DrawingEntityExistenceEdit edit;
    edit.entityId = ref.id();
    edit.shape = ref.shape();
    edit.layerId = ref.layerId();
    edit.color = ref.color();
    edit.linetype = ref.linetype();
    edit.lineweight = ref.lineweight();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Draw " + std::string(ShapeName(ref.shape())));
    return ref;
}

DrawingEntity& DrawingDocument::restoreEntity(ObjectId id, DrawShape shape, ObjectId layerId,
                                              int color, std::string linetype, int lineweight) {
    requireUnusedId(id, "restoreEntity");
    auto item = std::make_unique<DrawingEntity>(id, std::move(shape), layerId, color,
                                                std::move(linetype), lineweight);
    auto& ref = *item;
    entities_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const DrawingEntity*> DrawingDocument::entities() const {
    std::vector<const DrawingEntity*> all;
    all.reserve(entities_.size());
    for (const std::unique_ptr<DrawingEntity>& one : entities_) all.push_back(one.get());
    return all;
}

const DrawingEntity* DrawingDocument::findEntity(ObjectId id) const noexcept {
    for (const std::unique_ptr<DrawingEntity>& one : entities_)
        if (one->id() == id) return one.get();
    return nullptr;
}

DrawingEntity* DrawingDocument::findEntityForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<DrawingEntity>& one : entities_)
        if (one->id() == id) return one.get();
    return nullptr;
}

std::vector<ObjectId> DrawingDocument::applySheetEdit(const std::vector<ObjectId>& consumed,
                                                     const SheetEditResult& made,
                                                     const std::string& label,
                                                     std::string* why) {
    const auto refuse = [&](const std::string& message) {
        if (why != nullptr) *why = message;
        return std::vector<ObjectId>{};
    };
    if (!made.ok) return refuse(made.why);
    // EVERY ID HAS TO BE REAL BEFORE ANYTHING IS TOUCHED. Half-applied, the
    // edit would delete two of three lines and then stop, and the undo step
    // would be labelled as though it had worked.
    for (const ObjectId id : consumed)
        if (findEntity(id) == nullptr)
            return refuse("one of the objects this edit was to replace is no longer here");

    const bool ownsStep = !isTransactionOpen() && !applyingHistory();
    if (ownsStep) beginTransaction(label);

    std::vector<ObjectId> arrived;
    arrived.reserve(made.shapes.size());
    // ADDED FIRST, THEN THE OLD ONES REMOVED.
    //
    // The other way round, a trim that consumes its only line leaves the
    // drawing momentarily empty, and anything watching -- a selection, a
    // canvas mid-paint -- sees a state the user never asked for.
    for (const DrawShape& shape : made.shapes) arrived.push_back(addEntity(shape).id());
    for (const ObjectId id : consumed) removeObject(id);

    if (ownsStep && !commitTransaction()) return refuse("that edit was refused");
    return arrived;
}

bool DrawingDocument::transformEntities(const std::vector<ObjectId>& ids,
                                        const Matrix2D& transform) {
    // ONE UNDO STEP FOR THE WHOLE SELECTION. Moving forty lines is one thing
    // the user did, and undoing it a line at a time would stop somewhere no
    // drawing was ever in.
    const bool ownsStep = !isTransactionOpen() && !applyingHistory();
    if (ownsStep) beginTransaction("Transform");
    bool any = false;
    for (const ObjectId id : ids) {
        DrawingEntity* entity = findEntityForEdit(id);
        if (entity == nullptr) continue;
        // A LOCKED LAYER MEANS LOCKED. Silently moving something on one is the
        // failure a lock exists to prevent.
        const Layer* layer = findLayer(entity->layerId());
        if (layer != nullptr && layer->isLocked()) continue;
        DrawingEntityShapeEdit edit;
        edit.entityId = id;
        edit.before = entity->shape();
        entity->applyTransform(transform);
        edit.after = entity->shape();
        recordDelta(edit, "Transform");
        any = true;
    }
    if (ownsStep) commitTransaction();
    return any;
}

std::vector<ObjectId> DrawingDocument::copyEntities(const std::vector<ObjectId>& ids,
                                                    const Matrix2D& transform) {
    std::vector<ObjectId> made;
    const bool ownsStep = !isTransactionOpen() && !applyingHistory();
    if (ownsStep) beginTransaction("Copy");
    for (const ObjectId id : ids) {
        const DrawingEntity* source = findEntity(id);
        if (source == nullptr) continue;
        // THE COPY KEEPS THE ORIGINAL'S LAYER AND OVERRIDES, not the current
        // layer's. A copy that landed somewhere else would be a copy that
        // looks different from what was copied.
        DrawingEntity& copy = addEntity(TransformShape(source->shape(), transform));
        setEntityLayer(copy.id(), source->layerId());
        setEntityColor(copy.id(), source->color());
        setEntityLinetype(copy.id(), source->linetype());
        setEntityLineweight(copy.id(), source->lineweight());
        made.push_back(copy.id());
    }
    if (ownsStep) commitTransaction();
    return made;
}

bool DrawingDocument::setEntityLayer(ObjectId entityId, ObjectId layerId) {
    DrawingEntity* entity = findEntityForEdit(entityId);
    if (entity == nullptr || findLayer(layerId) == nullptr) return false;
    DrawingEntityPropertyEdit edit;
    SnapshotEntityInto(*entity, edit);
    edit.afterLayerId = layerId;
    entity->setLayerId(layerId);
    recordDelta(edit, "Layer");
    return true;
}

bool DrawingDocument::setEntityColor(ObjectId entityId, int color) {
    DrawingEntity* entity = findEntityForEdit(entityId);
    if (entity == nullptr) return false;
    DrawingEntityPropertyEdit edit;
    SnapshotEntityInto(*entity, edit);
    edit.afterColor = color;
    entity->setColor(color);
    recordDelta(edit, "Colour");
    return true;
}

bool DrawingDocument::setEntityLinetype(ObjectId entityId, std::string linetype) {
    DrawingEntity* entity = findEntityForEdit(entityId);
    if (entity == nullptr) return false;
    // BYLAYER IS ALWAYS ALLOWED; anything else has to be in the table, or the
    // entity names a linetype no DXF reader can resolve.
    if (linetype != "BYLAYER" && linetype != "BYBLOCK" &&
        findLinetypeNamed(linetype) == nullptr)
        return false;
    DrawingEntityPropertyEdit edit;
    SnapshotEntityInto(*entity, edit);
    edit.afterLinetype = linetype;
    entity->setLinetype(std::move(linetype));
    recordDelta(edit, "Linetype");
    return true;
}

bool DrawingDocument::setEntityLineweight(ObjectId entityId, int lineweight) {
    DrawingEntity* entity = findEntityForEdit(entityId);
    if (entity == nullptr) return false;
    DrawingEntityPropertyEdit edit;
    SnapshotEntityInto(*entity, edit);
    edit.afterLineweight = lineweight;
    entity->setLineweight(lineweight);
    recordDelta(edit, "Lineweight");
    return true;
}

int DrawingDocument::resolvedColorOnLayer(int ownColor, ObjectId layerId) const {
    if (ownColor != kColorByLayer && ownColor != kColorByBlock) return ownColor;
    const Layer* layer = findLayer(layerId);
    return layer != nullptr ? layer->color() : 7;
}

bool DrawingDocument::layerIsVisible(ObjectId layerId) const {
    const Layer* layer = findLayer(layerId);
    return layer == nullptr || layer->isVisible();
}

int DrawingDocument::resolvedColorOf(const DrawingEntity& entity) const {
    return resolvedColorOnLayer(entity.color(), entity.layerId());
}

int DrawingDocument::resolvedColorOfDimension(const DrawingDimension& dimension) const {
    return resolvedColorOnLayer(kColorByLayer, dimension.layerId());
}

bool DrawingDocument::isDimensionVisible(const DrawingDimension& dimension) const {
    return layerIsVisible(dimension.layerId());
}

std::string DrawingDocument::resolvedLinetypeOf(const DrawingEntity& entity) const {
    if (entity.linetype() != "BYLAYER" && entity.linetype() != "BYBLOCK")
        return entity.linetype();
    const Layer* layer = findLayer(entity.layerId());
    return layer != nullptr ? layer->linetype() : std::string(kContinuousLinetypeName);
}

int DrawingDocument::resolvedLineweightOf(const DrawingEntity& entity) const {
    if (entity.lineweight() >= 0) return entity.lineweight();
    const Layer* layer = findLayer(entity.layerId());
    return layer != nullptr ? layer->lineweight() : kLineweightDefault;
}

bool DrawingDocument::isEntityVisible(const DrawingEntity& entity) const {
    return layerIsVisible(entity.layerId());
}

std::vector<ObjectId> DrawingDocument::entitiesNear(Vec2 point, double apertureMm) const {
    // NEAREST FIRST, so a caller taking the front gets what the user aimed at.
    std::vector<std::pair<double, ObjectId>> hits;
    for (const std::unique_ptr<DrawingEntity>& one : entities_) {
        const Layer* layer = findLayer(one->layerId());
        // NOT PICKABLE: invisible, or locked. A lock that still let things be
        // picked would be a lock in name only.
        if (layer != nullptr && (!layer->isVisible() || layer->isLocked())) continue;
        const double distance = one->distanceTo(point);
        if (distance <= apertureMm) hits.emplace_back(distance, one->id());
    }
    std::sort(hits.begin(), hits.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<ObjectId> ids;
    ids.reserve(hits.size());
    for (const auto& hit : hits) ids.push_back(hit.second);
    return ids;
}

std::vector<ObjectId> DrawingDocument::entitiesInWindow(const Box2D& window,
                                                        bool crossing) const {
    std::vector<ObjectId> ids;
    for (const std::unique_ptr<DrawingEntity>& one : entities_) {
        const Layer* layer = findLayer(one->layerId());
        if (layer != nullptr && (!layer->isVisible() || layer->isLocked())) continue;
        const Box2D box = one->bounds();
        // WINDOW is fully inside; CROSSING is merely touching. Two different
        // rules, and AutoCAD picks between them by which way the box was
        // dragged -- which is the shell's business, not this one's.
        if (crossing ? box.touches(window) : box.insideOf(window)) ids.push_back(one->id());
    }
    return ids;
}

// =============================================================================
// Dimensions (M34)
// =============================================================================

DimensionStyle& DrawingDocument::addDimensionStyle(std::string name) {
    if (name.empty()) throw std::invalid_argument("addDimensionStyle: a style needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addDimensionStyle: '" + name + "' is already taken");
    auto item = std::make_unique<DimensionStyle>(std::move(name));
    auto& ref = *item;
    dimensionStyles_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    DimensionStyleExistenceEdit edit;
    edit.styleId = ref.id();
    edit.name = ref.name();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Add style " + ref.name());
    return ref;
}

DimensionStyle& DrawingDocument::restoreDimensionStyle(ObjectId id, std::string name) {
    requireUnusedId(id, "restoreDimensionStyle");
    auto item = std::make_unique<DimensionStyle>(id, std::move(name));
    auto& ref = *item;
    dimensionStyles_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const DimensionStyle*> DrawingDocument::dimensionStyles() const {
    std::vector<const DimensionStyle*> all;
    all.reserve(dimensionStyles_.size());
    for (const std::unique_ptr<DimensionStyle>& one : dimensionStyles_) all.push_back(one.get());
    return all;
}

const DimensionStyle* DrawingDocument::findDimensionStyle(ObjectId id) const noexcept {
    for (const std::unique_ptr<DimensionStyle>& one : dimensionStyles_)
        if (one->id() == id) return one.get();
    return nullptr;
}

DimensionStyle* DrawingDocument::findDimensionStyleForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<DimensionStyle>& one : dimensionStyles_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const DimensionStyle* DrawingDocument::findDimensionStyleNamed(
    const std::string& name) const noexcept {
    for (const std::unique_ptr<DimensionStyle>& one : dimensionStyles_)
        if (one->name() == name) return one.get();
    return nullptr;
}

namespace {
// The whole style into the before-half of a delta, so a restore cannot be
// partial. Written once for the same reason SnapshotLayerInto is.
void SnapshotStyleInto(const DimensionStyle& style, DimensionStyleEdit& edit) {
    edit.styleId = style.id();
    edit.beforeTextHeightMm = edit.afterTextHeightMm = style.textHeightMm();
    edit.beforeArrowSizeMm = edit.afterArrowSizeMm = style.arrowSizeMm();
    edit.beforeTextGapMm = edit.afterTextGapMm = style.textGapMm();
    edit.beforeExtensionGapMm = edit.afterExtensionGapMm = style.extensionGapMm();
    edit.beforeExtensionOvershootMm = edit.afterExtensionOvershootMm =
        style.extensionOvershootMm();
    edit.beforeDecimals = edit.afterDecimals = style.decimals();
    edit.beforeSuffix = edit.afterSuffix = style.suffix();
    edit.beforeOverallScale = edit.afterOverallScale = style.overallScale();
}

void ApplyStyleHalf(DimensionStyle& style, double textHeight, double arrow, double textGap,
                    double extensionGap, double overshoot, int decimals,
                    const std::string& suffix, double overallScale) {
    style.setTextHeightMm(textHeight);
    style.setArrowSizeMm(arrow);
    style.setTextGapMm(textGap);
    style.setExtensionGapMm(extensionGap);
    style.setExtensionOvershootMm(overshoot);
    style.setDecimals(decimals);
    style.setSuffix(suffix);
    style.setOverallScale(overallScale);
}
} // namespace

bool DrawingDocument::editDimensionStyle(ObjectId styleId, const DimensionStyle& to) {
    DimensionStyle* style = findDimensionStyleForEdit(styleId);
    if (style == nullptr) return false;
    DimensionStyleEdit edit;
    SnapshotStyleInto(*style, edit);
    edit.afterTextHeightMm = to.textHeightMm();
    edit.afterArrowSizeMm = to.arrowSizeMm();
    edit.afterTextGapMm = to.textGapMm();
    edit.afterExtensionGapMm = to.extensionGapMm();
    edit.afterExtensionOvershootMm = to.extensionOvershootMm();
    edit.afterDecimals = to.decimals();
    edit.afterSuffix = to.suffix();
    edit.afterOverallScale = to.overallScale();
    // THE NAME IS NOT TOUCHED. Renaming goes through renameObject, which
    // checks uniqueness -- and a style editor that also renamed would be a
    // second path to a name, which is how two objects end up sharing one.
    ApplyStyleHalf(*style, to.textHeightMm(), to.arrowSizeMm(), to.textGapMm(),
                   to.extensionGapMm(), to.extensionOvershootMm(), to.decimals(), to.suffix(),
                   to.overallScale());
    recordDelta(edit, "Style " + style->name());
    return true;
}

bool DrawingDocument::setCurrentDimensionStyle(ObjectId styleId) {
    if (findDimensionStyle(styleId) == nullptr) return false;
    if (currentStyleId_ == styleId) return true;
    CurrentDimensionStyleEdit edit;
    edit.before = currentStyleId_;
    edit.after = styleId;
    currentStyleId_ = styleId;
    recordDelta(edit, "Current style");
    return true;
}

Annotation& DrawingDocument::addAnnotation(AnnotationBody body, DimensionAnchor anchor,
                                           Vec2 positionMm) {
    // REFUSED AT THE DOOR, the way a section and a hole table are. A symbol
    // that cannot be drawn would otherwise sit on the paper as a blank, and
    // the drawing would refuse to save later for a reason nobody connects to
    // this moment.
    if (const auto* frame = std::get_if<FeatureControlFrameSpec>(&body)) {
        const std::string why = WhyFrameRefused(*frame);
        if (!why.empty()) throw std::invalid_argument("addAnnotation: " + why);
        for (const DatumReference& reference : frame->datums) {
            const Annotation* datum = findAnnotation(reference.datumId);
            if (datum == nullptr || !datum->isDatum())
                throw std::invalid_argument(
                    "addAnnotation: this frame names something that is not a datum in this "
                    "drawing");
        }
    }
    if (const auto* finish = std::get_if<SurfaceFinishSpec>(&body)) {
        const std::string why = WhySurfaceFinishRefused(*finish);
        if (!why.empty()) throw std::invalid_argument("addAnnotation: " + why);
    }
    if (const auto* balloon = std::get_if<BalloonSpec>(&body)) {
        if (findBomTable(balloon->tableId) == nullptr)
            throw std::invalid_argument(
                "addAnnotation: a balloon carries a row number from a parts list, and that "
                "list is not in this drawing");
        if (balloon->sourceFile.empty())
            throw std::invalid_argument("addAnnotation: a balloon has to name a part");
    }

    auto item = std::make_unique<Annotation>(std::move(body), anchor, positionMm,
                                             currentLayerId_);
    auto& ref = *item;
    ref.setSheetId(currentPageId_);
    annotations_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    AnnotationExistenceEdit edit;
    edit.annotationId = ref.id();
    edit.body = ref.body();
    edit.anchor = ref.anchor();
    edit.xMm = positionMm.x;
    edit.yMm = positionMm.y;
    edit.layerId = ref.layerId();
    edit.addedByTheEdit = true;
    recordDelta(edit, ref.isDatum() ? "Datum" : (ref.isFrame() ? "Feature control frame"
                                                               : "Surface finish"));
    return ref;
}

Annotation& DrawingDocument::restoreAnnotation(ObjectId id, AnnotationBody body,
                                               DimensionAnchor anchor, Vec2 positionMm,
                                               ObjectId layerId) {
    auto item = std::make_unique<Annotation>(id, std::move(body), anchor, positionMm, layerId);
    auto& ref = *item;
    annotations_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const Annotation*> DrawingDocument::annotations() const {
    std::vector<const Annotation*> out;
    out.reserve(annotations_.size());
    for (const std::unique_ptr<Annotation>& one : annotations_) out.push_back(one.get());
    return out;
}

const Annotation* DrawingDocument::findAnnotation(ObjectId id) const noexcept {
    for (const std::unique_ptr<Annotation>& one : annotations_)
        if (one->id() == id) return one.get();
    return nullptr;
}

Annotation* DrawingDocument::findAnnotationForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<Annotation>& one : annotations_)
        if (one->id() == id) return one.get();
    return nullptr;
}

bool DrawingDocument::setAnnotationPosition(ObjectId id, Vec2 positionMm) {
    Annotation* annotation = findAnnotationForEdit(id);
    if (annotation == nullptr) return false;
    AnnotationEdit edit;
    edit.annotationId = id;
    edit.beforeBody = edit.afterBody = annotation->body();
    edit.beforeXMm = annotation->positionMm().x;
    edit.beforeYMm = annotation->positionMm().y;
    edit.afterXMm = positionMm.x;
    edit.afterYMm = positionMm.y;
    annotation->setPositionMm(positionMm);
    recordDelta(edit, "Move the symbol");
    return true;
}

bool DrawingDocument::setAnnotationBody(ObjectId id, AnnotationBody body) {
    Annotation* annotation = findAnnotationForEdit(id);
    if (annotation == nullptr) return false;
    // A BODY THAT CANNOT BE DRAWN IS REFUSED NOW, not at the next repaint. A
    // frame that goes blank some time later points at a decision the user has
    // stopped thinking about -- the same reason a hole refuses an unsizable
    // thread the moment it is typed (M39).
    if (const auto* frame = std::get_if<FeatureControlFrameSpec>(&body)) {
        if (!WhyFrameRefused(*frame).empty()) return false;
        // ...and every datum it names has to BE a datum in this drawing. A
        // frame pointing at a dimension, or at nothing, would draw a letter
        // that belongs to something else.
        for (const DatumReference& reference : frame->datums) {
            const Annotation* datum = findAnnotation(reference.datumId);
            if (datum == nullptr || !datum->isDatum()) return false;
        }
    }
    if (const auto* finish = std::get_if<SurfaceFinishSpec>(&body))
        if (!WhySurfaceFinishRefused(*finish).empty()) return false;

    AnnotationEdit edit;
    edit.annotationId = id;
    edit.beforeBody = annotation->body();
    edit.afterBody = body;
    edit.beforeXMm = edit.afterXMm = annotation->positionMm().x;
    edit.beforeYMm = edit.afterYMm = annotation->positionMm().y;
    annotation->setBody(std::move(body));
    recordDelta(edit, "Edit the symbol");
    return true;
}

std::string DrawingDocument::datumLetterOf(ObjectId annotationId) const {
    const Annotation* asked = findAnnotation(annotationId);
    if (asked == nullptr || !asked->isDatum()) return {};
    // IN DOCUMENT ORDER, so the first datum placed is A. Derived rather than
    // stored, for the reason M38's section letters are: the symbol on the face
    // and every frame that refers to it ask the same question, so they cannot
    // come back with different answers.
    int index = 0;
    for (const std::unique_ptr<Annotation>& one : annotations_) {
        if (!one->isDatum()) continue;
        if (one->id() == annotationId) {
            std::string letter;
            int number = index + 1;
            while (number > 0) {
                const int remainder = (number - 1) % 26;
                letter.insert(letter.begin(), static_cast<char>('A' + remainder));
                number = (number - 1) / 26;
            }
            return letter;
        }
        ++index;
    }
    return {};
}

std::size_t DrawingDocument::framesReferringToDatum(ObjectId datumId) const {
    std::size_t count = 0;
    for (const std::unique_ptr<Annotation>& one : annotations_) {
        const auto* frame = std::get_if<FeatureControlFrameSpec>(&one->body());
        if (frame == nullptr) continue;
        for (const DatumReference& reference : frame->datums)
            if (reference.datumId == datumId) {
                ++count;
                break;
            }
    }
    return count;
}

std::optional<Vec2> DrawingDocument::annotationLeaderTipMm(ObjectId annotationId) const {
    const Annotation* annotation = findAnnotation(annotationId);
    if (annotation == nullptr) return std::nullopt;
    // THE SAME RESOLVER A DIMENSION USES, so a symbol dangles under exactly
    // the conditions a dimension does and at exactly the same tolerance.
    return resolveAnchor(annotation->anchor());
}

std::string DrawingDocument::whyAnnotationRefused(ObjectId annotationId) const {
    const Annotation* annotation = findAnnotation(annotationId);
    if (annotation == nullptr) return "this symbol is no longer in the drawing";
    if (const auto* finish = std::get_if<SurfaceFinishSpec>(&annotation->body()))
        return WhySurfaceFinishRefused(*finish);
    if (const auto* frame = std::get_if<FeatureControlFrameSpec>(&annotation->body())) {
        const std::string why = WhyFrameRefused(*frame);
        if (!why.empty()) return why;
        for (const DatumReference& reference : frame->datums) {
            const Annotation* datum = findAnnotation(reference.datumId);
            if (datum == nullptr || !datum->isDatum())
                return "this frame refers to a datum that is no longer in the drawing";
        }
    }
    if (const auto* balloon = std::get_if<BalloonSpec>(&annotation->body())) {
        // A BALLOON WITH NO ROW TO READ IS THE FAILURE THIS IS FOR. Left to
        // draw an empty circle, or worse a stale number, it ties a part on the
        // picture to a line in the list that is not about it.
        const BomTable* table = findBomTable(balloon->tableId);
        if (table == nullptr)
            return "this balloon reads a parts list that is no longer in the drawing";
        if (balloon->sourceFile.empty())
            return "this balloon names no part, so there is no row for it to carry";
        const BomContents rows = countBom(*table);
        if (!rows.ok)
            return "this balloon's parts list could not be counted: " + rows.why;
        for (const BomRow& row : rows.rows)
            if (row.sourcePath == balloon->sourceFile && row.partName == balloon->partName)
                return {};
        return "this balloon names a part that is not in the parts list it reads";
    }
    if (const auto* weld = std::get_if<WeldSymbolSpec>(&annotation->body()))
        return WhyWeldRefused(*weld);
    return {};
}

std::string DrawingDocument::annotationText(ObjectId annotationId) const {
    const Annotation* annotation = findAnnotation(annotationId);
    if (annotation == nullptr) return {};
    if (!whyAnnotationRefused(annotationId).empty()) return {};
    if (const auto* finish = std::get_if<SurfaceFinishSpec>(&annotation->body()))
        return SurfaceFinishText(*finish);
    if (const auto* frame = std::get_if<FeatureControlFrameSpec>(&annotation->body())) {
        // THE LETTERS COME FROM THE DATUMS THEMSELVES, resolved here and now.
        // This is the one place a frame's letters are worked out, which is what
        // makes it impossible for the frame and the symbol to disagree.
        std::vector<std::string> letters;
        letters.reserve(frame->datums.size());
        for (const DatumReference& reference : frame->datums)
            letters.push_back(datumLetterOf(reference.datumId));
        return FrameText(*frame, letters);
    }
    if (const auto* balloon = std::get_if<BalloonSpec>(&annotation->body())) {
        // THE NUMBER IS THE LIST'S ANSWER, asked for now. This is the one
        // place a balloon's number comes from, which is what makes it
        // impossible for the circle on the picture and the row in the table to
        // carry different item numbers.
        const BomTable* table = findBomTable(balloon->tableId);
        if (table == nullptr) return {};
        const BomContents rows = countBom(*table);
        for (const BomRow& row : rows.rows)
            if (row.sourcePath == balloon->sourceFile && row.partName == balloon->partName)
                return std::to_string(row.item);
        return {};
    }
    if (const auto* weld = std::get_if<WeldSymbolSpec>(&annotation->body()))
        return WeldSymbolText(*weld);
    return datumLetterOf(annotationId);
}

DrawingDimension& DrawingDocument::addDimension(DimensionKind kind, DimensionAnchor first,
                                                DimensionAnchor second, Vec2 linePositionMm) {
    auto item = std::make_unique<DrawingDimension>(kind, first, second, linePositionMm,
                                                   currentStyleId_, currentLayerId_);
    auto& ref = *item;
    ref.setSheetId(currentPageId_);
    dimensions_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);

    DimensionExistenceEdit edit;
    edit.dimensionId = ref.id();
    edit.kind = static_cast<int>(ref.kind());
    edit.first = ref.first();
    edit.second = ref.second();
    edit.direction = static_cast<int>(ref.direction());
    edit.lineXMm = linePositionMm.x;
    edit.lineYMm = linePositionMm.y;
    edit.styleId = ref.styleId();
    edit.layerId = ref.layerId();
    edit.textOverride = ref.textOverride();
    edit.addedByTheEdit = true;
    recordDelta(edit, "Dimension");
    return ref;
}

DrawingDimension& DrawingDocument::restoreDimension(ObjectId id, DimensionKind kind,
                                                    DimensionAnchor first,
                                                    DimensionAnchor second,
                                                    LinearDirection direction,
                                                    Vec2 linePositionMm, ObjectId styleId,
                                                    ObjectId layerId,
                                                    std::string textOverride) {
    requireUnusedId(id, "restoreDimension");
    auto item = std::make_unique<DrawingDimension>(id, kind, first, second, linePositionMm,
                                                   styleId, layerId);
    auto& ref = *item;
    ref.setDirection(direction);
    ref.setTextOverride(std::move(textOverride));
    dimensions_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const DrawingDimension*> DrawingDocument::dimensions() const {
    std::vector<const DrawingDimension*> all;
    all.reserve(dimensions_.size());
    for (const std::unique_ptr<DrawingDimension>& one : dimensions_) all.push_back(one.get());
    return all;
}

const DrawingDimension* DrawingDocument::findDimension(ObjectId id) const noexcept {
    for (const std::unique_ptr<DrawingDimension>& one : dimensions_)
        if (one->id() == id) return one.get();
    return nullptr;
}

DrawingDimension* DrawingDocument::findDimensionForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<DrawingDimension>& one : dimensions_)
        if (one->id() == id) return one.get();
    return nullptr;
}

namespace {
void SnapshotDimensionInto(const DrawingDimension& dimension, DimensionEdit& edit) {
    edit.dimensionId = dimension.id();
    edit.beforeDirection = edit.afterDirection = static_cast<int>(dimension.direction());
    edit.beforeXMm = edit.afterXMm = dimension.linePositionMm().x;
    edit.beforeYMm = edit.afterYMm = dimension.linePositionMm().y;
    edit.beforeStyleId = edit.afterStyleId = dimension.styleId();
    edit.beforeText = edit.afterText = dimension.textOverride();
}
} // namespace

bool DrawingDocument::setDimensionDirection(ObjectId dimensionId, LinearDirection direction) {
    DrawingDimension* dimension = findDimensionForEdit(dimensionId);
    if (dimension == nullptr) return false;
    DimensionEdit edit;
    SnapshotDimensionInto(*dimension, edit);
    edit.afterDirection = static_cast<int>(direction);
    dimension->setDirection(direction);
    recordDelta(edit, "Dimension direction");
    return true;
}

bool DrawingDocument::setDimensionLinePosition(ObjectId dimensionId, Vec2 at) {
    DrawingDimension* dimension = findDimensionForEdit(dimensionId);
    if (dimension == nullptr) return false;
    DimensionEdit edit;
    SnapshotDimensionInto(*dimension, edit);
    edit.afterXMm = at.x;
    edit.afterYMm = at.y;
    dimension->setLinePositionMm(at);
    recordDelta(edit, "Move dimension");
    return true;
}

bool DrawingDocument::setDimensionTextOverride(ObjectId dimensionId, std::string text) {
    DrawingDimension* dimension = findDimensionForEdit(dimensionId);
    if (dimension == nullptr) return false;
    DimensionEdit edit;
    SnapshotDimensionInto(*dimension, edit);
    edit.afterText = text;
    dimension->setTextOverride(std::move(text));
    recordDelta(edit, "Dimension text");
    return true;
}

bool DrawingDocument::setDimensionStyleOf(ObjectId dimensionId, ObjectId styleId) {
    DrawingDimension* dimension = findDimensionForEdit(dimensionId);
    if (dimension == nullptr || findDimensionStyle(styleId) == nullptr) return false;
    DimensionEdit edit;
    SnapshotDimensionInto(*dimension, edit);
    edit.afterStyleId = styleId;
    dimension->setStyleId(styleId);
    recordDelta(edit, "Dimension style");
    return true;
}

std::optional<Vec2> DrawingDocument::resolveAnchor(const DimensionAnchor& anchor) const {
    if (anchor.kind == DimensionAnchorKind::Free) return anchor.at;

    if (anchor.kind == DimensionAnchorKind::Entity) {
        const DrawingEntity* entity = findEntity(anchor.entityId);
        if (entity == nullptr) return std::nullopt;
        const std::vector<SnapCandidate> points = StaticSnapPointsOf(entity->shape());
        if (anchor.snapIndex < 0 ||
            static_cast<std::size_t>(anchor.snapIndex) >= points.size())
            return std::nullopt;
        return points[static_cast<std::size_t>(anchor.snapIndex)].at;
    }

    // IN A VIEW: the model point, re-found in the projection, then carried onto
    // the paper.
    //
    // THIS IS STILL NOT A TOPOLOGICAL NAME, and is not claimed as one -- a
    // projected curve does not yet carry which model edge it came from. What
    // it is, since M43, is a search that can no longer land somewhere else
    // quietly. Two rules do that:
    //
    //   THE ROLE HAS TO MATCH. A hole's centre only ever re-finds a centre.
    //   Without this, a diameter dimension whose bore moved a little could
    //   re-attach to a corner of the plate -- measuring a real distance
    //   between two real points and printing a plausible number that is not
    //   the dimension anybody put there.
    //
    //   AMBIGUITY IS REFUSED. If a second point of the same kind is nearer
    //   than twice the distance the first one moved, there is no way to tell
    //   which was meant, and the dimension dangles LOUDLY instead of picking.
    //   A point that did not move at all is never ambiguous, which is the
    //   ordinary case and stays free.
    const DrawingView* view = findView(anchor.viewId);
    if (view == nullptr || view->currentState() != ComputeState::Valid) return std::nullopt;

    // BOTH START AT INFINITY, not at the tolerance -- and it has to be BOTH.
    //
    // Started at the tolerance, "no rival at all" and "a rival exactly at the
    // edge of reach" are the same number -- and the ambiguity test below then
    // refuses a perfectly ordinary lone candidate that moved a little. Which
    // it did, the first time this ran.
    //
    // Changing only ONE of them back is an equivalent mutation and the gate
    // says so: whichever is left at infinity overwrites the other on the first
    // candidate. The defect needs the pair, which is how it was written.
    double bestDistance = std::numeric_limits<double>::infinity();
    double runnerUp = std::numeric_limits<double>::infinity();
    std::optional<Vec2> bestModelPoint;
    const auto offer = [&](Vec2 candidate, ViewPointRole role) {
        if (role != anchor.role) return;
        const double distance = std::hypot(candidate.x - anchor.at.x, candidate.y - anchor.at.y);
        if (distance > anchor.toleranceMm) return;
        // A POINT IS NOT ITS OWN RIVAL. Every edge that meets at a corner
        // offers that corner, so the same position arrives two or three times
        // -- and counted as competition it makes every corner in the drawing
        // ambiguous with itself. Which is exactly what happened the first time
        // this ran: a lone corner 0.57 mm away was refused because its own
        // twin was also 0.57 mm away.
        if (bestModelPoint.has_value() &&
            std::hypot(candidate.x - bestModelPoint->x, candidate.y - bestModelPoint->y) <
                1e-9)
            return;
        if (distance < bestDistance) {
            runnerUp = bestDistance;
            bestDistance = distance;
            bestModelPoint = candidate;
        } else if (distance < runnerUp) {
            runnerUp = distance;
        }
    };
    for (const ProjectedCurve& curve : view->projected().curves) {
        if (const auto* line = std::get_if<ProjectedLine>(&curve.shape)) {
            offer(line->a, ViewPointRole::Corner);
            offer(line->b, ViewPointRole::Corner);
            offer(Vec2{(line->a.x + line->b.x) / 2.0, (line->a.y + line->b.y) / 2.0},
                  ViewPointRole::Middle);
        } else if (const auto* arc = std::get_if<ProjectedArc>(&curve.shape)) {
            // THE CENTRE IS A SNAP POINT and it is the one a diameter is put
            // on -- a hole's centre is what a drafter points at, and it is not
            // on the curve at all.
            offer(arc->centre, ViewPointRole::Centre);
            offer(Vec2{arc->centre.x + arc->radius * std::cos(arc->startAngle),
                       arc->centre.y + arc->radius * std::sin(arc->startAngle)},
                  ViewPointRole::CurveEnd);
            offer(Vec2{arc->centre.x + arc->radius * std::cos(arc->endAngle),
                       arc->centre.y + arc->radius * std::sin(arc->endAngle)},
                  ViewPointRole::CurveEnd);
        } else if (const auto* polyline = std::get_if<ProjectedPolyline>(&curve.shape)) {
            for (const Vec2 point : polyline->points) offer(point, ViewPointRole::Corner);
        }
    }
    if (!bestModelPoint.has_value()) return std::nullopt;
    // TWICE THE DISTANCE IT MOVED. An anchor whose point is exactly where it
    // was left has bestDistance 0, so nothing else can be ambiguous with it;
    // one whose point moved 2 mm needs its nearest rival to be at least 4 mm
    // away before the answer is clear.
    if (runnerUp < 2.0 * bestDistance) return std::nullopt;

    // MODEL MILLIMETRES TIMES THE SCALE, plus where the view sits -- through
    // viewPointToSheetMm, which is now the only place that multiplication
    // happens, and the reason the MEASUREMENT below is taken in model space
    // before this.
    return viewPointToSheetMm(anchor.viewId, *bestModelPoint);
}

// =============================================================================
// The frame and the title block (M35)
// =============================================================================

namespace {

std::vector<TitleBlockFieldRecord> RecordOf(const TitleBlock& block) {
    std::vector<TitleBlockFieldRecord> out;
    out.reserve(block.fields().size());
    for (const TitleBlockField& field : block.fields())
        out.push_back(TitleBlockFieldRecord{field.label, field.value,
                                            static_cast<int>(field.source)});
    return out;
}

} // namespace

// =============================================================================
// Schematic (M36)
// =============================================================================

namespace {

std::vector<double> FlattenPoints(const std::vector<Vec2>& points) {
    std::vector<double> out;
    out.reserve(points.size() * 2);
    for (const Vec2 point : points) {
        out.push_back(point.x);
        out.push_back(point.y);
    }
    return out;
}

std::vector<Vec2> UnflattenPoints(const std::vector<double>& values) {
    std::vector<Vec2> out;
    out.reserve(values.size() / 2);
    for (std::size_t i = 0; i + 1 < values.size(); i += 2)
        out.push_back(Vec2{values[i], values[i + 1]});
    return out;
}

} // namespace

SymbolPlacement& DrawingDocument::addSymbol(std::string tag, std::string symbolName,
                                            Vec2 positionMm) {
    if (tag.empty()) throw std::invalid_argument("addSymbol: a component needs a tag");
    // TWO PARTS CANNOT SHARE A TAG. The tag is what every cross-reference and
    // wiring list points at, and two -K1s on one schematic is a wiring list
    // that sends an electrician to whichever one they find first.
    if (nameIsTaken(tag, kInvalidObjectId))
        throw std::invalid_argument("addSymbol: this drawing already has something called " +
                                    tag);
    if (symbolName.empty())
        throw std::invalid_argument("addSymbol: a component needs a symbol to be drawn as");

    auto item = std::make_unique<SymbolPlacement>(std::move(tag), std::move(symbolName),
                                                  positionMm, currentLayerId_);
    auto& ref = *item;
    SymbolExistenceEdit edit;
    edit.symbolId = ref.id();
    edit.tag = ref.tag();
    edit.symbolName = ref.symbolName();
    edit.xMm = positionMm.x;
    edit.yMm = positionMm.y;
    edit.layerId = ref.layerId();
    edit.addedByTheEdit = true;
    symbols_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    recordDelta(edit, "Place " + ref.tag());
    return ref;
}

SymbolPlacement& DrawingDocument::restoreSymbol(ObjectId id, std::string tag,
                                                std::string symbolName, Vec2 positionMm,
                                                double rotationRad, bool mirrored,
                                                ObjectId layerId) {
    requireUnusedId(id, "restoreSymbol");
    auto item = std::make_unique<SymbolPlacement>(id, std::move(tag), std::move(symbolName),
                                                  positionMm, rotationRad, mirrored, layerId);
    auto& ref = *item;
    symbols_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const SymbolPlacement*> DrawingDocument::symbols() const {
    std::vector<const SymbolPlacement*> out;
    out.reserve(symbols_.size());
    for (const auto& one : symbols_) out.push_back(one.get());
    return out;
}

const SymbolPlacement* DrawingDocument::findSymbol(ObjectId id) const noexcept {
    for (const auto& one : symbols_)
        if (one->id() == id) return one.get();
    return nullptr;
}

const SymbolPlacement* DrawingDocument::findSymbolTagged(const std::string& tag) const noexcept {
    for (const auto& one : symbols_)
        if (one->tag() == tag) return one.get();
    return nullptr;
}

SymbolPlacement* DrawingDocument::findSymbolForEdit(ObjectId id) noexcept {
    for (auto& one : symbols_)
        if (one->id() == id) return one.get();
    return nullptr;
}

namespace {

void SnapshotSymbolInto(const SymbolPlacement& symbol, SymbolPlacementEdit& edit) {
    edit.symbolId = symbol.id();
    edit.beforeXMm = edit.afterXMm = symbol.positionMm().x;
    edit.beforeYMm = edit.afterYMm = symbol.positionMm().y;
    edit.beforeRotationRad = edit.afterRotationRad = symbol.rotationRad();
    edit.beforeMirrored = edit.afterMirrored = symbol.isMirrored();
    edit.beforeTag = edit.afterTag = symbol.tag();
}

} // namespace

bool DrawingDocument::setSymbolPosition(ObjectId symbolId, Vec2 at) {
    SymbolPlacement* symbol = findSymbolForEdit(symbolId);
    if (symbol == nullptr) return false;
    SymbolPlacementEdit edit;
    SnapshotSymbolInto(*symbol, edit);
    edit.afterXMm = at.x;
    edit.afterYMm = at.y;
    symbol->setPositionMm(at);
    recordDelta(edit, "Move " + symbol->tag());
    return true;
}

bool DrawingDocument::setSymbolRotation(ObjectId symbolId, double radians) {
    SymbolPlacement* symbol = findSymbolForEdit(symbolId);
    if (symbol == nullptr) return false;
    SymbolPlacementEdit edit;
    SnapshotSymbolInto(*symbol, edit);
    edit.afterRotationRad = radians;
    symbol->setRotationRad(radians);
    recordDelta(edit, "Turn " + symbol->tag());
    return true;
}

bool DrawingDocument::setSymbolMirrored(ObjectId symbolId, bool mirrored) {
    SymbolPlacement* symbol = findSymbolForEdit(symbolId);
    if (symbol == nullptr || symbol->isMirrored() == mirrored) return false;
    SymbolPlacementEdit edit;
    SnapshotSymbolInto(*symbol, edit);
    edit.afterMirrored = mirrored;
    symbol->setMirrored(mirrored);
    recordDelta(edit, "Flip " + symbol->tag());
    return true;
}

WireEntity& DrawingDocument::addWire(std::vector<Vec2> pointsMm) {
    // A WIRE NEEDS TWO POINTS. One is a click somebody did not finish, and it
    // would sit on the sheet connecting nothing while looking like nothing.
    if (pointsMm.size() < 2)
        throw std::invalid_argument("addWire: a wire needs at least two points");
    auto item = std::make_unique<WireEntity>(pointsMm, currentLayerId_);
    auto& ref = *item;
    WireExistenceEdit edit;
    edit.wireId = ref.id();
    edit.pointsXY = FlattenPoints(pointsMm);
    edit.layerId = ref.layerId();
    edit.addedByTheEdit = true;
    wires_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    recordDelta(edit, "Draw wire");
    return ref;
}

WireEntity& DrawingDocument::restoreWire(ObjectId id, std::vector<Vec2> pointsMm,
                                         ObjectId layerId, std::string label) {
    requireUnusedId(id, "restoreWire");
    auto item = std::make_unique<WireEntity>(id, std::move(pointsMm), layerId,
                                             std::move(label));
    auto& ref = *item;
    wires_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const WireEntity*> DrawingDocument::wires() const {
    std::vector<const WireEntity*> out;
    out.reserve(wires_.size());
    for (const auto& one : wires_) out.push_back(one.get());
    return out;
}

const WireEntity* DrawingDocument::findWire(ObjectId id) const noexcept {
    for (const auto& one : wires_)
        if (one->id() == id) return one.get();
    return nullptr;
}

WireEntity* DrawingDocument::findWireForEdit(ObjectId id) noexcept {
    for (auto& one : wires_)
        if (one->id() == id) return one.get();
    return nullptr;
}

bool DrawingDocument::setWirePoints(ObjectId wireId, std::vector<Vec2> pointsMm) {
    WireEntity* wire = findWireForEdit(wireId);
    if (wire == nullptr || pointsMm.size() < 2) return false;
    WireEdit edit;
    edit.wireId = wireId;
    edit.beforePointsXY = FlattenPoints(wire->pointsMm());
    edit.beforeLabel = edit.afterLabel = wire->label();
    wire->setPointsMm(std::move(pointsMm));
    edit.afterPointsXY = FlattenPoints(wire->pointsMm());
    recordDelta(edit, "Move wire");
    return true;
}

bool DrawingDocument::setWireLabel(ObjectId wireId, std::string label) {
    WireEntity* wire = findWireForEdit(wireId);
    if (wire == nullptr) return false;
    WireEdit edit;
    edit.wireId = wireId;
    edit.beforePointsXY = edit.afterPointsXY = FlattenPoints(wire->pointsMm());
    edit.beforeLabel = wire->label();
    edit.afterLabel = label;
    wire->setLabel(std::move(label));
    recordDelta(edit, "Label wire");
    return true;
}

Netlist DrawingDocument::netlist() const {
    std::vector<WireRun> runs;
    for (const auto& wire : wires_)
        if (wire->isDrawable()) runs.push_back(wire->asRun());
    std::vector<PlacedSymbol> placed;
    for (const auto& symbol : symbols_) placed.push_back(symbol->asPlaced());

    Netlist built = BuildNetlist(runs, placed, BuiltInSymbols());

    // THE NAME COMES OFF THE WIRES. A net is derived and has nowhere to keep
    // one, so numbering writes it onto the wires and this reads it back.
    //
    // WHEN TWO WIRES IN ONE NET DISAGREE, the FIRST is taken and the
    // disagreement is reported separately (conflictingNetNames) -- picking
    // silently would hide that the schematic says two things about one wire,
    // and refusing to name the net at all would lose the name a user typed.
    for (Net& net : built.nets) {
        for (const ObjectId id : net.wires) {
            const WireEntity* wire = findWire(id);
            if (wire == nullptr || wire->label().empty()) continue;
            if (net.name.empty()) net.name = wire->label();
        }
    }
    return built;
}

std::vector<std::string> DrawingDocument::conflictingNetNames() const {
    std::vector<std::string> clashes;
    for (const Net& net : netlist().nets) {
        std::string first;
        for (const ObjectId id : net.wires) {
            const WireEntity* wire = findWire(id);
            if (wire == nullptr || wire->label().empty()) continue;
            if (first.empty()) {
                first = wire->label();
                continue;
            }
            if (wire->label() == first) continue;
            // Said as the pair, because "there is a conflict" is not something
            // a user can act on and "L1 and L2 are the same wire" is.
            clashes.push_back(first + " and " + wire->label());
            break;
        }
    }
    return clashes;
}

std::size_t DrawingDocument::numberNets(const std::string& prefix) {
    Netlist built = netlist();
    NumberNets(built, prefix);

    // ONE UNDO STEP FOR THE WHOLE SHEET. Numbering is one thing the user did,
    // and undoing it a wire at a time would stop somewhere no schematic was
    // ever in.
    const bool ownsStep = !isTransactionOpen() && !applyingHistory();
    if (ownsStep) beginTransaction("Number nets");
    std::size_t named = 0;
    for (const Net& net : built.nets) {
        if (net.name.empty()) continue;
        for (const ObjectId id : net.wires) {
            const WireEntity* wire = findWire(id);
            if (wire == nullptr || wire->label() == net.name) continue;
            if (setWireLabel(id, net.name)) ++named;
        }
    }
    if (ownsStep) commitTransaction();
    return named;
}

// =============================================================================
// The parts list (M35.6)
// =============================================================================

namespace {

std::vector<int> ColumnsAsInts(const std::vector<BomColumn>& columns) {
    std::vector<int> out;
    out.reserve(columns.size());
    for (const BomColumn column : columns) out.push_back(static_cast<int>(column));
    return out;
}

std::vector<BomColumn> ColumnsFromInts(const std::vector<int>& values) {
    std::vector<BomColumn> out;
    out.reserve(values.size());
    for (const int value : values) out.push_back(static_cast<BomColumn>(value));
    return out;
}

void SnapshotBomInto(const BomTable& table, BomEdit& edit) {
    edit.tableId = table.id();
    edit.beforeXMm = edit.afterXMm = table.positionMm().x;
    edit.beforeYMm = edit.afterYMm = table.positionMm().y;
    edit.beforeDepth = edit.afterDepth = static_cast<int>(table.depth());
    edit.beforeRowHeightMm = edit.afterRowHeightMm = table.rowHeightMm();
    edit.beforeGrowsUpward = edit.afterGrowsUpward = table.growsUpward();
    edit.beforeColumns = edit.afterColumns = ColumnsAsInts(table.columns());
}

} // namespace

// --- THE REVISION HISTORY (M48) ---------------------------------------------

std::string DrawingDocument::nextRevisionLetter() const {
    return NextRevisionLetter(revisions_.empty() ? std::string_view{}
                                                 : std::string_view(revisions_.back().letter));
}

std::string DrawingDocument::currentRevision() const {
    // THE LAST ROW, and nothing when there are none. An unissued drawing is
    // not at Rev A: it is at no revision, and printing A would be a claim
    // nobody made about a drawing nobody has released.
    return revisions_.empty() ? std::string{} : revisions_.back().letter;
}

std::string DrawingDocument::whyRevisionRefused(const Revision& revision) const {
    return WhyRevisionRefused(revision, revisions_);
}

void DrawingDocument::restoreRevision(Revision revision, std::size_t at) {
    // PUT BACK WHERE IT WAS. Appending on undo would reorder a history without
    // saying so, and the order is what "the latest issue" means.
    const std::size_t where = at < revisions_.size() ? at : revisions_.size();
    revisions_.insert(revisions_.begin() + static_cast<std::ptrdiff_t>(where),
                      std::move(revision));
}

bool DrawingDocument::addRevision(Revision revision) {
    if (!whyRevisionRefused(revision).empty()) return false;
    RevisionExistenceEdit edit;
    edit.letter = revision.letter;
    edit.description = revision.description;
    edit.date = revision.date;
    edit.by = revision.by;
    edit.at = revisions_.size();
    edit.addedByTheEdit = true;
    const std::string letter = revision.letter;
    revisions_.push_back(std::move(revision));
    recordDelta(edit, "Issue Rev " + letter);
    return true;
}

bool DrawingDocument::removeRevision(const std::string& letter) {
    for (std::size_t i = 0; i < revisions_.size(); ++i) {
        if (revisions_[i].letter != letter) continue;
        RevisionExistenceEdit edit;
        edit.letter = revisions_[i].letter;
        edit.description = revisions_[i].description;
        edit.date = revisions_[i].date;
        edit.by = revisions_[i].by;
        edit.at = i;
        edit.addedByTheEdit = false;
        revisions_.erase(revisions_.begin() + static_cast<std::ptrdiff_t>(i));
        recordDelta(edit, "Withdraw Rev " + letter);
        return true;
    }
    return false;
}

RevisionTable& DrawingDocument::addRevisionTable(std::string name, Vec2 positionMm) {
    if (name.empty())
        throw std::invalid_argument("addRevisionTable: a revision table needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument(
            "addRevisionTable: this drawing already has something called " + name);
    // NO SOURCE PATH AND NO ROWS. Unlike a parts list, which counts a file,
    // this reads the drawing it is on -- so there is nothing to name and
    // nothing that can go missing.
    auto item = std::make_unique<RevisionTable>(std::move(name), positionMm, currentLayerId_);
    auto& ref = *item;
    ref.setSheetId(currentPageId_);
    RevisionTableExistenceEdit edit;
    edit.tableId = ref.id();
    edit.name = ref.name();
    edit.xMm = positionMm.x;
    edit.yMm = positionMm.y;
    edit.widthMm = ref.widthMm();
    edit.rowHeightMm = ref.rowHeightMm();
    edit.addedByTheEdit = true;
    revisionTables_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    recordDelta(edit, "Add revision table " + ref.name());
    return ref;
}

RevisionTable& DrawingDocument::restoreRevisionTable(ObjectId id, std::string name,
                                                     Vec2 positionMm, double widthMm,
                                                     double rowHeightMm) {
    requireUnusedId(id, "restoreRevisionTable");
    auto item = std::make_unique<RevisionTable>(id, std::move(name), positionMm, widthMm,
                                                rowHeightMm, currentLayerId_);
    auto& ref = *item;
    revisionTables_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const RevisionTable*> DrawingDocument::revisionTables() const {
    std::vector<const RevisionTable*> out;
    out.reserve(revisionTables_.size());
    for (const auto& table : revisionTables_) out.push_back(table.get());
    return out;
}

const RevisionTable* DrawingDocument::findRevisionTable(ObjectId id) const noexcept {
    for (const auto& table : revisionTables_)
        if (table->id() == id) return table.get();
    return nullptr;
}

RevisionTable* DrawingDocument::findRevisionTableForEdit(ObjectId id) noexcept {
    for (auto& table : revisionTables_)
        if (table->id() == id) return table.get();
    return nullptr;
}

bool DrawingDocument::setRevisionTablePosition(ObjectId tableId, Vec2 at) {
    RevisionTable* table = findRevisionTableForEdit(tableId);
    if (table == nullptr) return false;
    RevisionTableEdit edit;
    edit.tableId = table->id();
    edit.beforeXMm = table->positionMm().x;
    edit.beforeYMm = table->positionMm().y;
    edit.afterXMm = at.x;
    edit.afterYMm = at.y;
    table->setPositionMm(at);
    recordDelta(edit, "Move " + table->name());
    return true;
}

std::string DrawingDocument::titleBlockValue(const TitleBlockField& field) const {
    // THE ONE CALLER. Everything a block derives is known here and nowhere
    // else all at once, which is what stops a painter, a plot and a DXF write
    // from printing three different revisions.
    return titleBlock_.valueOf(field, sheet(), currentSheetNumber(), sheetCount(),
                               currentRevision());
}

HoleTable& DrawingDocument::addHoleTable(std::string name, ObjectId viewId, Vec2 positionMm,
                                         Vec2 datumMm) {
    if (name.empty()) throw std::invalid_argument("addHoleTable: a hole table needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addHoleTable: this drawing already has something called " +
                                    name);
    // A TABLE OF WHICH VIEW'S HOLES. Without one it has nothing to read and no
    // page to measure positions on, and would sit on the paper as an empty box
    // nobody could explain.
    if (findView(viewId) == nullptr)
        throw std::invalid_argument("addHoleTable: a hole table is a table of a view's holes, "
                                    "and that view is not in this drawing");

    auto item = std::make_unique<HoleTable>(std::move(name), viewId, positionMm);
    auto& ref = *item;
    ref.setDatumMm(datumMm);
    HoleTableExistenceEdit edit;
    edit.tableId = ref.id();
    edit.name = ref.name();
    edit.viewId = viewId;
    edit.xMm = positionMm.x;
    edit.yMm = positionMm.y;
    edit.datumXMm = datumMm.x;
    edit.datumYMm = datumMm.y;
    edit.addedByTheEdit = true;
    ref.setSheetId(currentPageId_);
    holeTables_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    recordDelta(edit, "Add hole table " + ref.name());
    return ref;
}

HoleTable& DrawingDocument::restoreHoleTable(ObjectId id, std::string name, ObjectId viewId,
                                             Vec2 positionMm, Vec2 datumMm,
                                             std::vector<HoleColumn> columns,
                                             double rowHeightMm) {
    auto item = std::make_unique<HoleTable>(id, std::move(name), viewId, positionMm);
    auto& ref = *item;
    ref.setDatumMm(datumMm);
    if (!columns.empty()) ref.setColumns(std::move(columns));
    ref.setRowHeightMm(rowHeightMm);
    holeTables_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const HoleTable*> DrawingDocument::holeTables() const {
    std::vector<const HoleTable*> out;
    out.reserve(holeTables_.size());
    for (const std::unique_ptr<HoleTable>& one : holeTables_) out.push_back(one.get());
    return out;
}

const HoleTable* DrawingDocument::findHoleTable(ObjectId id) const noexcept {
    for (const std::unique_ptr<HoleTable>& one : holeTables_)
        if (one->id() == id) return one.get();
    return nullptr;
}

HoleTable* DrawingDocument::findHoleTableForEdit(ObjectId id) noexcept {
    for (const std::unique_ptr<HoleTable>& one : holeTables_)
        if (one->id() == id) return one.get();
    return nullptr;
}

bool DrawingDocument::setHoleTablePosition(ObjectId id, Vec2 positionMm) {
    HoleTable* table = findHoleTableForEdit(id);
    if (table == nullptr) return false;
    HoleTableEdit edit;
    edit.tableId = id;
    edit.beforeXMm = table->positionMm().x;
    edit.beforeYMm = table->positionMm().y;
    edit.beforeDatumXMm = edit.afterDatumXMm = table->datumMm().x;
    edit.beforeDatumYMm = edit.afterDatumYMm = table->datumMm().y;
    edit.afterXMm = positionMm.x;
    edit.afterYMm = positionMm.y;
    table->setPositionMm(positionMm);
    recordDelta(edit, "Move the hole table");
    return true;
}

bool DrawingDocument::setHoleTableDatum(ObjectId id, Vec2 datumMm) {
    HoleTable* table = findHoleTableForEdit(id);
    if (table == nullptr) return false;
    HoleTableEdit edit;
    edit.tableId = id;
    edit.beforeXMm = edit.afterXMm = table->positionMm().x;
    edit.beforeYMm = edit.afterYMm = table->positionMm().y;
    edit.beforeDatumXMm = table->datumMm().x;
    edit.beforeDatumYMm = table->datumMm().y;
    edit.afterDatumXMm = datumMm.x;
    edit.afterDatumYMm = datumMm.y;
    table->setDatumMm(datumMm);
    // MOVING THE DATUM REWRITES EVERY ROW, because every position is measured
    // from it. Nothing is stored, so nothing has to be recomputed -- the rows
    // are counted the next time anybody asks.
    recordDelta(edit, "Move the hole table's datum");
    return true;
}

bool DrawingDocument::removeHoleTable(ObjectId id) {
    // ONE DELETION PATH. Written twice -- here and in removeOwnObject -- the
    // two would drift, and a table deleted through the menu would record a
    // different undo step from one deleted with the key. That is the shape of
    // defect this project keeps closing, so this is a name for the other one.
    if (findHoleTable(id) == nullptr) return false;
    return removeObject(id);
}

HoleTableContents DrawingDocument::holesOf(const HoleTable& table) const {
    const DrawingView* view = findView(table.viewId());
    if (view == nullptr) {
        HoleTableContents out;
        out.why = "this hole table's view is no longer in the drawing";
        return out;
    }
    // THE VIEW'S FILE AND THE VIEW'S DIRECTION, asked for rather than stored.
    // A table that kept its own copy of either could describe a part the view
    // is not of, and every row in it would still be a correct row about
    // something.
    return HolesOfPart(view->sourcePath(), view->direction(), table.datumMm());
}

BomTable& DrawingDocument::addBomTable(std::string name, std::string sourcePath,
                                       Vec2 positionMm) {
    if (name.empty()) throw std::invalid_argument("addBomTable: a parts list needs a name");
    if (nameIsTaken(name, kInvalidObjectId))
        throw std::invalid_argument("addBomTable: this drawing already has something called " +
                                    name);
    // A LIST THAT NAMES NO FILE HAS NOTHING TO COUNT, and would sit on the
    // paper as an empty box nobody could explain.
    if (sourcePath.empty())
        throw std::invalid_argument("addBomTable: a parts list needs an assembly to count");

    auto item = std::make_unique<BomTable>(std::move(name), sourcePath, positionMm);
    auto& ref = *item;
    ref.setSourceStamp(SourceFileStamp(sourcePath));
    BomExistenceEdit edit;
    edit.tableId = ref.id();
    edit.name = ref.name();
    edit.sourcePath = ref.sourcePath();
    edit.xMm = positionMm.x;
    edit.yMm = positionMm.y;
    edit.addedByTheEdit = true;
    ref.setSheetId(currentPageId_);
    bomTables_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    recordDelta(edit, "Add parts list " + ref.name());
    return ref;
}

BomTable& DrawingDocument::restoreBomTable(ObjectId id, std::string name,
                                           std::string sourcePath, Vec2 positionMm,
                                           BomDepth depth, std::vector<BomColumn> columns,
                                           double rowHeightMm, bool growsUpward,
                                           long long sourceStamp) {
    requireUnusedId(id, "restoreBomTable");
    auto item = std::make_unique<BomTable>(id, std::move(name), std::move(sourcePath),
                                           positionMm);
    auto& ref = *item;
    ref.setDepth(depth);
    if (!columns.empty()) ref.setColumns(std::move(columns));
    ref.setRowHeightMm(rowHeightMm);
    ref.setGrowsUpward(growsUpward);
    ref.setSourceStamp(sourceStamp);
    bomTables_.push_back(std::move(item));
    registry_.registerObject(ref.id(), &ref);
    return ref;
}

std::vector<const BomTable*> DrawingDocument::bomTables() const {
    std::vector<const BomTable*> out;
    out.reserve(bomTables_.size());
    for (const auto& table : bomTables_) out.push_back(table.get());
    return out;
}

const BomTable* DrawingDocument::findBomTable(ObjectId id) const noexcept {
    for (const auto& table : bomTables_)
        if (table->id() == id) return table.get();
    return nullptr;
}

BomTable* DrawingDocument::findBomTableForEdit(ObjectId id) noexcept {
    for (auto& table : bomTables_)
        if (table->id() == id) return table.get();
    return nullptr;
}

bool DrawingDocument::setBomPosition(ObjectId tableId, Vec2 at) {
    BomTable* table = findBomTableForEdit(tableId);
    if (table == nullptr) return false;
    BomEdit edit;
    SnapshotBomInto(*table, edit);
    edit.afterXMm = at.x;
    edit.afterYMm = at.y;
    table->setPositionMm(at);
    recordDelta(edit, "Move " + table->name());
    return true;
}

bool DrawingDocument::setBomDepth(ObjectId tableId, BomDepth depth) {
    BomTable* table = findBomTableForEdit(tableId);
    if (table == nullptr || table->depth() == depth) return false;
    BomEdit edit;
    SnapshotBomInto(*table, edit);
    edit.afterDepth = static_cast<int>(depth);
    table->setDepth(depth);
    recordDelta(edit, "Parts list depth");
    return true;
}

bool DrawingDocument::setBomColumns(ObjectId tableId, std::vector<BomColumn> columns) {
    BomTable* table = findBomTableForEdit(tableId);
    if (table == nullptr) return false;
    BomEdit edit;
    SnapshotBomInto(*table, edit);
    if (!table->setColumns(std::move(columns))) return false;
    edit.afterColumns = ColumnsAsInts(table->columns());
    recordDelta(edit, "Parts list columns");
    return true;
}

bool DrawingDocument::setBomRowHeightMm(ObjectId tableId, double rowHeightMm) {
    BomTable* table = findBomTableForEdit(tableId);
    if (table == nullptr) return false;
    BomEdit edit;
    SnapshotBomInto(*table, edit);
    if (!table->setRowHeightMm(rowHeightMm)) return false;
    edit.afterRowHeightMm = table->rowHeightMm();
    recordDelta(edit, "Parts list rows");
    return true;
}

bool DrawingDocument::setBomGrowsUpward(ObjectId tableId, bool upward) {
    BomTable* table = findBomTableForEdit(tableId);
    if (table == nullptr || table->growsUpward() == upward) return false;
    BomEdit edit;
    SnapshotBomInto(*table, edit);
    edit.afterGrowsUpward = upward;
    table->setGrowsUpward(upward);
    recordDelta(edit, "Parts list direction");
    return true;
}

BomContents DrawingDocument::countBom(const BomTable& table) const {
    BomContents out;
    std::ifstream file(table.sourcePath(), std::ios::binary);
    if (!file) {
        // SAID, not returned empty. An empty parts list and one whose file has
        // gone look identical on paper, and only one of them is a drawing
        // somebody can build from.
        out.why = "the assembly this list counts is not where the drawing says it is";
        return out;
    }
    const AssemblyLoadResult loaded = loadAssemblyDocument(file);
    if (!loaded) {
        out.why = std::string("the assembly this list counts could not be read: ") +
                  loaded.message;
        return out;
    }
    return CountAssembly(*loaded.document, table.depth());
}

std::vector<ObjectId> DrawingDocument::staleBomTables() const {
    std::vector<ObjectId> stale;
    for (const auto& table : bomTables_) {
        // THE CONTENT, not the modification time (M32.4): two saves inside one
        // filesystem tick are indistinguishable by mtime, and a parts list
        // that quietly stopped noticing changes is the failure this whole
        // design exists to prevent.
        if (SourceFileStamp(table->sourcePath()) != table->sourceStamp())
            stale.push_back(table->id());
    }
    return stale;
}

bool DrawingDocument::markBomCounted(ObjectId tableId) {
    BomTable* table = findBomTableForEdit(tableId);
    if (table == nullptr) return false;
    table->setSourceStamp(SourceFileStamp(table->sourcePath()));
    return true;
}

double DrawingDocument::viewScaleFactor(ObjectId viewId) const noexcept {
    const DrawingView* view = findView(viewId);
    if (view == nullptr) return 1.0;
    // NO SECOND GUARD HERE.
    //
    // A factor of zero would collapse the view to a point and a negative one
    // would mirror it -- but DrawingScale::factor() already answers 1.0 for
    // any scale that is not valid(), so it cannot hand back either. A first
    // draft repeated the check anyway; a mutation deleting it survived, which
    // is what dead defensive code looks like from the outside. Two guards
    // where one is unreachable is the same "kept by hand, tested apart" shape
    // this project keeps closing, one size down.
    //
    // What an impossible scale actually does is pinned by M35_DXF_013.
    return view->effectiveScale(sheet().scale()).factor();
}

Vec2 DrawingDocument::viewPointToSheetMm(ObjectId viewId, Vec2 modelMm) const noexcept {
    const DrawingView* view = findView(viewId);
    // AN EQUIVALENT GUARD, kept on purpose and recorded as such (M35-42).
    //
    // Deleting this line changes nothing today: viewPositionMm answers {0, 0}
    // for an unknown view and viewScaleFactor answers 1, so the arithmetic
    // below already gives the point back unchanged. A mutation removing it
    // survives, and that is the honest result rather than a gap -- contriving
    // a test that could only pass would be measuring nothing.
    //
    // It stays because it says what the function means for a caller who has no
    // view, and because it stops that meaning depending on two other
    // functions' fallbacks staying what they are.
    if (view == nullptr) return modelMm;
    const Vec2 at = viewPositionMm(viewId);
    const double factor = viewScaleFactor(viewId);
    // THE BREAK IS PART OF THE MAPPING, NOT PART OF THE MODEL (M50). Folded
    // here, in the one place model millimetres become paper -- so everything
    // drawn is short and everything measured, which comes back through
    // sheetPointToViewMm, is not.
    const Vec2 folded = FoldPointMm(modelMm, view->breakSpan());
    return Vec2{at.x + folded.x * factor, at.y + folded.y * factor};
}

Vec2 DrawingDocument::sheetPointToViewMm(ObjectId viewId, Vec2 sheetMm) const noexcept {
    const DrawingView* view = findView(viewId);
    if (view == nullptr) return sheetMm;
    const Vec2 at = viewPositionMm(viewId);
    const double factor = viewScaleFactor(viewId);
    if (std::fabs(factor) < 1e-12) return sheetMm;
    const Vec2 folded{(sheetMm.x - at.x) / factor, (sheetMm.y - at.y) / factor};
    return UnfoldPointMm(folded, view->breakSpan());
}

std::string DrawingDocument::whyBreakRefused(ObjectId viewId, const BreakSpan& span) const {
    const DrawingView* view = findView(viewId);
    if (view == nullptr) return "that view is not in this drawing";
    // AGAINST THE PART'S OWN REACH, along the axis the break runs on. Asked of
    // the projection, so a break is judged against what is actually drawn
    // rather than against what somebody remembers the part being.
    const ProjectedExtent& extent = view->projected().extent;
    const double from = span.horizontal ? extent.min.x : extent.min.y;
    const double to = span.horizontal ? extent.max.x : extent.max.y;
    return WhyBreakRefused(span, from, to);
}

bool DrawingDocument::setBreakSpan(ObjectId viewId, double fromMm, double toMm,
                                   bool horizontal, double gapMm) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr) return false;
    BreakSpan span;
    span.active = true;
    span.fromMm = fromMm;
    span.toMm = toMm;
    span.horizontal = horizontal;
    span.gapMm = gapMm;
    if (!whyBreakRefused(viewId, span).empty()) return false;

    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterBreakActive = true;
    edit.afterBreakFromMm = fromMm;
    edit.afterBreakToMm = toMm;
    edit.afterBreakHorizontal = horizontal;
    edit.afterBreakGapMm = gapMm;
    view->setBreakSpan(span);
    recordDelta(edit, "Break " + view->name());
    // NOTHING IS REPROJECTED. A break changes how the curves reach the paper,
    // not what they are -- which is the whole point, and why this does not
    // mark the view dirty.
    return true;
}

double DrawingDocument::suggestedBreakGapMm(ObjectId viewId) const noexcept {
    constexpr double kOnThePaperMm = 6.0;
    const double factor = viewScaleFactor(viewId);
    if (std::fabs(factor) < 1e-12) return kOnThePaperMm;
    return kOnThePaperMm / factor;
}

std::vector<ProjectedCurve> DrawingDocument::drawableCurves(ObjectId viewId) const {
    const DrawingView* view = findView(viewId);
    if (view == nullptr) return {};
    return SplitAtBreak(view->projected().curves, view->breakSpan());
}

bool DrawingDocument::clearBreakSpan(ObjectId viewId) {
    DrawingView* view = findViewForEdit(viewId);
    if (view == nullptr || !view->breakSpan().active) return false;
    DrawingViewPlacementEdit edit;
    SnapshotViewInto(*view, edit);
    edit.afterBreakActive = false;
    view->setBreakSpan(BreakSpan{});
    recordDelta(edit, "Unbreak " + view->name());
    return true;
}

SheetFrameGeometry DrawingDocument::frame() const noexcept {
    // COMPUTED, EVERY TIME. There is nothing to keep in step because there is
    // nothing kept.
    return FrameOf(sheet(), frameMargins(), frameZoneTargetMm());
}

Vec2 DrawingDocument::titleBlockOriginMm() const noexcept {
    const SheetFrameGeometry border = frame();
    // WITHOUT A FRAME, hard against the paper's own bottom-right corner. A
    // title block that vanished because the margins were wrong would take the
    // drawing's identity with it.
    const Vec2 corner = border.ok ? border.innerMaxMm
                                  : Vec2{sheet().widthMm(), 0.0};
    const double bottom = border.ok ? border.innerMinMm.y : 0.0;
    return Vec2{corner.x - titleBlock().widthMm(), bottom};
}

bool DrawingDocument::setTitleBlockField(const std::string& label, std::string value) {
    TitleBlockEdit edit;
    edit.before = RecordOf(blockForEdit());
    edit.beforeWidthMm = edit.afterWidthMm = blockForEdit().widthMm();
    edit.beforeRowHeightMm = edit.afterRowHeightMm = blockForEdit().rowHeightMm();
    edit.beforeVisible = edit.afterVisible = blockForEdit().isVisible();
    // REFUSED FOR A DERIVED FIELD, by the block itself -- and nothing is
    // recorded, so the undo history does not fill with edits that changed
    // nothing.
    if (!blockForEdit().setField(label, std::move(value))) return false;
    edit.after = RecordOf(blockForEdit());
    recordDelta(edit, "Title block");
    return true;
}

bool DrawingDocument::addTitleBlockField(std::string label, TitleBlockSource source) {
    TitleBlockEdit edit;
    edit.before = RecordOf(blockForEdit());
    edit.beforeWidthMm = edit.afterWidthMm = blockForEdit().widthMm();
    edit.beforeRowHeightMm = edit.afterRowHeightMm = blockForEdit().rowHeightMm();
    edit.beforeVisible = edit.afterVisible = blockForEdit().isVisible();
    if (!blockForEdit().addField(std::move(label), source)) return false;
    edit.after = RecordOf(blockForEdit());
    recordDelta(edit, "Title block");
    return true;
}

bool DrawingDocument::removeTitleBlockField(const std::string& label) {
    TitleBlockEdit edit;
    edit.before = RecordOf(blockForEdit());
    edit.beforeWidthMm = edit.afterWidthMm = blockForEdit().widthMm();
    edit.beforeRowHeightMm = edit.afterRowHeightMm = blockForEdit().rowHeightMm();
    edit.beforeVisible = edit.afterVisible = blockForEdit().isVisible();
    if (!blockForEdit().removeField(label)) return false;
    edit.after = RecordOf(blockForEdit());
    recordDelta(edit, "Title block");
    return true;
}

bool DrawingDocument::setTitleBlockSize(double widthMm, double rowHeightMm) {
    TitleBlockEdit edit;
    edit.before = edit.after = RecordOf(blockForEdit());
    edit.beforeWidthMm = blockForEdit().widthMm();
    edit.beforeRowHeightMm = blockForEdit().rowHeightMm();
    edit.beforeVisible = edit.afterVisible = blockForEdit().isVisible();
    // BOTH OR NEITHER. A block that took the new width and kept the old row
    // height would draw a box whose lines do not meet, and the half that
    // failed would be the half nobody looked at.
    if (!blockForEdit().setWidthMm(widthMm)) return false;
    if (!blockForEdit().setRowHeightMm(rowHeightMm)) {
        blockForEdit().setWidthMm(edit.beforeWidthMm);
        return false;
    }
    edit.afterWidthMm = blockForEdit().widthMm();
    edit.afterRowHeightMm = blockForEdit().rowHeightMm();
    recordDelta(edit, "Title block size");
    return true;
}

bool DrawingDocument::setTitleBlockVisible(bool visible) {
    if (blockForEdit().isVisible() == visible) return false;
    TitleBlockEdit edit;
    edit.before = edit.after = RecordOf(blockForEdit());
    edit.beforeWidthMm = edit.afterWidthMm = blockForEdit().widthMm();
    edit.beforeRowHeightMm = edit.afterRowHeightMm = blockForEdit().rowHeightMm();
    edit.beforeVisible = blockForEdit().isVisible();
    blockForEdit().setVisible(visible);
    edit.afterVisible = visible;
    recordDelta(edit, "Title block");
    return true;
}

bool DrawingDocument::setFrameMargins(const FrameMargins& margins) {
    // REFUSED WHEN THEY DO NOT FIT, here rather than at draw time. A margin
    // wider than the paper leaves no inside, and finding that out when the
    // frame silently stops drawing is finding it out too late.
    if (!margins.fitsOn(paperForEdit().widthMm(), paperForEdit().heightMm())) return false;
    SheetFrameEdit edit;
    edit.beforeBindingMm = currentPageForEdit().marginsForEdit().bindingMm;
    edit.beforeOtherMm = currentPageForEdit().marginsForEdit().otherMm;
    edit.afterBindingMm = margins.bindingMm;
    edit.afterOtherMm = margins.otherMm;
    edit.beforeZoneTargetMm = edit.afterZoneTargetMm = currentPageForEdit().zoneForEdit();
    edit.beforeVisible = edit.afterVisible = currentPageForEdit().frameShownForEdit();
    currentPageForEdit().marginsForEdit() = margins;
    recordDelta(edit, "Frame");
    return true;
}

bool DrawingDocument::setFrameZoneTargetMm(double zoneTargetMm) {
    if (!(zoneTargetMm > 0.0)) return false;
    SheetFrameEdit edit;
    edit.beforeBindingMm = edit.afterBindingMm = currentPageForEdit().marginsForEdit().bindingMm;
    edit.beforeOtherMm = edit.afterOtherMm = currentPageForEdit().marginsForEdit().otherMm;
    edit.beforeZoneTargetMm = currentPageForEdit().zoneForEdit();
    edit.afterZoneTargetMm = zoneTargetMm;
    edit.beforeVisible = edit.afterVisible = currentPageForEdit().frameShownForEdit();
    currentPageForEdit().zoneForEdit() = zoneTargetMm;
    recordDelta(edit, "Frame zones");
    return true;
}

bool DrawingDocument::setFrameVisible(bool visible) {
    if (currentPageForEdit().frameShownForEdit() == visible) return false;
    SheetFrameEdit edit;
    edit.beforeBindingMm = edit.afterBindingMm = currentPageForEdit().marginsForEdit().bindingMm;
    edit.beforeOtherMm = edit.afterOtherMm = currentPageForEdit().marginsForEdit().otherMm;
    edit.beforeZoneTargetMm = edit.afterZoneTargetMm = currentPageForEdit().zoneForEdit();
    edit.beforeVisible = currentPageForEdit().frameShownForEdit();
    edit.afterVisible = visible;
    currentPageForEdit().frameShownForEdit() = visible;
    recordDelta(edit, "Frame");
    return true;
}

void DrawingDocument::restoreTitleBlock(TitleBlock block) { blockForEdit() = std::move(block); }

void DrawingDocument::restoreFrame(const FrameMargins& margins, double zoneTargetMm,
                                   bool visible) {
    currentPageForEdit().marginsForEdit() = margins;
    if (zoneTargetMm > 0.0) currentPageForEdit().zoneForEdit() = zoneTargetMm;
    currentPageForEdit().frameShownForEdit() = visible;
}

DrawingDocument::DimensionProposal DrawingDocument::proposeDimension(
    DimensionKind kind, const std::vector<ObjectId>& entityIds) const {
    DimensionProposal out;
    const auto refuse = [&out](std::string why) {
        out.why = std::move(why);
        return out;
    };
    if (entityIds.empty()) return refuse("nothing is selected to dimension");
    if (entityIds.size() > 2)
        return refuse("a dimension measures one thing or two, not " +
                      std::to_string(entityIds.size()));

    std::vector<const DrawingEntity*> picked;
    for (const ObjectId id : entityIds) {
        const DrawingEntity* entity = findEntity(id);
        if (entity == nullptr) return refuse("something in the selection is not drawn geometry");
        picked.push_back(entity);
    }
    const auto snapsOf = [](const DrawingEntity& entity) {
        return StaticSnapPointsOf(entity.shape());
    };

    // WHERE THE DIMENSION LINE STARTS OUT: clear of what it measures, by a
    // fixed offset, so a new dimension is never born sitting on top of the
    // geometry. The user drags it where they want it; this only has to be
    // somewhere they can see it.
    constexpr double kBornClearMm = 12.0;

    switch (kind) {
        case DimensionKind::Radius:
        case DimensionKind::Diameter: {
            if (picked.size() != 1)
                return refuse("a radius or a diameter measures one round thing");
            const bool round = std::holds_alternative<DrawCircle>(picked[0]->shape()) ||
                               std::holds_alternative<DrawArc>(picked[0]->shape());
            if (!round)
                return refuse("a radius or a diameter needs a circle or an arc, and this is "
                              "neither");
            // Index 0 is the centre and index 1 is a point ON the curve, for a
            // circle and for an arc alike -- which is what StaticSnapPointsOf
            // guarantees, and why this does not go looking for them itself.
            const std::vector<SnapCandidate> points = snapsOf(*picked[0]);
            if (points.size() < 2) return refuse("that curve has no rim to measure to");
            out.first = DimensionAnchor::onEntity(picked[0]->id(), 0);
            out.second = DimensionAnchor::onEntity(picked[0]->id(), 1);
            const Vec2 rim = points[1].at;
            out.linePositionMm = Vec2{rim.x + kBornClearMm, rim.y + kBornClearMm};
            break;
        }
        case DimensionKind::Angular: {
            if (picked.size() != 2)
                return refuse("an angle is between two things, so two must be selected");
            const std::vector<SnapCandidate> a = snapsOf(*picked[0]);
            const std::vector<SnapCandidate> b = snapsOf(*picked[1]);
            if (a.empty() || b.empty()) return refuse("one of those has no point to measure to");
            out.first = DimensionAnchor::onEntity(picked[0]->id(), 0);
            out.second = DimensionAnchor::onEntity(picked[1]->id(), 0);
            // THE VERTEX IS WHERE THE TWO LINES CROSS when they do, because
            // that is the angle a reader means. Only when they are parallel --
            // where there is no crossing -- does it fall back to the midpoint,
            // and then the angle it reports is nearly nothing, which is the
            // truth about two parallel lines.
            const auto* first = std::get_if<DrawLine>(&picked[0]->shape());
            const auto* second = std::get_if<DrawLine>(&picked[1]->shape());
            std::optional<Vec2> crossing;
            if (first != nullptr && second != nullptr)
                crossing = LineLineIntersection(first->a, first->b, second->a, second->b, false);
            out.linePositionMm =
                crossing.value_or(Vec2{(a[0].at.x + b[0].at.x) / 2.0,
                                       (a[0].at.y + b[0].at.y) / 2.0});
            break;
        }
        case DimensionKind::Linear: {
            if (picked.size() == 2) {
                const std::vector<SnapCandidate> a = snapsOf(*picked[0]);
                const std::vector<SnapCandidate> b = snapsOf(*picked[1]);
                if (a.empty() || b.empty())
                    return refuse("one of those has no point to measure from");
                out.first = DimensionAnchor::onEntity(picked[0]->id(), 0);
                out.second = DimensionAnchor::onEntity(picked[1]->id(), 0);
                out.linePositionMm = Vec2{(a[0].at.x + b[0].at.x) / 2.0,
                                          (a[0].at.y + b[0].at.y) / 2.0 - kBornClearMm};
                break;
            }
            // ONE THING: its two ends. A circle's "ends" would be two
            // quadrants, which measures a diameter through the long way round
            // -- so a round thing is sent back to ask for the dimension that
            // actually says what it means.
            if (std::holds_alternative<DrawCircle>(picked[0]->shape()))
                return refuse("a circle is measured by its diameter or its radius, not by a "
                              "length across it");
            const std::vector<SnapCandidate> points = snapsOf(*picked[0]);
            std::vector<int> ends;
            for (std::size_t i = 0; i < points.size(); ++i)
                if (points[i].mode == SnapMode::Endpoint) ends.push_back(static_cast<int>(i));
            if (ends.size() < 2) return refuse("that has no two ends to measure between");
            out.first = DimensionAnchor::onEntity(picked[0]->id(), ends.front());
            out.second = DimensionAnchor::onEntity(picked[0]->id(), ends.back());
            const Vec2 from = points[static_cast<std::size_t>(ends.front())].at;
            const Vec2 to = points[static_cast<std::size_t>(ends.back())].at;
            out.linePositionMm =
                Vec2{(from.x + to.x) / 2.0, (from.y + to.y) / 2.0 - kBornClearMm};
            break;
        }
    }
    out.ok = true;
    return out;
}

DimensionMeasurement DrawingDocument::measure(const DrawingDimension& dimension) const {
    DimensionMeasurement out;
    const std::optional<Vec2> first = resolveAnchor(dimension.first());
    const std::optional<Vec2> second = resolveAnchor(dimension.second());
    if (!first.has_value() || !second.has_value()) {
        out.why = "this dimension has lost what it was measuring";
        return out;
    }
    out.firstMm = *first;
    out.secondMm = *second;

    // THE MEASUREMENT IS IN MODEL MILLIMETRES, always -- the size of the PART.
    //
    // The anchors come back in sheet millimetres because that is where they
    // are drawn, so a dimension inside a view has to come back the other way.
    // THAT IS ONE FUNCTION (M50), not a division: sheetPointToViewMm undoes
    // the position, the scale AND the break, in the same order the forward
    // mapping applied them.
    //
    // It used to divide by the scale here. That was right until a view could
    // be BROKEN, and then it would have measured the FOLDED bar: 600 mm of
    // steel dimensioned as 203, with every other number on the drawing
    // agreeing. Going back through the inverse is what makes the true length
    // true by construction rather than by a rule somebody remembers.
    const ObjectId viewId = dimension.first().kind == DimensionAnchorKind::InView
                                ? dimension.first().viewId
                                : dimension.second().viewId;
    const Vec2 firstModel = sheetPointToViewMm(viewId, *first);
    const Vec2 secondModel = sheetPointToViewMm(viewId, *second);

    const double dx = secondModel.x - firstModel.x;
    const double dy = secondModel.y - firstModel.y;

    switch (dimension.kind()) {
        case DimensionKind::Linear:
            switch (dimension.direction()) {
                case LinearDirection::Aligned: out.valueMm = std::hypot(dx, dy); break;
                case LinearDirection::Horizontal: out.valueMm = std::fabs(dx); break;
                case LinearDirection::Vertical: out.valueMm = std::fabs(dy); break;
            }
            break;
        case DimensionKind::Radius:
            // THE TWO ANCHORS ARE THE CENTRE AND A POINT ON THE CURVE, so the
            // radius is the distance between them. Storing a radius instead
            // would be storing a measurement, which is the thing this whole
            // design refuses to do.
            out.valueMm = std::hypot(dx, dy);
            break;
        case DimensionKind::Diameter: out.valueMm = 2.0 * std::hypot(dx, dy); break;
        case DimensionKind::Angular: {
            // BETWEEN THE TWO ANCHORS, ABOUT THE DIMENSION LINE'S POSITION.
            // The vertex is where the user dragged the arc to, which is how
            // AutoCAD's DIMANGULAR behaves for two points.
            const Vec2 vertex = dimension.linePositionMm();
            const double a = std::atan2(first->y - vertex.y, first->x - vertex.x);
            const double b = std::atan2(second->y - vertex.y, second->x - vertex.x);
            double between = std::fabs(b - a);
            constexpr double kTwoPiLocal = 6.283185307179586476925286766559;
            if (between > kTwoPiLocal / 2.0) between = kTwoPiLocal - between;
            // IN DEGREES, because that is what a drawing says. The one
            // conversion, here.
            out.valueMm = between * 180.0 / (kTwoPiLocal / 2.0);
            break;
        }
    }
    out.ok = true;
    return out;
}

bool DrawingDocument::setDimensionTolerance(ObjectId dimensionId,
                                            DimensionTolerance tolerance) {
    DrawingDimension* found = findDimensionForEdit(dimensionId);
    if (found == nullptr) return false;
    // A FIT THIS BUILD CANNOT COMPUTE IS REFUSED HERE, not accepted and shown
    // blank. The user asked for a fit; letting it through would leave a
    // drawing that looks toleranced and specifies nothing.
    if (tolerance.kind == ToleranceKind::Fit) {
        const DimensionMeasurement measured = measure(*found);
        if (!measured.ok) return false;
        if (!FitDeviation(measured.valueMm, tolerance.fitCode).has_value()) return false;
    }
    DimensionToleranceEdit edit;
    edit.dimensionId = dimensionId;
    edit.beforeKind = static_cast<int>(found->tolerance().kind);
    edit.beforeUpperMm = found->tolerance().upperMm;
    edit.beforeLowerMm = found->tolerance().lowerMm;
    edit.beforeFitCode = found->tolerance().fitCode;
    edit.beforeDecimals = found->tolerance().decimals;
    edit.afterKind = static_cast<int>(tolerance.kind);
    edit.afterUpperMm = tolerance.upperMm;
    edit.afterLowerMm = tolerance.lowerMm;
    edit.afterFitCode = tolerance.fitCode;
    edit.afterDecimals = tolerance.decimals;
    found->setTolerance(std::move(tolerance));
    recordDelta(edit, "Tolerance");
    return true;
}

bool DrawingDocument::setGeneralToleranceClass(GeneralToleranceClass klass) {
    if (generalTolerance_ == klass) return false;
    GeneralToleranceEdit edit;
    edit.before = static_cast<int>(generalTolerance_);
    edit.after = static_cast<int>(klass);
    generalTolerance_ = klass;
    recordDelta(edit, "General tolerance");
    return true;
}

std::string DrawingDocument::generalToleranceNote() const {
    return GeneralToleranceNote(generalTolerance_);
}

std::optional<Deviations> DrawingDocument::dimensionFit(
    const DrawingDimension& dimension) const {
    if (dimension.tolerance().kind != ToleranceKind::Fit) return std::nullopt;
    const DimensionMeasurement measured = measure(dimension);
    if (!measured.ok) return std::nullopt;
    // DERIVED FROM THE SIZE THE DIMENSION CURRENTLY READS. That is the whole
    // reason a fit stores its code: change the model, the size changes, and
    // the deviations follow -- an H7 on a 25 bore that became a 30 bore is
    // still an H7, and its numbers are not the ones it had yesterday.
    return FitDeviation(measured.valueMm, dimension.tolerance().fitCode);
}

bool DrawingDocument::dimensionIsBasic(const DrawingDimension& dimension) const noexcept {
    return dimension.tolerance().kind == ToleranceKind::Basic;
}

std::string DrawingDocument::dimensionToleranceText(const DrawingDimension& dimension) const {
    const DimensionTolerance& tolerance = dimension.tolerance();
    if (tolerance.kind == ToleranceKind::None || tolerance.kind == ToleranceKind::Basic)
        return {};

    const DimensionStyle* style = findDimensionStyle(dimension.styleId());

    // HOW MANY DECIMALS THE TOLERANCE NEEDS.
    //
    // One more than the size is the starting point: a tolerance shown to fewer
    // decimals than the size it qualifies rounds away the thing it exists to
    // state.
    //
    // BUT IT MUST NEVER ROUND TO ZERO. On a sheet whose style shows whole
    // millimetres, one more decimal is one decimal, and an H7 at 60 mm prints
    // as "0.0/0.0" -- a drawing that states a fit and shows no tolerance,
    // which is worse than one that shows none at all because it looks
    // finished. So the count grows until the deviation actually appears.
    //
    // Found by the self test, on a sheet a previous check had restyled to zero
    // decimals.
    const auto decimalsFor = [&](double largest) {
        if (tolerance.decimals >= 0) return tolerance.decimals;
        int decimals = style != nullptr ? style->decimals() + 1 : 3;
        // Four is the cap: ISO tables are in micrometres, which is three
        // decimals of a millimetre, and a fifth digit would be printing noise.
        while (decimals < 4 && largest > 0.0 &&
               largest < 0.5 * std::pow(10.0, -decimals))
            ++decimals;
        return decimals;
    };

    const auto printerFor = [&](double a, double b) {
        DimensionStyle printer{"tolerance"};
        printer.setDecimals(decimalsFor(std::max(std::fabs(a), std::fabs(b))));
        return printer;
    };

    if (tolerance.kind == ToleranceKind::Fit) {
        const std::optional<Deviations> fit = dimensionFit(dimension);
        // A FIT THIS BUILD CANNOT COMPUTE SAYS SO, loudly, where its numbers
        // would have been. Printing the code alone would look finished.
        if (!fit.has_value()) return tolerance.fitCode + " <?>";
        const DimensionStyle printer = printerFor(fit->upperMm, fit->lowerMm);
        return tolerance.fitCode + " " + printer.format(fit->upperMm) + "/" +
               printer.format(fit->lowerMm);
    }
    if (tolerance.kind == ToleranceKind::Symmetric) {
        const DimensionStyle printer = printerFor(tolerance.upperMm, tolerance.upperMm);
        return "\xC2\xB1" + printer.format(std::fabs(tolerance.upperMm));
    }
    if (tolerance.kind == ToleranceKind::Deviation) {
        const DimensionStyle printer = printerFor(tolerance.upperMm, tolerance.lowerMm);
        const std::string upper = (tolerance.upperMm >= 0.0 ? "+" : "") +
                                  printer.format(tolerance.upperMm);
        const std::string lower = (tolerance.lowerMm >= 0.0 ? "+" : "") +
                                  printer.format(tolerance.lowerMm);
        return upper + "/" + lower;
    }
    // Limits: the two SIZES rather than the two deviations. The decimals come
    // from the DEVIATIONS even though the sizes are printed -- otherwise a
    // 60 mm size with a 0.03 band prints 60.0/60.0 and says nothing.
    const DimensionMeasurement measured = measure(dimension);
    if (!measured.ok) return {};
    const DimensionStyle printer = printerFor(tolerance.upperMm, tolerance.lowerMm);
    return printer.format(measured.valueMm + tolerance.upperMm) + "/" +
           printer.format(measured.valueMm + tolerance.lowerMm);
}

std::string DrawingDocument::dimensionText(const DrawingDimension& dimension) const {
    if (!dimension.textOverride().empty()) return dimension.textOverride();
    const DimensionMeasurement measured = measure(dimension);
    // A DANGLING DIMENSION SHOWS NO NUMBER. Showing the last one it read would
    // be a drawing stating a size nothing on it measures.
    if (!measured.ok) return "<?>";
    const DimensionStyle* style = findDimensionStyle(dimension.styleId());
    const std::string body =
        style != nullptr ? style->format(measured.valueMm)
                         : std::to_string(static_cast<int>(measured.valueMm));
    std::string text = body;
    switch (dimension.kind()) {
        // THE PREFIXES ISO 129 ASKS FOR. A radius without its R and a diameter
        // without its symbol are two numbers a reader cannot tell apart.
        case DimensionKind::Radius: text = "R" + body; break;
        case DimensionKind::Diameter:
            text = "\xE2\x8C\x80" + body; // U+2300 DIAMETER SIGN
            break;
        case DimensionKind::Angular:
            text = body + "\xC2\xB0"; // U+00B0 DEGREE SIGN
            break;
        case DimensionKind::Linear: break;
    }

    // --- AND THE TOLERANCE (M37) --------------------------------------------
    //
    // BUILT FROM dimensionToleranceText, not repeated here. The canvas sets
    // the tolerance in smaller type and so needs the two halves separately;
    // defining the whole as base-plus-part is what stops the two from ever
    // saying different things.
    const std::string tolerance = dimensionToleranceText(dimension);
    if (tolerance.empty()) return text;
    // LIMITS REPLACE THE SIZE rather than following it: a drawing showing
    // "25.00 25.10/24.90" states the same size twice, and the pair IS the
    // dimension.
    if (dimension.tolerance().kind == ToleranceKind::Limits) return tolerance;
    return text + " " + tolerance;
}

std::vector<ObjectId> DrawingDocument::danglingDimensions() const {
    std::vector<ObjectId> lost;
    for (const std::unique_ptr<DrawingDimension>& one : dimensions_)
        if (!measure(*one).ok) lost.push_back(one->id());
    return lost;
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
    for (const std::unique_ptr<DimensionStyle>& one : dimensionStyles_) {
        DimensionStyle* style = one.get();
        visit(NamedSlot{style->id(), style->name(),
                        [style](const std::string& n) { style->setName(n); }});
    }
    // A COMPONENT'S TAG IS ITS NAME, so it goes through the same walk: two
    // parts sharing a tag is a wiring list that sends an electrician to
    // whichever one they find first.
    for (const std::unique_ptr<SymbolPlacement>& one : symbols_) {
        SymbolPlacement* symbol = one.get();
        visit(NamedSlot{symbol->id(), symbol->tag(),
                        [symbol](const std::string& n) { symbol->setTag(n); }});
    }
    for (const std::unique_ptr<BomTable>& one : bomTables_) {
        BomTable* table = one.get();
        visit(NamedSlot{table->id(), table->name(),
                        [table](const std::string& n) { table->setName(n); }});
    }
}

bool DrawingDocument::removeOwnObject(ObjectId id) {
    ObjectRegistry::ObjectRef* found = registry_.find(id);
    if (found == nullptr) return false;
    const ObjectRegistry::ObjectRef handle = *found;

    // A COMPONENT: nothing owns it. What DOES change is the netlist, and that
    // is derived, so it simply reports one fewer part -- there is nothing to
    // cascade because there was nothing stored.
    if (const SymbolPlacement* symbol = findSymbol(id)) {
        if (!applyingHistory()) {
            SymbolExistenceEdit edit;
            edit.symbolId = id;
            edit.tag = symbol->tag();
            edit.symbolName = symbol->symbolName();
            edit.xMm = symbol->positionMm().x;
            edit.yMm = symbol->positionMm().y;
            edit.rotationRad = symbol->rotationRad();
            edit.mirrored = symbol->isMirrored();
            edit.layerId = symbol->layerId();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete " + symbol->tag());
        }
        for (auto it = symbols_.begin(); it != symbols_.end(); ++it) {
            if ((*it)->id() != id) continue;
            registry_.unregisterObject(id);
            symbols_.erase(it);
            return true;
        }
        return false;
    }

    // A WIRE: the same, and the net it was part of simply becomes two, or one
    // fewer -- which is exactly what deleting a wire means.
    if (const WireEntity* wire = findWire(id)) {
        if (!applyingHistory()) {
            WireExistenceEdit edit;
            edit.wireId = id;
            edit.pointsXY = FlattenPoints(wire->pointsMm());
            edit.label = wire->label();
            edit.layerId = wire->layerId();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete wire");
        }
        for (auto it = wires_.begin(); it != wires_.end(); ++it) {
            if ((*it)->id() != id) continue;
            registry_.unregisterObject(id);
            wires_.erase(it);
            return true;
        }
        return false;
    }

    // A PARTS LIST: nothing reads it, and its rows were never stored, so
    // there is nothing to cascade -- deleting one takes only itself.
    if (const Annotation* annotation = findAnnotation(id)) {
        // A DATUM STILL NAMED BY FRAMES IS NOT DELETED.
        //
        // The two alternatives are both worse. Cascading the delete throws
        // away frames the user spent time on and did not ask to lose.
        // Letting them dangle leaves a document that will not SAVE -- a
        // drawing a user cannot get out of because of a delete nobody warned
        // them about, which is the failure M39's hole tables were changed to
        // avoid. Refusing loses nothing and says exactly what to do next.
        if (annotation->isDatum() && framesReferringToDatum(id) > 0) return false;

        if (!applyingHistory()) {
            AnnotationExistenceEdit edit;
            edit.annotationId = id;
            edit.body = annotation->body();
            edit.anchor = annotation->anchor();
            edit.xMm = annotation->positionMm().x;
            edit.yMm = annotation->positionMm().y;
            edit.layerId = annotation->layerId();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete the symbol");
        }
        for (auto it = annotations_.begin(); it != annotations_.end(); ++it) {
            if ((*it)->id() != id) continue;
            registry_.unregisterObject(id);
            annotations_.erase(it);
            return true;
        }
        return false;
    }

    if (const HoleTable* table = findHoleTable(id)) {
        if (!applyingHistory()) {
            HoleTableExistenceEdit edit;
            edit.tableId = id;
            edit.name = table->name();
            edit.viewId = table->viewId();
            edit.xMm = table->positionMm().x;
            edit.yMm = table->positionMm().y;
            edit.datumXMm = table->datumMm().x;
            edit.datumYMm = table->datumMm().y;
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete hole table " + table->name());
        }
        for (auto it = holeTables_.begin(); it != holeTables_.end(); ++it) {
            if ((*it)->id() != id) continue;
            registry_.unregisterObject(id);
            holeTables_.erase(it);
            return true;
        }
        return false;
    }

    if (const BomTable* table = findBomTable(id)) {
        if (!applyingHistory()) {
            BomExistenceEdit edit;
            edit.tableId = id;
            edit.name = table->name();
            edit.sourcePath = table->sourcePath();
            edit.xMm = table->positionMm().x;
            edit.yMm = table->positionMm().y;
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete " + table->name());
        }
        for (auto it = bomTables_.begin(); it != bomTables_.end(); ++it) {
            if ((*it)->id() != id) continue;
            registry_.unregisterObject(id);
            bomTables_.erase(it);
            return true;
        }
        return false;
    }

    // A VIEW: nothing owns it and nothing else reads it yet. Annotation that
    // measures it arrives in M34, and that is when this grows a cascade.
    if (const DrawingView* view = findView(id)) {
        // ...AND ITS HOLE TABLES (M39.4). A table reads its view's file and
        // measures on its view's page; with the view gone it has nothing to
        // read and no page to measure on, and the drawing would refuse to save
        // -- a document that cannot be saved because of a delete the user was
        // never warned about is the worst of the three outcomes.
        for (bool again = true; again;) {
            again = false;
            for (const std::unique_ptr<HoleTable>& one : holeTables_) {
                if (one->viewId() != id) continue;
                removeObject(one->id());
                again = true;
                break;
            }
        }
        // ...EXCEPT ITS CHILDREN. A projected view's place is composed from
        // its parent's, so a child whose parent is gone has nowhere to be.
        // Taken FIRST so each records its own delta and one undo puts the
        // whole family back -- the same cascade a mate's relations get.
        for (bool again = true; again;) {
            again = false;
            for (const std::unique_ptr<DrawingView>& one : views_) {
                if (one->parentViewId() != id) continue;
                removeObject(one->id());
                again = true;
                break;
            }
        }
        if (!applyingHistory()) {
            DrawingViewExistenceEdit edit;
            SnapshotViewExistence(*view, edit);
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

    // AN ENTITY: nothing owns it and nothing else reads it. Annotation that
    // measures one arrives in M34, and that is when this grows a cascade.
    if (const DrawingEntity* entity = findEntity(id)) {
        if (!applyingHistory()) {
            DrawingEntityExistenceEdit edit;
            edit.entityId = id;
            edit.shape = entity->shape();
            edit.layerId = entity->layerId();
            edit.color = entity->color();
            edit.linetype = entity->linetype();
            edit.lineweight = entity->lineweight();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Erase " + std::string(ShapeName(entity->shape())));
        }
        registry_.unregisterObject(id);
        for (auto it = entities_.begin(); it != entities_.end(); ++it)
            if ((*it)->id() == id) {
                entities_.erase(it);
                break;
            }
        return true;
    }

    // A DIMENSION: nothing owns it.
    if (const DrawingDimension* dimension = findDimension(id)) {
        if (!applyingHistory()) {
            DimensionExistenceEdit edit;
            edit.dimensionId = id;
            edit.kind = static_cast<int>(dimension->kind());
            edit.first = dimension->first();
            edit.second = dimension->second();
            edit.direction = static_cast<int>(dimension->direction());
            edit.lineXMm = dimension->linePositionMm().x;
            edit.lineYMm = dimension->linePositionMm().y;
            edit.styleId = dimension->styleId();
            edit.layerId = dimension->layerId();
            edit.textOverride = dimension->textOverride();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Erase dimension");
        }
        registry_.unregisterObject(id);
        for (auto it = dimensions_.begin(); it != dimensions_.end(); ++it)
            if ((*it)->id() == id) {
                dimensions_.erase(it);
                break;
            }
        return true;
    }

    // A DIMENSION STYLE, except the one every drawing has and any that are in
    // use -- a dimension whose style is gone has no way to be drawn.
    if (const DimensionStyle* style = findDimensionStyle(id)) {
        if (!applyingHistory()) {
            if (style->name() == kDefaultDimensionStyleName) return false;
            if (id == currentStyleId_) return false;
            for (const std::unique_ptr<DrawingDimension>& one : dimensions_)
                if (one->styleId() == id) return false;
            DimensionStyleExistenceEdit edit;
            edit.styleId = id;
            edit.name = style->name();
            edit.addedByTheEdit = false;
            recordDelta(edit, "Delete style " + style->name());
        }
        registry_.unregisterObject(id);
        for (auto it = dimensionStyles_.begin(); it != dimensionStyles_.end(); ++it)
            if ((*it)->id() == id) {
                dimensionStyles_.erase(it);
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
            // A LAYER WITH GEOMETRY ON IT CANNOT GO EITHER. AutoCAD refuses
            // this too, and the alternative -- deleting the geometry with it
            // -- would throw away work in response to a command about a
            // table entry. The count is in the caller's message, not here.
            for (const std::unique_ptr<DrawingEntity>& one : entities_)
                if (one->layerId() == id) return false;
            for (const std::unique_ptr<DrawingDimension>& one : dimensions_)
                if (one->layerId() == id) return false;
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
    if (const auto* edit = std::get_if<DrawingEntityExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findEntity(edit->entityId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreEntity(edit->entityId, edit->shape, edit->layerId, edit->color,
                          edit->linetype, edit->lineweight);
        else
            removeObject(edit->entityId);
        return;
    }
    if (const auto* edit = std::get_if<DrawingEntityShapeEdit>(&delta)) {
        if (DrawingEntity* entity = findEntityForEdit(edit->entityId))
            entity->setShape(forward ? edit->after : edit->before);
        return;
    }
    if (const auto* edit = std::get_if<DrawingEntityPropertyEdit>(&delta)) {
        DrawingEntity* entity = findEntityForEdit(edit->entityId);
        if (entity == nullptr) return;
        entity->setLayerId(forward ? edit->afterLayerId : edit->beforeLayerId);
        entity->setColor(forward ? edit->afterColor : edit->beforeColor);
        entity->setLinetype(forward ? edit->afterLinetype : edit->beforeLinetype);
        entity->setLineweight(forward ? edit->afterLineweight : edit->beforeLineweight);
        return;
    }
    if (const auto* edit = std::get_if<DimensionExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findDimension(edit->dimensionId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreDimension(edit->dimensionId, static_cast<DimensionKind>(edit->kind),
                             edit->first, edit->second,
                             static_cast<LinearDirection>(edit->direction),
                             Vec2{edit->lineXMm, edit->lineYMm}, edit->styleId, edit->layerId,
                             edit->textOverride);
        else
            removeObject(edit->dimensionId);
        return;
    }
    if (const auto* edit = std::get_if<DimensionEdit>(&delta)) {
        DrawingDimension* dimension = findDimensionForEdit(edit->dimensionId);
        if (dimension == nullptr) return;
        dimension->setDirection(
            static_cast<LinearDirection>(forward ? edit->afterDirection : edit->beforeDirection));
        dimension->setLinePositionMm(Vec2{forward ? edit->afterXMm : edit->beforeXMm,
                                          forward ? edit->afterYMm : edit->beforeYMm});
        dimension->setStyleId(forward ? edit->afterStyleId : edit->beforeStyleId);
        dimension->setTextOverride(forward ? edit->afterText : edit->beforeText);
        return;
    }
    if (const auto* edit = std::get_if<DimensionStyleExistenceEdit>(&delta)) {
        const bool shouldExist = forward ? edit->addedByTheEdit : !edit->addedByTheEdit;
        const bool doesExist = findDimensionStyle(edit->styleId) != nullptr;
        if (shouldExist == doesExist) return;
        if (shouldExist)
            restoreDimensionStyle(edit->styleId, edit->name);
        else
            removeObject(edit->styleId);
        return;
    }
    if (const auto* edit = std::get_if<DimensionStyleEdit>(&delta)) {
        DimensionStyle* style = findDimensionStyleForEdit(edit->styleId);
        if (style == nullptr) return;
        ApplyStyleHalf(*style,
                       forward ? edit->afterTextHeightMm : edit->beforeTextHeightMm,
                       forward ? edit->afterArrowSizeMm : edit->beforeArrowSizeMm,
                       forward ? edit->afterTextGapMm : edit->beforeTextGapMm,
                       forward ? edit->afterExtensionGapMm : edit->beforeExtensionGapMm,
                       forward ? edit->afterExtensionOvershootMm
                               : edit->beforeExtensionOvershootMm,
                       forward ? edit->afterDecimals : edit->beforeDecimals,
                       forward ? edit->afterSuffix : edit->beforeSuffix,
                       forward ? edit->afterOverallScale : edit->beforeOverallScale);
        return;
    }
    if (const auto* edit = std::get_if<CurrentDimensionStyleEdit>(&delta)) {
        currentStyleId_ = forward ? edit->after : edit->before;
        return;
    }
    if (const auto* edit = std::get_if<DimensionToleranceEdit>(&delta)) {
        DrawingDimension* found = findDimensionForEdit(edit->dimensionId);
        if (found == nullptr) return;
        DimensionTolerance tolerance;
        tolerance.kind =
            static_cast<ToleranceKind>(forward ? edit->afterKind : edit->beforeKind);
        tolerance.upperMm = forward ? edit->afterUpperMm : edit->beforeUpperMm;
        tolerance.lowerMm = forward ? edit->afterLowerMm : edit->beforeLowerMm;
        tolerance.fitCode = forward ? edit->afterFitCode : edit->beforeFitCode;
        tolerance.decimals = forward ? edit->afterDecimals : edit->beforeDecimals;
        found->setTolerance(std::move(tolerance));
        return;
    }

    if (const auto* edit = std::get_if<GeneralToleranceEdit>(&delta)) {
        generalTolerance_ =
            static_cast<GeneralToleranceClass>(forward ? edit->after : edit->before);
        return;
    }

    if (const auto* edit = std::get_if<SymbolExistenceEdit>(&delta)) {
        const bool wanted = forward == edit->addedByTheEdit;
        if (wanted) {
            if (findSymbol(edit->symbolId) == nullptr)
                restoreSymbol(edit->symbolId, edit->tag, edit->symbolName,
                              Vec2{edit->xMm, edit->yMm}, edit->rotationRad, edit->mirrored,
                              edit->layerId);
        } else {
            for (auto it = symbols_.begin(); it != symbols_.end(); ++it) {
                if ((*it)->id() != edit->symbolId) continue;
                registry_.unregisterObject(edit->symbolId);
                symbols_.erase(it);
                break;
            }
        }
        return;
    }

    if (const auto* edit = std::get_if<SymbolPlacementEdit>(&delta)) {
        SymbolPlacement* symbol = findSymbolForEdit(edit->symbolId);
        if (symbol == nullptr) return;
        symbol->setPositionMm(forward ? Vec2{edit->afterXMm, edit->afterYMm}
                                      : Vec2{edit->beforeXMm, edit->beforeYMm});
        symbol->setRotationRad(forward ? edit->afterRotationRad : edit->beforeRotationRad);
        symbol->setMirrored(forward ? edit->afterMirrored : edit->beforeMirrored);
        symbol->setTag(forward ? edit->afterTag : edit->beforeTag);
        return;
    }

    if (const auto* edit = std::get_if<WireExistenceEdit>(&delta)) {
        const bool wanted = forward == edit->addedByTheEdit;
        if (wanted) {
            if (findWire(edit->wireId) == nullptr)
                restoreWire(edit->wireId, UnflattenPoints(edit->pointsXY), edit->layerId,
                            edit->label);
        } else {
            for (auto it = wires_.begin(); it != wires_.end(); ++it) {
                if ((*it)->id() != edit->wireId) continue;
                registry_.unregisterObject(edit->wireId);
                wires_.erase(it);
                break;
            }
        }
        return;
    }

    if (const auto* edit = std::get_if<WireEdit>(&delta)) {
        WireEntity* wire = findWireForEdit(edit->wireId);
        if (wire == nullptr) return;
        wire->setPointsMm(UnflattenPoints(forward ? edit->afterPointsXY
                                                  : edit->beforePointsXY));
        wire->setLabel(forward ? edit->afterLabel : edit->beforeLabel);
        return;
    }

    if (const auto* edit = std::get_if<AnnotationExistenceEdit>(&delta)) {
        const bool wanted = forward == edit->addedByTheEdit;
        if (wanted) {
            if (findAnnotation(edit->annotationId) == nullptr)
                restoreAnnotation(edit->annotationId, edit->body, edit->anchor,
                                  Vec2{edit->xMm, edit->yMm}, edit->layerId);
        } else {
            for (auto it = annotations_.begin(); it != annotations_.end(); ++it) {
                if ((*it)->id() != edit->annotationId) continue;
                registry_.unregisterObject(edit->annotationId);
                annotations_.erase(it);
                break;
            }
        }
        return;
    }

    if (const auto* edit = std::get_if<AnnotationEdit>(&delta)) {
        Annotation* annotation = findAnnotationForEdit(edit->annotationId);
        if (annotation == nullptr) return;
        annotation->setBody(forward ? edit->afterBody : edit->beforeBody);
        annotation->setPositionMm(forward ? Vec2{edit->afterXMm, edit->afterYMm}
                                          : Vec2{edit->beforeXMm, edit->beforeYMm});
        return;
    }

    if (const auto* edit = std::get_if<HoleTableExistenceEdit>(&delta)) {
        const bool wanted = forward == edit->addedByTheEdit;
        if (wanted) {
            if (findHoleTable(edit->tableId) == nullptr)
                restoreHoleTable(edit->tableId, edit->name, edit->viewId,
                                 Vec2{edit->xMm, edit->yMm},
                                 Vec2{edit->datumXMm, edit->datumYMm}, {}, 7.0);
        } else {
            for (auto it = holeTables_.begin(); it != holeTables_.end(); ++it) {
                if ((*it)->id() != edit->tableId) continue;
                registry_.unregisterObject(edit->tableId);
                holeTables_.erase(it);
                break;
            }
        }
        return;
    }

    if (const auto* edit = std::get_if<HoleTableEdit>(&delta)) {
        HoleTable* table = findHoleTableForEdit(edit->tableId);
        if (table == nullptr) return;
        table->setPositionMm(forward ? Vec2{edit->afterXMm, edit->afterYMm}
                                     : Vec2{edit->beforeXMm, edit->beforeYMm});
        table->setDatumMm(forward ? Vec2{edit->afterDatumXMm, edit->afterDatumYMm}
                                  : Vec2{edit->beforeDatumXMm, edit->beforeDatumYMm});
        return;
    }

    if (const auto* edit = std::get_if<RevisionExistenceEdit>(&delta)) {
        const bool wanted = forward == edit->addedByTheEdit;
        if (wanted) {
            Revision one;
            one.letter = edit->letter;
            one.description = edit->description;
            one.date = edit->date;
            one.by = edit->by;
            // BACK WHERE IT WAS, letter and all. Re-deriving the letter here
            // would be the bug Revision.h is about: undo would renumber a
            // history that other people's paperwork already cites.
            restoreRevision(std::move(one), edit->at);
        } else {
            for (auto it = revisions_.begin(); it != revisions_.end(); ++it) {
                if (it->letter != edit->letter) continue;
                revisions_.erase(it);
                break;
            }
        }
        return;
    }

    if (const auto* edit = std::get_if<RevisionTableExistenceEdit>(&delta)) {
        const bool wanted = forward == edit->addedByTheEdit;
        if (wanted) {
            if (findRevisionTable(edit->tableId) == nullptr)
                restoreRevisionTable(edit->tableId, edit->name, Vec2{edit->xMm, edit->yMm},
                                     edit->widthMm, edit->rowHeightMm);
        } else {
            for (auto it = revisionTables_.begin(); it != revisionTables_.end(); ++it) {
                if ((*it)->id() != edit->tableId) continue;
                registry_.unregisterObject(edit->tableId);
                revisionTables_.erase(it);
                break;
            }
        }
        return;
    }

    if (const auto* edit = std::get_if<RevisionTableEdit>(&delta)) {
        RevisionTable* table = findRevisionTableForEdit(edit->tableId);
        if (table == nullptr) return;
        table->setPositionMm(forward ? Vec2{edit->afterXMm, edit->afterYMm}
                                     : Vec2{edit->beforeXMm, edit->beforeYMm});
        return;
    }

    if (const auto* edit = std::get_if<BomExistenceEdit>(&delta)) {
        const bool wanted = forward == edit->addedByTheEdit;
        if (wanted) {
            if (findBomTable(edit->tableId) == nullptr)
                restoreBomTable(edit->tableId, edit->name, edit->sourcePath,
                                Vec2{edit->xMm, edit->yMm}, BomDepth::TopLevel, {}, 8.0, true,
                                SourceFileStamp(edit->sourcePath));
        } else {
            for (auto it = bomTables_.begin(); it != bomTables_.end(); ++it) {
                if ((*it)->id() != edit->tableId) continue;
                registry_.unregisterObject(edit->tableId);
                bomTables_.erase(it);
                break;
            }
        }
        return;
    }

    if (const auto* edit = std::get_if<BomEdit>(&delta)) {
        BomTable* table = findBomTableForEdit(edit->tableId);
        if (table == nullptr) return;
        table->setPositionMm(forward ? Vec2{edit->afterXMm, edit->afterYMm}
                                     : Vec2{edit->beforeXMm, edit->beforeYMm});
        table->setDepth(static_cast<BomDepth>(forward ? edit->afterDepth : edit->beforeDepth));
        table->setRowHeightMm(forward ? edit->afterRowHeightMm : edit->beforeRowHeightMm);
        table->setGrowsUpward(forward ? edit->afterGrowsUpward : edit->beforeGrowsUpward);
        table->setColumns(ColumnsFromInts(forward ? edit->afterColumns : edit->beforeColumns));
        return;
    }

    if (const auto* edit = std::get_if<TitleBlockEdit>(&delta)) {
        // THE WHOLE BLOCK, REPLACED. Rebuilt from the record rather than
        // patched field by field, so there is no state in which half of it is
        // the old one.
        const std::vector<TitleBlockFieldRecord>& want = forward ? edit->after : edit->before;
        std::vector<TitleBlockField> fields;
        fields.reserve(want.size());
        for (const TitleBlockFieldRecord& field : want)
            fields.push_back(TitleBlockField{field.label, field.value,
                                             static_cast<TitleBlockSource>(field.source)});
        // Through the RAW path, not the guarded one: rebuilding through
        // addField/removeField cannot express "these exact rows", because the
        // guards keep Title and Drawing No. and would leave them behind. See
        // TitleBlock::restoreFields.
        blockForEdit().restoreFields(std::move(fields));
        blockForEdit().restoreSize(forward ? edit->afterWidthMm : edit->beforeWidthMm,
                                forward ? edit->afterRowHeightMm : edit->beforeRowHeightMm);
        blockForEdit().setVisible(forward ? edit->afterVisible : edit->beforeVisible);
        return;
    }

    if (const auto* edit = std::get_if<SheetFrameEdit>(&delta)) {
        currentPageForEdit().marginsForEdit().bindingMm = forward ? edit->afterBindingMm : edit->beforeBindingMm;
        currentPageForEdit().marginsForEdit().otherMm = forward ? edit->afterOtherMm : edit->beforeOtherMm;
        currentPageForEdit().zoneForEdit() = forward ? edit->afterZoneTargetMm : edit->beforeZoneTargetMm;
        currentPageForEdit().frameShownForEdit() = forward ? edit->afterVisible : edit->beforeVisible;
        return;
    }

    if (const auto* edit = std::get_if<SheetEdit>(&delta)) {
        const int size = forward ? edit->afterSize : edit->beforeSize;
        const int orientation = forward ? edit->afterOrientation : edit->beforeOrientation;
        paperForEdit().setOrientation(static_cast<SheetOrientation>(orientation));
        paperForEdit().setProjectionAngle(
            static_cast<ProjectionAngle>(forward ? edit->afterAngle : edit->beforeAngle));
        paperForEdit().setScale(DrawingScale{
            forward ? edit->afterScaleNumerator : edit->beforeScaleNumerator,
            forward ? edit->afterScaleDenominator : edit->beforeScaleDenominator});
        // A CUSTOM SHEET carries its own millimetres, and setSize alone cannot
        // put them back -- the size table has no entry for Custom.
        //
        // NO SWAP. A custom sheet ignores orientation (see Sheet::widthMm), so
        // what the delta holds is what the user typed. The first draft swapped
        // them for a landscape sheet and M32_UNDO_002 came back 250 x 500.
        if (static_cast<SheetSize>(size) == SheetSize::Custom)
            paperForEdit().setCustomSize(forward ? edit->afterWidthMm : edit->beforeWidthMm,
                                 forward ? edit->afterHeightMm : edit->beforeHeightMm);
        // AFTER setCustomSize, which forces the size to Custom as a side
        // effect -- the same ordering restoreSheet has to observe.
        paperForEdit().setSize(static_cast<SheetSize>(size));
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
        if (shouldExist) {
            DrawingView& back =
                restoreView(edit->viewId, edit->name, ComputeState::Dirty, edit->sourcePath,
                            edit->bodyName, static_cast<ViewDirection>(edit->direction),
                            Vec2{edit->positionXMm, edit->positionYMm},
                            DrawingScale{edit->scaleNumerator, edit->scaleDenominator},
                            edit->ownScale, edit->showHidden, edit->showTangent,
                            edit->parentViewId, edit->alignmentOffsetMm);
            // THE CUT COMES BACK WITH IT. A restored section with no cut line
            // projects the WHOLE part and looks entirely reasonable.
            if (edit->sectionActive) {
                DrawingView::SectionCut cut;
                cut.active = true;
                cut.fromMm = Vec2{edit->sectionFromXMm, edit->sectionFromYMm};
                cut.toMm = Vec2{edit->sectionToXMm, edit->sectionToYMm};
                cut.arrowSide = edit->sectionArrowSide;
                back.setSectionCut(cut);
            }
            // ...AND SO DOES THE CIRCLE (M49). A restored detail with no
            // circle projects the WHOLE part at the enlarged scale, and looks
            // like a view somebody put there on purpose.
            if (edit->detailActive) {
                DrawingView::DetailFrame frame;
                frame.active = true;
                frame.centreMm = Vec2{edit->detailCentreXMm, edit->detailCentreYMm};
                frame.radiusMm = edit->detailRadiusMm;
                back.setDetailFrame(frame);
            }
            // ...AND SO DOES THE BREAK. Restored without it, a broken view
            // comes back showing the whole three metres of bar, which reads as
            // a view somebody forgot to break rather than as a lost edit.
            back.setShowsFlatPattern(edit->flatPattern);
            if (edit->breakActive) {
                BreakSpan span;
                span.active = true;
                span.fromMm = edit->breakFromMm;
                span.toMm = edit->breakToMm;
                span.horizontal = edit->breakHorizontal;
                span.gapMm = edit->breakGapMm;
                back.setBreakSpan(span);
            }
        }
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
        view->setShowsHiddenLines(forward ? edit->afterShowHidden : edit->beforeShowHidden);
        view->setShowsTangentEdges(forward ? edit->afterShowTangent : edit->beforeShowTangent);
        view->setAlignmentOffsetMm(forward ? edit->afterAlignmentOffsetMm
                                           : edit->beforeAlignmentOffsetMm);
        DrawingView::SectionCut cut;
        cut.active = forward ? edit->afterSectionActive : edit->beforeSectionActive;
        cut.fromMm = Vec2{forward ? edit->afterSectionFromXMm : edit->beforeSectionFromXMm,
                          forward ? edit->afterSectionFromYMm : edit->beforeSectionFromYMm};
        cut.toMm = Vec2{forward ? edit->afterSectionToXMm : edit->beforeSectionToXMm,
                        forward ? edit->afterSectionToYMm : edit->beforeSectionToYMm};
        cut.arrowSide = forward ? edit->afterSectionArrowSide : edit->beforeSectionArrowSide;
        view->setSectionCut(cut);
        DrawingView::DetailFrame frame;
        frame.active = forward ? edit->afterDetailActive : edit->beforeDetailActive;
        frame.centreMm = Vec2{forward ? edit->afterDetailCentreXMm : edit->beforeDetailCentreXMm,
                              forward ? edit->afterDetailCentreYMm : edit->beforeDetailCentreYMm};
        frame.radiusMm = forward ? edit->afterDetailRadiusMm : edit->beforeDetailRadiusMm;
        view->setDetailFrame(frame);
        BreakSpan span;
        span.active = forward ? edit->afterBreakActive : edit->beforeBreakActive;
        span.fromMm = forward ? edit->afterBreakFromMm : edit->beforeBreakFromMm;
        span.toMm = forward ? edit->afterBreakToMm : edit->beforeBreakToMm;
        span.horizontal = forward ? edit->afterBreakHorizontal : edit->beforeBreakHorizontal;
        span.gapMm = forward ? edit->afterBreakGapMm : edit->beforeBreakGapMm;
        view->setBreakSpan(span);
        graph_.markDirty(edit->viewId);
        return;
    }

    // A DELTA THIS DOCUMENT DOES NOT KNOW IS AN ERROR, not something to skip:
    // a silently ignored delta is an undo that half-happened, with nothing
    // said. The Part and Assembly sides refuse the same way.
    throw std::runtime_error("this drawing cannot undo a change of that kind");
}

} // namespace paramcad
