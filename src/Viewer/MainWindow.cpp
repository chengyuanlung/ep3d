#include "Viewer/MainWindow.h"

#include "Core/Document/PartDocument.h"
#include "Core/Physics/MassProperties.h"
#include "Viewer/DesignTokens.h"
#include "Viewer/DocumentOutline.h"
#include "Viewer/DocumentPresenter.h"
#include "Core/Import/SketchImporter.h"
#include "Import/Dxf/DxfReader.h"
#include "Viewer/OcctViewWidget.h"

#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QPalette>
#include <QVariant>
#include <functional>

namespace paramcad {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;

} // namespace

MainWindow::MainWindow(PartDocument& document, DocumentPresenter& presenter, QWidget* parent)
    : QMainWindow(parent), document_(&document), presenter_(&presenter) {
    setWindowTitle(QStringLiteral("EP3D - Parametric CAD"));
    setMinimumSize(ui::size::kMinWindowWidth, ui::size::kMinWindowHeight);

    viewer_ = new OcctViewWidget(this);
    viewer_->setPresenter(presenter_);
    setCentralWidget(viewer_); // the viewer is the dominant work area (UI spec 1)

    buildMenus();
    buildToolbar();
    buildDocks();

    statusLeft_ = new QLabel(this);
    statusRight_ = new QLabel(this);
    statusRight_->setFont(ui::numericFont(font()));
    statusBar()->setMinimumHeight(ui::size::kStatusBarHeight);
    statusBar()->addWidget(statusLeft_, 1);
    statusBar()->addPermanentWidget(statusRight_);

    connect(viewer_, &OcctViewWidget::selectionChanged, this,
            &MainWindow::onViewerSelectionChanged);

    // Docks get a deliberate share of the width rather than whatever their
    // contents ask for: the viewer must stay the dominant area (UI spec 5),
    // and the tree needs enough room that names elide only when genuinely long.
    resizeDocks({findChild<QDockWidget*>()}, {ui::size::kModelTreeMinWidth + 100},
                Qt::Horizontal);

    refreshAll();
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* importDxf = file->addAction(QStringLiteral("&Import DXF..."));
    importDxf->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    connect(importDxf, &QAction::triggered, this, &MainWindow::onImportDxfRequested);
    file->addSeparator();
    QAction* quit = file->addAction(QStringLiteral("E&xit"));
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
    QAction* fit = view->addAction(QStringLiteral("&Fit All"));
    fit->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(fit, &QAction::triggered, this, &MainWindow::onFitAllRequested);

    QAction* toggleHidden = view->addAction(QStringLiteral("Show/&Hide Selected"));
    toggleHidden->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    connect(toggleHidden, &QAction::triggered, this, &MainWindow::onToggleHiddenRequested);

    QMenu* model = menuBar()->addMenu(QStringLiteral("&Model"));
    QAction* recompute = model->addAction(QStringLiteral("&Recompute"));
    recompute->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    connect(recompute, &QAction::triggered, this, &MainWindow::onRecomputeRequested);

    // ApplicationShortcut, not the WindowShortcut default. The 3D view is a
    // NATIVE child window (WA_NativeWindow is required for OCCT to own its own
    // surface), and while it holds focus the default context did not deliver
    // these keys -- so every shortcut the menus advertise silently did nothing
    // whenever the user had last clicked in the viewport, which is most of the
    // time in a CAD application. A menu that promises a shortcut it does not
    // honour is worse than one that promises nothing.
    for (QAction* action : {fit, toggleHidden, recompute})
        action->setShortcutContext(Qt::ApplicationShortcut);
}

void MainWindow::buildToolbar() {
    // Small by design (UI spec 9): only commands M4 actually supports.
    QToolBar* bar = addToolBar(QStringLiteral("Main"));
    bar->setIconSize(QSize(ui::size::kToolbarIcon, ui::size::kToolbarIcon));
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    bar->setMovable(false);

    QAction* fit = bar->addAction(QStringLiteral("Fit All"));
    fit->setToolTip(QStringLiteral("Fit the whole model in the view (Ctrl+Shift+F)"));
    connect(fit, &QAction::triggered, this, &MainWindow::onFitAllRequested);

    QAction* hide = bar->addAction(QStringLiteral("Show/Hide"));
    hide->setToolTip(QStringLiteral("Show or hide the selected solid (Ctrl+H)"));
    connect(hide, &QAction::triggered, this, &MainWindow::onToggleHiddenRequested);

    QAction* recompute = bar->addAction(QStringLiteral("Recompute"));
    recompute->setToolTip(QStringLiteral("Recompute the document (Ctrl+R)"));
    connect(recompute, &QAction::triggered, this, &MainWindow::onRecomputeRequested);
}

void MainWindow::buildDocks() {
    // --- Left: Model Tree -------------------------------------------------
    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels({QStringLiteral("Model"), QStringLiteral("State")});
    tree_->setMinimumWidth(ui::size::kModelTreeMinWidth);
    tree_->setUniformRowHeights(true);
    // The design token was defined and never applied -- the self-validation
    // report claimed 26 px rows while the tree rendered 16 px, which UI review
    // measured. Applying it is what makes the token real.
    tree_->setStyleSheet(QStringLiteral("QTreeWidget::item { height: %1px; }")
                             .arg(ui::size::kTreeRowHeight));
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            &MainWindow::onTreeSelectionChanged);

    auto* treeDock = new QDockWidget(QStringLiteral("Model Tree"), this);
    treeDock->setWidget(tree_);
    treeDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::LeftDockWidgetArea, treeDock);

    // --- Right: Property Panel -------------------------------------------
    properties_ = new QTableWidget(this);
    properties_->setColumnCount(3);
    properties_->setHorizontalHeaderLabels(
        {QStringLiteral("Property"), QStringLiteral("Value"), QStringLiteral("Unit")});
    properties_->verticalHeader()->setVisible(false);
    properties_->verticalHeader()->setDefaultSectionSize(ui::size::kPropertyRowHeight);
    properties_->setMinimumWidth(ui::size::kPropertyPanelWidth);
    properties_->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Enter commits, Esc cancels -- Qt's default item-editor behaviour, which
    // UI spec 7 asks for and which a custom editor would have to re-implement.
    properties_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                 QAbstractItemView::EditKeyPressed |
                                 QAbstractItemView::AnyKeyPressed);
    properties_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    properties_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    properties_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    connect(properties_, &QTableWidget::cellChanged, this, &MainWindow::onPropertyCommitted);

    auto* propertyDock = new QDockWidget(QStringLiteral("Properties"), this);
    propertyDock->setWidget(properties_);
    propertyDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, propertyDock);
}

void MainWindow::rebuildTree() {
    const DocumentOutline outline(*document_);
    const OutlineNode root = outline.build(hiddenIds());

    tree_->clear();
    const std::function<QTreeWidgetItem*(const OutlineNode&)> makeItem =
        [&](const OutlineNode& node) -> QTreeWidgetItem* {
        auto* item = new QTreeWidgetItem();
        const ui::StatePresentation presentation =
            ui::presentationFor(node.state, tree_->palette());

        // Kind tag + marker + name: state is carried by TEXT first, colour
        // second (UI spec 11).
        QString label = ui::kindTag(node.kind) + QStringLiteral(" ");
        if (!presentation.marker.isEmpty()) label += presentation.marker + QStringLiteral(" ");
        label += QString::fromStdString(node.name);
        item->setText(0, label);
        item->setText(1, presentation.label);
        item->setForeground(0, presentation.color);
        item->setForeground(1, presentation.color);
        if (presentation.bold) {
            QFont bold = item->font(0);
            bold.setBold(true);
            item->setFont(0, bold);
        }
        item->setData(0, kIdRole, QVariant::fromValue<qulonglong>(node.id));

        // The full name FIRST: names elide in the column, and the report claimed
        // the tooltip carried them when it carried only type and state.
        QString tip = QString::fromStdString(node.name) + QStringLiteral("\n") +
                      QString::fromStdString(node.typeLabel) + QStringLiteral(" - ") +
                      presentation.label;
        if (!node.diagnostic.empty())
            tip += QStringLiteral("\n") + QString::fromStdString(node.diagnostic);
        item->setToolTip(0, tip);
        item->setToolTip(1, tip);

        for (const OutlineNode& child : node.children) item->addChild(makeItem(child));
        return item;
    };

    tree_->addTopLevelItem(makeItem(root));
    tree_->expandAll();
}

void MainWindow::rebuildProperties() {
    const DocumentOutline outline(*document_);
    const std::vector<PropertyRow> rows = outline.propertiesOf(selectedId_);

    properties_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const PropertyRow& row = rows[static_cast<std::size_t>(i)];

        auto* label = new QTableWidgetItem(QString::fromStdString(row.group) +
                                           QStringLiteral(" / ") +
                                           QString::fromStdString(row.label));
        label->setFlags(Qt::ItemIsEnabled); // read-only, and visibly so

        auto* value = new QTableWidgetItem(QString::fromStdString(row.value));
        value->setFont(ui::numericFont(properties_->font()));
        value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (row.editable) {
            value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            value->setData(kIdRole, QVariant::fromValue<qulonglong>(row.parameterId));
            value->setToolTip(QStringLiteral("Editable - type a value and press Enter"));
        } else {
            value->setFlags(Qt::ItemIsEnabled);
            // Read-only rows use the palette's own disabled colour, so they stay
            // distinguishable AND legible under any theme (UI spec 14/19).
            value->setForeground(
                properties_->palette().color(QPalette::Disabled, QPalette::Text));
        }

        // Unit in its own column so a value and its unit can never collide or
        // be confused with each other (UI spec 8).
        auto* unit = new QTableWidgetItem(QString::fromStdString(row.unitLabel));
        unit->setFlags(Qt::ItemIsEnabled);

        properties_->setItem(i, 0, label);
        properties_->setItem(i, 1, value);
        properties_->setItem(i, 2, unit);
    }
}

void MainWindow::updateStatus() {
    const MassProperties& mp = document_->massProperties();
    if (mp.valid) {
        statusRight_->setText(
            QStringLiteral("Volume %1 mm^3   Mass %2 kg   COM (%3, %4, %5) mm")
                .arg(mp.volumeMm3, 0, 'f', 1)
                .arg(mp.massKg, 0, 'f', 4)
                .arg(mp.centerOfMassMm.x, 0, 'f', 2)
                .arg(mp.centerOfMassMm.y, 0, 'f', 2)
                .arg(mp.centerOfMassMm.z, 0, 'f', 2));
    } else {
        // Never show stale numbers as if they were current (UI spec 13,
        // ADR-M3-006). The values are retained internally; the UI says so
        // rather than displaying them.
        statusRight_->setText(QStringLiteral("Mass properties: not current"));
    }
}

void MainWindow::refreshAll() {
    updatingWidgets_ = true;
    rebuildTree();
    rebuildProperties();
    updatingWidgets_ = false;
    viewer_->refreshFromDocument();
    updateStatus();
    // Report any failure in the state we are ALREADY in. Doing this only from
    // onRecomputeRequested left a freshly-opened document showing a failed tree
    // with an empty status bar -- the user could see that something was wrong
    // but not what (UI spec 12).
    reportHealth();
}

std::set<ObjectId> MainWindow::hiddenIds() const {
    // Ask the presenter which ids are hidden, without the outline needing any
    // notion of a viewer.
    std::set<ObjectId> hidden;
    const DocumentOutline outline(*document_);
    const std::function<void(const OutlineNode&)> visit = [&](const OutlineNode& node) {
        if (node.id != kInvalidObjectId && presenter_->isHidden(node.id)) hidden.insert(node.id);
        for (const OutlineNode& child : node.children) visit(child);
    };
    visit(outline.build());
    return hidden;
}

void MainWindow::reportHealth() {
    const DocumentOutline outline(*document_);
    const OutlineNode root = outline.build(hiddenIds());
    for (const OutlineNode& child : root.children) {
        if (child.state != OutlineState::Failed) continue;
        // Name the affected object AND the reason, never just "failed"
        // (UI spec 12).
        QString message = QString::fromStdString(child.name) + QStringLiteral(" failed");
        if (!child.diagnostic.empty())
            message += QStringLiteral(": ") + QString::fromStdString(child.diagnostic);
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return;
    }
    statusLeft_->setText(selectionSummary());
    statusLeft_->setToolTip(QString());
}

QString MainWindow::selectionSummary() const {
    if (selectedId_ == kInvalidObjectId) return QStringLiteral("No selection");
    // A name, not an ObjectId: UI spec 17 requires that no task need knowledge
    // of internal ids or developer terminology, and the status bar was showing
    // "Selected object 12".
    const DocumentOutline outline(*document_);
    const OutlineNode root = outline.build(hiddenIds());
    const std::function<const OutlineNode*(const OutlineNode&)> find =
        [&](const OutlineNode& node) -> const OutlineNode* {
        if (node.id == selectedId_) return &node;
        for (const OutlineNode& child : node.children)
            if (const OutlineNode* hit = find(child)) return hit;
        return nullptr;
    };
    if (const OutlineNode* node = find(root))
        return QStringLiteral("Selected: %1 (%2)")
            .arg(QString::fromStdString(node->name))
            .arg(QString::fromStdString(node->typeLabel));
    return QStringLiteral("No selection");
}

void MainWindow::selectObject(ObjectId id) {
    selectedId_ = id;
    updatingWidgets_ = true;
    rebuildProperties();

    // Mirror the selection into the tree so Tree, Viewer and Properties always
    // name the same semantic object (UI spec 10; a mismatch here is one of
    // spec 23's automatic REQUEST CHANGES triggers).
    if (id == kInvalidObjectId) {
        // Deselection must actually clear the tree. Guarding this on a valid id
        // left the tree highlighting an object the panel and status bar no
        // longer described -- and because re-clicking the already-current row
        // emits no signal, the user could not get back to it.
        tree_->setCurrentItem(nullptr);
        tree_->clearSelection();
    } else {
        QTreeWidgetItemIterator it(tree_);
        while (*it) {
            const auto rowId = static_cast<ObjectId>((*it)->data(0, kIdRole).toULongLong());
            if (rowId == id) {
                tree_->setCurrentItem(*it);
                break;
            }
            ++it;
        }
    }
    // Selection travels to the 3D view as well, so all three surfaces agree
    // whichever one the user acted on (UI spec 10).
    viewer_->showSelection(id);
    updatingWidgets_ = false;

    reportHealth();
}

void MainWindow::onTreeSelectionChanged() {
    if (updatingWidgets_) return;
    const QList<QTreeWidgetItem*> selected = tree_->selectedItems();
    if (selected.isEmpty()) {
        selectObject(kInvalidObjectId);
        return;
    }
    selectObject(static_cast<ObjectId>(selected.front()->data(0, kIdRole).toULongLong()));
}

void MainWindow::onViewerSelectionChanged(qulonglong objectId) {
    if (updatingWidgets_) return;
    selectObject(static_cast<ObjectId>(objectId));
}

void MainWindow::onPropertyCommitted(int row, int column) {
    if (updatingWidgets_ || column != 1) return;
    QTableWidgetItem* item = properties_->item(row, column);
    if (item == nullptr) return;

    const auto parameterId = static_cast<ObjectId>(item->data(kIdRole).toULongLong());
    if (parameterId == kInvalidObjectId) return;

    bool ok = false;
    const double value = item->text().toDouble(&ok);
    if (!ok) {
        // Immediate feedback, no modal dialog (UI spec 7).
        statusLeft_->setText(QStringLiteral("'%1' is not a number").arg(item->text()));
        updatingWidgets_ = true;
        rebuildProperties(); // restore the committed value
        updatingWidgets_ = false;
        return;
    }

    // Edits flow through the document facade -- the panel never writes into a
    // Parameter or a Feature directly (UI spec 20).
    document_->setParameterValue(parameterId, value);
    onRecomputeRequested();
}

void MainWindow::onRecomputeRequested() {
    const bool ok = presenter_->recomputeForDisplay();
    const ObjectId keep = selectedId_;
    refreshAll();
    selectObject(keep); // an edit must not silently change the selection

    // refreshAll() has already reported whatever state the document is now in
    // (reportHealth), so success only needs to say so without overwriting a
    // failure message that is still true.
    if (ok) statusLeft_->setText(selectionSummary());
}

void MainWindow::onImportDxfRequested() {
    // The dialog is the ONLY part of this workflow a test cannot drive, so it
    // is the only part that lives in the slot. Everything after it is in
    // importDxfFile, which the import UI test calls directly.
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import DXF"), QString(),
        QStringLiteral("DXF files (*.dxf);;All files (*)"));
    if (path.isEmpty()) return; // cancelled; nothing said, nothing changed
    importDxfFile(path);
}

QString MainWindow::importDxfFile(const QString& path) {
    const DxfReadResult read = ReadDxfFile(path.toStdString());
    if (!read) {
        // The CAUSE, not "import failed" (spec 11). The reader already
        // distinguishes missing from unreadable from malformed; throwing that
        // away at the display boundary is what M5's Blocked state did.
        const QString message = QStringLiteral("DXF import failed (%1): %2")
                                    .arg(QString::fromUtf8(DxfReadErrorName(read.error)),
                                         QString::fromStdString(read.message));
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    }

    const QString name = QFileInfo(path).completeBaseName();
    const SketchImportResult imported = ImportGeometryIntoNewSketch(
        *document_, name.isEmpty() ? std::string("Imported") : name.toStdString(),
        read.geometry);
    if (!imported) {
        const QString message =
            QStringLiteral("DXF import failed: %1").arg(QString::fromStdString(imported.message));
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    }

    // The imported sketch is an ordinary document object, so the ordinary
    // refresh path shows it: recompute, then rebuild tree, panel and view.
    onRecomputeRequested();
    selectObject(imported.sketchId);

    QString message = QStringLiteral("Imported %1: %2")
                          .arg(QFileInfo(path).fileName(),
                               QString::fromStdString(imported.message));
    if (imported.skippedCount > 0) {
        // Skipped entities are reported in the status bar rather than only in a
        // log: a user who cannot see that something was dropped will assume
        // nothing was.
        message += QStringLiteral(" [%1 skipped]").arg(imported.skippedCount);
    }
    statusLeft_->setText(message);
    statusLeft_->setToolTip(message);
    return message;
}

void MainWindow::onFitAllRequested() { viewer_->fitAll(); }

void MainWindow::onToggleHiddenRequested() {
    if (selectedId_ == kInvalidObjectId) {
        statusLeft_->setText(QStringLiteral("Select a solid first, then Show/Hide it"));
        return;
    }
    // Visibility is view state (ADR-M4-014): the presenter owns it, the
    // document never hears about it, and nothing is recomputed.
    presenter_->toggleHidden(selectedId_);
    const ObjectId keep = selectedId_;
    refreshAll();
    selectObject(keep);
    statusLeft_->setText(presenter_->isHidden(keep) ? QStringLiteral("Hidden")
                                                    : QStringLiteral("Shown"));
}

} // namespace paramcad
