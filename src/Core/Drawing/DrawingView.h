#pragma once

#include "Core/Drawing/Sheet.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Recompute/IRecomputable.h"

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
                DrawingScale scale, bool ownScale);

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

    // Where the view's ORIGIN sits on the paper, in sheet millimetres from the
    // bottom-left corner. The origin is the projection of the model origin,
    // not the centre of what is drawn -- so a part that grows does not drag
    // its own view across the page.
    Vec2 positionMm() const noexcept { return positionMm_; }
    void setPositionMm(Vec2 positionMm) noexcept { positionMm_ = positionMm; }

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

    ComputeState currentState() const noexcept { return state_; }
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
    ComputeState state_{ComputeState::Dirty};
    std::string diagnostic_;
};

} // namespace paramcad
