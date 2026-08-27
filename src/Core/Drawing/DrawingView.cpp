#include "Core/Drawing/DrawingView.h"

#include "Core/Drawing/DetailClip.h"
#include "Core/Drawing/FlatPattern.h"
#include "Core/Feature/SheetContourFeature.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include "Core/Drawing/DrawingDocument.h"



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

// The two a section plane needs on top of the three above (M38). Vec3 is a
// bare triple with no arithmetic, and adding operators to the shared header
// for one caller is a change every file pays for.
Vec3 Add(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 Scale(const Vec3& a, double by) noexcept { return Vec3{a.x * by, a.y * by, a.z * by}; }

// A DIRECTION THAT IS ACTUALLY A DIRECTION. Normalised answers zero for a zero
// input rather than NaN, so this is how a caller finds out -- and a NaN would
// travel silently into the projector and come back as a view of nothing.
bool IsRealDirection(const Vec3& a) noexcept { return Dot(a, a) > 1.0e-18; }

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

    // --- THE FLAT PATTERN IS NOT A PROJECTION (M53) -------------------------
    //
    // It is the chain the part was folded from, laid out straight. So this
    // does not touch the kernel at all: there is nothing to look at the solid
    // from, and hidden-line removal of a folded part would answer a question
    // nobody asked.
    //
    // AND IT CANNOT FLATTEN WHAT IT DID NOT FOLD. A solid that arrived through
    // STEP has no chain -- no record of which faces were flanges, which
    // cylinders were bends, or which way the metal went. Handing back a
    // rectangle anyway would be a blank somebody would cut.
    if (flatPattern_) {
        LoadResult loaded = loadPartDocumentFromFile(sourcePath_);
        if (!loaded)
            return fail("the part this flat pattern is of could not be read: " +
                        loaded.message);
        const PartDocument& part = *loaded.document;
        if (!part.sheetMetal().isSheetMetal)
            return fail("'" + sourcePath_ +
                        "' is not a sheet metal part, so there is nothing to unfold");

        const SheetContourFeature* contour = nullptr;
        double widthMm = 0.0;
        for (const std::unique_ptr<Body>& body : part.bodies()) {
            if (!bodyName_.empty() && body->name() != bodyName_) continue;
            for (const std::unique_ptr<Feature>& feature : body->features()) {
                const auto* folded = dynamic_cast<const SheetContourFeature*>(feature.get());
                if (folded == nullptr) continue;
                // MORE THAN ONE IS REFUSED, not silently the first. Two
                // contours in a body is two blanks, and a drawing that showed
                // one of them without saying which is a drawing of half a
                // part.
                if (contour != nullptr)
                    return fail("this part has more than one folded section, and a flat "
                                "pattern can only be of one -- say which body");
                contour = folded;
                const Parameter* width = nullptr;
                for (const std::unique_ptr<Parameter>& one : part.parameters().items())
                    if (one->id() == folded->widthParameterId()) width = one.get();
                if (width == nullptr)
                    return fail("the folded section's width parameter is not in the part");
                widthMm = width->value();
            }
        }
        if (contour == nullptr)
            return fail("nothing in this part was folded from a section, so this program "
                        "has no record of how to unfold it");

        const FlatPatternResultGeometry blank =
            FlatPatternOf(contour->contour(), part.sheetMetal().material,
                          part.sheetMetal().thicknessMm, widthMm);
        if (!blank.ok) return fail(blank.why);

        projected_ = FlatPatternDrawing(blank);
        sourceStamp_ = SourceFileStamp(sourcePath_);
        state_ = ComputeState::Valid;
        diagnostic_.clear();
        return {RecomputeStatus::Success, {}};
    }

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    // "THAT BODY, IN THAT FILE" -- the same resolver an instance uses, so a
    // view and an instance of the same file cannot disagree about what is in
    // it (M32.2).
    const SourceShapeResult resolved = ResolveSourceShape(sourcePath_, bodyName_, context);
    if (!resolved) return fail(resolved.message);

    // --- A DETAIL IS ITS PARENT'S VIEW, ENLARGED (M49) ----------------------
    //
    // So the direction is READ FROM THE PARENT, here, every recompute -- not
    // stored and not asked of the caller. A stored direction would sit still
    // while somebody turned the parent, and the detail would go on showing a
    // face that is no longer there: correctly drawn, correctly labelled, and
    // of nothing on this drawing.
    const DrawingView* detailParent = nullptr;
    if (detail_.active) {
        if (!detail_.usable())
            return fail("this detail's circle has no size, so there is nothing to enlarge");
        auto* owner = dynamic_cast<DrawingDocument*>(&context.document);
        detailParent = owner != nullptr ? owner->findView(parentViewId_) : nullptr;
        if (detailParent == nullptr)
            return fail("a detail view is taken from another view, and this one has no "
                        "parent");
        // A DETAIL OF A DETAIL IS A REAL THING AND NOT THIS ONE. Its circle
        // would be in coordinates that have already been cropped once, and the
        // failure would be a view of somewhere nobody pointed at rather than a
        // refusal.
        if (detailParent->isDetail())
            return fail("a detail of a detail is not supported yet -- take it from the view "
                        "that is not already a detail");
        direction_ = detailParent->direction();
    }

    // THE CAMERA COMES FROM THE ONE TABLE (CameraFor), so the projector never
    // decides which way up this view is.
    ViewCamera camera = CameraFor(direction_);
    DrawingProjectionRequest request;
    request.includeHidden = showHidden_;
    request.includeSmooth = showTangent_;

    // --- A SECTION IS THE ORDINARY PROJECTION OF A CUT SOLID (M38) ----------
    //
    // The cut line lives on the PARENT'S page, so the plane is worked out from
    // the parent's camera HERE, every recompute. Storing the 3D plane instead
    // would leave it pointing the old way the moment somebody turned the
    // parent -- and the section would then be of a place the line no longer
    // crosses, which looks like a perfectly good section of the wrong thing.
    if (section_.active) {
        if (!section_.usable()) return fail("this section's cut line has no length");
        auto* drawing = dynamic_cast<DrawingDocument*>(&context.document);
        const DrawingView* parent =
            drawing != nullptr ? drawing->findView(parentViewId_) : nullptr;
        if (parent == nullptr)
            return fail("a section view has to be cut from another view, and this one has "
                        "no parent");
        // A SECTION OF A SECTION IS A REAL THING AND NOT THIS ONE. Allowing it
        // by accident would build the plane from a camera that is itself
        // derived, and the failure would be a view of something nobody asked
        // for rather than a refusal.
        if (parent->isSection())
            return fail("a section of a section is not supported yet -- cut from a plain "
                        "view instead");

        const ViewCamera parentCamera = CameraFor(parent->direction());
        // The parent's page, as three directions in model space. THE SAME
        // construction the projector uses (see OcctDrawingProjection), because
        // the cut line's coordinates came off that page.
        const Vec3 sight{-parentCamera.towards.x, -parentCamera.towards.y,
                         -parentCamera.towards.z};
        const Vec3 pageX = Cross(parentCamera.up, sight);
        const Vec3 pageY = parentCamera.up;

        const Vec2 along{section_.toMm.x - section_.fromMm.x,
                         section_.toMm.y - section_.fromMm.y};
        const Vec3 lineDirection = Add(Scale(pageX, along.x), Scale(pageY, along.y));
        // The arrows point across the line, in the parent's page. Their
        // direction IS the section view's line of sight.
        const Vec3 arrow = Normalised(Scale(Cross(lineDirection, sight),
                                            section_.arrowSide >= 0 ? 1.0 : -1.0));
        if (!IsRealDirection(arrow))
            return fail("this section's cut line has no direction");

        camera.towards = arrow;
        // THE PARENT'S UP, unless the arrows point along it -- which happens on
        // a horizontal cut, where the section is effectively a top or bottom
        // view and its up is the parent's line of sight instead.
        camera.up = std::fabs(Dot(arrow, parentCamera.up)) > 0.9
                        ? parentCamera.towards
                        : parentCamera.up;

        request.section.active = true;
        request.section.origin = Add(Scale(pageX, section_.fromMm.x),
                                     Scale(pageY, section_.fromMm.y));
        // THE NORMAL POINTS AT THE MATERIAL THAT GOES. Everything between the
        // reader and the plane is removed, and the reader is looking along the
        // arrows -- so the removed side is the one the arrows come FROM.
        request.section.normal = Scale(arrow, -1.0);
    }

    request.towards = camera.towards;
    request.up = camera.up;

    DrawingProjectionResult projection = context.kernel->projectForDrawing(resolved.shape,
                                                                          request);
    if (!projection) return fail(projection.message);

    projected_ = std::move(projection.drawing);
    projected_.cutLoops = std::move(projection.cutLoops);

    // ...AND THEN THE CROP (M49), on the curves the parent's camera produced.
    //
    // Not a second trip through the kernel: hidden-line removal is what costs
    // here, and the answer for this camera is already in hand.
    if (detail_.active) {
        projected_.curves = ClipToCircle(projected_.curves, detail_.centreMm,
                                         detail_.radiusMm);
        // A DETAIL OF NOTHING IS REFUSED. An empty circle with a caption under
        // it does not read as a mistake -- it reads as "this area is
        // featureless", which is a statement about the part that nobody made.
        if (projected_.curves.empty())
            return fail("this detail's circle is not over anything -- move it onto the "
                        "feature it is meant to enlarge");
        // The cut faces belong to the parent's section, and a detail is not
        // taken from one yet, so there is nothing to carry.
        projected_.cutLoops.clear();
        projected_.extent = ProjectedExtent{};
        for (const ProjectedCurve& curve : projected_.curves)
            GrowExtent(projected_.extent, curve);
    }
    // WHEN THE MODEL WAS LAST WRITTEN, taken AFTER a successful projection.
    // Taken before, a failed build would still stamp the view as current and
    // the drawing would stop offering to update itself.
    sourceStamp_ = SourceFileStamp(sourcePath_);
    state_ = ComputeState::Valid;
    diagnostic_.clear();
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
