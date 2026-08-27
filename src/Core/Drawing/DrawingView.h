#pragma once

#include "Core/Drawing/BreakFold.h"
#include "Core/Drawing/ProjectedGeometry.h"
#include "Core/Drawing/Sheet.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Recompute/IRecomputable.h"

#include <cmath>
#include <string>
#include <string_view>

namespace paramcad {

// WHICH WAY THE PART IS LOOKED AT (M32, roadmap §24).
//
// The six orthographic directions plus an isometric. Named rather than given
// as a vector, because "the front view" is what a drawing says and what a
// reader expects to find top-left -- and because the direction and the UP
// vector have to agree. A caller that handed in a direction alone would leave
// "up" to be guessed, and a guessed up is a view that is right but rotated.
//
// FIRST ANGLE vs THIRD ANGLE is NOT here. That is a property of the SHEET --
// it decides where the top view is placed relative to the front, not what any
// single view looks like -- and putting it on a view would let one drawing
// hold both conventions at once, which is the one thing a projection symbol
// exists to promise cannot happen.
enum class ViewDirection {
    Front,
    Back,
    Left,
    Right,
    Top,
    Bottom,
    Isometric,
};

std::string_view toString(ViewDirection direction) noexcept;

// The camera for a direction: where it looks FROM, and which way is up.
//
// ONE TABLE. Every consumer -- the projector, the alignment rules, the view
// label -- reads it here, so a change of convention is one edit rather than a
// hunt.
struct ViewCamera {
    Vec3 towards{0.0, 1.0, 0.0}; // the direction of sight, in model space
    Vec3 up{0.0, 0.0, 1.0};
};

ViewCamera CameraFor(ViewDirection direction) noexcept;

// HOW A CHILD VIEW LINES UP WITH ITS PARENT.
//
// An orthographic drawing is not a set of independent pictures: the top view
// sits directly above or below the front and shares its horizontal position,
// and the side view sits beside it and shares its vertical one. That is what
// lets a reader carry a measurement from one view to another with a ruler.
enum class ViewAlignment {
    None,       // a base view -- it sits where it was put
    Horizontal, // slides left and right, sharing the parent's height on the page
    Vertical,   // slides up and down, sharing the parent's horizontal position
};

std::string_view toString(ViewAlignment alignment) noexcept;

// Which way a child looking `child` lines up beside a parent looking `parent`,
// and which side it falls on -- +1 or -1 along that axis, in THIRD angle.
//
// DERIVED FROM THE CAMERA TABLE, not written down a second time. "Top goes
// above the front" is not an independent fact: it follows from the top view
// looking along the front view's up vector. A second table would be a second
// thing to keep in step the day a direction is added.
//
// `sign` is 0 when the two directions are not square to each other -- an
// isometric beside a front view, say. Such a view is not aligned, and saying
// so is better than inventing a side for it to sit on.
struct ViewAlignmentRule {
    ViewAlignment alignment = ViewAlignment::None;
    int sign = 0;
};

ViewAlignmentRule AlignmentOf(ViewDirection parent, ViewDirection child) noexcept;

// ONE PROJECTED VIEW ON A SHEET.
//
// WHAT IT HOLDS IS A SENTENCE, not geometry: "that file, seen from there, at
// that scale, sitting here". The same decision Instance made (ADR-M22-003) and
// for the same reason -- the model file is the source of truth, so a model
// that was edited shows up here on the next rebuild, and a model that went
// away stops the view BY NAME instead of leaving a stale picture nobody can
// trace back to anything.
//
// THE CURVES IT PRODUCES ARE DERIVED (M32.2). They have no ObjectId, they are
// not registered, they are not undoable and they are thrown away and rebuilt
// whenever the model changes. That is the whole difference between a view and
// the lines a user draws on the sheet, and it is why the two are separate
// types rather than one entity list with a flag: a flag would make every
// question -- can this be deleted, dragged, renamed, saved -- a question
// somebody has to remember to ask.
class DrawingView final : public IRecomputable {
public:
    DrawingView(std::string name, std::string sourcePath, std::string bodyName,
                ViewDirection direction, Vec2 positionMm);
    DrawingView(ObjectId id, std::string name, ComputeState state, std::string sourcePath,
                std::string bodyName, ViewDirection direction, Vec2 positionMm,
                DrawingScale scale, bool ownScale, bool showHidden, bool showTangent,
                ObjectId parentViewId, double alignmentOffsetMm);

    ObjectId id() const noexcept override { return id_; }
    static std::string_view typeName() noexcept { return "DrawingView"; }

    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    const std::string& sourcePath() const noexcept { return sourcePath_; }
    // Which body in that file. EMPTY means "the whole thing" -- for a part
    // that is every body, for an assembly it is the assembly. Unlike an
    // Instance, an empty name here is NOT ambiguous: a drawing view of a
    // multi-body part shows the part.
    const std::string& bodyName() const noexcept { return bodyName_; }

    ViewDirection direction() const noexcept { return direction_; }
    void setDirection(ViewDirection direction) noexcept { direction_ = direction; }

    // Where a BASE view's origin sits on the paper, in sheet millimetres from
    // the bottom-left corner. The origin is the projection of the model
    // origin, not the centre of what is drawn -- so a part that grows does not
    // drag its own view across the page.
    //
    // MEANINGLESS FOR A CHILD. A projected view's place on the paper is
    // COMPOSED from its parent's, exactly as an instance's placement is
    // composed from its frame (ADR-M10-002) -- so moving the parent moves the
    // children and nothing had to be told. Ask the document
    // (viewPositionMm) rather than reading this.
    Vec2 positionMm() const noexcept { return positionMm_; }
    void setPositionMm(Vec2 positionMm) noexcept { positionMm_ = positionMm; }

    // --- Alignment (M32.3) ---------------------------------------------------
    ObjectId parentViewId() const noexcept { return parentViewId_; }
    // How far along its alignment axis this child sits from its parent, in
    // SHEET millimetres. Signed: which way is decided by the alignment rule
    // and the sheet's projection angle, so a positive offset always means
    // "further from the parent".
    double alignmentOffsetMm() const noexcept { return alignmentOffsetMm_; }
    void setAlignmentOffsetMm(double offsetMm) noexcept { alignmentOffsetMm_ = offsetMm; }
    void setParentViewId(ObjectId parentViewId) noexcept { parentViewId_ = parentViewId; }

    // --- Is the model still what this was drawn from? (M32.3) ----------------
    //
    // RUNTIME ONLY, never serialized. A reopened drawing has every view dirty
    // anyway, so a stamp written to the file would be a fact about a previous
    // session that could only ever mislead.
    long long sourceStamp() const noexcept { return sourceStamp_; }

    // A VIEW MAY OVERRIDE THE SHEET'S SCALE, and a drawing that shows one
    // detail at 2:1 beside a general view at 1:5 is ordinary. `ownScale` says
    // whether this view has an opinion at all, so "same as the sheet" survives
    // the sheet later being changed -- storing the resolved number instead
    // would silently pin the view the day somebody rescales the paper.
    bool hasOwnScale() const noexcept { return ownScale_; }
    const DrawingScale& scale() const noexcept { return scale_; }
    void setScale(const DrawingScale& scale);
    void clearScale() noexcept { ownScale_ = false; }

    // The scale actually used, given the sheet's. One reader, so a view label
    // and the projector cannot disagree about what this view is drawn at.
    DrawingScale effectiveScale(const DrawingScale& sheetScale) const noexcept;

    // --- What this view draws, and the conventions it draws it by -----------
    //
    // THE DRAWING'S CHOICE, NOT THE PROJECTOR'S. Hidden lines dashed is
    // standard on a mechanical view and wrong on a presentation view; tangent
    // edges are conventionally absent. Both live on the VIEW because two views
    // of the same part on one sheet may reasonably differ.
    bool showsHiddenLines() const noexcept { return showHidden_; }
    bool showsTangentEdges() const noexcept { return showTangent_; }
    void setShowsHiddenLines(bool show) noexcept { showHidden_ = show; }
    void setShowsTangentEdges(bool show) noexcept { showTangent_ = show; }

    // WHERE THE KNIFE WENT (M38).
    //
    // The line is drawn ON THE PARENT VIEW, in the parent's model millimetres
    // -- which is what makes it a sentence rather than geometry: turn the
    // parent and the cut follows, because the plane is worked out from the
    // parent's camera every time this is recomputed.
    //
    // `arrowSide` is which way the arrows point, and so which half survives.
    // Everything between the reader and the cutting plane is removed, which is
    // the convention every drawing standard uses and the one thing here that
    // is a coin toss if it is not written down.
    struct SectionCut {
        bool active = false;
        Vec2 fromMm{};
        Vec2 toMm{};
        int arrowSide = 1;

        bool usable() const noexcept {
            return active && (std::fabs(toMm.x - fromMm.x) > 1e-9 ||
                              std::fabs(toMm.y - fromMm.y) > 1e-9);
        }
    };
    const SectionCut& sectionCut() const noexcept { return section_; }
    void setSectionCut(SectionCut cut) noexcept { section_ = cut; }
    bool isSection() const noexcept { return section_.active; }

    // WHERE THE MAGNIFYING GLASS WENT (M49).
    //
    // A circle on the PARENT, in the parent's model millimetres -- the same
    // kind of sentence a section's cut line is, and for the same reason: the
    // detail is re-cropped from the parent's own projection at every
    // recompute, so a parent that changed carries its detail with it.
    //
    // THE DIRECTION IS NOT HERE. A detail is its parent's view enlarged, not a
    // new camera, so it is READ FROM THE PARENT at every recompute rather than
    // stored. Stored, it would sit still while the parent was turned, and the
    // detail would go on showing a face that is no longer there -- correctly
    // drawn, correctly labelled, and of nothing on this drawing.
    struct DetailFrame {
        bool active = false;
        Vec2 centreMm{};
        double radiusMm = 0.0;

        bool usable() const noexcept { return active && radiusMm > 1e-9; }
    };
    const DetailFrame& detailFrame() const noexcept { return detail_; }
    void setDetailFrame(DetailFrame frame) noexcept { detail_ = frame; }
    bool isDetail() const noexcept { return detail_.active; }

    // WHERE THE MIDDLE WENT (M50).
    //
    // NOT A KIND OF VIEW. Any view may be broken -- a base view, a projected
    // one, a section -- because a break is not a way of looking at the part,
    // it is a way of putting a long one on a short sheet.
    //
    // And it is NOT A CUT: nothing is removed from the projection. The span
    // says how model millimetres map onto paper, the mapping has an inverse,
    // and everything that measures goes through the inverse. See BreakFold.h
    // for why that is the whole design.
    const BreakSpan& breakSpan() const noexcept { return break_; }
    void setBreakSpan(BreakSpan span) noexcept { break_ = span; }
    bool isBroken() const noexcept { return break_.usable(); }

    // THE BLANK, BEFORE ANYTHING IS FOLDED (M53).
    //
    // A flat pattern is a KIND OF VIEW, like a section and a detail -- it goes
    // on a sheet, takes dimensions, and can itself be broken. What is
    // different is where its curves come from: not a projection of the solid,
    // but the CHAIN the part was folded from (see FlatPattern.h).
    //
    // Which is why this is a flag on an ordinary view rather than a separate
    // type. Everything else about it -- where it sits, what scale it is at,
    // what it is called -- is what any view has, and a second type would be a
    // second answer to all of it.
    bool showsFlatPattern() const noexcept { return flatPattern_; }
    void setShowsFlatPattern(bool flat) noexcept { flatPattern_ = flat; }

    // THE CURVES, IN MODEL MILLIMETRES (see ProjectedGeometry.h). Derived:
    // no ObjectId, not registered, not undoable, thrown away and rebuilt
    // whenever the model changes.
    const ProjectedDrawing& projected() const noexcept { return projected_; }

    // How much PAPER this view takes at a given scale. Model extent times the
    // scale, in one place -- a caller that multiplied for itself would be the
    // second place the scale is applied, and the first place to get it wrong.
    double paperWidthMm(const DrawingScale& sheetScale) const noexcept;
    double paperHeightMm(const DrawingScale& sheetScale) const noexcept;

    ComputeState currentState() const noexcept { return state_; }

    // WHICH PAGE THIS SITS ON (M44). kInvalidObjectId means the drawing's
    // first page, which is what every object made before there was more than
    // one page belongs to.
    ObjectId sheetId() const noexcept { return sheetId_; }
    void setSheetId(ObjectId sheetId) noexcept { sheetId_ = sheetId; }


    // A PROJECTION PUT THERE BY A TEST, so the rules that read one can be
    // tested without a kernel (M43).
    //
    // What reads a projection is not only the painter: an in-view anchor
    // searches it, and the rules that search decides by -- the kind of point,
    // and refusing an ambiguous choice -- are the whole of M43. Those rules
    // need two candidates placed exactly where the decision is hard, and real
    // geometry cannot be asked for that.
    //
    // No production path calls this. The recompute below is the only thing
    // that fills a projection in a running program.
    void setProjectionForTesting(ProjectedDrawing drawing) {
        projected_ = std::move(drawing);
        state_ = ComputeState::Valid;
    }
    const std::string& diagnostic() const noexcept { return diagnostic_; }

    // --- IRecomputable -------------------------------------------------------
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId id_;
    std::string name_;
    std::string sourcePath_;
    std::string bodyName_;
    ViewDirection direction_{ViewDirection::Front};
    Vec2 positionMm_{0.0, 0.0};
    DrawingScale scale_{1, 1};
    bool ownScale_{false};
    ObjectId parentViewId_{kInvalidObjectId};
    double alignmentOffsetMm_{0.0};
    long long sourceStamp_{0};
    bool showHidden_{true};
    bool showTangent_{false};
    ProjectedDrawing projected_;
    SectionCut section_;
    DetailFrame detail_;
    BreakSpan break_;
    bool flatPattern_ = false;
    // M44. Set when the object is added, from whichever page was current.
    ObjectId sheetId_ = kInvalidObjectId;
    ComputeState state_{ComputeState::Dirty};
    std::string diagnostic_;
};

} // namespace paramcad
