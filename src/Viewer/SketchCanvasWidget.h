#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Sketch/ISketchSolver.h"
#include "Core/Sketch/SketchTypes.h"
#include "Viewer/SketchCanvas.h"
#include "Viewer/SketchCommands.h"

#include <QColor>
#include <QString>
#include <QWidget>

#include <vector>

namespace paramcad {

class PartDocument;

// M12.3 -- the 2D sketch canvas as a widget.
//
// EVERYTHING it decides comes from SketchCanvas.h and SketchCommands.h; what is
// left here is painting and event plumbing. That division is the M6.14 lesson
// applied before the fact rather than after: the parts a unit test can reach
// are outside this file, so the only thing a smoke test still has to ask the
// WIDGET is what was actually drawn -- which is why paintEvent records counts.
//
// It is a QWidget with QPainter and NOT an OCCT view. That is stage one of the
// owner's decision on 2026-08-20; stage two puts the same SketchCanvasModel
// behind an OCCT overlay drawn on the sketch's plane in the 3D view. Nothing in
// the layer below this file knows which of the two is rendering it.
class SketchCanvasWidget : public QWidget {
    Q_OBJECT

public:
    explicit SketchCanvasWidget(QWidget* parent = nullptr);

    // Non-owning; the document must outlive this widget. Passing
    // kInvalidObjectId puts the canvas in its empty state rather than showing
    // stale geometry from the sketch that was open before.
    void setSketch(PartDocument* document, ObjectId sketchId);
    ObjectId sketchId() const noexcept { return sketchId_; }

    SketchTool tool() const noexcept { return model_.tool(); }
    void setTool(SketchTool tool);

    void fitView();
    const CanvasView& view() const noexcept { return view_; }

    // --- Commands, each returning the status line it produced ---------------
    //
    // Same shape as MainWindow::importDxfFile and undoCommand: a test drives
    // the REAL command path and asserts what the USER was told, rather than a
    // parallel path that only resembles it.
    QString applyConstraint(SketchEditKind kind);
    QString applyDimension(SketchEditKind explicitKind);
    QString deleteSelection();
    QString deleteConstraint(SketchConstraintId constraintId);
    QString commitDimensionText(SketchConstraintId constraintId, const QString& text);

    // A click at a SKETCH coordinate, driving the same path a mouse click does.
    // Exposed so the whole draw-dimension-constrain workflow is reachable
    // without a human and without synthesising Qt mouse events at a position
    // that depends on the window's size.
    QString clickAt(Vec2 sketchMm);

    // What the user is being told right now.
    QString promptText() const;
    SketchStatusLine statusLine() const;
    std::size_t selectionCount() const noexcept { return model_.selection().size(); }
    const std::vector<SketchElementRef>& selection() const noexcept { return model_.selection(); }
    void clearSelection();

    // Selects an element by hit-testing a sketch position, exactly as a click
    // under the Select tool does.
    bool selectAt(Vec2 sketchMm);

    // The dimension whose label contains this sketch point, or an invalid id.
    // Used by the double-click editor, by the drag, and by tests.
    SketchConstraintId dimensionAt(Vec2 sketchMm) const;

    // --- Dragging a dimension's value (M16) --------------------------------
    //
    // Three calls, matching press / move / release, so the whole interaction is
    // reachable without synthesising Qt mouse events at window-dependent
    // coordinates -- the same reason clickAt() exists.
    //
    // The document is touched ONCE, on release: the move preview writes
    // through `editSketch` without recording, so a drag across the canvas is
    // one undo step and not one per pixel (roadmap section 15).
    bool beginDimensionDrag(Vec2 sketchMm);
    bool updateDimensionDrag(Vec2 sketchMm);
    QString finishDimensionDrag();
    bool isDraggingDimension() const noexcept {
        return draggedDimension_ != kInvalidSketchConstraintId;
    }

    // Puts every dimension in this sketch back on automatic placement, in one
    // undo step. The way out of a layout the user has tangled.
    QString autoPlaceAllDimensions();

    // Prefix, suffix and tolerance for one dimension. Tolerances arrive in the
    // dimension's DISPLAY unit -- degrees for an angle -- and are converted
    // here, the same single conversion site CommitDimensionValue uses.
    QString commitDimensionFormat(SketchConstraintId constraintId, const QString& prefix,
                                  const QString& suffix, double plusDisplay,
                                  double minusDisplay);
    // What that dimension currently carries, for filling the dialog.
    bool dimensionFormatOf(SketchConstraintId constraintId, QString* prefix, QString* suffix,
                           double* plusDisplay, double* minusDisplay) const;
    // The exact text a dimension is showing, prefix and tolerance included.
    QString dimensionDisplayText(SketchConstraintId constraintId) const;

    // --- What was actually PAINTED -----------------------------------------
    // Recorded during paintEvent, not recomputed. A canvas that holds four
    // lines and draws none is exactly the M6.14 failure, and only a count taken
    // inside the painter can see it.
    int paintedEntities() const noexcept { return paintedEntities_; }
    int paintedDimensions() const noexcept { return paintedDimensions_; }
    // Arrowheads and angular arcs actually stroked. Counted separately because
    // "a dimension was drawn" and "it was drawn AS a dimension" are different
    // claims: the first survives a version that prints only the number.
    int paintedDimensionArrows() const noexcept { return paintedDimensionArrows_; }
    int paintedDimensionArcs() const noexcept { return paintedDimensionArcs_; }
    int paintedConstraintGlyphs() const noexcept { return paintedConstraintGlyphs_; }
    bool hasPaintedOnce() const noexcept { return paintedOnce_; }

    // --- Highlighting one constraint (roadmap 6.3 / 8.2 point 2) ------------
    //
    // Separate from the selection ON PURPOSE. Picking a row in the constraint
    // panel must not clobber what the user has selected on the canvas -- that
    // is the same rule roadmap 13 states for dialog fields -- so this is its
    // own channel: the glyph gets a ring, and the geometry the constraint names
    // gets a heavier stroke. Set `kInvalidSketchConstraintId` to clear.
    void setHighlightedConstraint(SketchConstraintId constraintId);
    SketchConstraintId highlightedConstraint() const noexcept { return highlighted_; }

    // The constraint badge under a point in SKETCH millimetres, or an invalid
    // id. Exposed so a smoke test can click one without inventing pixels.
    SketchConstraintId constraintBadgeAt(Vec2 sketchMm) const;
    // Where a badge is, in sketch mm -- the centre of the box it is drawn in.
    // A test that had to re-derive this from the anchor would be asserting its
    // own arithmetic rather than the widget's.
    bool constraintBadgeCentre(SketchConstraintId constraintId, Vec2* sketchMm) const;

    // TRIM mode: the next click removes the piece of line it lands on, back to
    // the nearest crossing.
    //
    // A MODE rather than a selection command, because the pick point is the
    // input -- "which piece" cannot be said by selecting the line, only by
    // pointing at the part to remove. Everything else in the sketch acts as a
    // cutter, which is AutoCAD's ENTER-for-all default and the only sensible
    // one when there is no way to nominate cutters yet.
    bool trimming() const noexcept { return trimming_; }
    void setTrimming(bool on);
    // Trims at a point, as a click in trim mode does. Reports what happened;
    // never empty, because "nothing visible happened" needs a reason.
    QString trimAt(Vec2 sketchMm);

    // EXTEND mode: the next click stretches the end of the line it lands on to
    // the first thing beyond it. A mode for the same reason Trim is one -- the
    // pick point chooses which END, and a selection cannot say that.
    bool extending() const noexcept { return extending_; }
    void setExtending(bool on);
    QString extendAt(Vec2 sketchMm);

    // USE mode (M17.6, ADR-M17-029): the next click turns the projected
    // reference edge under the cursor into an ordinary sketch entity.
    //
    // A MODE, for the same reason Trim is one: a reference is not part of the
    // selection model -- it has no SketchElementRef, cannot be constrained,
    // dimensioned or dragged -- so "which edge" can only be said by pointing
    // at it. Making references selectable instead would have meant letting all
    // four of those be attempted on geometry the model has no identity for.
    // DIMENSION mode (M17.18, ADR-M17-041): pick the geometry, then click
    // where the dimension line goes.
    //
    // Onshape's shape, and the reason it is worth copying: a dimension is
    // BOTH a measurement and a thing that occupies space on the drawing, and
    // the second half was previously decided for the user and then dragged
    // afterwards. One click says where it goes at the moment they are already
    // thinking about it.
    //
    // The KIND is inferred from the picks -- a line is a length, two points a
    // distance, a circle a diameter -- by the same requestDimension the eight
    // separate dimension buttons use, so the tool and the buttons cannot come
    // to disagree about what a selection means.
    bool dimensioning() const noexcept { return dimensioning_; }
    void setDimensioning(bool on);
    // One click in dimension mode: a pick, or the placement that finishes it.
    QString dimensionClickAt(Vec2 sketchMm);

    bool useReference() const noexcept { return useMode_; }
    void setUseReference(bool on);
    // Converts the reference at a point, as a click in Use mode does. Reports
    // what happened; never empty, because a refusal a user cannot read is
    // indistinguishable from a command that did nothing.
    QString useReferenceAt(Vec2 sketchMm);

    // How many reference items the last paint drew. The underlay is the whole
    // point of sketching on a face, and "the projection ran" is not the same
    // claim as "the projection reached the screen" -- which is the M6.14
    // lesson this project keeps paying for.
    int paintedReferences() const noexcept { return paintedReferences_; }
    // Whether the dimension about to be placed is being SHOWN. "The tool knows
    // what it would make" and "the user can see where it will go" are two
    // claims, and only the second is what makes a placement click something
    // other than a guess.
    int paintedDimensionGhosts() const noexcept { return paintedDimensionGhosts_; }

    // Mirrors the selection across the LAST line in it.
    //
    // "Everything selected except the mirror" rather than a separate pick: the
    // mirror is the line the user selected last, which is the order they
    // naturally work in (choose the things, then choose the axis) and needs no
    // extra mode to express.
    QString applyMirror();

    // SPLIT: the FIRST thing selected is cut, the rest do the cutting. Returns
    // what to show; never empty.
    QString applySplit();

    // TRANSFORM: move, turn or resize the selection, optionally leaving a copy.
    QString applyTransform(const SketchTransform& transform);

    // Rounds the corner between the TWO selected lines with an arc.
    QString applyFillet(double radiusMm);

    // Chamfers the corner between the TWO selected lines.
    //
    // A selection command rather than a mode: both inputs are entities, and
    // there is nothing a pick point would add that the selection does not
    // already say.
    QString applyChamfer(double distanceA, double distanceB);

    // Offsets the ONE selected entity by `distanceMm` (roadmap 4.1.1).
    //
    // The SIGN picks the side: positive is the left of a line's start->end
    // direction and outward for a curve, negative the other way. A sign rather
    // than a second pick, because a modal "now click which side" would need a
    // whole interaction state for one bit -- and the bit is already in the
    // number the user is typing.
    QString applyOffset(double distanceMm);

    // Toggles the selected entities between construction and normal geometry
    // (roadmap 4.1.1). Reports what happened; empty selection is refused with a
    // reason, never silently.
    QString toggleConstruction();

    // Switches the highlighted dimension between DRIVING and MEASURING
    // (M17.19, ADR-M17-042). Acts on the constraint the user picked -- a badge
    // on the canvas or a row in the panel -- because a dimension is chosen by
    // pointing at the dimension, not at the geometry it measures.
    QString toggleDimensionDriven();

    // Construction entities actually stroked during the last paint. They are
    // drawn DASHED, and a flag whose only evidence is a JSON field is a flag
    // the user cannot use.
    int paintedConstructionEntities() const noexcept { return paintedConstructionEntities_; }
    // How many tangent handles were actually stroked (M18). Counted, like every
    // other readback here, because "the code that draws it ran" is the only
    // thing a headless test can check about drawing -- and a handle nobody can
    // see is a handle nobody can grab.
    int paintedSplineHandles() const noexcept { return paintedSplineHandles_; }

    // Adds the sketch's origin point if it has none, and selects it either way,
    // so the command always leaves the user with the origin picked and ready to
    // dimension from. Reports what happened.
    QString addOriginPoint();
    // The origin point's entity id, or an invalid id.
    SketchEntityId originPoint() const;

    // Deletes the highlighted constraint. Empty when nothing is highlighted.
    QString deleteHighlightedConstraint();

    // --- Dragging geometry (M17) --------------------------------------------
    //
    // Grab a point, move it, drop it. The constraints decide what actually
    // happens; this only decides WHAT was grabbed and when it is committed.
    //
    // Split into begin/update/finish for the same reason the dimension drag is
    // (ADR-M16): the moves are previews that record nothing, and the release is
    // the single undo step.
    bool beginGeometryDrag(Vec2 sketchMm);
    void updateGeometryDrag(Vec2 sketchMm);
    // Commits as ONE undo step and reports what happened. Empty when no drag
    // was running.
    QString finishGeometryDrag();
    // Puts everything back, recording nothing. Esc mid-drag.
    bool cancelGeometryDrag();
    bool isDraggingGeometry() const noexcept {
        return dragged_.entityId != kInvalidSketchEntityId;
    }
    // What the last drag move was told, so a smoke test can report the reason a
    // drag did not move rather than only that it did not.
    SketchSolveStatus lastDragStatus() const noexcept { return lastDragStatus_; }

    // The hover half of a mouse move, without the event.
    //
    // Exposed because a repaint driven by hovering is what erased a refusal
    // from the status line, and the only way to test that it no longer does is
    // to hover the way a mouse does.
    void hoverAt(Vec2 sketchMm);

    // Whether the last constraint or dimension command CHANGED anything.
    //
    // The status string alone cannot say: a refusal and a report are both just
    // text. The shell needs to know so it can keep a refusal on screen until
    // the user acts, rather than letting the next mouse move erase the only
    // explanation of why nothing happened.
    bool lastCommandApplied() const noexcept { return lastCommandApplied_; }

    // WHAT ESCAPE MEANS, in one place: abandon a dimension drag if one is in
    // flight, otherwise leave the drawing tool (dropping whatever it had
    // half-drawn), otherwise clear the selection. Reports whether anything
    // changed.
    //
    // A method rather than only a key handler, for the reason Delete has one:
    // the decision has to be reachable by something other than a Qt key event,
    // or the only thing that can test it is a running window with focus.
    bool pressEscape();

    // WHAT DELETE MEANS, in one place: the selected geometry, or -- when
    // nothing is selected and a constraint badge is picked -- that constraint.
    //
    // One method because there are two ways in. The canvas handles the key
    // itself, AND the shell binds Del to a toolbar action scoped to this
    // widget; Qt gives the action first refusal, so a decision written only in
    // keyPressEvent is a decision the user never reaches. Both call this.
    QString deleteSelectionOrHighlightedConstraint();

    // Selection handles actually stroked during the last paint: the small dots
    // on a selected entity's ends and centres. A handle the user is told to
    // click and cannot see is the M6.14 shape again.
    int paintedSelectionHandles() const noexcept { return paintedSelectionHandles_; }

    // Glyphs drawn in the highlighted state during the last paint. 0 or 1 in
    // practice; counted rather than assumed so a test can tell "the row is
    // highlighted" from "the ring reached the screen".
    int paintedHighlightedGlyphs() const noexcept { return paintedHighlightedGlyphs_; }
    // Entities stroked as belonging to the highlighted constraint.
    int paintedHighlightedEntities() const noexcept { return paintedHighlightedEntities_; }

    // The colour geometry was LAST STROKED with, for entities that were not
    // selected. Invalid until something has been drawn.
    //
    // Roadmap 8.1 wants blue under-constrained, black fully constrained, red in
    // trouble, and 8.2 point 1 keeps colour as a SECOND channel next to the
    // status bar's words -- but a second channel that never reaches the screen
    // is not a channel. `solveStatus()` answers what the document thinks;
    // nothing but this answers what was painted, which is the whole point of
    // the counters above.
    QColor paintedGeometryColour() const noexcept { return paintedGeometryColour_; }

signals:
    // The document changed and the rest of the shell must refresh.
    void documentChanged(const QString& status);
    // Only what is shown changed: prompt, selection, view.
    void presentationChanged();
    // A double-click landed on a dimension: the shell opens the editor.
    void dimensionActivated(qulonglong constraintId);
    // A constraint badge was clicked on the canvas, or the click landed
    // somewhere that means "no constraint" (0). The shell moves the panel's
    // selection to match, so the two views never disagree about what is being
    // looked at.
    void constraintPicked(qulonglong constraintId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    const Sketch* sketch() const;
    void syncViewSize();
    // Pick tolerance in sketch mm for the CURRENT zoom, so a click is the same
    // number of PIXELS forgiving at every scale.
    double toleranceMm() const;
    QString applyEdit(const SketchEdit& edit);
    void refreshAfterDocumentChange();

    PartDocument* document_ = nullptr;
    ObjectId sketchId_ = kInvalidObjectId;

    SketchCanvasModel model_;
    CanvasView view_;
    SnapResult hoverSnap_;
    bool hasHover_ = false;
    bool panning_ = false;
    QPointF lastPanPos_;

    // The dimension being dragged, and where its value sat when the drag
    // started. The original is kept so the release can put it back and then
    // record ONE delta from there to the final position.
    SketchConstraintId draggedDimension_ = kInvalidSketchConstraintId;
    bool dragHadPlacement_ = false;
    Vec2 dragOriginalLabel_{};
    Vec2 dragGrabOffset_{};
    bool fittedOnce_ = false;

    int paintedEntities_ = 0;
    int paintedDimensions_ = 0;
    int paintedDimensionArrows_ = 0;
    int paintedDimensionArcs_ = 0;
    int paintedConstraintGlyphs_ = 0;
    int paintedReferences_ = 0;
    int paintedDimensionGhosts_ = 0;
    bool paintedOnce_ = false;
    QColor paintedGeometryColour_{};
    // The point being dragged, its sketch's geometry when the drag started, and
    // the last thing the solver said. Invalid entity id means no drag.
    SketchElementRef dragged_{};
    std::vector<std::pair<SketchEntityId, SketchGeometry>> dragBefore_;
    SketchSolveStatus lastDragStatus_{SketchSolveStatus::Solved};
    bool lastCommandApplied_ = false;
    bool trimming_ = false;
    bool extending_ = false;
    bool useMode_ = false;
    bool dimensioning_ = false;
    // The reference the cursor is over in Use mode, so the click target is
    // visible BEFORE the click. A mode whose target only shows once it has
    // been converted is a mode that gets used wrongly once per sketch.
    SketchReferenceId hoveredReference_ = kInvalidSketchReferenceId;
    std::vector<SketchEntityId> lastCreatedEntities_;
    std::vector<SketchConstraintId> lastCreatedConstraints_;
    int paintedConstructionEntities_ = 0;
    int paintedSplineHandles_ = 0;
    int paintedSelectionHandles_ = 0;
    int paintedHighlightedGlyphs_ = 0;
    int paintedHighlightedEntities_ = 0;

    SketchConstraintId highlighted_ = kInvalidSketchConstraintId;
};

} // namespace paramcad
