#pragma once

#include "Core/Document/ObjectId.h"
#include "Viewer/FaceSketch.h"
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

    // How SOLIDS are drawn (M17.9, ADR-M17-032).
    //
    // Sketches are always wireframe and are deliberately not affected: they
    // have no faces, so "shaded" has nothing to say about them, and switching
    // the whole scene would make the one object that cannot be shaded look
    // broken in shaded mode.
    enum class SolidDisplay { Shaded, Wireframe };

    SolidDisplay solidDisplay() const noexcept { return solidDisplay_; }
    // Applied to the presentations already in the scene rather than by
    // rebuilding it: a rebuild would drop the selection, and changing how
    // something is drawn is not a reason to stop having it selected.
    void setSolidDisplay(SolidDisplay mode);

    void fitAll();

    // Highlights the presentation for this ObjectId, or clears the highlight
    // for kInvalidObjectId. Selection travelled viewer -> tree only; a tree
    // click left the 3D view unchanged, so the two disagreed about what was
    // selected (raised as a Major by UI review).
    void showSelection(ObjectId id);

    // How many presentations the last refresh put in the scene, and how many of
    // those are sketches (M17.7).
    //
    // "The presenter listed the sketch" and "the sketch is in the 3D scene" are
    // different claims, and this project has shipped the gap between two such
    // claims more than once. These count what was actually handed to the
    // interactive context.
    int displayedPresentationCount() const noexcept {
        return static_cast<int>(presentations_.size());
    }
    int displayedSketchCount() const noexcept { return displayedSketches_; }

    // ObjectId of the currently selected solid, or kInvalidObjectId.
    ObjectId selectedObjectId() const noexcept { return selectedObjectId_; }

    // The face under the last left-click, in the terms the Qt-free planner
    // needs (M17.5). `picked` is false when the click hit nothing, and
    // `planar` is false when it hit a face no sketch can live on -- the
    // widget REPORTS both rather than deciding what they mean, so the
    // judgement stays in PlanSketchOnFace where a test can reach it.
    const PickedFace& pickedFace() const noexcept { return pickedFace_; }

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
    // Fills pickedFace_ from the context's current sub-shape selection.
    void readPickedFace();

    Handle(V3d_Viewer) viewer_;
    Handle(V3d_View) view_;
    Handle(AIS_InteractiveContext) context_;

    // Presentation -> semantic identity. Rebuilt wholesale by every refresh.
    std::map<AIS_InteractiveObject*, ObjectId> presentationToObject_;
    std::vector<Handle(AIS_Shape)> presentations_;

    DocumentPresenter* presenter_ = nullptr;
    ObjectId selectedObjectId_ = kInvalidObjectId;
    PickedFace pickedFace_{};
    int displayedSketches_ = 0;
    SolidDisplay solidDisplay_ = SolidDisplay::Shaded;
    // The solid presentations alone, so a display-mode switch can reach them
    // without touching the sketches drawn alongside them.
    std::vector<Handle(AIS_Shape)> solidPresentations_;

    enum class DragMode { None, Rotate, Pan };
    bool fittedOnce_ = false;
    DragMode dragMode_ = DragMode::None;
    int lastX_ = 0;
    int lastY_ = 0;
};

} // namespace paramcad
