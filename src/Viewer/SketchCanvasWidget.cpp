#include "Viewer/SketchCanvasWidget.h"

#include "Core/Document/PartDocument.h"
#include "Core/Sketch/ISketchSolver.h"
#include "Core/Sketch/Sketch.h"

#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <variant>

namespace paramcad {
namespace {

constexpr double kPi = 3.14159265358979323846;

// A click is forgiving by PIXELS, never by millimetres: 8 mm is generous at
// 1:1 and invisible at 50:1, and a tolerance that changes meaning with the zoom
// is a tolerance the user cannot learn.
constexpr double kPickRadiusPx = 8.0;

QPointF ToQt(Vec2 p) { return QPointF(p.x, p.y); }

// Colours. Roadmap 8.1's semantics -- blue under-constrained, black fully
// constrained, red in trouble -- with A06's rule applied: NONE of these is the
// only channel. The status bar carries a text badge, the constraint list has a
// column, and selection is a stroke width as well as a colour.
struct Palette {
    QColor background{250, 250, 250};
    QColor grid{233, 233, 233};
    QColor gridMajor{216, 216, 216};
    QColor axis{176, 176, 176};
    QColor underConstrained{40, 90, 210};
    QColor fullyConstrained{25, 25, 25};
    QColor trouble{200, 40, 40};
    QColor selection{240, 140, 0};
    QColor hover{120, 190, 255};
    QColor dimension{90, 90, 90};
    QColor dimensionText{25, 25, 25};
    QColor preview{150, 150, 150};
    QColor glyph{70, 110, 70};
    // Projected reference geometry: a muted teal, distinct from both the blue
    // of under-constrained geometry and the grey of the axes -- and never the
    // ONLY thing separating it from either (A06). See the painting loop.
    QColor reference{110, 150, 155};
};

const Palette& Colours() {
    static const Palette palette;
    return palette;
}

QColor GeometryColour(SketchSolveStatus status) {
    const Palette& palette = Colours();
    switch (status) {
    case SketchSolveStatus::Solved: return palette.fullyConstrained;
    case SketchSolveStatus::UnderConstrained: return palette.underConstrained;
    case SketchSolveStatus::OverConstrained:
    case SketchSolveStatus::Conflicting:
    case SketchSolveStatus::InvalidInput:
    case SketchSolveStatus::NumericalFailure: return palette.trouble;
    }
    return palette.underConstrained;
}

// The badge's box in PIXELS, from the Qt-free layout.
//
// ONE definition, read by the painter AND by the hit-test. They used to be the
// same code only because they were the same three lines; once a badge became
// clickable that stopped being good enough, because the first edit to either
// copy makes a badge unclickable exactly where it is drawn.
//
// Pixels and not millimetres: a badge is READ rather than measured, so it keeps
// its size on screen at every zoom -- the same argument as the arrowheads
// below. Wide enough for TWO characters, because "//" and "|_" exist and a
// second channel that is clipped into something unreadable is not a channel.
// How far `p` is from the SEGMENT, in mm.
//
// The segment and not the infinite line: trim asks "which line did you click
// on", and an infinite line would answer with one the user's cursor is nowhere
// near.
double DistanceToSegmentMm(const SketchLine& line, Vec2 p) noexcept {
    const double du = line.end.x - line.start.x;
    const double dv = line.end.y - line.start.y;
    const double lengthSquared = du * du + dv * dv;
    double t = 0.0;
    if (lengthSquared > 1e-18) {
        t = ((p.x - line.start.x) * du + (p.y - line.start.y) * dv) / lengthSquared;
        t = std::clamp(t, 0.0, 1.0);
    }
    const double nx = line.start.x + du * t - p.x;
    const double ny = line.start.y + dv * t - p.y;
    return std::sqrt(nx * nx + ny * ny);
}

QRectF BadgeBox(const CanvasView& view, const ConstraintBadge& badge) {
    const Vec2 p = view.toPixels(badge.anchorMm);
    return QRectF(p.x + 6.0, p.y + 6.0 + badge.slot * 15.0, 19.0, 14.0);
}

// Half-angle of an arrowhead, and its length in PIXELS.
//
// Pixels, not millimetres: the dimension line belongs to the drawing and scales
// with the zoom, but an arrowhead is READ rather than measured. One that scaled
// would be an invisible speck at 1:10 and would swallow a small feature at
// 50:1 -- which is exactly when a user is zoomed in to check it.
constexpr double kArrowLengthPx = 11.0;
constexpr double kArrowHalfWidthPx = 3.6;

// A solid filled head, the mechanical-drawing convention. An open V reads as a
// leader; a filled triangle reads as a dimension terminator.
QPainterPath ArrowHead(QPointF tip, QPointF direction) {
    const double length = std::hypot(direction.x(), direction.y());
    QPointF unit(1.0, 0.0);
    if (length > 1e-9) unit = QPointF(direction.x() / length, direction.y() / length);
    const QPointF back(tip.x() - unit.x() * kArrowLengthPx, tip.y() - unit.y() * kArrowLengthPx);
    const QPointF side(-unit.y() * kArrowHalfWidthPx, unit.x() * kArrowHalfWidthPx);

    QPainterPath path;
    path.moveTo(tip);
    path.lineTo(back + side);
    path.lineTo(back - side);
    path.closeSubpath();
    return path;
}

} // namespace

SketchCanvasWidget::SketchCanvasWidget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);
    setAutoFillBackground(true);
}

const Sketch* SketchCanvasWidget::sketch() const {
    if (document_ == nullptr || sketchId_ == kInvalidObjectId) return nullptr;
    return document_->findSketch(sketchId_);
}

void SketchCanvasWidget::setSketch(PartDocument* document, ObjectId sketchId) {
    document_ = document;
    sketchId_ = sketchId;
    model_.setTool(SketchTool::Select);
    model_.clearSelection();
    fittedOnce_ = false;
    hasHover_ = false;
    syncViewSize();
    fitView();
    update();
    emit presentationChanged();
}

void SketchCanvasWidget::syncViewSize() {
    view_.widthPx = std::max(width(), 1);
    view_.heightPx = std::max(height(), 1);
}

void SketchCanvasWidget::fitView() {
    syncViewSize();
    const Sketch* target = sketch();
    if (target == nullptr) {
        view_ = CanvasView{};
        syncViewSize();
        return;
    }
    const int widthPx = view_.widthPx;
    const int heightPx = view_.heightPx;
    view_ = FitView(*target, widthPx, heightPx);
    fittedOnce_ = true;
    update();
}

double SketchCanvasWidget::toleranceMm() const { return view_.toSketchLength(kPickRadiusPx); }

void SketchCanvasWidget::setTool(SketchTool tool) {
    // PICKING A TOOL LEAVES TRIM MODE.
    //
    // Trim intercepts the whole click (it has to -- the pick point IS its
    // input), so a trim mode left running makes every drawing tool look broken:
    // the button lights up, the cursor is a crosshair, and nothing is ever
    // drawn. There is one active mode on this canvas, and choosing a tool is
    // choosing it.
    trimming_ = false;
    extending_ = false;
    useMode_ = false;
    dimensioning_ = false;
    hoveredReference_ = kInvalidSketchReferenceId;
    model_.setTool(tool);
    update();
    emit presentationChanged();
}

void SketchCanvasWidget::clearSelection() {
    if (model_.clearSelection()) {
        update();
        emit presentationChanged();
    }
}

QString SketchCanvasWidget::promptText() const { return QString::fromStdString(model_.prompt()); }

SketchStatusLine SketchCanvasWidget::statusLine() const {
    const Sketch* target = sketch();
    if (target == nullptr) {
        SketchStatusLine line;
        line.badge = "NO SKETCH";
        line.text = "No sketch open";
        return line;
    }
    return DescribeSketchStatus(*target);
}

void SketchCanvasWidget::refreshAfterDocumentChange() {
    // The solve that follows an edit is what produces the DOF the status bar
    // reports, so a canvas that skipped it would show the PREVIOUS sketch's
    // state next to the new geometry.
    if (document_ != nullptr) (void)document_->recompute();
    update();
}

QString SketchCanvasWidget::applyEdit(const SketchEdit& edit) {
    if (document_ == nullptr || sketchId_ == kInvalidObjectId) return QStringLiteral("No sketch.");
    if (!edit.valid()) return QString();

    const SketchEditOutcome outcome = ApplySketchEdit(*document_, sketchId_, edit);
    // Kept so a command can act on what it just made -- Offset selects its
    // copy. Set on failure too, to the empty list, so a stale success can never
    // be mistaken for this one's result.
    lastCreatedEntities_ = outcome.createdEntities;
    // The CONSTRAINTS too (M17.18). The dimension tool places the dimension it
    // just made, and it can only do that if it knows which one that is --
    // searching the sketch for "the newest" would be a position used as
    // identity, which is what ADR-M4-004 forbids.
    lastCreatedConstraints_ = outcome.createdConstraints;
    // Told to the model whether it succeeded or not: on failure the empty list
    // is what keeps a chained line from inventing a reference to geometry that
    // was never created.
    model_.afterApply(outcome.createdEntities);
    if (outcome.applied) {
        refreshAfterDocumentChange();
        const QString status = QString::fromStdString(outcome.status);
        emit documentChanged(status);
        return status;
    }
    update();
    emit presentationChanged();
    return QString::fromStdString(outcome.status);
}

QString SketchCanvasWidget::clickAt(Vec2 sketchMm) {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    // EXTEND MODE, like trim, takes the click whole.
    if (extending_) {
        const QString status = extendAt(sketchMm);
        update();
        emit presentationChanged();
        return status;
    }

    // DIMENSION MODE takes the click whole: every click in it is either a pick
    // or a placement, and letting one fall through to ordinary selection would
    // silently drop the picks made so far.
    if (dimensioning_) {
        const QString status = dimensionClickAt(sketchMm);
        update();
        emit presentationChanged();
        return status;
    }

    // USE MODE takes the click whole, for the same reason Trim does.
    if (useMode_) {
        const QString status = useReferenceAt(sketchMm);
        update();
        emit presentationChanged();
        return status;
    }

    // TRIM MODE takes the click whole. It is a mode, and a mode that let
    // clicks fall through to selection would be a mode only in name.
    if (trimming_) {
        const QString status = trimAt(sketchMm);
        update();
        emit presentationChanged();
        return status;
    }

    if (model_.tool() == SketchTool::Select) {
        // A constraint badge is a small, deliberate target drawn on top of
        // everything, and roadmap 6.3's own words for this are "click the
        // constraint icon on the canvas, then press Delete".
        const SketchConstraintId badge = constraintBadgeAt(sketchMm);
        if (badge != kInvalidSketchConstraintId) {
            // The entity selection is cleared ON PURPOSE, and this is the one
            // place a highlight touches it. Delete has to mean one thing: with
            // edges still selected, it would be a question rather than a
            // command. Picking a badge is the user saying "I mean THIS
            // constraint, not those edges".
            model_.clearSelection();
            setHighlightedConstraint(badge);
            emit constraintPicked(static_cast<qulonglong>(ToObjectId(badge)));
            update();
            emit presentationChanged();
            return promptText();
        }
        // Clicking anything else drops the badge highlight, for the same
        // reason: it keeps Delete unambiguous.
        if (highlighted_ != kInvalidSketchConstraintId) {
            setHighlightedConstraint(kInvalidSketchConstraintId);
            emit constraintPicked(0);
        }
        model_.selectAt(*target, sketchMm, toleranceMm());
        update();
        emit presentationChanged();
        return promptText();
    }

    const SnapResult snap = SnapCursor(*target, sketchMm, toleranceMm(), GridStepMm(view_),
                                       model_.suppressInference());
    const SketchEdit edit = model_.click(snap);
    if (!edit.valid()) {
        update();
        emit presentationChanged();
        return promptText();
    }
    return applyEdit(edit);
}

bool SketchCanvasWidget::selectAt(Vec2 sketchMm) {
    const Sketch* target = sketch();
    if (target == nullptr) return false;
    const bool changed = model_.selectAt(*target, sketchMm, toleranceMm());
    if (changed) {
        update();
        emit presentationChanged();
    }
    return changed;
}

QString SketchCanvasWidget::applyConstraint(SketchEditKind kind) {
    lastCommandApplied_ = false;
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");
    std::string whyNot;
    const SketchEdit edit = model_.requestConstraint(*target, kind, &whyNot);
    if (!edit.valid()) return QString::fromStdString(whyNot);
    lastCommandApplied_ = true;
    const QString status = applyEdit(edit);
    // A constraint that was applied has done its job; leaving the selection
    // behind invites a second identical constraint on the next keypress.
    model_.clearSelection();
    update();
    return status;
}

QString SketchCanvasWidget::applyDimension(SketchEditKind explicitKind) {
    lastCommandApplied_ = false;
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");
    std::string whyNot;
    const SketchEdit edit = model_.requestDimension(*target, explicitKind, &whyNot);
    if (!edit.valid()) return QString::fromStdString(whyNot);
    lastCommandApplied_ = true;
    const QString status = applyEdit(edit);
    model_.clearSelection();
    update();
    return status;
}

QString SketchCanvasWidget::deleteSelection() {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");
    std::string whyNot;
    const SketchEdit edit = model_.requestDelete(*target, &whyNot);
    if (!edit.valid()) return QString::fromStdString(whyNot);
    const QString status = applyEdit(edit);
    model_.clearSelection();
    update();
    return status;
}

void SketchCanvasWidget::setHighlightedConstraint(SketchConstraintId constraintId) {
    if (highlighted_ == constraintId) return;
    highlighted_ = constraintId;
    update();
    // Presentation only: highlighting changes nothing in the document, so it
    // must not reach the undo stack (roadmap 15 point 1).
    emit presentationChanged();
}

SketchConstraintId SketchCanvasWidget::constraintBadgeAt(Vec2 sketchMm) const {
    const Sketch* target = sketch();
    if (target == nullptr) return kInvalidSketchConstraintId;
    const QPointF pixels = ToQt(view_.toPixels(sketchMm));
    // LAST match wins, so the badge drawn on top is the one picked. Badges
    // stack downwards from a shared anchor and their boxes do not overlap, but
    // the rule costs nothing and is the one the painter implies.
    SketchConstraintId hit = kInvalidSketchConstraintId;
    for (const ConstraintBadge& badge : ConstraintBadgesFor(*target))
        if (BadgeBox(view_, badge).contains(pixels)) hit = badge.id;
    return hit;
}

bool SketchCanvasWidget::constraintBadgeCentre(SketchConstraintId constraintId,
                                               Vec2* sketchMm) const {
    const Sketch* target = sketch();
    if (target == nullptr || sketchMm == nullptr) return false;
    for (const ConstraintBadge& badge : ConstraintBadgesFor(*target)) {
        if (badge.id != constraintId) continue;
        const QPointF centre = BadgeBox(view_, badge).center();
        *sketchMm = view_.toSketch(Vec2{centre.x(), centre.y()});
        return true;
    }
    return false;
}

QString SketchCanvasWidget::deleteHighlightedConstraint() {
    if (highlighted_ == kInvalidSketchConstraintId) return QString();
    return deleteConstraint(highlighted_);
}

SketchEntityId SketchCanvasWidget::originPoint() const {
    const Sketch* target = sketch();
    return target != nullptr ? FindSketchOrigin(*target) : kInvalidSketchEntityId;
}

void SketchCanvasWidget::setTrimming(bool on) {
    if (trimming_ == on) return;
    trimming_ = on;
    if (on) {
        extending_ = false;
        useMode_ = false;
        dimensioning_ = false;
        // Trim takes its input from WHERE you click, so a selection would only
        // be there to be mistaken for the thing being trimmed.
        model_.setTool(SketchTool::Select);
        model_.clearSelection();
    }
    update();
    emit presentationChanged();
}

void SketchCanvasWidget::setDimensioning(bool on) {
    if (dimensioning_ == on) return;
    dimensioning_ = on;
    if (on) {
        trimming_ = false;
        extending_ = false;
        useMode_ = false;
        // The selection is the tool's INPUT here, unlike Trim and Use where a
        // live selection could only be mistaken for the target. It starts
        // empty so the first click begins a new dimension rather than
        // dimensioning whatever happened to be selected when the mode was
        // switched on.
        model_.setTool(SketchTool::Select);
        model_.clearSelection();
    }
    update();
    emit presentationChanged();
}

QString SketchCanvasWidget::dimensionClickAt(Vec2 sketchMm) {
    lastCommandApplied_ = false;
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    // READY MEANS PLACE. Once the selection describes a dimension, the next
    // click is where the dimension goes -- including a click on geometry,
    // which is how Onshape behaves and the only rule that does not require a
    // user to find empty space to finish in.
    std::string whyNot;
    const SketchEdit ready = model_.requestDimension(*target, SketchEditKind::None, &whyNot);
    if (ready.valid()) {
        // WHERE THE USER PUT IT, carried ON the edit so the dimension and its
        // position are one undo step. It is written into the same placement
        // the M16 drag machinery owns, so a dimension placed at creation and
        // one dragged there afterwards are the same stored fact.
        SketchEdit placed = ready;
        placed.hasDimensionPlacement = true;
        placed.dimensionPlacement = sketchMm;

        const QString status = applyEdit(placed);
        if (lastCreatedConstraints_.empty()) {
            // The edit was refused. The selection is KEPT, because the user's
            // picks are still what they meant and making them pick again would
            // punish them for the document's answer.
            return status;
        }
        lastCommandApplied_ = true;
        model_.clearSelection();
        update();
        emit presentationChanged();
        return status;
    }

    // Not ready: this click is a PICK. Adding to the selection rather than
    // replacing it, because a dimension between two points needs both and the
    // ordinary select-click replaces.
    const SketchElementRef hit = PickElement(*target, sketchMm, toleranceMm());
    if (hit.entityId == kInvalidSketchEntityId) {
        model_.clearSelection();
        update();
        emit presentationChanged();
        return QStringLiteral("Dimension: click the geometry to measure.");
    }
    model_.toggleSelection(hit);
    update();
    emit presentationChanged();

    // What it WILL be, before it is made. A user who has picked one of two
    // points needs to know the tool is waiting rather than confused.
    std::string nextWhy;
    const SketchEdit next = model_.requestDimension(*target, SketchEditKind::None, &nextWhy);
    if (next.valid())
        return QStringLiteral("Dimension: %1 -- click where the dimension line goes.")
            .arg(QString::fromStdString(SketchEditKindName(next.kind)));
    return QStringLiteral("Dimension: %1").arg(QString::fromStdString(nextWhy));
}

void SketchCanvasWidget::setUseReference(bool on) {
    if (useMode_ == on) return;
    useMode_ = on;
    if (on) {
        trimming_ = false;
        extending_ = false;
        dimensioning_ = false;
        // Like Trim, Use takes its input from WHERE you click, so a live
        // selection could only be mistaken for the thing about to be converted.
        model_.setTool(SketchTool::Select);
        model_.clearSelection();
    }
    hoveredReference_ = kInvalidSketchReferenceId;
    update();
    emit presentationChanged();
}

QString SketchCanvasWidget::useReferenceAt(Vec2 sketchMm) {
    lastCommandApplied_ = false;
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");
    if (target->references().empty())
        return QStringLiteral("This sketch has no projected reference geometry -- make a "
                              "sketch on a face to get some.");

    // The DECISION, including every refusal, is PlanConvertReference's, so
    // that all of it is reachable without a mouse. What is left here is asking
    // what was clicked and showing the answer.
    const ConvertReferencePlan plan =
        PlanConvertReference(*target, ReferenceAt(*target, sketchMm, toleranceMm()));
    if (!plan.ok) return QString::fromStdString(plan.message);

    const QString applied = applyEdit(plan.edit);
    if (!lastCreatedEntities_.empty()) {
        lastCommandApplied_ = true;
        // The PLAN's wording, not applyEdit's generic "Line added": what a user
        // needs to know here is what got fixed and what is still free.
        return QString::fromStdString(plan.message);
    }
    return applied;
}

QString SketchCanvasWidget::trimAt(Vec2 sketchMm) {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    // What was clicked. Only a LINE can be trimmed, and PickElement already
    // prefers points -- so ask for the curve under the cursor directly.
    // Lines AND arcs -- both can be trimmed since M17. PickElement is no use
    // here: it prefers POINTS, and trim is asked about the body of a curve.
    SketchEntityId victim = kInvalidSketchEntityId;
    double best = toleranceMm();
    for (const SketchEntity& entity : target->entities()) {
        double distance = 0.0;
        if (const auto* line = std::get_if<SketchLine>(&entity.geometry)) {
            distance = DistanceToSegmentMm(*line, sketchMm);
        } else if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) {
            const double angle =
                std::atan2(sketchMm.y - arc->center.y, sketchMm.x - arc->center.x);
            if (!AngleOnArcSweep(*arc, angle)) continue; // beside the arc, not on it
            const double radial = std::hypot(sketchMm.x - arc->center.x,
                                             sketchMm.y - arc->center.y);
            distance = std::abs(radial - arc->radiusMm);
        } else if (std::holds_alternative<SketchEllipse>(entity.geometry) ||
                   std::holds_alternative<SketchEllipticalArc>(entity.geometry) ||
                   std::holds_alternative<SketchSpline>(entity.geometry)) {
            // Through the SHARED picker, which already knows how to measure the
            // distance to an ellipse. Adding a second sampler here would be a
            // second answer to the same question, and this one decides what
            // gets deleted.
            Vec2 nearest{};
            distance = DistanceToSketchGeometry(entity.geometry, sketchMm, &nearest);
            if (distance < 0.0) continue;
        } else {
            continue;
        }
        if (distance > best) continue;
        best = distance;
        victim = entity.id;
    }
    if (victim == kInvalidSketchEntityId)
        return QStringLiteral("Click the part of a line or arc you want to remove.");

    // EVERYTHING ELSE cuts. Nominating cutters is a second picking mode this
    // command does not have yet, and "all of them" is what AutoCAD does when
    // the user presses ENTER at the cutter prompt.
    std::vector<SketchEntityId> cutters;
    for (const SketchEntity& entity : target->entities())
        if (entity.id != victim) cutters.push_back(entity.id);

    const TrimPlan plan = PlanTrim(*target, victim, cutters, sketchMm);
    if (!plan.ok) return QString::fromStdString(plan.why);

    if (!document_->setSketchEntityGeometry(sketchId_, plan.target, plan.result))
        return QStringLiteral("The sketch refused that trim.");
    refreshAfterDocumentChange();
    update();
    const QString status =
        QStringLiteral("Trimmed the %1 of the line back to the nearest crossing.")
            .arg(plan.trimmedStart ? QStringLiteral("start") : QStringLiteral("end"));
    emit documentChanged(status);
    return status;
}

void SketchCanvasWidget::setExtending(bool on) {
    if (extending_ == on) return;
    extending_ = on;
    if (on) {
        // The picking modes are mutually exclusive: one click can only mean one
        // thing, and a canvas in two of them would have to choose silently.
        trimming_ = false;
        useMode_ = false;
        dimensioning_ = false;
        model_.setTool(SketchTool::Select);
        model_.clearSelection();
    }
    update();
    emit presentationChanged();
}

QString SketchCanvasWidget::extendAt(Vec2 sketchMm) {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    SketchEntityId victim = kInvalidSketchEntityId;
    double best = toleranceMm();
    for (const SketchEntity& entity : target->entities()) {
        const auto* line = std::get_if<SketchLine>(&entity.geometry);
        if (line == nullptr) continue;
        const double distance = DistanceToSegmentMm(*line, sketchMm);
        if (distance > best) continue;
        best = distance;
        victim = entity.id;
    }
    if (victim == kInvalidSketchEntityId)
        return QStringLiteral("Click near the end of a line you want to extend.");

    std::vector<SketchEntityId> boundaries;
    for (const SketchEntity& entity : target->entities())
        if (entity.id != victim) boundaries.push_back(entity.id);

    const TrimPlan plan = PlanExtend(*target, victim, boundaries, sketchMm);
    if (!plan.ok) return QString::fromStdString(plan.why);
    if (!document_->setSketchEntityGeometry(sketchId_, plan.target, plan.result))
        return QStringLiteral("The sketch refused that extend.");
    refreshAfterDocumentChange();
    update();
    const QString status = QStringLiteral("Extended the %1 of the line to the first crossing.")
                               .arg(plan.trimmedStart ? QStringLiteral("start")
                                                      : QStringLiteral("end"));
    emit documentChanged(status);
    return status;
}

namespace {

// An ellipse (or a piece of one) as a screen polyline.
//
// Qt's drawArc cannot draw a ROTATED ellipse -- its rectangle is axis-aligned,
// and rotating the painter for each one would leave the pen width and the
// dash pattern rotated with it. Sampling is what every renderer does for this,
// and the sample count is chosen from the on-screen size so a 6-pixel ellipse
// is not paying for 200 segments and a full-screen one does not show facets.
QPolygonF EllipsePolyline(const CanvasView& view, Vec2 centre, double major, double minor,
                          double rotation, double fromParam, double sweep) {
    const double pixels = view.toPixelLength(major);
    const int steps = std::clamp(static_cast<int>(pixels * 1.5), 24, 400);
    QPolygonF polyline;
    polyline.reserve(steps + 1);
    for (int i = 0; i <= steps; ++i) {
        const Vec2 at = PointOnEllipse(centre, major, minor, rotation,
                                       fromParam + sweep * (static_cast<double>(i) / steps));
        const Vec2 p = view.toPixels(at);
        polyline << QPointF(p.x, p.y);
    }
    return polyline;
}

// The sweep an elliptical arc covers, in its own direction.
double EllipseArcSweep(const SketchEllipticalArc& arc) {
    constexpr double kTwoPiLocal = 6.283185307179586476925286766559;
    double sweep = arc.endParamRad - arc.startParamRad;
    if (arc.counterClockwise) {
        while (sweep <= 0.0) sweep += kTwoPiLocal;
    } else {
        while (sweep >= 0.0) sweep -= kTwoPiLocal;
    }
    return sweep;
}

// Draws whichever of the two ellipse kinds `geometry` is, or returns false when
// it is neither. Shared by the reference underlay and the geometry pass for the
// reason every shared drawing helper here exists: two copies of a sampling rule
// are two places for a curve to come out a different shape.
// A spline as a screen polyline, sampled finely enough for its size on screen.
bool DrawSplineGeometry(QPainter& painter, const CanvasView& view,
                        const SketchGeometry& geometry) {
    const auto* spline = std::get_if<SketchSpline>(&geometry);
    if (spline == nullptr || spline->points.size() < kMinSplinePoints) return false;
    // Sampled from the SHARED sampler, so what is drawn is what gets picked and
    // what the profile's winding test walks. A prettier curve drawn here would
    // be a curve the rest of the program does not believe in.
    double longest = 0.0;
    for (std::size_t i = 1; i < spline->points.size(); ++i)
        longest = std::max(longest, std::hypot(spline->points[i].x - spline->points[i - 1].x,
                                               spline->points[i].y - spline->points[i - 1].y));
    const int perSpan = std::clamp(static_cast<int>(view.toPixelLength(longest) / 4.0), 4, 40);
    const std::vector<Vec2> sampled = SampleSpline(*spline, perSpan);
    QPolygonF polyline;
    polyline.reserve(static_cast<int>(sampled.size()) + 1);
    for (const Vec2& at : sampled) {
        const Vec2 p = view.toPixels(at);
        polyline << QPointF(p.x, p.y);
    }
    if (spline->closed && !polyline.isEmpty()) polyline << polyline.front();
    painter.drawPolyline(polyline);
    return true;
}

bool DrawEllipseGeometry(QPainter& painter, const CanvasView& view,
                         const SketchGeometry& geometry) {
    constexpr double kTwoPiLocal = 6.283185307179586476925286766559;
    if (const auto* full = std::get_if<SketchEllipse>(&geometry)) {
        painter.drawPolyline(EllipsePolyline(view, full->center, full->majorRadiusMm,
                                             full->minorRadiusMm, full->rotationRad, 0.0,
                                             kTwoPiLocal));
        return true;
    }
    if (const auto* piece = std::get_if<SketchEllipticalArc>(&geometry)) {
        painter.drawPolyline(EllipsePolyline(view, piece->center, piece->majorRadiusMm,
                                             piece->minorRadiusMm, piece->rotationRad,
                                             piece->startParamRad, EllipseArcSweep(*piece)));
        return true;
    }
    return false;
}

} // namespace

QString SketchCanvasWidget::applyTransform(const SketchTransform& transform) {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    std::vector<SketchEntityId> entities;
    for (const SketchElementRef& ref : model_.selection()) {
        if (std::find(entities.begin(), entities.end(), ref.entityId) != entities.end()) continue;
        if (target->findEntity(ref.entityId) == nullptr) continue;
        entities.push_back(ref.entityId);
    }
    if (entities.empty()) return QStringLiteral("Select what to transform first.");

    const TransformOutcome outcome = ApplyTransform(*document_, sketchId_, entities, transform);
    if (!outcome.applied) return QString::fromStdString(outcome.status);

    refreshAfterDocumentChange();
    // THE COPY becomes the selection, so a second transform continues from what
    // was just made rather than making a copy of the copy's source.
    if (!outcome.created.empty()) {
        std::vector<SketchElementRef> selection;
        for (const SketchEntityId id : outcome.created)
            selection.push_back(SketchElementRef{id, SketchSubElement::Whole});
        model_.setSelection(std::move(selection));
    }
    update();
    return QString::fromStdString(outcome.status);
}

QString SketchCanvasWidget::applySplit() {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    std::vector<SketchEntityId> entities;
    for (const SketchElementRef& ref : model_.selection()) {
        if (std::find(entities.begin(), entities.end(), ref.entityId) != entities.end()) continue;
        if (target->findEntity(ref.entityId) == nullptr) continue;
        entities.push_back(ref.entityId);
    }
    if (entities.size() < 2)
        return QStringLiteral("Select what to split FIRST, then what crosses it.");

    // THE FIRST selected is the one that gets cut. Said in the prompt and in
    // the tooltip, because the alternative -- guessing from which entity has
    // the most crossings, say -- is a rule the user would have to learn by
    // being surprised.
    const SketchEntityId victim = entities.front();
    const std::vector<SketchEntityId> cutters(entities.begin() + 1, entities.end());

    const SplitOutcome outcome = ApplySplit(*document_, sketchId_, victim, cutters);
    if (!outcome.applied) return QString::fromStdString(outcome.status);

    refreshAfterDocumentChange();
    std::vector<SketchElementRef> selection;
    for (const SketchEntityId id : outcome.created)
        selection.push_back(SketchElementRef{id, SketchSubElement::Whole});
    model_.setSelection(std::move(selection));
    update();
    return QString::fromStdString(outcome.status);
}

QString SketchCanvasWidget::applyMirror() {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    std::vector<SketchEntityId> entities;
    for (const SketchElementRef& ref : model_.selection()) {
        if (std::find(entities.begin(), entities.end(), ref.entityId) != entities.end()) continue;
        if (target->findEntity(ref.entityId) == nullptr) continue;
        entities.push_back(ref.entityId);
    }
    if (entities.size() < 2)
        return QStringLiteral("Select what to mirror, then the line to mirror it across.");

    // THE LAST LINE selected is the mirror. Checked to actually BE a line, so a
    // user who selected in a different order is told what is wrong rather than
    // having their last circle used as an axis.
    const SketchEntityId mirror = entities.back();
    const SketchEntity* axis = target->findEntity(mirror);
    if (axis == nullptr || !std::holds_alternative<SketchLine>(axis->geometry))
        return QStringLiteral("The LAST thing selected has to be the line to mirror across.");
    entities.pop_back();

    const MirrorOutcome outcome = ApplyMirror(*document_, sketchId_, entities, mirror);
    if (!outcome.applied) return QString::fromStdString(outcome.status);

    refreshAfterDocumentChange();
    std::vector<SketchElementRef> selection;
    for (const SketchEntityId id : outcome.created)
        selection.push_back(SketchElementRef{id, SketchSubElement::Whole});
    model_.setSelection(std::move(selection));
    update();
    const QString status = QString::fromStdString(outcome.status);
    emit documentChanged(status);
    return status;
}

QString SketchCanvasWidget::applyFillet(double radiusMm) {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    std::vector<SketchEntityId> lines;
    for (const SketchElementRef& ref : model_.selection()) {
        if (std::find(lines.begin(), lines.end(), ref.entityId) != lines.end()) continue;
        const SketchEntity* entity = target->findEntity(ref.entityId);
        if (entity == nullptr || !std::holds_alternative<SketchLine>(entity->geometry)) continue;
        lines.push_back(ref.entityId);
    }
    if (lines.size() != 2)
        return QStringLiteral("Fillet needs exactly 2 lines selected; %1 found.")
            .arg(lines.size());

    const ChamferOutcome outcome =
        ApplyFillet(*document_, sketchId_, lines[0], lines[1], radiusMm);
    if (!outcome.applied) return QString::fromStdString(outcome.status);

    refreshAfterDocumentChange();
    model_.setSelection({SketchElementRef{outcome.created, SketchSubElement::Whole}});
    update();
    const QString status = QString::fromStdString(outcome.status);
    emit documentChanged(status);
    return status;
}

QString SketchCanvasWidget::applyChamfer(double distanceA, double distanceB) {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    std::vector<SketchEntityId> lines;
    for (const SketchElementRef& ref : model_.selection()) {
        if (std::find(lines.begin(), lines.end(), ref.entityId) != lines.end()) continue;
        const SketchEntity* entity = target->findEntity(ref.entityId);
        if (entity == nullptr || !std::holds_alternative<SketchLine>(entity->geometry)) continue;
        lines.push_back(ref.entityId);
    }
    if (lines.size() != 2)
        return QStringLiteral("Chamfer needs exactly 2 lines selected; %1 found.")
            .arg(lines.size());

    const ChamferOutcome outcome =
        ApplyChamfer(*document_, sketchId_, lines[0], lines[1], distanceA, distanceB);
    if (!outcome.applied) return QString::fromStdString(outcome.status);

    refreshAfterDocumentChange();
    // The chamfer itself is what the user will dimension next.
    model_.setSelection({SketchElementRef{outcome.created, SketchSubElement::Whole}});
    update();
    const QString status = QString::fromStdString(outcome.status);
    emit documentChanged(status);
    return status;
}

QString SketchCanvasWidget::applyOffset(double distanceMm) {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    // EXACTLY ONE. Offsetting several at once is a different command -- the
    // copies would need to be joined to each other to be useful, and guessing
    // that is not something to do behind the user's back.
    std::vector<SketchEntityId> entities;
    for (const SketchElementRef& ref : model_.selection()) {
        if (std::find(entities.begin(), entities.end(), ref.entityId) != entities.end()) continue;
        if (target->findEntity(ref.entityId) == nullptr) continue;
        entities.push_back(ref.entityId);
    }
    if (entities.empty()) return QStringLiteral("Select one line, circle or arc to offset.");
    if (entities.size() > 1)
        return QStringLiteral("Offset works on one entity at a time; %1 are selected.")
            .arg(entities.size());

    std::string whyNot;
    const SketchEdit edit = MakeOffsetEdit(*target, entities.front(), std::abs(distanceMm),
                                           distanceMm >= 0.0 ? 1.0 : -1.0, &whyNot);
    if (!edit.valid()) return QString::fromStdString(whyNot);

    const QString status = applyEdit(edit);
    // The COPY is what the user wants to work on next -- dimension it, offset
    // it again, make it construction. Leaving the source selected would make
    // the second offset silently repeat the first.
    if (!lastCreatedEntities_.empty())
        model_.setSelection(
            {SketchElementRef{lastCreatedEntities_.front(), SketchSubElement::Whole}});
    update();
    emit presentationChanged();
    return status;
}

QString SketchCanvasWidget::toggleDimensionDriven() {
    lastCommandApplied_ = false;
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");
    if (highlighted_ == kInvalidSketchConstraintId)
        return QStringLiteral("Select a dimension -- click its value on the canvas or its row "
                              "in the panel.");

    const SketchConstraint* constraint = target->findConstraint(highlighted_);
    if (constraint == nullptr) return QStringLiteral("That dimension is gone.");
    if (!IsDimensional(constraint->data))
        return QStringLiteral("Only a dimension can be a reference; %1 is a relationship.")
            .arg(QString::fromLatin1(ConstraintKindName(constraint->data)));

    const bool wantDriven = !constraint->driven;
    if (!document_->setSketchConstraintDriven(sketchId_, highlighted_, wantDriven))
        return QStringLiteral("The document refused that.");

    lastCommandApplied_ = true;
    const QString message =
        wantDriven
            ? QStringLiteral("Now a reference: it measures the geometry instead of driving it, "
                             "and is drawn in brackets.")
            : QStringLiteral("Now driving the geometry again.");
    refreshAfterDocumentChange();
    emit documentChanged(message);
    return message;
}

QString SketchCanvasWidget::toggleConstruction() {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");
    if (model_.selection().empty())
        return QStringLiteral("Select the geometry to switch to or from construction first.");

    // ONE direction for the whole selection, decided by what is there now: if
    // ANY selected entity is still normal geometry, the command makes them all
    // construction. A per-entity flip would turn a mixed selection inside out
    // and leave the user worse off than before they pressed it.
    bool anyNormal = false;
    std::vector<SketchEntityId> entities;
    for (const SketchElementRef& ref : model_.selection()) {
        if (std::find(entities.begin(), entities.end(), ref.entityId) != entities.end()) continue;
        if (target->findEntity(ref.entityId) == nullptr) continue;
        entities.push_back(ref.entityId);
        if (!target->isConstruction(ref.entityId)) anyNormal = true;
    }
    if (entities.empty()) return QStringLiteral("Nothing in the selection can be construction.");

    const std::size_t changed =
        document_->setSketchEntitiesConstruction(sketchId_, entities, anyNormal);
    if (changed == 0)
        return QStringLiteral("Nothing changed: that geometry is already %1.")
            .arg(anyNormal ? QStringLiteral("construction") : QStringLiteral("normal"));

    refreshAfterDocumentChange();
    update();
    const QString status =
        QStringLiteral("%1 %2 switched to %3 geometry.")
            .arg(changed)
            .arg(changed == 1 ? QStringLiteral("entity") : QStringLiteral("entities"))
            .arg(anyNormal ? QStringLiteral("construction") : QStringLiteral("normal"));
    emit documentChanged(status);
    return status;
}

QString SketchCanvasWidget::addOriginPoint() {
    const Sketch* target = sketch();
    if (target == nullptr) return QStringLiteral("No sketch.");

    QString status;
    SketchEntityId origin = FindSketchOrigin(*target);
    if (origin == kInvalidSketchEntityId) {
        status = applyEdit(MakeOriginPointEdit());
        // Re-read: the sketch pointer may have been invalidated by the edit,
        // and the id only exists once the edit has been applied.
        target = sketch();
        if (target == nullptr) return status;
        origin = FindSketchOrigin(*target);
        if (origin == kInvalidSketchEntityId) return status;
    } else {
        status = QStringLiteral("This sketch already has an origin point; it is now selected.");
    }

    // Selected either way. The command exists to be the first half of "measure
    // from here to there", so leaving the user to hunt for a 4-pixel cross
    // afterwards would waste the click it just saved them.
    model_.setSelection({SketchElementRef{origin, SketchSubElement::Whole}});
    update();
    emit presentationChanged();
    return status;
}

bool SketchCanvasWidget::beginGeometryDrag(Vec2 sketchMm) {
    const Sketch* target = sketch();
    if (target == nullptr || document_ == nullptr) return false;

    // A POINT, not an entity. Dragging "a line" is really dragging one of its
    // ends, and the handles the canvas already draws are exactly the targets
    // (ADR-M17-003) -- so the same hit-test answers both questions.
    const SketchElementRef hit = PickElement(*target, sketchMm, toleranceMm());
    if (hit.entityId == kInvalidSketchEntityId) return false;
    if (!IsPointRef(*target, hit)) return false;

    dragged_ = hit;
    dragBefore_ = document_->sketchGeometrySnapshot(sketchId_);
    lastDragStatus_ = SketchSolveStatus::Solved;
    return true;
}

void SketchCanvasWidget::updateGeometryDrag(Vec2 sketchMm) {
    if (!isDraggingGeometry() || document_ == nullptr) return;
    lastDragStatus_ = document_->previewSketchDrag(sketchId_, dragged_, sketchMm);
    update();
    emit presentationChanged();
}

QString SketchCanvasWidget::finishGeometryDrag() {
    if (!isDraggingGeometry() || document_ == nullptr) return QString();
    const SketchElementRef dragged = dragged_;
    const std::vector<std::pair<SketchEntityId, SketchGeometry>> before = dragBefore_;
    dragged_ = SketchElementRef{};
    dragBefore_.clear();

    const std::size_t moved = document_->commitSketchDrag(sketchId_, before, "Drag geometry");
    if (moved == 0) {
        // Nothing moved, so there is nothing to record -- and the user still
        // deserves to know WHY their drag did nothing (roadmap 8).
        update();
        emit presentationChanged();
        return QStringLiteral("Nothing moved: the constraints hold %1 where it is (%2).")
            .arg(QString::fromStdString(DescribeElementRef(*sketch(), dragged)))
            .arg(QString::fromLatin1(SolveStatusName(lastDragStatus_)));
    }
    refreshAfterDocumentChange();
    update();
    const QString status = QStringLiteral("Dragged %1; %2 %3 moved.")
                               .arg(QString::fromStdString(DescribeElementRef(*sketch(), dragged)))
                               .arg(moved)
                               .arg(moved == 1 ? QStringLiteral("entity") : QStringLiteral("entities"));
    emit documentChanged(status);
    return status;
}

bool SketchCanvasWidget::cancelGeometryDrag() {
    if (!isDraggingGeometry() || document_ == nullptr) return false;
    document_->restoreSketchGeometry(sketchId_, dragBefore_);
    dragged_ = SketchElementRef{};
    dragBefore_.clear();
    update();
    emit presentationChanged();
    return true;
}

bool SketchCanvasWidget::pressEscape() {
    // A drag in flight is the most urgent thing to abandon: it is the only
    // state where the geometry on screen is not yet anything the document has
    // agreed to.
    if (cancelGeometryDrag()) return true;
    // Trim is a mode, so Esc leaves it -- the same one press that leaves a
    // drawing tool (ADR-M17-002).
    if (trimming_) {
        setTrimming(false);
        return true;
    }
    if (extending_) {
        setExtending(false);
        return true;
    }
    if (dimensioning_) {
        // Esc abandons a HALF-MADE dimension first, and only leaves the mode
        // when there is nothing half-made -- so one press does not throw away
        // both the picks and the tool, which is two undos' worth of work for
        // one keystroke.
        if (!model_.selection().empty()) {
            model_.clearSelection();
            update();
            emit presentationChanged();
            return true;
        }
        setDimensioning(false);
        return true;
    }
    if (useMode_) {
        setUseReference(false);
        return true;
    }
    if (isDraggingDimension()) {
        // Abandoned, not committed: the preview is rolled back and nothing
        // reaches the undo stack.
        const SketchConstraintId id = draggedDimension_;
        const bool hadPlacement = dragHadPlacement_;
        const Vec2 original = dragOriginalLabel_;
        draggedDimension_ = kInvalidSketchConstraintId;
        if (document_ != nullptr) {
            if (hadPlacement)
                document_->previewSketchDimensionPlacement(sketchId_, id, original);
            else
                document_->previewClearSketchDimensionPlacement(sketchId_, id);
        }
        update();
        emit presentationChanged();
        return true;
    }
    if (!model_.cancel()) return false;
    update();
    // The shell listens for this to put the toolbar's checked button back on
    // the arrow. Leaving a tool without saying so is how the canvas and the
    // toolbar came to disagree.
    emit presentationChanged();
    return true;
}

QString SketchCanvasWidget::deleteSelectionOrHighlightedConstraint() {
    // A picked constraint badge wins ONLY when no geometry is selected, and
    // clicking a badge clears that selection, so in practice the two never
    // compete. The check stays anyway: the highlight can also arrive from the
    // constraint panel, which does not touch the canvas selection
    // (ADR-M12-013), and silently deleting the wrong thing is worse than any
    // amount of belt and braces.
    if (model_.selection().empty() && highlighted_ != kInvalidSketchConstraintId)
        return deleteHighlightedConstraint();
    return deleteSelection();
}

QString SketchCanvasWidget::deleteConstraint(SketchConstraintId constraintId) {
    SketchEdit edit;
    edit.kind = SketchEditKind::DeleteConstraints;
    edit.constraints.push_back(constraintId);
    edit.label = "Delete constraint";
    return applyEdit(edit);
}

QString SketchCanvasWidget::commitDimensionText(SketchConstraintId constraintId,
                                                const QString& text) {
    const Sketch* target = sketch();
    if (target == nullptr || document_ == nullptr) return QStringLiteral("No sketch.");
    const SketchEditOutcome outcome =
        CommitDimensionValue(*document_, *target, constraintId, text.toStdString());
    if (outcome.applied) {
        refreshAfterDocumentChange();
        const QString status = QString::fromStdString(outcome.status);
        emit documentChanged(status);
        return status;
    }
    return QString::fromStdString(outcome.status);
}

bool SketchCanvasWidget::beginDimensionDrag(Vec2 sketchMm) {
    const Sketch* target = sketch();
    if (target == nullptr || document_ == nullptr) return false;
    const SketchConstraintId hit = dimensionAt(sketchMm);
    if (hit == kInvalidSketchConstraintId) return false;

    draggedDimension_ = hit;
    const Vec2* existing = target->dimensionPlacement(hit);
    dragHadPlacement_ = existing != nullptr;
    // Where the label IS right now -- placed or computed. Starting from the
    // computed position is what stops a dimension jumping to the cursor the
    // instant it is grabbed.
    Vec2 current{};
    if (existing != nullptr) {
        current = *existing;
    } else {
        bool ok = false;
        current = AutomaticDimensionLabel(*document_, *target, hit, &ok);
        if (!ok) {
            draggedDimension_ = kInvalidSketchConstraintId;
            return false;
        }
    }
    dragOriginalLabel_ = current;
    // Grab OFFSET, so the label keeps its position relative to the cursor
    // rather than snapping its centre under it.
    dragGrabOffset_ = Vec2{current.x - sketchMm.x, current.y - sketchMm.y};
    return true;
}

bool SketchCanvasWidget::updateDimensionDrag(Vec2 sketchMm) {
    if (draggedDimension_ == kInvalidSketchConstraintId || document_ == nullptr) return false;
    const Vec2 target{sketchMm.x + dragGrabOffset_.x, sketchMm.y + dragGrabOffset_.y};
    // NOT the recording facade, and NOT editSketch either. One delta per mouse
    // move would make a single drag a thousand-step undo record; editSketch
    // would mark the sketch dirty on every move and stale everything
    // downstream of it because a label had shifted.
    document_->previewSketchDimensionPlacement(sketchId_, draggedDimension_, target);
    update();
    return true;
}

QString SketchCanvasWidget::finishDimensionDrag() {
    if (draggedDimension_ == kInvalidSketchConstraintId) return QString();
    const SketchConstraintId id = draggedDimension_;
    draggedDimension_ = kInvalidSketchConstraintId;

    const Sketch* target = sketch();
    if (target == nullptr || document_ == nullptr) return QString();
    const Vec2* placed = target->dimensionPlacement(id);
    if (placed == nullptr) return QString();
    const Vec2 finalLabel = *placed;

    // Put it back the way it was, WITHOUT recording, then make the one move
    // that the undo stack should see. The preview has already moved it; this
    // is what turns a whole drag into a single reversible step.
    if (dragHadPlacement_)
        document_->previewSketchDimensionPlacement(sketchId_, id, dragOriginalLabel_);
    else
        document_->previewClearSketchDimensionPlacement(sketchId_, id);
    if (!document_->setSketchDimensionPlacement(sketchId_, id, finalLabel)) return QString();

    update();
    const QString status = QStringLiteral("Dimension moved. Sketch > Auto-place Dimensions "
                                          "puts it back.");
    emit documentChanged(status);
    return status;
}

QString SketchCanvasWidget::autoPlaceAllDimensions() {
    const Sketch* target = sketch();
    if (target == nullptr || document_ == nullptr) return QStringLiteral("No sketch.");
    std::vector<SketchConstraintId> placed;
    for (const Sketch::DimensionPlacement& placement : target->dimensionPlacements())
        placed.push_back(placement.constraintId);
    if (placed.empty()) return QStringLiteral("Every dimension is already placed automatically.");

    document_->beginTransaction("Auto-place dimensions");
    for (const SketchConstraintId id : placed)
        (void)document_->clearSketchDimensionPlacement(sketchId_, id);
    if (!document_->commitTransaction()) return QStringLiteral("The document refused the change.");

    update();
    const QString status = QStringLiteral("%1 dimension%2 put back on automatic placement.")
                               .arg(placed.size())
                               .arg(placed.size() == 1 ? "" : "s");
    emit documentChanged(status);
    return status;
}

namespace {

// True when this dimension is measured in radians and shown in degrees.
bool IsAngularDimension(const Sketch& sketch, SketchConstraintId id) {
    const SketchConstraint* constraint = sketch.findConstraint(id);
    return constraint != nullptr && std::holds_alternative<AngleConstraint>(constraint->data);
}

} // namespace

QString SketchCanvasWidget::commitDimensionFormat(SketchConstraintId constraintId,
                                                  const QString& prefix, const QString& suffix,
                                                  double plusDisplay, double minusDisplay) {
    const Sketch* target = sketch();
    if (target == nullptr || document_ == nullptr) return QStringLiteral("No sketch.");
    const SketchConstraint* constraint = target->findConstraint(constraintId);
    if (constraint == nullptr || !IsDimensional(constraint->data))
        return QStringLiteral("That is not a dimension.");
    if (!std::isfinite(plusDisplay) || !std::isfinite(minusDisplay))
        return QStringLiteral("A tolerance must be a finite number.");
    if (plusDisplay < 0.0 || minusDisplay < 0.0)
        return QStringLiteral("Tolerances are magnitudes: enter them as positive numbers.");

    // DEGREES IN, RADIANS STORED -- for the tolerance exactly as for the value.
    const double scale = IsAngularDimension(*target, constraintId) ? kPi / 180.0 : 1.0;
    Sketch::DimensionFormat format;
    format.constraintId = constraintId;
    format.prefix = prefix.toStdString();
    format.suffix = suffix.toStdString();
    format.plusTolerance = plusDisplay * scale;
    format.minusTolerance = minusDisplay * scale;

    if (!document_->setSketchDimensionFormat(sketchId_, constraintId, format))
        return QStringLiteral("The document refused that format.");

    update();
    const QString status = format.isDefault()
                               ? QStringLiteral("Dimension format cleared.")
                               : QStringLiteral("Dimension now reads %1.")
                                     .arg(dimensionDisplayText(constraintId));
    emit documentChanged(status);
    return status;
}

bool SketchCanvasWidget::dimensionFormatOf(SketchConstraintId constraintId, QString* prefix,
                                           QString* suffix, double* plusDisplay,
                                           double* minusDisplay) const {
    const Sketch* target = sketch();
    if (target == nullptr) return false;
    const SketchConstraint* constraint = target->findConstraint(constraintId);
    if (constraint == nullptr || !IsDimensional(constraint->data)) return false;

    const double scale = IsAngularDimension(*target, constraintId) ? 180.0 / kPi : 1.0;
    const Sketch::DimensionFormat* format = target->dimensionFormat(constraintId);
    if (prefix != nullptr)
        *prefix = format != nullptr ? QString::fromStdString(format->prefix) : QString();
    if (suffix != nullptr)
        *suffix = format != nullptr ? QString::fromStdString(format->suffix) : QString();
    if (plusDisplay != nullptr)
        *plusDisplay = format != nullptr ? format->plusTolerance * scale : 0.0;
    if (minusDisplay != nullptr)
        *minusDisplay = format != nullptr ? format->minusTolerance * scale : 0.0;
    return true;
}

QString SketchCanvasWidget::dimensionDisplayText(SketchConstraintId constraintId) const {
    const Sketch* target = sketch();
    if (target == nullptr || document_ == nullptr) return QString();
    // Asked of the SAME builder the canvas paints from, so the answer cannot
    // drift from what is on screen.
    for (const DimensionAnnotation& annotation :
         DimensionAnnotationsFor(*document_, *target, view_.pixelsPerMm))
        if (annotation.id == constraintId) return QString::fromStdString(annotation.text);
    return QString();
}

SketchConstraintId SketchCanvasWidget::dimensionAt(Vec2 sketchMm) const {
    const Sketch* target = sketch();
    if (target == nullptr || document_ == nullptr) return kInvalidSketchConstraintId;
    const double tolerance = view_.toSketchLength(14.0);
    SketchConstraintId best = kInvalidSketchConstraintId;
    double bestDistance = tolerance;
    for (const DimensionAnnotation& annotation :
         DimensionAnnotationsFor(*document_, *target, view_.pixelsPerMm)) {
        const double dx = annotation.labelMm.x - sketchMm.x;
        const double dy = annotation.labelMm.y - sketchMm.y;
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = annotation.id;
        }
    }
    return best;
}

// =============================================================================
// Events
// =============================================================================

void SketchCanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncViewSize();
    if (!fittedOnce_) fitView();
}

void SketchCanvasWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        lastPanPos_ = event->position();
        return;
    }
    if (event->button() == Qt::RightButton) {
        if (model_.cancel()) {
            update();
            emit presentationChanged();
        }
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    model_.setSuppressInference((event->modifiers() & Qt::ShiftModifier) != 0);
    const Vec2 sketchMm = view_.toSketch(Vec2{event->position().x(), event->position().y()});

    // A dimension under the cursor is GRABBED rather than clicked through --
    // but a constraint badge outranks it, so a badge sitting over a dimension
    // line is still reachable. Only under Select: with a drawing tool active
    // the click is placing geometry, and stealing it would turn every
    // annotation into an obstacle.
    //
    // The pick itself lives in clickAt(), not here, so the programmatic entry
    // and the mouse take the SAME path. They did not, once, and the badge was
    // clickable with a real mouse and invisible to everything else.
    if (model_.tool() == SketchTool::Select &&
        constraintBadgeAt(sketchMm) == kInvalidSketchConstraintId &&
        beginDimensionDrag(sketchMm))
        return;

    clickAt(sketchMm);

    // A GRAB, after the click has selected. Doing it in this order means a
    // press both selects what is under the cursor and picks it up, which is
    // what makes drag feel like direct manipulation rather than a second mode.
    // Only under Select, and only when nothing else claimed the press.
    if (model_.tool() == SketchTool::Select && !trimming_ && !extending_)
        (void)beginGeometryDrag(sketchMm);
}

void SketchCanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const Vec2 sketchMm = view_.toSketch(Vec2{event->position().x(), event->position().y()});

    // A SPLINE ENDS ON A DOUBLE-CLICK, because it is the one tool whose point
    // count the user decides. Handled before the dimension lookup: a
    // double-click while drawing is unambiguously "that is the last point",
    // and letting it fall through to "activate the dimension under the cursor"
    // would open an edit box in the middle of drawing.
    const SketchEdit finished = model_.finishPendingSpline();
    if (finished.valid()) {
        const SketchEditOutcome outcome = ApplySketchEdit(*document_, sketchId_, finished);
        model_.afterApply(outcome.createdEntities);
        refreshAfterDocumentChange();
        update();
        emit presentationChanged();
        return;
    }

    const SketchConstraintId dimension = dimensionAt(sketchMm);
    if (dimension == kInvalidSketchConstraintId) return;
    emit dimensionActivated(static_cast<qulonglong>(ToObjectId(dimension)));
}

void SketchCanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (isDraggingGeometry()) {
        updateGeometryDrag(view_.toSketch(Vec2{event->position().x(), event->position().y()}));
        return;
    }
    if (isDraggingDimension()) {
        updateDimensionDrag(
            view_.toSketch(Vec2{event->position().x(), event->position().y()}));
        return;
    }
    if (panning_) {
        const QPointF delta = event->position() - lastPanPos_;
        lastPanPos_ = event->position();
        view_.panByPixels(Vec2{delta.x(), delta.y()});
        update();
        return;
    }
    model_.setSuppressInference((event->modifiers() & Qt::ShiftModifier) != 0);
    hoverAt(view_.toSketch(Vec2{event->position().x(), event->position().y()}));
}

void SketchCanvasWidget::hoverAt(Vec2 sketchMm) {
    const Sketch* target = sketch();
    if (target == nullptr) return;
    hoverSnap_ = SnapCursor(*target, sketchMm, toleranceMm(), GridStepMm(view_),
                            model_.suppressInference());
    hasHover_ = true;
    // In Use mode the cursor also arms a reference, so the edge about to be
    // converted is visible before the click rather than after it.
    hoveredReference_ = useMode_ ? ReferenceAt(*target, sketchMm, toleranceMm())
                                 : kInvalidSketchReferenceId;
    update();
    emit presentationChanged();
}

void SketchCanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) panning_ = false;
    if (event->button() != Qt::LeftButton) return;
    if (isDraggingDimension()) {
        finishDimensionDrag();
        return;
    }
    // A grab that never moved commits nothing (commitSketchDrag records only
    // what changed), so a plain click costs no undo step -- which is what lets
    // the press both select and pick up.
    if (isDraggingGeometry()) {
        const QString status = finishGeometryDrag();
        if (!status.isEmpty()) emit presentationChanged();
    }
}

void SketchCanvasWidget::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    view_.zoomAt(Vec2{event->position().x(), event->position().y()}, std::pow(1.15, steps));
    update();
    event->accept();
}

void SketchCanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Shift) {
        model_.setSuppressInference(true);
        emit presentationChanged();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        pressEscape();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        const QString status = deleteSelectionOrHighlightedConstraint();
        if (!status.isEmpty()) emit documentChanged(status);
        return;
    }
    QWidget::keyPressEvent(event);
}

void SketchCanvasWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Shift) {
        model_.setSuppressInference(false);
        emit presentationChanged();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

// =============================================================================
// Painting
// =============================================================================

void SketchCanvasWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const Palette& palette = Colours();
    painter.fillRect(rect(), palette.background);

    paintedEntities_ = 0;
    paintedDimensions_ = 0;
    paintedDimensionArrows_ = 0;
    paintedDimensionArcs_ = 0;
    paintedConstraintGlyphs_ = 0;
    paintedOnce_ = true;
    // Cleared, not carried over: a repaint that draws nothing must not keep
    // reporting the colour of a sketch that is no longer there.
    paintedGeometryColour_ = QColor();
    paintedConstructionEntities_ = 0;
    paintedReferences_ = 0;
    paintedDimensionGhosts_ = 0;
    paintedSelectionHandles_ = 0;
    paintedHighlightedGlyphs_ = 0;
    paintedHighlightedEntities_ = 0;

    const Sketch* target = sketch();

    // --- Grid and axes ------------------------------------------------------
    const double step = GridStepMm(view_);
    if (step > 0.0) {
        const Vec2 topLeft = view_.toSketch(Vec2{0.0, 0.0});
        const Vec2 bottomRight =
            view_.toSketch(Vec2{static_cast<double>(width()), static_cast<double>(height())});
        const double firstX = std::floor(topLeft.x / step) * step;
        const double lastX = bottomRight.x;
        const double firstY = std::floor(bottomRight.y / step) * step;
        const double lastY = topLeft.y;

        // Bounded so a pathological zoom cannot ask for a million lines.
        const int maxLines = 400;
        int drawn = 0;
        for (double x = firstX; x <= lastX && drawn < maxLines; x += step, ++drawn) {
            const bool major = std::abs(std::fmod(x, step * 5.0)) < step * 0.25;
            painter.setPen(QPen(major ? palette.gridMajor : palette.grid, 1.0));
            const double px = view_.toPixels(Vec2{x, 0.0}).x;
            painter.drawLine(QPointF(px, 0.0), QPointF(px, height()));
        }
        drawn = 0;
        for (double y = firstY; y <= lastY && drawn < maxLines; y += step, ++drawn) {
            const bool major = std::abs(std::fmod(y, step * 5.0)) < step * 0.25;
            painter.setPen(QPen(major ? palette.gridMajor : palette.grid, 1.0));
            const double py = view_.toPixels(Vec2{0.0, y}).y;
            painter.drawLine(QPointF(0.0, py), QPointF(width(), py));
        }
    }

    painter.setPen(QPen(palette.axis, 1.4));
    const Vec2 origin = view_.toPixels(Vec2{0.0, 0.0});
    painter.drawLine(QPointF(0.0, origin.y), QPointF(width(), origin.y));
    painter.drawLine(QPointF(origin.x, 0.0), QPointF(origin.x, height()));
    // The origin marker: a square, so it is distinguishable from every snap
    // marker below without relying on its colour.
    painter.setPen(QPen(palette.axis, 1.6));
    painter.drawRect(QRectF(origin.x - 4.0, origin.y - 4.0, 8.0, 8.0));

    if (target == nullptr) {
        painter.setPen(palette.dimensionText);
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("No sketch open.\nSketch > New Sketch to start drawing."));
        return;
    }

    // THE SKETCH'S colour, which is what TROUBLE looks like: a conflict or an
    // invalid constraint is a property of the whole system of equations, and
    // there is no honest way to say which entity is at fault.
    const QColor sketchColour = GeometryColour(target->solveStatus());
    // ...but "pinned" is PER ENTITY (M17.29).
    //
    // This used to be one colour for everything, and it was wrong in both
    // directions once splines existed: a fully dimensioned rectangle drew as
    // loose because something else in its sketch was not, and a sketch with a
    // spline in it could never go black at all -- no constraint can name an
    // interior point, so it always keeps freedom.
    const bool sketchIsInTrouble = target->solveStatus() != SketchSolveStatus::Solved &&
                                   target->solveStatus() != SketchSolveStatus::UnderConstrained;
    const auto colourFor = [&](SketchEntityId id) {
        if (sketchIsInTrouble) return sketchColour;
        return target->isEntityFullyConstrained(id) ? palette.fullyConstrained
                                                    : palette.underConstrained;
    };

    // The entities the highlighted constraint names, if any. Resolved from the
    // constraint itself rather than remembered when the row was clicked: a
    // constraint whose geometry has since been deleted must stop highlighting
    // it, and a remembered list would go on pointing at ids that are gone.
    std::vector<SketchEntityId> highlightedEntities;
    if (highlighted_ != kInvalidSketchConstraintId) {
        if (const SketchConstraint* constraint = target->findConstraint(highlighted_))
            highlightedEntities = ReferencedEntities(constraint->data);
    }
    const auto isHighlighted = [&](SketchEntityId id) {
        return std::find(highlightedEntities.begin(), highlightedEntities.end(), id) !=
               highlightedEntities.end();
    };

    // --- Projected reference geometry (M17.6, ADR-M17-029) ------------------
    //
    // Drawn FIRST, so everything the user has actually drawn sits on top of it.
    // An underlay that covers the model is not an underlay.
    //
    // Three channels separate it from real geometry, not one (A06): a distinct
    // colour, a DOT-dashed stroke, and a thinner line. Construction geometry is
    // already dashed, so dot-dash is what keeps the two apart in a monochrome
    // screenshot -- and telling them apart matters, because a construction line
    // is part of the sketch and a reference is not.
    //
    // Vertices are drawn as small hollow squares rather than the crosses a real
    // Point entity gets, for the same reason: a user must be able to see at a
    // glance which points they can already constrain and which they must
    // convert first.
    for (const SketchReference& reference : target->references()) {
        const bool armed = useMode_ && reference.id == hoveredReference_;
        QPen pen(armed ? palette.selection : palette.reference, armed ? 2.2 : 1.2);
        pen.setStyle(Qt::DashDotLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        if (const auto* point = std::get_if<SketchPoint>(&reference.geometry)) {
            const Vec2 p = view_.toPixels(point->position);
            // Solid outline for the marker: a dot-dashed 5-pixel square is a
            // smudge, and the stroke style has already done its work on the
            // curves around it.
            QPen markerPen(pen.color(), armed ? 2.2 : 1.4);
            painter.setPen(markerPen);
            painter.drawRect(QRectF(p.x - 2.5, p.y - 2.5, 5.0, 5.0));
            ++paintedReferences_;
        } else if (const auto* line = std::get_if<SketchLine>(&reference.geometry)) {
            painter.drawLine(ToQt(view_.toPixels(line->start)), ToQt(view_.toPixels(line->end)));
            ++paintedReferences_;
        } else if (const auto* circle = std::get_if<SketchCircle>(&reference.geometry)) {
            const Vec2 c = view_.toPixels(circle->center);
            const double r = view_.toPixelLength(circle->radiusMm);
            painter.drawEllipse(QPointF(c.x, c.y), r, r);
            ++paintedReferences_;
        } else if (const auto* arc = std::get_if<SketchArc>(&reference.geometry)) {
            const Vec2 c = view_.toPixels(arc->center);
            const double r = view_.toPixelLength(arc->radiusMm);
            double sweep = arc->endAngleRad - arc->startAngleRad;
            while (sweep <= 0.0) sweep += 2.0 * kPi;
            if (!arc->counterClockwise) sweep -= 2.0 * kPi;
            painter.drawArc(QRectF(c.x - r, c.y - r, 2.0 * r, 2.0 * r),
                            static_cast<int>(arc->startAngleRad * 180.0 / kPi * 16.0),
                            static_cast<int>(sweep * 180.0 / kPi * 16.0));
            ++paintedReferences_;
        } else if (DrawEllipseGeometry(painter, view_, reference.geometry) ||
                   DrawSplineGeometry(painter, view_, reference.geometry)) {
            ++paintedReferences_;
        }
    }

    // --- Geometry -----------------------------------------------------------
    for (const SketchEntity& entity : target->entities()) {
        const bool selected =
            model_.isSelected(SketchElementRef{entity.id, SketchSubElement::Whole}) ||
            model_.isSelected(SketchElementRef{entity.id, SketchSubElement::StartPoint}) ||
            model_.isSelected(SketchElementRef{entity.id, SketchSubElement::EndPoint}) ||
            model_.isSelected(SketchElementRef{entity.id, SketchSubElement::CenterPoint});
        // Selection is a WIDTH as well as a colour (A06): a monochrome
        // screenshot still shows which entity is selected.
        //
        // A highlighted constraint's geometry is widened too, and deliberately
        // NOT recoloured: colour already carries the solve status here, and
        // overwriting it would trade one channel for another rather than adding
        // one. Selection still wins -- it is the user's own current pick.
        const bool related = !selected && isHighlighted(entity.id);
        double width = 1.8;
        if (selected) width = 3.0;
        else if (related) width = 3.4;
        QPen pen(selected ? palette.selection : colourFor(entity.id), width);
        // Construction geometry is DASHED and thinner -- the drawing convention
        // for a centreline, and a second channel next to the colour (A06). It
        // is the only cue the user has that a line will not become an edge.
        if (entity.construction) {
            pen.setStyle(Qt::DashLine);
            pen.setWidthF(std::max(1.0, width - 0.6));
            ++paintedConstructionEntities_;
        }
        painter.setPen(pen);
        // Recorded from the pen that is about to stroke, not from the status
        // that chose it -- a readback taken any earlier proves only that the
        // colour was COMPUTED.
        if (!selected) paintedGeometryColour_ = pen.color();
        if (related) ++paintedHighlightedEntities_;
        painter.setBrush(Qt::NoBrush);

        if (const auto* point = std::get_if<SketchPoint>(&entity.geometry)) {
            const Vec2 p = view_.toPixels(point->position);
            painter.drawLine(QPointF(p.x - 4.0, p.y), QPointF(p.x + 4.0, p.y));
            painter.drawLine(QPointF(p.x, p.y - 4.0), QPointF(p.x, p.y + 4.0));
            ++paintedEntities_;
        } else if (const auto* line = std::get_if<SketchLine>(&entity.geometry)) {
            painter.drawLine(ToQt(view_.toPixels(line->start)), ToQt(view_.toPixels(line->end)));
            ++paintedEntities_;
        } else if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry)) {
            const Vec2 c = view_.toPixels(circle->center);
            const double r = view_.toPixelLength(circle->radiusMm);
            painter.drawEllipse(QPointF(c.x, c.y), r, r);
            ++paintedEntities_;
        } else if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) {
            const Vec2 c = view_.toPixels(arc->center);
            const double r = view_.toPixelLength(arc->radiusMm);
            // Qt angles are in 1/16 degree and measured counter-clockwise from
            // +x with y UP -- the same convention the sketch uses, so the only
            // conversion is the unit.
            double sweep = arc->endAngleRad - arc->startAngleRad;
            while (sweep <= 0.0) sweep += 2.0 * kPi;
            if (!arc->counterClockwise) sweep -= 2.0 * kPi;
            const int startSixteenths = static_cast<int>(arc->startAngleRad * 180.0 / kPi * 16.0);
            const int sweepSixteenths = static_cast<int>(sweep * 180.0 / kPi * 16.0);
            painter.drawArc(QRectF(c.x - r, c.y - r, 2.0 * r, 2.0 * r), startSixteenths,
                            sweepSixteenths);
            ++paintedEntities_;
        } else if (DrawEllipseGeometry(painter, view_, entity.geometry) ||
                   DrawSplineGeometry(painter, view_, entity.geometry)) {
            ++paintedEntities_;
            // THE POINTS IT WAS DRAWN THROUGH, as small marks. A spline with no
            // visible points is one a user cannot tell from an arc, and cannot
            // aim at to drag.
            if (const auto* spline = std::get_if<SketchSpline>(&entity.geometry)) {
                const QPen kept = painter.pen();
                painter.setPen(QPen(kept.color(), 1.0));
                for (const Vec2& at : spline->points) {
                    const Vec2 p = view_.toPixels(at);
                    painter.drawRect(QRectF(p.x - 1.5, p.y - 1.5, 3.0, 3.0));
                }
                painter.setPen(kept);
            }
        }
    }

    // --- The dimension about to be placed (M17.18, ADR-M17-041) -------------
    //
    // A GHOST, drawn at the cursor while the selection already describes a
    // dimension. Without it the user is asked to click a position for
    // something they cannot see, and "click where the dimension line goes"
    // becomes a guess they only get to check afterwards.
    //
    // Deliberately plain: two witness lines and the value. It is not trying to
    // be the finished dimension -- it is showing WHERE the finished one will
    // sit, and a preview indistinguishable from the real thing would leave the
    // user unsure whether the click had already happened.
    if (dimensioning_ && hasHover_ && target != nullptr) {
        std::string whyNot;
        const SketchEdit ready = model_.requestDimension(*target, SketchEditKind::None, &whyNot);
        if (ready.valid() && !ready.refs.empty()) {
            std::vector<Vec2> anchors;
            for (const SketchElementRef& ref : ready.refs) {
                bool ok = false;
                const Vec2 at = ResolveElementPoint(*target, ref, &ok);
                if (ok) anchors.push_back(at);
            }
            if (!anchors.empty()) {
                QPen ghost(palette.preview, 1.4);
                ghost.setStyle(Qt::DashLine);
                painter.setPen(ghost);
                painter.setBrush(Qt::NoBrush);
                const Vec2 at = view_.toPixels(hoverSnap_.point);
                for (Vec2 anchor : anchors) {
                    const Vec2 from = view_.toPixels(anchor);
                    painter.drawLine(QPointF(from.x, from.y), QPointF(at.x, at.y));
                }
                // The VALUE, because that is what the user is deciding where to
                // put -- a naked line says where but not what.
                painter.setPen(QPen(palette.dimensionText, 1.0));
                painter.drawText(QPointF(at.x + 8.0, at.y - 6.0),
                                 QString::fromStdString(SketchEditKindName(ready.kind)));
                ++paintedDimensionGhosts_;
            }
        }
    }

    // --- Constraint glyphs (roadmap 6.3: constraints must be VISIBLE) -------
    QFont glyphFont = font();
    glyphFont.setPointSizeF(std::max(7.0, glyphFont.pointSizeF() - 1.0));
    glyphFont.setBold(true);
    painter.setFont(glyphFont);
    for (const ConstraintBadge& badge : ConstraintBadgesFor(*target)) {
        const QRectF box = BadgeBox(view_, badge);
        const bool highlighted = badge.id == highlighted_;
        // The ring is an extra SHAPE around the badge, not a colour swap (A06):
        // it survives a monochrome screenshot, and it does not compete with the
        // red an at-fault badge already uses -- a highlighted at-fault
        // constraint has to stay legible as both.
        if (highlighted) {
            painter.setPen(QPen(palette.selection, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box.adjusted(-3.0, -3.0, 3.0, 3.0), 4.0, 4.0);
            ++paintedHighlightedGlyphs_;
        }
        painter.setPen(QPen(badge.offending ? palette.trouble : palette.glyph,
                            highlighted ? 2.0 : 1.0));
        painter.setBrush(QColor(255, 255, 255, 220));
        painter.drawRect(box);
        painter.setBrush(Qt::NoBrush);
        painter.drawText(box, Qt::AlignCenter, QString::fromStdString(badge.glyph));
        ++paintedConstraintGlyphs_;
    }

    // --- Selection handles --------------------------------------------------
    //
    // The small dots on a selected entity's ends and centres. They exist so the
    // click-the-line-then-click-its-end flow is DISCOVERABLE: the endpoints were
    // always pickable, but a user had no way to know it and no target to aim at.
    //
    // A handle already IN the selection is filled and larger, so "the line" and
    // "this end of the line" are distinguishable at a glance -- the difference
    // decides whether Dimension will accept the selection.
    {
        std::vector<SketchEntityId> handled;
        for (const SketchElementRef& selected : model_.selection()) {
            if (std::find(handled.begin(), handled.end(), selected.entityId) != handled.end())
                continue;
            handled.push_back(selected.entityId);
            for (const SketchElementRef& handle : EntityHandles(*target, selected.entityId)) {
                bool ok = false;
                const Vec2 at = ResolveElementPoint(*target, handle, &ok);
                if (!ok) continue;
                const Vec2 p = view_.toPixels(at);
                const bool picked = model_.isSelected(handle);
                painter.setPen(QPen(palette.selection, picked ? 2.0 : 1.4));
                painter.setBrush(picked ? QBrush(palette.selection)
                                        : QBrush(QColor(255, 255, 255, 230)));
                const double r = picked ? 4.2 : 3.2;
                painter.drawEllipse(QPointF(p.x, p.y), r, r);
                ++paintedSelectionHandles_;
            }
        }
        painter.setBrush(Qt::NoBrush);
    }

    // --- Dimensions ---------------------------------------------------------
    //
    // Drawn the way a drawing draws them: extension lines standing off the
    // geometry, a dimension line between them, filled arrowheads at its ends,
    // and the value sitting ON the line with the background knocked out behind
    // it. EVERY position comes from DimensionAnnotationsFor -- this block
    // decides nothing except how many pixels an arrowhead is (ADR-M12-001).
    QFont dimensionFont = font();
    painter.setFont(dimensionFont);
    const QFontMetrics metrics(dimensionFont);
    if (document_ != nullptr) {
        for (const DimensionAnnotation& annotation :
             DimensionAnnotationsFor(*document_, *target, view_.pixelsPerMm)) {
            const QColor colour = annotation.offending ? palette.trouble : palette.dimension;

            // Extension lines are THINNER than the dimension line, so the eye
            // follows the measurement rather than the witness marks.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(colour, 0.9));
            for (const DimensionSegment& segment : annotation.extensionLines)
                painter.drawLine(ToQt(view_.toPixels(segment.fromMm)),
                                 ToQt(view_.toPixels(segment.toMm)));

            painter.setPen(QPen(colour, 1.3));
            for (const DimensionSegment& segment : annotation.dimensionLines)
                painter.drawLine(ToQt(view_.toPixels(segment.fromMm)),
                                 ToQt(view_.toPixels(segment.toMm)));

            if (annotation.hasArc) {
                const Vec2 centre = view_.toPixels(annotation.arc.centreMm);
                const double radius = view_.toPixelLength(annotation.arc.radiusMm);
                const double sweep = annotation.arc.endRad - annotation.arc.startRad;
                painter.drawArc(
                    QRectF(centre.x - radius, centre.y - radius, 2.0 * radius, 2.0 * radius),
                    static_cast<int>(annotation.arc.startRad * 180.0 / kPi * 16.0),
                    static_cast<int>(sweep * 180.0 / kPi * 16.0));
                ++paintedDimensionArcs_;
            }

            // Arrowheads. The direction is a SKETCH direction, and sketch +v is
            // up while pixel +y is down, so the y component flips on the way in
            // -- the same single flip CanvasView owns for positions.
            painter.setPen(Qt::NoPen);
            painter.setBrush(colour);
            for (const DimensionArrow& arrow : annotation.arrows) {
                const Vec2 tip = view_.toPixels(arrow.tipMm);
                painter.drawPath(ArrowHead(QPointF(tip.x, tip.y),
                                           QPointF(arrow.directionMm.x, -arrow.directionMm.y)));
                ++paintedDimensionArrows_;
            }

            // The value, rotated with the dimension and knocked out of whatever
            // it lies over. Without the knock-out the number sits on top of its
            // own dimension line and neither can be read.
            const QString text = QString::fromStdString(annotation.text);
            const Vec2 label = view_.toPixels(annotation.labelMm);
            const double textWidth = metrics.horizontalAdvance(text);
            const double textHeight = metrics.height();
            const QRectF box(-textWidth * 0.5 - 3.0, -textHeight * 0.5 - 1.0, textWidth + 6.0,
                             textHeight + 2.0);

            painter.save();
            painter.translate(label.x, label.y);
            // Negated: a counter-clockwise sketch angle is a clockwise screen
            // rotation once y points down.
            painter.rotate(-annotation.textAngleRad * 180.0 / kPi);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(palette.background.red(), palette.background.green(),
                                    palette.background.blue(), 235));
            painter.drawRect(box);
            painter.setPen(annotation.offending ? palette.trouble : palette.dimensionText);
            painter.setBrush(Qt::NoBrush);
            painter.drawText(box, Qt::AlignCenter, text);
            painter.restore();

            ++paintedDimensions_;
        }
    }

    // --- The shape being drawn ---------------------------------------------
    const std::vector<Vec2>& pending = model_.pendingPoints();
    if (!pending.empty()) {
        painter.setPen(QPen(palette.preview, 1.4, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        Vec2 cursor = hasHover_ ? hoverSnap_.point : pending.back();
        switch (model_.tool()) {
        case SketchTool::Line:
            painter.drawLine(ToQt(view_.toPixels(pending.back())), ToQt(view_.toPixels(cursor)));
            break;
        case SketchTool::Rectangle: {
            const Vec2 a = view_.toPixels(pending.front());
            const Vec2 b = view_.toPixels(cursor);
            painter.drawRect(QRectF(QPointF(a.x, a.y), QPointF(b.x, b.y)).normalized());
            break;
        }
        case SketchTool::Circle:
        case SketchTool::Arc: {
            const Vec2 c = view_.toPixels(pending.front());
            const double dx = cursor.x - pending.front().x;
            const double dy = cursor.y - pending.front().y;
            const double r = view_.toPixelLength(std::sqrt(dx * dx + dy * dy));
            painter.drawEllipse(QPointF(c.x, c.y), r, r);
            break;
        }
        default:
            break;
        }
        for (const Vec2& point : pending) {
            const Vec2 p = view_.toPixels(point);
            painter.drawEllipse(QPointF(p.x, p.y), 3.0, 3.0);
        }
    }

    // --- The snap marker ----------------------------------------------------
    //
    // SHAPE carries the meaning, colour only reinforces it (A06, and the same
    // idea roadmap 24.4 records for drawing snap points): square = endpoint,
    // circle = centre, diamond = origin, small dot = grid, none = free.
    if (hasHover_) {
        const Vec2 p = view_.toPixels(hoverSnap_.point);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(palette.hover, 2.0));
        switch (hoverSnap_.kind) {
        case SnapKind::Endpoint:
            painter.drawRect(QRectF(p.x - 5.0, p.y - 5.0, 10.0, 10.0));
            break;
        case SnapKind::CenterPoint:
            painter.drawEllipse(QPointF(p.x, p.y), 5.0, 5.0);
            break;
        case SnapKind::Origin: {
            QPainterPath diamond;
            diamond.moveTo(p.x, p.y - 6.0);
            diamond.lineTo(p.x + 6.0, p.y);
            diamond.lineTo(p.x, p.y + 6.0);
            diamond.lineTo(p.x - 6.0, p.y);
            diamond.closeSubpath();
            painter.drawPath(diamond);
            break;
        }
        case SnapKind::OnCurve:
            painter.drawLine(QPointF(p.x - 5.0, p.y - 5.0), QPointF(p.x + 5.0, p.y + 5.0));
            painter.drawLine(QPointF(p.x - 5.0, p.y + 5.0), QPointF(p.x + 5.0, p.y - 5.0));
            break;
        case SnapKind::Grid:
            painter.drawEllipse(QPointF(p.x, p.y), 2.0, 2.0);
            break;
        case SnapKind::Free:
            break;
        }
    }
}

} // namespace paramcad
