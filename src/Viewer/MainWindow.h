#pragma once

#include "Core/Document/ObjectId.h"
#include <QMainWindow>
#include <QString>
#include <set>

class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QLabel;

namespace paramcad {

class PartDocument;
class DocumentPresenter;
class OcctViewWidget;

// The CAD application shell (UI spec 5): menu, toolbar, left Model Tree,
// dominant central 3D viewer, right Property Panel, status bar.
//
// Ownership (UI spec 20, ADR-M4-006): this window holds a NON-OWNING pointer to
// the document. Qt objects never own semantic CAD objects; every row in the
// tree and the property panel refers to an ObjectId, and every edit goes
// through PartDocument's facade.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(PartDocument& document, DocumentPresenter& presenter, QWidget* parent = nullptr);

    // Selected semantic object, or kInvalidObjectId. Exposed so a smoke test
    // can assert Tree/Viewer/Property agreement without a human (UI spec 10).
    ObjectId selectedObjectId() const noexcept { return selectedId_; }

    // Rebuilds tree, properties and viewer from current document state.
    void refreshAll();

    // Imports `path` and refreshes the shell. Separated from the menu slot so
    // the whole workflow -- read, import, recompute, redisplay, report -- is
    // reachable without a file dialog, which is the only part of it a test
    // cannot drive. Returns the message shown in the status bar.
    QString importDxfFile(const QString& path);

    // Selects an object as if the user had clicked it in the tree.
    void selectObject(ObjectId id);

    // True when the property table fits its panel, so every VALUE is on screen.
    //
    // Exposed because this is a fact about what the user can SEE, and the
    // defect it guards was invisible to every data-level test: the rows were
    // correct and fully populated, and one ninety-character diagnostic had
    // pushed the value column out of the visible area. `propertiesOf` returned
    // ten good rows while the user saw ten labels and no values.
    bool propertyPanelFitsItsPanel() const;

private slots:
    void onTreeSelectionChanged();
    void onViewerSelectionChanged(qulonglong objectId);
    void onPropertyCommitted(int row, int column);
    void onRecomputeRequested();
    void onFitAllRequested();
    void onToggleHiddenRequested();
    void onImportDxfRequested();

private:
    void buildMenus();
    void buildToolbar();
    void buildDocks();
    void rebuildTree();
    void rebuildProperties();
    void updateStatus();
    void reportHealth();
    QString selectionSummary() const;
    std::set<ObjectId> hiddenIds() const;

    PartDocument* document_;
    DocumentPresenter* presenter_;

    OcctViewWidget* viewer_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QTableWidget* properties_ = nullptr;
    QLabel* statusLeft_ = nullptr;
    QLabel* statusRight_ = nullptr;

    ObjectId selectedId_ = kInvalidObjectId;
    bool updatingWidgets_ = false; // guards against selection feedback loops
};

} // namespace paramcad
