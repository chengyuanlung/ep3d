#include "Core/Drawing/DrawingView.h"

#include "Core/Document/SourceShapeResolver.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"

#include <cmath>
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

std::string_view toString(ViewAlignment alignment) noexcept {
    switch (alignment) {
        case ViewAlignment::None: return "None";
        case ViewAlignment::Horizontal: return "Horizontal";
        case ViewAlignment::Vertical: return "Vertical";
    }
    return "None";
}

namespace {

double Dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 Normalised(const Vec3& v) noexcept {
    const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 1.0e-12)) return Vec3{0.0, 0.0, 0.0};
    return Vec3{v.x / length, v.y / length, v.z / length};
}

} // namespace

ViewAlignmentRule AlignmentOf(ViewDirection parent, ViewDirection child) noexcept {
    // DERIVED, NOT TABULATED. "The top view goes above the front" is not an
    // independent fact -- it follows from the top view looking ALONG the front
    // view's up vector. Writing the placements down as a second table would be
    // a second thing to keep in step, and the first draft of this file already
    // had one table too many.
    const ViewCamera from = CameraFor(parent);
    const ViewCamera to = CameraFor(child);
    const Vec3 sight = Normalised(Vec3{-from.towards.x, -from.towards.y, -from.towards.z});
    const Vec3 up = Normalised(from.up);
    // The page's X, by the same formula the projector uses: page Y is up, and
    // page X completes the frame.
    const Vec3 pageX = Normalised(Cross(up, sight));
    const Vec3 look = Normalised(to.towards);

    constexpr double kSquare = 1.0 - 1.0e-9;

    // LOOKING ALONG THE PARENT'S UP means the child is the view from above or
    // below, so it slides vertically. In third angle the view from ABOVE goes
    // above -- and looking from above means looking along MINUS up, so the
    // sign is the negative of the dot.
    const double vertical = Dot(look, up);
    if (std::fabs(vertical) >= kSquare)
        return ViewAlignmentRule{ViewAlignment::Vertical, vertical > 0.0 ? -1 : 1};

    const double horizontal = Dot(look, pageX);
    if (std::fabs(horizontal) >= kSquare)
        return ViewAlignmentRule{ViewAlignment::Horizontal, horizontal > 0.0 ? -1 : 1};

    // NOT SQUARE TO THE PARENT -- an isometric beside a front view. Such a
    // view is not aligned to anything, and saying so is better than inventing
    // a side for it to sit on.
    return ViewAlignmentRule{};
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
                         Vec2 positionMm, DrawingScale scale, bool ownScale, bool showHidden,
                         bool showTangent, ObjectId parentViewId, double alignmentOffsetMm)
    : id_(RestoreObjectId(id)),
      name_(std::move(name)),
      sourcePath_(std::move(sourcePath)),
      bodyName_(std::move(bodyName)),
      direction_(direction),
      positionMm_(positionMm),
      scale_(scale),
      ownScale_(ownScale),
      parentViewId_(parentViewId),
      alignmentOffsetMm_(alignmentOffsetMm),
      showHidden_(showHidden),
      showTangent_(showTangent),
      state_(state) {}

double DrawingView::paperWidthMm(const DrawingScale& sheetScale) const noexcept {
    return projected_.extent.widthMm() * effectiveScale(sheetScale).factor();
}

double DrawingView::paperHeightMm(const DrawingScale& sheetScale) const noexcept {
    return projected_.extent.heightMm() * effectiveScale(sheetScale).factor();
}

void DrawingView::setScale(const DrawingScale& scale) {
    if (!scale.valid()) return;
    scale_ = scale;
    ownScale_ = true;
}

DrawingScale DrawingView::effectiveScale(const DrawingScale& sheetScale) const noexcept {
    return ownScale_ ? scale_ : sheetScale;
}

RecomputeResult DrawingView::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        // THE OLD CURVES GO. A view that failed while still holding what it
        // drew last time is a drawing that shows a part which no longer
        // builds -- and the tree says "failed" over a picture that looks
        // fine, which is the worst of both.
        projected_ = ProjectedDrawing{};
        state_ = ComputeState::Failed;
        diagnostic_ = message;
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    // "THAT BODY, IN THAT FILE" -- the same resolver an instance uses, so a
    // view and an instance of the same file cannot disagree about what is in
    // it (M32.2).
    const SourceShapeResult resolved = ResolveSourceShape(sourcePath_, bodyName_, context);
    if (!resolved) return fail(resolved.message);

    // THE CAMERA COMES FROM THE ONE TABLE (CameraFor), so the projector never
    // decides which way up this view is.
    const ViewCamera camera = CameraFor(direction_);
    DrawingProjectionRequest request;
    request.towards = camera.towards;
    request.up = camera.up;
    request.includeHidden = showHidden_;
    request.includeSmooth = showTangent_;

    DrawingProjectionResult projection = context.kernel->projectForDrawing(resolved.shape,
                                                                          request);
    if (!projection) return fail(projection.message);

    projected_ = std::move(projection.drawing);
    // WHEN THE MODEL WAS LAST WRITTEN, taken AFTER a successful projection.
    // Taken before, a failed build would still stamp the view as current and
    // the drawing would stop offering to update itself.
    sourceStamp_ = SourceFileStamp(sourcePath_);
    state_ = ComputeState::Valid;
    diagnostic_.clear();
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
