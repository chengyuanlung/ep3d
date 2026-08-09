#pragma once

#include "Core/Document/ObjectId.h"
#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <QPoint>
#include <QWidget>
#include <map>
#include <vector>

namespace paramcad {

class DocumentPresenter;

// Minimal OCCT-backed 3D view embedded in a Qt widget (spec 17, ADR-M4-006).
//
// This is the ONLY place AIS/V3d types appear outside Kernel/Occt, and it is
// outside src/Core entirely -- Core links neither Qt nor OCCT, and the
// Core-only test executable still imports zero OCCT DLLs.
//
// Ownership: presentation objects (AIS_Shape) are transient and owned solely
// here; semantic objects are owned by the document and only ever referenced by
// ObjectId. Selection resolves a picked AIS object back to an ObjectId through
// a map rebuilt on every display rebuild, so a stale presentation object can
// never resolve to a document object.
class OcctViewWidget : public QWidget {
    Q_OBJECT

public:
    explicit OcctViewWidget(QWidget* parent = nullptr);
    ~OcctViewWidget() override;

    // Non-owning; the presenter (and its document) must outlive this widget.
    void setPresenter(DocumentPresenter* presenter);

    // Rebuilds the displayed solids from the document's current state. Called
    // after a recompute -- the viewer observes results, it is never part of
    // producing them.
    void refreshFromDocument();

    void fitAll();

    // Highlights the presentation for this ObjectId, or clears the highlight
    // for kInvalidObjectId. Selection travelled viewer -> tree only; a tree
    // click left the 3D view unchanged, so the two disagreed about what was
    // selected (raised as a Major by UI review).
    void showSelection(ObjectId id);

    // ObjectId of the currently selected solid, or kInvalidObjectId.
    ObjectId selectedObjectId() const noexcept { return selectedObjectId_; }

signals:
    void selectionChanged(qulonglong objectId);

protected:
    // Qt paints nothing here: OCCT owns the window surface.
    QPaintEngine* paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void initializeViewer();
    // Logical (Qt) -> device (OCCT) pixels. THE single conversion site for
    // mouse input; see the definition for what went wrong without it.
    QPoint toDevicePixels(const QPointF& logical) const;
    void clearPresentations();

    Handle(V3d_Viewer) viewer_;
    Handle(V3d_View) view_;
    Handle(AIS_InteractiveContext) context_;

    // Presentation -> semantic identity. Rebuilt wholesale by every refresh.
    std::map<AIS_InteractiveObject*, ObjectId> presentationToObject_;
    std::vector<Handle(AIS_Shape)> presentations_;

    DocumentPresenter* presenter_ = nullptr;
    ObjectId selectedObjectId_ = kInvalidObjectId;

    enum class DragMode { None, Rotate, Pan };
    bool fittedOnce_ = false;
    DragMode dragMode_ = DragMode::None;
    int lastX_ = 0;
    int lastY_ = 0;
};

} // namespace paramcad
