#pragma once

#include "Core/Document/DocumentBase.h"
#include "Core/Drawing/DimensionStyle.h"
#include "Core/Drawing/DrawingDimension.h"
#include "Core/Drawing/DrawingEntity.h"
#include "Core/Drawing/SheetFrame.h"
#include "Core/Drawing/TitleBlock.h"
#include "Core/Drawing/DrawingTables.h"
#include "Core/Drawing/DrawingView.h"
#include "Core/Drawing/Sheet.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paramcad {

// THE THIRD DOCUMENT TYPE (M32, roadmap §24).
//
// A drawing is a SHEET OF PAPER with views of other documents on it and
// annotation over the top. It is the first document whose contents have a
// physical size, and the first whose whole purpose is to LEAVE the program.
//
// WHY A THIRD TYPE RATHER THAN A MODE OF A PART:
//
// A drawing references documents, it does not contain them. Its geometry is
// two-dimensional and measured in paper millimetres, not model millimetres.
// Its objects are layers and views and annotations, none of which a part has.
// Every one of those would have become an "if this document is really a
// drawing" branch inside PartDocument, and the P3 lift M23 paid for exists
// precisely so a new type costs a subclass rather than a flag.
//
// WHAT IS ON A SHEET IS TWO DIFFERENT KINDS OF THING, and keeping them apart
// is the load-bearing decision of this whole block:
//
//   * DERIVED -- the curves a view projects out of a 3D model. No ObjectId,
//     not registered, not undoable, thrown away and rebuilt whenever the
//     model changes. A user cannot drag one, delete one or put it on another
//     layer, because it is not a thing they made.
//   * AUTHORED -- the lines, circles, text and dimensions a user draws on the
//     sheet. ObjectId, layer, linetype, undo, selection. These arrive in M33,
//     ported from EasyCad, whose entity model had already made these choices
//     and proved them against DXF.
//
// They are separate lists of separate types, not one list with a flag. A flag
// would turn "can this be deleted / dragged / renamed / saved" into four
// questions somebody has to remember to ask, and the day one of them is
// forgotten a projected edge becomes editable and the drawing stops matching
// the model.
class DrawingDocument final : public DocumentBase {
public:
    explicit DrawingDocument(std::string name);
    DrawingDocument(ObjectId id, std::string name);

    DocumentType type() const noexcept override { return DocumentType::Drawing; }

    // --- The paper -----------------------------------------------------------
    const Sheet& sheet() const noexcept { return sheet_; }
    // Through the facade, so every change is one undo step and the views that
    // sit on the paper are told it moved.
    bool setSheetSize(SheetSize size);
    bool setSheetOrientation(SheetOrientation orientation);
    bool setSheetScale(const DrawingScale& scale);
    bool setSheetCustomSize(double widthMm, double heightMm);
    // The load path: writes the sheet with NO undo record, because opening a
    // file is not something the user did (ADR-M9-001). `customWidthMm` and
    // `customHeightMm` are read only for a Custom sheet, and are PORTRAIT
    // millimetres as `setCustomSize` stores them.
    void restoreSheet(SheetSize size, SheetOrientation orientation, DrawingScale scale,
                      double customWidthMm, double customHeightMm, ProjectionAngle angle);

    // --- Layers (the DXF table model) ----------------------------------------
    //
    // Every drawing has layer "0" from the moment it is constructed, and it
    // can be neither renamed nor deleted. That is AutoCAD's rule and it is
    // kept because a DXF without layer 0 is a DXF other programs refuse.
    Layer& addLayer(std::string name, int color = 7,
                    std::string linetype = kContinuousLinetypeName);
    Layer& restoreLayer(ObjectId id, std::string name, int color, std::string linetype, bool on,
                        bool frozen, bool locked, int lineweight);
    std::vector<const Layer*> layers() const;
    const Layer* findLayer(ObjectId id) const noexcept;
    const Layer* findLayerNamed(const std::string& name) const noexcept;

    // WHICH LAYER NEW GEOMETRY LANDS ON. Document state, not presentation: a
    // drawing reopened on another machine must put new lines where the last
    // person left the current layer, or two people drawing the same detail
    // put it on two different layers.
    ObjectId currentLayerId() const noexcept { return currentLayerId_; }
    bool setCurrentLayer(ObjectId layerId);
    // The load path, for the same reason -- and it does NOT apply the frozen
    // or locked refusal: a file may legitimately have been left with a locked
    // layer current, and refusing it here would silently move the current
    // layer somewhere the user did not put it.
    void restoreCurrentLayer(ObjectId layerId);
    void restoreCurrentDimensionStyle(ObjectId styleId);

    bool setLayerColor(ObjectId layerId, int color);
    bool setLayerLinetype(ObjectId layerId, std::string linetype);
    bool setLayerOn(ObjectId layerId, bool on);
    bool setLayerFrozen(ObjectId layerId, bool frozen);
    bool setLayerLocked(ObjectId layerId, bool locked);
    bool setLayerLineweight(ObjectId layerId, int lineweight);

    // --- Linetypes -----------------------------------------------------------
    Linetype& addLinetype(std::string name, std::string description,
                          std::vector<double> pattern);
    Linetype& restoreLinetype(ObjectId id, std::string name, std::string description,
                              std::vector<double> pattern);
    std::vector<const Linetype*> linetypes() const;
    const Linetype* findLinetype(ObjectId id) const noexcept;
    const Linetype* findLinetypeNamed(const std::string& name) const noexcept;

    // --- Views ---------------------------------------------------------------
    //
    // THROWS on a refusal, matching addInstance: a view with no source file
    // can never build, so it is refused at the door rather than left as a
    // permanent failure in the tree whose cause is a blank field.
    DrawingView& addView(std::string name, std::string sourcePath, std::string bodyName,
                         ViewDirection direction, Vec2 positionMm);
    // A CHILD VIEW: the same model, seen from another side, LINED UP with its
    // parent (M32.3).
    //
    // It takes no position of its own. Where it sits is COMPOSED from its
    // parent's place plus an offset along the alignment axis -- exactly as an
    // instance's placement is composed from its frame (ADR-M10-002) -- so
    // moving the parent moves the children and nothing had to be told.
    //
    // Refused when the child is not square to the parent: an isometric beside
    // a front view is not aligned to anything, and inventing a side for it to
    // sit on would put it somewhere the user cannot predict.
    DrawingView& addProjectedView(std::string name, ObjectId parentViewId,
                                  ViewDirection direction, double offsetMm);

    DrawingView& restoreView(ObjectId id, std::string name, ComputeState state,
                             std::string sourcePath, std::string bodyName,
                             ViewDirection direction, Vec2 positionMm, DrawingScale scale,
                             bool ownScale, bool showHidden, bool showTangent,
                             ObjectId parentViewId, double alignmentOffsetMm);

    // WHERE A VIEW ACTUALLY SITS, base or child. The one reader, so a
    // renderer, a plot and the "does it fit" check cannot disagree.
    Vec2 viewPositionMm(ObjectId viewId) const;

    // Why this view cannot be projected off that one, or empty when it can.
    std::string whyViewCannotBeProjectedFrom(ObjectId parentViewId,
                                             ViewDirection direction) const;

    // WHICH VIEWS ARE BEHIND THEIR MODELS. A drawing does not watch the disk;
    // it answers when asked, and the shell turns that into "3 views are out of
    // date -- update?" rather than silently rebuilding everything.
    std::vector<ObjectId> staleViews() const;
    std::vector<const DrawingView*> views() const;
    const DrawingView* findView(ObjectId id) const noexcept;
    const DrawingView* findViewNamed(const std::string& name) const noexcept;

    bool setViewPosition(ObjectId viewId, Vec2 positionMm);
    bool setViewDirection(ObjectId viewId, ViewDirection direction);
    bool setViewScale(ObjectId viewId, const DrawingScale& scale);
    bool clearViewScale(ObjectId viewId);
    bool setSheetProjectionAngle(ProjectionAngle angle);
    bool setViewAlignmentOffsetMm(ObjectId viewId, double offsetMm);
    bool setViewShowsHiddenLines(ObjectId viewId, bool show);
    bool setViewShowsTangentEdges(ObjectId viewId, bool show);

    // Why this view cannot be placed there, or empty when it can. The sheet
    // is a finite piece of paper and a view outside it is a view nobody
    // prints -- said here rather than discovered at plot time.
    std::string whyViewCannotSitAt(Vec2 positionMm) const;

    // --- Authored geometry (M33) ---------------------------------------------
    //
    // What a user DRAWS on the sheet, as against what a view projects. The
    // distinction this document was built around in M32.1, now with the other
    // half in it.
    //
    // NEW GEOMETRY LANDS ON THE CURRENT LAYER, always. A caller that named a
    // layer per entity would be a caller that could put something on a frozen
    // one, and the user would have drawn a line that appears not to have been
    // drawn.
    DrawingEntity& addEntity(DrawShape shape);
    DrawingEntity& restoreEntity(ObjectId id, DrawShape shape, ObjectId layerId, int color,
                                 std::string linetype, int lineweight);
    std::vector<const DrawingEntity*> entities() const;
    const DrawingEntity* findEntity(ObjectId id) const noexcept;

    // MOVE, COPY, ROTATE, SCALE, MIRROR -- all one operation, because they
    // are. EasyCad had five commands over one `ApplyTransform`, and so does
    // this: what differs is the matrix the shell builds, not what happens to
    // the geometry.
    bool transformEntities(const std::vector<ObjectId>& ids, const Matrix2D& transform);
    // ...and the copying half, which is the same transform applied to clones.
    std::vector<ObjectId> copyEntities(const std::vector<ObjectId>& ids,
                                       const Matrix2D& transform);

    bool setEntityLayer(ObjectId entityId, ObjectId layerId);
    bool setEntityColor(ObjectId entityId, int color);
    bool setEntityLinetype(ObjectId entityId, std::string linetype);
    bool setEntityLineweight(ObjectId entityId, int lineweight);

    // WHAT IS UNDER THE CURSOR, nearest first, within `apertureMm`. Entities
    // on invisible or locked layers are NOT offered: a lock that still let
    // things be picked would be a lock in name only.
    std::vector<ObjectId> entitiesNear(Vec2 point, double apertureMm) const;
    // Window (fully inside) or crossing (touching) -- two different rules,
    // and a package with only one of them surprises everybody who has met the
    // other.
    std::vector<ObjectId> entitiesInWindow(const Box2D& window, bool crossing) const;

    // The layer an entity is drawn with, resolved through ByLayer. ONE reader,
    // so the canvas, a plot and a DXF write cannot disagree about what colour
    // something is.
    int resolvedColorOf(const DrawingEntity& entity) const;
    std::string resolvedLinetypeOf(const DrawingEntity& entity) const;
    int resolvedLineweightOf(const DrawingEntity& entity) const;
    bool isEntityVisible(const DrawingEntity& entity) const;
    // A DIMENSION IS ON A LAYER TOO, and it obeys the same two rules -- so it
    // asks the same two functions rather than a copy of them. A dimension
    // carries no colour of its own yet, which is why it always resolves
    // ByLayer.
    int resolvedColorOfDimension(const DrawingDimension& dimension) const;
    bool isDimensionVisible(const DrawingDimension& dimension) const;

    // --- The frame and the title block (M35) ---------------------------------
    //
    // BOTH ARE DERIVED FROM THE SHEET WHERE THEY CAN BE. The frame is two
    // rectangles and a ring of zone labels, all of them a function of the
    // paper; the title block's scale, size and projection rows are read from
    // the sheet and cannot be typed. What is stored is only what a user
    // actually decides: the margins, and the text of the free fields.
    //
    // If either were entities on the paper, resizing the sheet would leave the
    // old border and the old size printed in the corner -- and it would look
    // completely plausible.
    const TitleBlock& titleBlock() const noexcept { return titleBlock_; }
    const FrameMargins& frameMargins() const noexcept { return frameMargins_; }
    double frameZoneTargetMm() const noexcept { return zoneTargetMm_; }
    bool isFrameVisible() const noexcept { return frameVisible_; }

    // WHERE THE FRAME IS, right now, on this paper. Asked for, never stored.
    SheetFrameGeometry frame() const noexcept;
    // The title block's bottom-left corner: hard against the frame's
    // bottom-right, which is where ISO 7200 puts it and where a reader's eye
    // goes first. Derived, so a resize moves it.
    Vec2 titleBlockOriginMm() const noexcept;

    bool setTitleBlockField(const std::string& label, std::string value);
    bool addTitleBlockField(std::string label, TitleBlockSource source);
    bool removeTitleBlockField(const std::string& label);
    bool setTitleBlockSize(double widthMm, double rowHeightMm);
    bool setTitleBlockVisible(bool visible);
    bool setFrameMargins(const FrameMargins& margins);
    bool setFrameZoneTargetMm(double zoneTargetMm);
    bool setFrameVisible(bool visible);
    // The loader's raw path, like every other restore here: no undo record,
    // no validation, because the loader has already done both.
    void restoreTitleBlock(TitleBlock block);
    void restoreFrame(const FrameMargins& margins, double zoneTargetMm, bool visible);

    // --- Dimension styles (M34) ----------------------------------------------
    //
    // Every drawing has ISO-25 from the moment it is constructed and it cannot
    // be deleted -- the same rule layer "0" follows, and for the same reason: a
    // dimension has to have a style.
    DimensionStyle& addDimensionStyle(std::string name);
    DimensionStyle& restoreDimensionStyle(ObjectId id, std::string name);
    std::vector<const DimensionStyle*> dimensionStyles() const;
    const DimensionStyle* findDimensionStyle(ObjectId id) const noexcept;
    const DimensionStyle* findDimensionStyleNamed(const std::string& name) const noexcept;
    // Editing a style through the facade, so every change is one undo step and
    // the drawing knows to repaint.
    bool editDimensionStyle(ObjectId styleId, const DimensionStyle& to);
    ObjectId currentDimensionStyleId() const noexcept { return currentStyleId_; }
    bool setCurrentDimensionStyle(ObjectId styleId);

    // --- Dimensions ------------------------------------------------------------
    DrawingDimension& addDimension(DimensionKind kind, DimensionAnchor first,
                                   DimensionAnchor second, Vec2 linePositionMm);
    DrawingDimension& restoreDimension(ObjectId id, DimensionKind kind, DimensionAnchor first,
                                       DimensionAnchor second, LinearDirection direction,
                                       Vec2 linePositionMm, ObjectId styleId, ObjectId layerId,
                                       std::string textOverride);
    std::vector<const DrawingDimension*> dimensions() const;
    const DrawingDimension* findDimension(ObjectId id) const noexcept;
    bool setDimensionDirection(ObjectId dimensionId, LinearDirection direction);
    bool setDimensionLinePosition(ObjectId dimensionId, Vec2 at);
    bool setDimensionTextOverride(ObjectId dimensionId, std::string text);
    bool setDimensionStyleOf(ObjectId dimensionId, ObjectId styleId);

    // WHAT A DIMENSION CURRENTLY READS -- resolved, never stored.
    //
    // THE one place an anchor becomes a coordinate. A canvas, a plot, a DXF
    // write and a "is anything dangling" check all ask here, so none of them
    // can disagree about what the drawing says.
    DimensionMeasurement measure(const DrawingDimension& dimension) const;
    // The text it shows: the override if there is one, otherwise the
    // measurement through its style.
    std::string dimensionText(const DrawingDimension& dimension) const;

    // WHAT A DIMENSION ON THIS SELECTION WOULD MEASURE (M34).
    //
    // ONE rule about which snap points a pick turns into anchors, so the menu,
    // the toolbar and a script all put the same dimension on the same pick --
    // rather than three hand-copied readings of "a circle means its centre and
    // its rim", which is precisely the shape of defect this project keeps
    // finding.
    //
    // It refuses rather than guesses: a diameter asked for on a line has no
    // sensible answer, and inventing one would put a number on the drawing
    // that measures something the user did not point at.
    struct DimensionProposal {
        bool ok = false;
        std::string why;
        DimensionAnchor first;
        DimensionAnchor second;
        Vec2 linePositionMm{};
    };
    DimensionProposal proposeDimension(DimensionKind kind,
                                       const std::vector<ObjectId>& entityIds) const;

    // WHICH DIMENSIONS HAVE LOST WHAT THEY MEASURED. Answered when asked, like
    // staleViews -- and it has to be visible, because a dangling dimension is
    // the one failure a drawing must never hide.
    std::vector<ObjectId> danglingDimensions() const;

    // --- DocumentBase --------------------------------------------------------
    DocumentRecomputeReport recompute() override;
    bool removeOwnObject(ObjectId id) override;

protected:
    void forEachOwnNamed(const std::function<void(const NamedSlot&)>& visit) override;
    void applyOwnDelta(const UndoDelta& delta, bool forward) override;

private:
    DrawingView* findViewForEdit(ObjectId id) noexcept;
    DrawingEntity* findEntityForEdit(ObjectId id) noexcept;
    DrawingDimension* findDimensionForEdit(ObjectId id) noexcept;
    DimensionStyle* findDimensionStyleForEdit(ObjectId id) noexcept;
    // Where an anchor actually is, in SHEET millimetres, or nothing when it
    // has lost what it pointed at.
    std::optional<Vec2> resolveAnchor(const DimensionAnchor& anchor) const;
    // What ByLayer means, in ONE place. Both an entity and a dimension route
    // through here, so the canvas cannot end up drawing the two by different
    // rules.
    int resolvedColorOnLayer(int ownColor, ObjectId layerId) const;
    bool layerIsVisible(ObjectId layerId) const;
    Layer* findLayerForEdit(ObjectId id) noexcept;
    // The seeded table every drawing starts with: layer 0 and CONTINUOUS.
    // Written through the restore path so a new document carries no undo
    // history (ADR-M9-001) -- "Undo" on a fresh drawing must not delete the
    // layer everything is on.
    void seedTables();

    Sheet sheet_;
    std::vector<std::unique_ptr<Layer>> layers_;
    std::vector<std::unique_ptr<Linetype>> linetypes_;
    std::vector<std::unique_ptr<DrawingView>> views_;
    std::vector<std::unique_ptr<DrawingEntity>> entities_;
    std::vector<std::unique_ptr<DimensionStyle>> dimensionStyles_;
    std::vector<std::unique_ptr<DrawingDimension>> dimensions_;
    ObjectId currentStyleId_{kInvalidObjectId};
    TitleBlock titleBlock_;
    FrameMargins frameMargins_;
    double zoneTargetMm_ = 100.0;
    bool frameVisible_ = true;
    ObjectId currentLayerId_{kInvalidObjectId};
};

} // namespace paramcad
