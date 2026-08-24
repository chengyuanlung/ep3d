#include "Viewer/OcctViewWidget.h"
#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
#include "Kernel/Occt/OcctFaceQuery.h"
#include "Kernel/Occt/OcctSketchWireframe.h"
#include "Kernel/Occt/OcctShape.h"
#include "Viewer/DocumentPresenter.h"

#include <cmath>

#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <Aspect_Handle.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_NameOfColor.hxx>
#include <WNT_Window.hxx>
#include <QMouseEvent>
#include <QPoint>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>

namespace paramcad {

OcctViewWidget::OcctViewWidget(QWidget* parent) : QWidget(parent) {
    // OCCT draws directly into this window; Qt must not paint over it or
    // double-buffer it.
    //
    // WA_NativeWindow is required, not optional: without it this widget shares
    // its top-level window's HWND, so winId() below hands OCCT the WHOLE
    // window and the 3D view paints over the toolbar, property panel and
    // status bar. The symptom is a window that looks like nothing but a
    // viewport -- which is exactly what the first run produced.
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);
}

OcctViewWidget::~OcctViewWidget() = default;

void OcctViewWidget::initializeViewer() {
    if (!view_.IsNull()) return;

    Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection();
    Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(display);

    viewer_ = new V3d_Viewer(driver);
    viewer_->SetDefaultLights();
    viewer_->SetLightOn();
    context_ = new AIS_InteractiveContext(viewer_);

    view_ = viewer_->CreateView();
    Handle(WNT_Window) window =
        new WNT_Window(reinterpret_cast<Aspect_Handle>(winId()));
    view_->SetWindow(window);
    if (!window->IsMapped()) window->Map();
    view_->SetBackgroundColor(Quantity_NOC_GRAY30);
    view_->MustBeResized();
    view_->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.08);
}

void OcctViewWidget::setPresenter(DocumentPresenter* presenter) {
    presenter_ = presenter;
    refreshFromDocument();
}

void OcctViewWidget::setSolidDisplay(SolidDisplay mode) {
    if (solidDisplay_ == mode) return;
    solidDisplay_ = mode;
    if (context_.IsNull()) return; // chosen before the view exists; applies at first refresh
    const Standard_Integer aisMode = mode == SolidDisplay::Shaded ? AIS_Shaded : AIS_WireFrame;
    for (const Handle(AIS_Shape)& presentation : solidPresentations_)
        context_->SetDisplayMode(presentation, aisMode, Standard_False);
    if (!view_.IsNull()) view_->Redraw();
}

void OcctViewWidget::clearPresentations() {
    if (context_.IsNull()) return;
    for (const Handle(AIS_Shape)& presentation : presentations_)
        context_->Remove(presentation, Standard_False);
    presentations_.clear();
    solidPresentations_.clear();
    // The map is rebuilt wholesale rather than patched: a presentation that no
    // longer exists must not remain resolvable to a document object.
    presentationToObject_.clear();
    selectedObjectId_ = kInvalidObjectId;
    // A face belonging to a presentation that is gone is not a face any more.
    // Leaving it behind is how "Sketch on face" stays enabled after the solid
    // it referred to has been deleted.
    pickedFace_ = PickedFace{};
    displayedSketches_ = 0;
}

void OcctViewWidget::refreshFromDocument() {
    initializeViewer();
    clearPresentations();
    if (presenter_ == nullptr) {
        view_->Redraw();
        return;
    }

    // WHAT TO DRAW AND WHERE, asked of the presenter (M27).
    //
    // Resolving an id back to a shape used to happen here, by walking a part's
    // bodies -- which is document knowledge, and precisely the knowledge that
    // cannot live in the widget once there are two kinds of document. The
    // widget's job is OCCT; which document type it is looking at is not its
    // business and no longer reaches it.
    for (const DocumentPresenter::DisplayedShape& displayed : presenter_->displayableShapes()) {
        const ObjectId id = displayed.id;
        const auto* occtShape = dynamic_cast<const OcctShape*>(displayed.shape->handle());
        if (occtShape == nullptr) continue; // a foreign kernel's shape is not displayable

        Handle(AIS_Shape) presentation = new AIS_Shape(occtShape->shape());
        // WHERE IT GOES, as a display transform rather than transformed
        // geometry. An assembly places the SAME part shape once per instance
        // (§19), so moving the geometry would move every other instance with
        // it -- and the viewer must not rewrite what it is shown in any case.
        // Identity for a part costs nothing.
        {
            const Transform3D& place = displayed.placement;
            gp_Trsf motion;
            // ROTATION THEN TRANSLATION -- the same composition the kernel's
            // placeShape fixes, quoted here so the picture and the geometry
            // cannot disagree about what a placement means.
            motion.SetTransformation(
                gp_Quaternion(place.rotation.x, place.rotation.y, place.rotation.z,
                              place.rotation.w),
                gp_Vec(place.translation.x, place.translation.y, place.translation.z));
            presentation->SetLocalTransformation(motion);
        }
        // AIS_Shape defaults to wireframe; a CAD viewer showing a solid as
        // yellow edges is not showing a solid. AIS_Shaded is display mode 1.
        // FACES are what is selectable, not the whole solid (M17.5, superseding
        // the "whole-object selection only" half of ADR-M4-004).
        //
        // Object selection is NOT lost by this: SelectedInteractive() still
        // resolves a picked face back to its AIS_Shape, and that is what the
        // ObjectId lookup below uses. What changes is that the pick also knows
        // WHICH face -- which is the whole of what sketching on a face needs.
        context_->Display(presentation,
                          solidDisplay_ == SolidDisplay::Shaded ? AIS_Shaded : AIS_WireFrame,
                          AIS_Shape::SelectionMode(TopAbs_FACE), Standard_False);
        presentations_.push_back(presentation);
        solidPresentations_.push_back(presentation);
        presentationToObject_[presentation.get()] = id;
    }

    // --- Sketches, drawn where they actually are (M17.7, ADR-M17-030) -------
    //
    // After Finish Sketch the user looks at the part, and until now the sketch
    // simply was not there -- it existed only on the 2D canvas, so nothing in
    // the part view showed where it sat relative to anything else.
    //
    // Drawn as a WIREFRAME and in a distinct colour, because a sketch is not a
    // solid and must not read as one. The colour is not the only channel: a
    // sketch has no faces, so it is visibly a set of curves however it is
    // shaded.
    // SKETCHES ARE A PART'S. displayableSketches() is already empty for an
    // assembly, so this loop simply does not run there -- but the document
    // it reads through has to be asked for as a part, not assumed to be one.
    PartDocument* partDocument = presenter_->partOrNull();
    for (ObjectId id : partDocument == nullptr ? std::vector<ObjectId>{}
                                              : presenter_->displayableSketches()) {
        PartDocument& document = *partDocument;
        const Sketch* sketch = document.findSketch(id);
        if (sketch == nullptr) continue;
        // A support frame that is GONE means the sketch's plane is unknown, and
        // PadFeature already refuses to build on one (M10 gate I). Drawing it
        // at its embedded fallback plane would put the outline somewhere the
        // model does not think it is -- the silent-relocation defect that gate
        // exists to prevent, wearing display clothes.
        if (document.sketchSupportFrameIsMissing(id)) continue;

        std::vector<SketchGeometry> geometry;
        geometry.reserve(sketch->entities().size());
        // The user's own geometry, construction included: a centreline IS part
        // of the sketch, and a 3D view that showed only some of it would leave
        // the user hunting for the line they are sure they drew.
        //
        // The projected reference underlay is deliberately NOT here. It is a
        // copy of edges the solid already draws, so in 3D it would land exactly
        // on top of them and fight for the same pixels while adding nothing.
        for (const SketchEntity& entity : sketch->entities())
            geometry.push_back(entity.geometry);

        const SketchWireframe wireframe = BuildSketchWireframe(
            geometry, PlaneOfSketchFrame(document.effectiveSketchFrame(id)));
        if (!wireframe.shape.isValid()) continue;
        const auto* occtShape = dynamic_cast<const OcctShape*>(wireframe.shape.handle());
        if (occtShape == nullptr) continue;

        Handle(AIS_Shape) presentation = new AIS_Shape(occtShape->shape());
        presentation->SetColor(Quantity_NOC_ORANGE2);
        presentation->SetWidth(2.0);
        // AIS_WireFrame (display mode 0) and WHOLE-OBJECT selection: a sketch
        // has no face to pick, so the face mode the solids use above would make
        // it unselectable. Clicking it selects the sketch, which is what makes
        // "Edit Selected Sketch" reachable from the part view.
        context_->Display(presentation, AIS_WireFrame, 0, Standard_False);
        presentations_.push_back(presentation);
        presentationToObject_[presentation.get()] = id;
        ++displayedSketches_;
    }

    view_->Redraw();
}

void OcctViewWidget::fitAll() {
    initializeViewer();
    // MustBeResized first: FitAll computes against the view's current
    // dimensions, so calling it before the widget has its final laid-out size
    // fits to the wrong viewport and leaves the camera zoomed into a corner.
    // That is what happened when main() called fitAll() immediately after
    // show(), before the layout had run.
    view_->MustBeResized();
    view_->FitAll(0.05, Standard_False);
    view_->ZFitAll();
    view_->Redraw();
}

void OcctViewWidget::showSelection(ObjectId id) {
    if (context_.IsNull()) return;
    selectedObjectId_ = id;
    context_->ClearSelected(Standard_False);
    if (id != kInvalidObjectId) {
        for (const auto& entry : presentationToObject_) {
            if (entry.second != id) continue;
            for (const Handle(AIS_Shape)& presentation : presentations_) {
                if (presentation.get() != entry.first) continue;
                context_->AddOrRemoveSelected(presentation, Standard_False);
                break;
            }
            break;
        }
    }
    view_->Redraw();
}

void OcctViewWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // The first genuinely-sized moment. Fitting here rather than from main()
    // means the initial camera is correct no matter when the caller asks.
    if (!fittedOnce_) {
        fittedOnce_ = true;
        fitAll();
    }
}

void OcctViewWidget::paintEvent(QPaintEvent*) {
    initializeViewer();
    view_->Redraw();
}

void OcctViewWidget::resizeEvent(QResizeEvent*) {
    if (view_.IsNull()) return;
    // MustBeResized alone updates the view's idea of its size but paints
    // nothing. Under WA_PaintOnScreen Qt issues no compensating repaint for the
    // newly exposed area either, so enlarging the window left the old image in
    // one corner and undrawn black everywhere else. Redraw is what actually
    // fills the new viewport.
    view_->MustBeResized();
    view_->Redraw();
}

QPoint OcctViewWidget::toDevicePixels(const QPointF& logical) const {
    // Qt reports mouse positions in LOGICAL pixels; OCCT's view is sized in
    // DEVICE pixels. On a scaled display the two differ by devicePixelRatio, so
    // passing Qt's coordinates straight through made OCCT hit-test a point at
    // 1/ratio of the true distance from the top-left corner -- the user clicked
    // beside the solid and selected it anyway, because the test happened
    // somewhere up and to the left of the cursor.
    //
    // Invisible at 100% scaling, which is why it survived every check made on
    // the 1920x1080 secondary display and was found only by using the
    // application on the 200%-scaled primary one.
    const double ratio = devicePixelRatioF();
    return QPoint(static_cast<int>(logical.x() * ratio),
                  static_cast<int>(logical.y() * ratio));
}

void OcctViewWidget::readPickedFace() {
    pickedFace_ = PickedFace{};
    if (!context_->HasSelectedShape()) return;

    // The whole answer, assigned whole. PlaneOfFace reads the plane AND the
    // face's boundary (M17_FQ_002); this used to copy the four scalar fields
    // across into a second struct and leave the boundary behind, which is why
    // sketching on a face projected nothing while every test passed. There is
    // one struct now, so there is nothing here to get wrong.
    pickedFace_ = PlaneOfFace(context_->SelectedShape());

    // WHICH FEATURE made it (M17.13). PlaneOfFace sees a bare face and has no
    // owner to ask; the owner is resolvable here, because the same click
    // already told us which presentation was hit. Without this a pocket floor
    // is a face nothing can name, and picking it is refused.
    if (pickedFace_.planar && presenter_ != nullptr &&
        selectedObjectId_ != kInvalidObjectId) {
        PartDocument* pickPart = presenter_->partOrNull();
        if (pickPart == nullptr) return;
        PartDocument& document = *pickPart;
        for (const auto& body : document.bodies())
            for (const auto& feature : body->features()) {
                if (feature->id() != selectedObjectId_) continue;
                const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get());
                if (solid == nullptr) continue;
                for (const FacePlane& face : FacesOf(solid->currentShape())) {
                    if (!face.planar || face.createdBy == 0) continue;
                    // Matched by plane, which is all a bare face and an owned
                    // one have in common -- and enough, because two distinct
                    // faces of one solid do not share a plane AND a normal
                    // unless the solid is degenerate.
                    const Vec3 d{face.point.x - pickedFace_.point.x,
                                 face.point.y - pickedFace_.point.y,
                                 face.point.z - pickedFace_.point.z};
                    const double along = d.x * pickedFace_.normal.x +
                                         d.y * pickedFace_.normal.y +
                                         d.z * pickedFace_.normal.z;
                    const double facing = face.normal.x * pickedFace_.normal.x +
                                          face.normal.y * pickedFace_.normal.y +
                                          face.normal.z * pickedFace_.normal.z;
                    if (std::fabs(along) > 1e-6 || facing < 0.999) continue;
                    pickedFace_.createdBy = face.createdBy;
                    break;
                }
            }
    }
}

void OcctViewWidget::mousePressEvent(QMouseEvent* event) {
    initializeViewer();
    const QPoint device = toDevicePixels(event->position());
    lastX_ = device.x();
    lastY_ = device.y();

    if (event->button() == Qt::LeftButton) {
        context_->MoveTo(lastX_, lastY_, view_, Standard_True);
        context_->SelectDetected();
        selectedObjectId_ = kInvalidObjectId;
        pickedFace_ = PickedFace{};
        for (context_->InitSelected(); context_->MoreSelected(); context_->NextSelected()) {
            const Handle(AIS_InteractiveObject) picked = context_->SelectedInteractive();
            if (picked.IsNull()) continue;
            const auto it = presentationToObject_.find(picked.get());
            if (it != presentationToObject_.end()) selectedObjectId_ = it->second;
            readPickedFace();
        }
        view_->Redraw();
        emit selectionChanged(static_cast<qulonglong>(selectedObjectId_));
        dragMode_ = DragMode::Rotate;
        view_->StartRotation(lastX_, lastY_);
    } else if (event->button() == Qt::MiddleButton) {
        dragMode_ = DragMode::Pan;
    }
}

void OcctViewWidget::mouseMoveEvent(QMouseEvent* event) {
    if (view_.IsNull()) return;
    const QPoint device = toDevicePixels(event->position());
    const int x = device.x();
    const int y = device.y();
    if (dragMode_ == DragMode::Rotate && (event->buttons() & Qt::LeftButton)) {
        view_->Rotation(x, y);
    } else if (dragMode_ == DragMode::Pan && (event->buttons() & Qt::MiddleButton)) {
        view_->Pan(x - lastX_, lastY_ - y);
    }
    lastX_ = x;
    lastY_ = y;
}

void OcctViewWidget::mouseReleaseEvent(QMouseEvent*) { dragMode_ = DragMode::None; }

void OcctViewWidget::wheelEvent(QWheelEvent* event) {
    initializeViewer();
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    view_->SetZoom(steps > 0.0 ? 1.1 * steps : 1.0 / (1.1 * -steps));
    view_->Redraw();
}

} // namespace paramcad
