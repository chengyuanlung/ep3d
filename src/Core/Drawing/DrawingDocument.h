#pragma once

#include "Core/Document/DocumentBase.h"
#include "Core/Drawing/DrawingTables.h"
#include "Core/Drawing/DrawingView.h"
#include "Core/Drawing/Sheet.h"

#include <memory>
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

    // --- DocumentBase --------------------------------------------------------
    DocumentRecomputeReport recompute() override;
    bool removeOwnObject(ObjectId id) override;

protected:
    void forEachOwnNamed(const std::function<void(const NamedSlot&)>& visit) override;
    void applyOwnDelta(const UndoDelta& delta, bool forward) override;

private:
    DrawingView* findViewForEdit(ObjectId id) noexcept;
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
    ObjectId currentLayerId_{kInvalidObjectId};
};

} // namespace paramcad
