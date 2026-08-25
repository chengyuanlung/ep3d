#include "Core/Drawing/DrawingView.h"

#include "Core/Recompute/RecomputeContext.h"

#include <utility>

namespace paramcad {

std::string_view toString(ViewDirection direction) noexcept {
    switch (direction) {
        case ViewDirection::Front: return "Front";
        case ViewDirection::Back: return "Back";
        case ViewDirection::Left: return "Left";
        case ViewDirection::Right: return "Right";
        case ViewDirection::Top: return "Top";
        case ViewDirection::Bottom: return "Bottom";
        case ViewDirection::Isometric: return "Isometric";
    }
    return "Front";
}

ViewCamera CameraFor(ViewDirection direction) noexcept {
    // THE CONVENTION, once: +Z is up in the model, and the FRONT view looks
    // along +Y. Everything else follows from that by turning, which is why
    // these are written as a table rather than derived by a formula nobody
    // can check against a drawing.
    //
    // The `up` of a top or bottom view cannot be +Z -- you are looking down
    // it -- so those two take +Y and -Y, which is what puts the front of the
    // part at the bottom of the top view. That is the one place this table is
    // not mechanical, and it is the place a formula would have got wrong.
    switch (direction) {
        case ViewDirection::Front: return ViewCamera{Vec3{0.0, 1.0, 0.0}, Vec3{0.0, 0.0, 1.0}};
        case ViewDirection::Back: return ViewCamera{Vec3{0.0, -1.0, 0.0}, Vec3{0.0, 0.0, 1.0}};
        case ViewDirection::Left: return ViewCamera{Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 0.0, 1.0}};
        case ViewDirection::Right: return ViewCamera{Vec3{-1.0, 0.0, 0.0}, Vec3{0.0, 0.0, 1.0}};
        case ViewDirection::Top: return ViewCamera{Vec3{0.0, 0.0, -1.0}, Vec3{0.0, 1.0, 0.0}};
        case ViewDirection::Bottom: return ViewCamera{Vec3{0.0, 0.0, 1.0}, Vec3{0.0, -1.0, 0.0}};
        case ViewDirection::Isometric:
            // The standard isometric: down the (-1, 1, -1) diagonal, so all
            // three axes make equal angles with the paper.
            //
            // ITS UP IS NOT +Z. +Z has a component ALONG this direction, and a
            // camera whose up leans into its own line of sight projects that
            // up to a shorter vector -- or, looked at another way, is not a
            // camera. What is wanted is the part of +Z perpendicular to the
            // sight line, which is (-1, 1, 2). The five square-on directions
            // can take +Z unchanged because they are perpendicular to it
            // already; this one cannot, and the first draft took it anyway.
            //
            // Found by M32_VIEW_004, which asked the table the one question
            // that has an answer for every entry at once.
            return ViewCamera{Vec3{-1.0, 1.0, -1.0}, Vec3{-1.0, 1.0, 2.0}};
    }
    return ViewCamera{};
}

DrawingView::DrawingView(std::string name, std::string sourcePath, std::string bodyName,
                         ViewDirection direction, Vec2 positionMm)
    : id_(ObjectIdGenerator::Next()),
      name_(std::move(name)),
      sourcePath_(std::move(sourcePath)),
      bodyName_(std::move(bodyName)),
      direction_(direction),
      positionMm_(positionMm) {}

DrawingView::DrawingView(ObjectId id, std::string name, ComputeState state,
                         std::string sourcePath, std::string bodyName, ViewDirection direction,
                         Vec2 positionMm, DrawingScale scale, bool ownScale)
    : id_(RestoreObjectId(id)),
      name_(std::move(name)),
      sourcePath_(std::move(sourcePath)),
      bodyName_(std::move(bodyName)),
      direction_(direction),
      positionMm_(positionMm),
      scale_(scale),
      ownScale_(ownScale),
      state_(state) {}

void DrawingView::setScale(const DrawingScale& scale) {
    if (!scale.valid()) return;
    scale_ = scale;
    ownScale_ = true;
}

DrawingScale DrawingView::effectiveScale(const DrawingScale& sheetScale) const noexcept {
    return ownScale_ ? scale_ : sheetScale;
}

RecomputeResult DrawingView::recompute(const RecomputeContext& context) {
    (void)context;
    // M32.1 BUILDS NOTHING, and says so rather than reporting success.
    //
    // A view that answered Success while producing no geometry would be a node
    // the tree shows as up to date over an empty patch of paper -- and the
    // next milestone would have to work out whether the projector was broken
    // or had never been called. The projection arrives in M32.2; until then
    // this is an honest "not yet".
    state_ = ComputeState::Failed;
    diagnostic_ = "this view has nothing projected into it yet (M32.2)";
    return RecomputeResult{RecomputeStatus::Failed, diagnostic_};
}

} // namespace paramcad
