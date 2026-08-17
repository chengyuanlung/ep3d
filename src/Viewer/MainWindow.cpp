#include "Viewer/MainWindow.h"

#include "Core/Document/PartDocument.h"
#include "Core/Physics/MassProperties.h"
#include "Viewer/DesignTokens.h"
#include "Viewer/DocumentOutline.h"
#include "Viewer/DocumentPresenter.h"
#include "Core/Import/SketchImporter.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Body/Body.h"
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
#include <algorithm>
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
    // The VALUE column stretches; the other two size to their content.
    //
    // It was the other way round, and one row destroyed the panel: the sketch's
    // "Profile / Diagnostic" value is a ninety-character sentence, and
    // ResizeToContents on the value column sized it to that sentence. The table
    // then grew wider than its dock, and every value -- name, entity count,
    // origin, degrees of freedom -- was pushed out of the visible area. The
    // owner selected an imported sketch and saw a panel of labels with no
    // values at all.
    //
    // Property NAMES are short and bounded, so sizing them to content is safe.
    // A value can be any length, so it must take the space that is left and
    // elide, with the full text in the tooltip -- nothing is lost, and no single
    // row can push the rest off the panel.
    // ...and the LABEL column is capped, which the first version of this fix
    // was missing. ResizeToContents on column 0 is only safe while labels are
    // short, and "Reconstruction / Skipped item" was already long enough to
    // squeeze the value column to nine characters. Interactive with an explicit
    // width bounds it; the value column then always has room left.
    properties_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    properties_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    properties_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    // The cap itself is applied per rebuild, in clampLabelColumn(), because it
    // depends on how wide the dock actually is -- a fixed maximum computed from
    // the panel's MINIMUM width starves the value column on a narrow panel and
    // wastes space on a wide one.
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
    std::vector<PropertyRow> rows = outline.propertiesOf(selectedId_);

    // Reconstruction provenance, appended HERE rather than in DocumentOutline.
    //
    // The report is session state held by the shell (ADR-M7-017: it is not part
    // of the document and is not persisted), so ViewerCore has no business
    // knowing about it -- and giving it a reconstruction dependency to display
    // three rows would invert the layering for nothing.
    const auto reconstruction = reconstructionReports_.find(selectedId_);
    if (reconstruction != reconstructionReports_.end()) {
        const ReconstructionReport& report = reconstruction->second;
        // Spec 27 item 11 and spec 3: explicit and inferred must be visibly
        // distinguishable, so they are separate rows rather than one total.
        // A single "11 constraints reconstructed" would hide the fact that two
        // came from the drawing and nine were this program's own inference.
        //
        // LABELS ARE KEPT SHORT ON PURPOSE. The property table sizes its label
        // column to content, which is only safe while labels are bounded --
        // that is the documented assumption the M6.14 fix rests on. The first
        // draft of these rows said "From source dimensions" and "Inferred by
        // EP3D", and the panel-fit guard failed immediately: 39 characters was
        // enough to push the value column out of the dock again. The guard was
        // right and the labels were wrong.
        rows.push_back(PropertyRow{"Reconstruction", "From source",
                                   std::to_string(report.explicitCount()), "", false,
                                   kInvalidObjectId, 0.0});
        rows.push_back(
            PropertyRow{"Reconstruction", "Inferred",
                        std::to_string(report.entries.size() - report.explicitCount()), "",
                        false, kInvalidObjectId, 0.0});
        // ALWAYS shown, including "0". A row that appears only when something
        // was skipped cannot be told from one where nothing asked the question,
        // and "nothing was skipped" is the claim a user needs before trusting
        // the result.
        rows.push_back(PropertyRow{"Reconstruction", "Skipped",
                                   std::to_string(report.skipped.size()), "", false,
                                   kInvalidObjectId, 0.0});
        for (const ReconstructionSkip& skip : report.skipped) {
            std::string detail = std::string(ReconstructionSkipReasonName(skip.reason)) + ": " +
                                 skip.detail;
            if (!skip.sourceRef.empty()) detail += " (source " + skip.sourceRef + ")";
            rows.push_back(PropertyRow{"Reconstruction", "Skipped item", detail, "", false,
                                       kInvalidObjectId, 0.0});
        }
    }

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
            value->setToolTip(QString::fromStdString(row.value) +
                              QStringLiteral("\nEditable - type a value and press Enter"));
        } else {
            value->setFlags(Qt::ItemIsEnabled);
            // Read-only rows use the palette's own disabled colour, so they stay
            // distinguishable AND legible under any theme (UI spec 14/19).
            value->setForeground(
                properties_->palette().color(QPalette::Disabled, QPalette::Text));
            // The full text, because the cell elides. A diagnostic the user can
            // only read half of is a diagnostic that has not been delivered.
            value->setToolTip(QString::fromStdString(row.value));
        }

        // Unit in its own column so a value and its unit can never collide or
        // be confused with each other (UI spec 8).
        auto* unit = new QTableWidgetItem(QString::fromStdString(row.unitLabel));
        unit->setFlags(Qt::ItemIsEnabled);

        properties_->setItem(i, 0, label);
        properties_->setItem(i, 1, value);
        properties_->setItem(i, 2, unit);
    }

    // After the rows exist, so the label column can be sized to what is
    // actually in it and then capped.
    clampLabelColumn();
}

const ReconstructionReport* MainWindow::reconstructionReportFor(ObjectId sketchId) const {
    const auto it = reconstructionReports_.find(sketchId);
    return it == reconstructionReports_.end() ? nullptr : &it->second;
}

void MainWindow::forgetProvenanceFor(ObjectId sketchId) {
    reconstructionReports_.erase(sketchId);
}

void MainWindow::forgetAllProvenance() { reconstructionReports_.clear(); }

void MainWindow::pruneProvenance() {
    // Provenance describes a sketch that is IN this document. An entry whose
    // sketch has gone is not merely stale -- ObjectIds are handed out by a
    // process counter and a loaded document's ids come from its file, so the
    // entry could later be read as belonging to an unrelated sketch that
    // happens to reuse the number. Cheap, and it runs after every recompute,
    // so no future removal path can leave the map wrong by forgetting to say
    // so (round 2's M3).
    if (document_ == nullptr) {
        forgetAllProvenance();
        return;
    }
    for (auto it = reconstructionReports_.begin(); it != reconstructionReports_.end();)
        it = document_->findSketch(it->first) == nullptr ? reconstructionReports_.erase(it)
                                                         : std::next(it);
}

std::string MainWindow::displayedPropertyValue(const std::string& label) const {
    for (int row = 0; row < properties_->rowCount(); ++row) {
        const QTableWidgetItem* name = properties_->item(row, 0);
        const QTableWidgetItem* value = properties_->item(row, 1);
        if (name == nullptr || value == nullptr) continue;
        // Labels are rendered as "Group / Label", so match the tail.
        const QString text = name->text();
        const int slash = text.lastIndexOf(QStringLiteral(" / "));
        const QString bare = slash < 0 ? text : text.mid(slash + 3);
        if (bare == QString::fromStdString(label)) return value->text().toStdString();
    }
    return {};
}

bool MainWindow::panelFitGuardCanFail() {
    if (properties_->rowCount() == 0) return false;
    const int restore = properties_->columnWidth(0);
    // Deliberately starve the value column.
    properties_->setColumnWidth(0, properties_->viewport()->width() -
                                       properties_->columnWidth(2) -
                                       (ui::size::kMinValueColumnWidth / 2));
    const bool refused = !propertyPanelFitsItsPanel();
    properties_->setColumnWidth(0, restore);
    return refused;
}

void MainWindow::clampLabelColumn() {
    // Size the label column to its content, then take width back from it until
    // the VALUE column clears its readable minimum.
    //
    // Labels elide with a tooltip when that happens, which is the right way
    // round: a truncated label is still identifiable from its neighbours, while
    // a truncated value is just a shorter number.
    properties_->resizeColumnToContents(0);
    const int available = properties_->viewport()->width() - properties_->columnWidth(2);
    const int maxLabel = std::max(60, available - ui::size::kMinValueColumnWidth);
    if (properties_->columnWidth(0) > maxLabel) properties_->setColumnWidth(0, maxLabel);
}

bool MainWindow::propertyPanelFitsItsPanel() const {
    if (properties_->rowCount() == 0) return true;
    // Two separate questions, because M6.14 turned out to have two answers.
    //
    // FIRST: does the table fit its viewport? That is what the original guard
    // asked -- and on its own it is nearly a tautology, because column 1 is
    // Stretch, so the header length equals the viewport width by construction.
    // It still earns its place: it catches a column-mode regression, which is
    // exactly the mutation that produced M6.14 in the first place.
    if (properties_->horizontalHeader()->length() > properties_->viewport()->width()) return false;

    // SECOND, and this is the half independent review found missing: is the
    // VALUE column wide enough to read? Fitting and being readable are not the
    // same thing. A 133-character diagnostic squeezed into nine characters
    // "fits" perfectly, and three different skip rows then render identically.
    return properties_->columnWidth(1) >= ui::size::kMinValueColumnWidth;
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
    pruneProvenance(); // no entry may outlive the sketch it describes
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

    // M7: reconstruct design intent from the dimensions the file carried,
    // immediately and automatically (spec 27 step 2).
    //
    // Automatic rather than a separate command, because the alternative is a
    // user staring at an unconstrained import with no reason to suspect the
    // drawing's own dimensions could be applied. Re-running is refused
    // (ADR-M7-007), so importing the same file twice cannot accumulate
    // parameters.
    //
    // A reconstruction FAILURE is not an import failure: the geometry is
    // already in and usable. It is reported, not rolled back over.
    const ReconstructionResult reconstruction =
        ReconstructSketch(*document_, imported.sketchId, read.geometry.dimensions);
    if (reconstruction) reconstructionReports_[imported.sketchId] = reconstruction.report;

    // Solve FIRST, then extrude.
    //
    // Ordering is not incidental here. At import time the drawn corners still
    // miss by up to the coincidence tolerance -- that is the whole point of
    // reconstruction -- and BuildProfile uses the model's much tighter 1e-6 mm
    // connectivity tolerance, so the profile is still OPEN. Checking it before
    // the solve silently created no Pad at all, and the viewer went on showing
    // the previous solid. It took running the application to see that; the
    // profile check looked obviously correct.
    onRecomputeRequested();

    // EXTRUDE IT, so the user can actually see the thing they imported.
    //
    // Without this the shell imports a sketch, reconstructs its dimensions and
    // then goes on displaying whatever solid was already there -- so editing a
    // reconstructed Width re-solves the sketch correctly while the 3D view and
    // the volume readout describe something else entirely. Spec 27 steps 9 and
    // 10 did not happen for an imported file. No automated test could see it:
    // every test wires its own Pad to the imported sketch, which is exactly
    // what the application did not do.
    //
    // Only when the sketch actually yields a closed profile: an open or
    // multi-loop import is a legal state, and failing the import over it would
    // be worse than showing nothing.
    if (const Sketch* importedSketch = document_->findSketch(imported.sketchId)) {
        if (BuildProfile(*importedSketch)) {
            Body* body = document_->bodies().empty() ? &document_->addBody("Body001")
                                                     : document_->bodies().front().get();
            const Parameter* existing = document_->parameters().findByName("PadLength");
            const ObjectId lengthId =
                existing != nullptr
                    ? existing->id()
                    : document_->addParameter("PadLength", 20.0, UnitType::Millimeter).id();
            document_->addPadFeature(*body, name.toStdString() + "_Pad", imported.sketchId,
                                     lengthId);
        }
    }

    // The imported sketch is an ordinary document object, so the ordinary
    // refresh path shows it: recompute, then rebuild tree, panel and view.
    onRecomputeRequested();
    selectObject(imported.sketchId);

    QString message = QStringLiteral("Imported %1: %2")
                          .arg(QFileInfo(path).fileName(),
                               QString::fromStdString(imported.message));
    if (!reconstruction.createdParameters.empty())
        message += QStringLiteral(" [%1 dimension(s) reconstructed]")
                       .arg(reconstruction.createdParameters.size());
    if (reconstruction.skippedCount > 0)
        message += QStringLiteral(" [%1 not reconstructed]").arg(reconstruction.skippedCount);
    // A reconstruction that FAILED outright said nothing at all until round 2
    // found it: no report was stored, so the panel showed no Reconstruction
    // rows, and the status bar was word-for-word what a drawing carrying no
    // dimensions produces. The user could not tell "this drawing had no
    // dimensions" from "I could not use the dimensions it had" -- while the
    // sketch was still extruded and the volume readout still updated. Spec 18
    // requires the failure be reported; this is the report.
    if (!reconstruction)
        message += QStringLiteral(" [dimensions NOT reconstructed: %1]")
                       .arg(QString::fromStdString(reconstruction.message));
    // The IMPORT's own skip count is deliberately NOT appended here.
    //
    // Skipped entities must be visible -- a user who cannot see that something
    // was dropped will assume nothing was -- and they are: SketchImporter's
    // message already ends with "; skipped N", which `imported.message` carries
    // above. Appending "[N skipped]" printed the same number a second time, and
    // M7 made that conspicuous by adding a third bracketed clause between them.
    // Found by reading the status bar in the running application.
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
