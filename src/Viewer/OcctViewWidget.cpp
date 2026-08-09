#include "Viewer/OcctViewWidget.h"
#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/ISolidFeature.h"
#include "Kernel/Occt/OcctShape.h"
#include "Viewer/DocumentPresenter.h"

#include <Aspect_Handle.hxx>
#include <Quantity_Color.hxx>
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

void OcctViewWidget::clearPresentations() {
    if (context_.IsNull()) return;
    for (const Handle(AIS_Shape)& presentation : presentations_)
        context_->Remove(presentation, Standard_False);
    presentations_.clear();
    // The map is rebuilt wholesale rather than patched: a presentation that no
    // longer exists must not remain resolvable to a document object.
    presentationToObject_.clear();
    selectedObjectId_ = kInvalidObjectId;
}

void OcctViewWidget::refreshFromDocument() {
    initializeViewer();
    clearPresentations();
    if (presenter_ == nullptr) {
        view_->Redraw();
        return;
    }

    PartDocument& document = presenter_->document();
    for (ObjectId id : presenter_->displayableSolids()) {
        // Resolve the id back to its shape. The viewer reads document state; it
        // never mutates it and never caches a semantic pointer beyond this call.
        const ISolidFeature* solid = nullptr;
        for (const auto& body : document.bodies()) {
            for (const auto& feature : body->features()) {
                if (feature->id() != id) continue;
                solid = dynamic_cast<const ISolidFeature*>(feature.get());
                break;
            }
            if (solid != nullptr) break;
        }
        if (solid == nullptr) continue;

        const auto* occtShape = dynamic_cast<const OcctShape*>(solid->currentShape().handle());
        if (occtShape == nullptr) continue; // a foreign kernel's shape is not displayable

        Handle(AIS_Shape) presentation = new AIS_Shape(occtShape->shape());
        // AIS_Shape defaults to wireframe; a CAD viewer showing a solid as
        // yellow edges is not showing a solid. AIS_Shaded is display mode 1.
        context_->Display(presentation, AIS_Shaded, 0, Standard_False);
        presentations_.push_back(presentation);
        presentationToObject_[presentation.get()] = id;
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

void OcctViewWidget::mousePressEvent(QMouseEvent* event) {
    initializeViewer();
    const QPoint device = toDevicePixels(event->position());
    lastX_ = device.x();
    lastY_ = device.y();

    if (event->button() == Qt::LeftButton) {
        // Whole-object selection only; persistent face/edge selection is
        // explicitly out of scope (ADR-M4-004).
        context_->MoveTo(lastX_, lastY_, view_, Standard_True);
        context_->SelectDetected();
        selectedObjectId_ = kInvalidObjectId;
        for (context_->InitSelected(); context_->MoreSelected(); context_->NextSelected()) {
            const Handle(AIS_InteractiveObject) picked = context_->SelectedInteractive();
            if (picked.IsNull()) continue;
            const auto it = presentationToObject_.find(picked.get());
            if (it != presentationToObject_.end()) selectedObjectId_ = it->second;
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
