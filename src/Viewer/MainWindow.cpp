#include "Viewer/MainWindow.h"

#include "Core/Document/PartDocument.h"
#include "Core/Physics/MassProperties.h"
#include "Viewer/DesignTokens.h"
#include "Viewer/DocumentOutline.h"
#include "Core/Feature/ISolidFeature.h"
#include "Kernel/Occt/OcctFaceQuery.h"
#include "Viewer/FaceSketch.h"
#include "Viewer/DocumentPresenter.h"
#include "Core/Import/SketchImporter.h"
#include "Viewer/PropertyEditing.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Body/Body.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Import/Dxf/DxfReader.h"
#include "Viewer/OcctViewWidget.h"
#include "Viewer/SketchCanvasWidget.h"
#include "Viewer/SketchCommands.h"
#include "Viewer/SketchIcons.h"
#include <QImage>
#include <QPixmap>

#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QFileDialog>
#include "Core/Serialization/PartDocumentSerializer.h"
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QStackedWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QPalette>
#include <QVariant>
#include <algorithm>
#include <functional>

namespace paramcad {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;
// Which editable aspect a property cell writes to (M11.3). Without it the
// commit handler sees only a parameter id and cannot tell a value from an
// expression.
constexpr int kFieldRole = Qt::UserRole + 2;

} // namespace

MainWindow::MainWindow(PartDocument& document, DocumentPresenter& presenter, QWidget* parent)
    : QMainWindow(parent), document_(&document), presenter_(&presenter) {
    setWindowTitle(QStringLiteral("EP3D - Parametric CAD"));
    setMinimumSize(ui::size::kMinWindowWidth, ui::size::kMinWindowHeight);

    // M12: the work area is a STACK of two dominant views -- the 3D viewer and
    // the 2D sketch canvas -- rather than one. Sketching is a MODE (todo 14's
    // "Mode concept"), and a mode that shares the viewport with the model is
    // how the 3D navigation gestures and the 2D drawing gestures end up
    // fighting over the same mouse buttons.
    centralStack_ = new QStackedWidget(this);
    viewer_ = new OcctViewWidget(this);
    viewer_->setPresenter(presenter_);
    sketchCanvas_ = new SketchCanvasWidget(this);
    centralStack_->addWidget(viewer_);
    centralStack_->addWidget(sketchCanvas_);
    centralStack_->setCurrentWidget(viewer_);
    setCentralWidget(centralStack_);

    connect(sketchCanvas_, &SketchCanvasWidget::documentChanged, this,
            &MainWindow::onSketchDocumentChanged);
    connect(sketchCanvas_, &SketchCanvasWidget::presentationChanged, this,
            &MainWindow::onSketchPresentationChanged);
    connect(sketchCanvas_, &SketchCanvasWidget::dimensionActivated, this,
            &MainWindow::onDimensionActivated);
    connect(sketchCanvas_, &SketchCanvasWidget::constraintPicked, this,
            &MainWindow::onConstraintPickedOnCanvas);

    buildMenus();
    buildToolbar();
    buildDocks();
    buildSketchUi();

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
    QAction* newAction = file->addAction(QStringLiteral("&New"));
    newAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    connect(newAction, &QAction::triggered, this, &MainWindow::onNewRequested);
    QAction* openAction = file->addAction(QStringLiteral("&Open..."));
    openAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenRequested);
    QAction* saveAction = file->addAction(QStringLiteral("&Save"));
    saveAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
    connect(saveAction, &QAction::triggered, this, &MainWindow::onSaveRequested);
    QAction* saveAsAction = file->addAction(QStringLiteral("Save &As..."));
    saveAsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::onSaveAsRequested);
    file->addSeparator();
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

    // --- How solids are drawn (M17.9, ADR-M17-032) --------------------------
    //
    // CHECKABLE and EXCLUSIVE, in a QActionGroup: the two are states of one
    // setting, not two commands, and a menu showing both ticked would be
    // describing something the viewer cannot be in. The tick is also the only
    // place the current mode is written down for the user -- wireframe and
    // shaded are obvious on a solid, but not on a document that has none yet.
    view->addSeparator();
    auto* displayGroup = new QActionGroup(this);
    displayGroup->setExclusive(true);
    solidShadedAction_ = view->addAction(QStringLiteral("&Solid"));
    solidShadedAction_->setCheckable(true);
    solidShadedAction_->setChecked(true);
    solidShadedAction_->setToolTip(QStringLiteral("Draw solids shaded"));
    displayGroup->addAction(solidShadedAction_);
    connect(solidShadedAction_, &QAction::triggered, this,
            [this]() { setSolidDisplayCommand(false); });

    solidWireframeAction_ = view->addAction(QStringLiteral("&Wireframe"));
    solidWireframeAction_->setCheckable(true);
    solidWireframeAction_->setToolTip(
        QStringLiteral("Draw solids as edges only.\nSketches are always drawn this way -- "
                       "they have no faces to shade."));
    displayGroup->addAction(solidWireframeAction_);
    connect(solidWireframeAction_, &QAction::triggered, this,
            [this]() { setSolidDisplayCommand(true); });
    view->addSeparator();

    QAction* toggleHidden = view->addAction(QStringLiteral("Show/&Hide Selected"));
    toggleHidden->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    connect(toggleHidden, &QAction::triggered, this, &MainWindow::onToggleHiddenRequested);

    // M9.5: the history commands. Until these existed, M9's undo machinery was
    // complete and UNREACHABLE from the running application -- the shape M8 was
    // caught in when three of its four required features had no sample.
    QMenu* edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    undoAction_ = edit->addAction(QStringLiteral("&Undo"));
    undoAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Z")));
    connect(undoAction_, &QAction::triggered, this, &MainWindow::onUndoRequested);
    deleteObjectAction_ = edit->addAction(QStringLiteral("&Delete Selected Object"));
    // Del, application-wide: the 3D view is a native child window and a
    // window-scoped shortcut does not reach it (see the note below).
    deleteObjectAction_->setShortcut(QKeySequence(QStringLiteral("Del")));
    connect(deleteObjectAction_, &QAction::triggered, this,
            &MainWindow::onDeleteObjectRequested);
    edit->addSeparator();
    redoAction_ = edit->addAction(QStringLiteral("&Redo"));
    redoAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Y")));
    connect(redoAction_, &QAction::triggered, this, &MainWindow::onRedoRequested);

    QMenu* model = menuBar()->addMenu(QStringLiteral("&Model"));
    QAction* recompute = model->addAction(QStringLiteral("&Recompute"));
    recompute->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    connect(recompute, &QAction::triggered, this, &MainWindow::onRecomputeRequested);

    model->addSeparator();
    suppressAction_ = model->addAction(QStringLiteral("&Suppress/Unsuppress Selected"));
    suppressAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+U")));
    connect(suppressAction_, &QAction::triggered, this, &MainWindow::onSuppressRequested);
    rollbackAction_ = model->addAction(QStringLiteral("Roll &Back to Selected"));
    connect(rollbackAction_, &QAction::triggered, this, &MainWindow::onRollbackRequested);
    rollForwardAction_ = model->addAction(QStringLiteral("Roll &Forward to End"));
    connect(rollForwardAction_, &QAction::triggered, this, &MainWindow::onRollForwardRequested);

    QMenu* insert = menuBar()->addMenu(QStringLiteral("&Insert"));
    insertPadAction_ = insert->addAction(QStringLiteral("&Pad from Selected Sketch"));
    connect(insertPadAction_, &QAction::triggered, this, &MainWindow::onInsertPadRequested);
    insertPocketAction_ = insert->addAction(QStringLiteral("P&ocket from Selected Sketch"));
    connect(insertPocketAction_, &QAction::triggered, this, &MainWindow::onInsertPocketRequested);
    insertRevolveAction_ = insert->addAction(QStringLiteral("&Revolve Selected Sketch"));
    connect(insertRevolveAction_, &QAction::triggered, this,
            &MainWindow::onInsertRevolveRequested);
    insert->addSeparator();
    insertFilletAction_ = insert->addAction(QStringLiteral("&Fillet on Current Solid"));
    connect(insertFilletAction_, &QAction::triggered, this, &MainWindow::onInsertFilletRequested);
    insertChamferAction_ = insert->addAction(QStringLiteral("&Chamfer on Current Solid"));
    connect(insertChamferAction_, &QAction::triggered, this,
            &MainWindow::onInsertChamferRequested);

    // ApplicationShortcut, not the WindowShortcut default. The 3D view is a
    // NATIVE child window (WA_NativeWindow is required for OCCT to own its own
    // surface), and while it holds focus the default context did not deliver
    // these keys -- so every shortcut the menus advertise silently did nothing
    // whenever the user had last clicked in the viewport, which is most of the
    // time in a CAD application. A menu that promises a shortcut it does not
    // honour is worse than one that promises nothing.
    for (QAction* action : {fit, toggleHidden, recompute, undoAction_, redoAction_,
                           suppressAction_, deleteObjectAction_})
        action->setShortcutContext(Qt::ApplicationShortcut);
    refreshCommandStates();
}

void MainWindow::buildToolbar() {
    // Small by design (UI spec 9): only commands M4 actually supports.
    QToolBar* bar = addToolBar(QStringLiteral("Main"));
    bar->setIconSize(QSize(ui::size::kToolbarIcon, ui::size::kToolbarIcon));
    // Icons BESIDE the text, not instead of it.
    //
    // The sketch bar is icon-only because it holds thirty-odd tools and the
    // labels would not fit; this one holds five commands the user already knows
    // by name, so dropping the words to save room nobody needs would trade
    // legibility for nothing.
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->setMovable(false);

    // Same rule as the sketch bar: baked from the ACTIVE palette, once, after
    // the theme has been applied.
    const QPalette iconPalette = palette();
    const auto icon = [&iconPalette](ui::SketchIcon which) {
        return ui::MakeSketchIcon(which, iconPalette);
    };

    // Undo and Redo FIRST, and on the toolbar at all.
    //
    // They were menu-and-shortcut only. Ctrl+Z is not the problem -- everyone
    // knows it -- but a CAD user reaches for the toolbar to find out whether
    // there is anything TO undo, and a greyed-out button answers that at a
    // glance where a menu they have to open does not. The actions are the same
    // ones the Edit menu holds, so enabling stays in one place
    // (refreshCommandStates).
    bar->addAction(undoAction_);
    undoAction_->setIcon(icon(ui::SketchIcon::Undo));
    undoAction_->setToolTip(QStringLiteral("Undo the last change (Ctrl+Z)"));
    bar->addAction(redoAction_);
    redoAction_->setIcon(icon(ui::SketchIcon::Redo));
    redoAction_->setToolTip(QStringLiteral("Redo the change that was undone (Ctrl+Y)"));
    bar->addSeparator();

    QAction* fit = bar->addAction(icon(ui::SketchIcon::FitSketch), QStringLiteral("Fit All"));
    fit->setToolTip(QStringLiteral("Fit the whole model in the view (Ctrl+Shift+F)"));
    connect(fit, &QAction::triggered, this, &MainWindow::onFitAllRequested);

    QAction* hide = bar->addAction(icon(ui::SketchIcon::Visibility), QStringLiteral("Show/Hide"));
    hide->setToolTip(QStringLiteral("Show or hide the selected solid (Ctrl+H)"));
    connect(hide, &QAction::triggered, this, &MainWindow::onToggleHiddenRequested);

    QAction* recompute = bar->addAction(icon(ui::SketchIcon::Recompute), QStringLiteral("Recompute"));
    recompute->setToolTip(QStringLiteral("Recompute the document (Ctrl+R)"));
    connect(recompute, &QAction::triggered, this, &MainWindow::onRecomputeRequested);

    mainToolBar_ = bar;

    // --- The MODEL toolbar --------------------------------------------------
    //
    // Pad, Pocket and Revolve existed only under Insert, and a command a user
    // has to go looking for in a menu is one they do not know is there. Fillet
    // and Chamfer join them because they are the same kind of act -- something
    // done TO the solid -- and splitting them across two surfaces would be a
    // distinction the user has to learn for no benefit.
    //
    // The actions are the SAME objects the menu holds, so enabling stays in one
    // place (refreshCommandStates) and the two surfaces cannot disagree about
    // whether a command is available.
    addToolBarBreak();
    QToolBar* model = addToolBar(QStringLiteral("Model"));
    model->setIconSize(QSize(ui::size::kToolbarIcon, ui::size::kToolbarIcon));
    model->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    model->setMovable(false);

    insertPadAction_->setIcon(icon(ui::SketchIcon::Pad));
    // SHORT on the bar, descriptive in the menu. Qt shows iconText() beside a
    // toolbar icon, so "Pad from Selected Sketch" can stay where there is room
    // for it without turning the toolbar into a sentence.
    insertPadAction_->setIconText(QStringLiteral("Pad"));
    insertPadAction_->setToolTip(
        QStringLiteral("Pad\nExtrude the selected sketch into a solid.\n"
                       "The sketch needs one closed outer loop; any loops inside it become "
                       "holes."));
    model->addAction(insertPadAction_);

    insertPocketAction_->setIcon(icon(ui::SketchIcon::Pocket));
    insertPocketAction_->setIconText(QStringLiteral("Pocket"));
    insertPocketAction_->setToolTip(
        QStringLiteral("Pocket\nCut the selected sketch's profile into the current solid.\n"
                       "This is how a HOLE is made: draw a circle and pocket it."));
    model->addAction(insertPocketAction_);

    insertRevolveAction_->setIcon(icon(ui::SketchIcon::Revolve));
    insertRevolveAction_->setIconText(QStringLiteral("Revolve"));
    insertRevolveAction_->setToolTip(
        QStringLiteral("Revolve\nSpin the selected sketch about one of its lines.\n"
                       "Mark the axis Construction (Q) and it is chosen for you."));
    model->addAction(insertRevolveAction_);

    model->addSeparator();
    insertFilletAction_->setIcon(icon(ui::SketchIcon::Fillet));
    insertFilletAction_->setIconText(QStringLiteral("Fillet"));
    insertFilletAction_->setToolTip(QStringLiteral("Fillet\nRound every edge of the current solid"));
    model->addAction(insertFilletAction_);
    insertChamferAction_->setIcon(icon(ui::SketchIcon::Chamfer));
    insertChamferAction_->setIconText(QStringLiteral("Chamfer"));
    insertChamferAction_->setToolTip(
        QStringLiteral("Chamfer\nCut every edge of the current solid"));
    model->addAction(insertChamferAction_);
    modelToolBar_ = model;
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

        // A REFUSED edit keeps the text the user typed, not the stored value.
        // Losing a typo means retyping the whole expression to fix one
        // character, which defeats the point of a positioned error message.
        const bool rejectedHere = row.editable && row.parameterId == rejectedEditParameter_ &&
                                  row.field == rejectedEditField_ &&
                                  rejectedEditParameter_ != kInvalidObjectId;
        const std::string shownText = rejectedHere ? rejectedEditText_ : row.value;

        auto* value = new QTableWidgetItem(QString::fromStdString(shownText));
        value->setFont(ui::numericFont(properties_->font()));
        // Expressions read left to right and are not numbers; right-aligning
        // one puts its start off the left edge of a narrow column.
        value->setTextAlignment(row.field == PropertyField::Expression
                                    ? (Qt::AlignLeft | Qt::AlignVCenter)
                                    : (Qt::AlignRight | Qt::AlignVCenter));
        if (row.editable && row.field == PropertyField::Reversed) {
            // A CHECKBOX, not a word to type. "Reversed" is a yes/no fact, and
            // making the user type "yes" into a cell would be a worse control
            // than the one Qt already has -- and one more thing to spell.
            //
            // The text stays alongside it: a bare tick reads as nothing in a
            // screenshot, and no fact here travels on one channel (A06).
            value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            value->setCheckState(row.numericValue < 0.0 ? Qt::Checked : Qt::Unchecked);
            value->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            value->setData(kIdRole, QVariant::fromValue<qulonglong>(row.parameterId));
            value->setData(kFieldRole, static_cast<int>(row.field));
        } else if (row.editable) {
            value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            value->setData(kIdRole, QVariant::fromValue<qulonglong>(row.parameterId));
            value->setData(kFieldRole, static_cast<int>(row.field));
            QString tip;
            if (rejectedHere && !rejectedEditDetail_.empty()) {
                // The caret rendering, which exists nowhere else: three lines
                // showing what was typed, what is at fault, and why.
                tip = QString::fromStdString(rejectedEditDetail_);
            } else if (row.field == PropertyField::Expression) {
                tip = QStringLiteral(
                    "Expression - type one and press Enter; leave it empty for a plain "
                    "value.\nExamples:  #Width / 2    #Base + 20 mm    max(#A, #B)");
                if (!row.value.empty())
                    tip = QString::fromStdString(row.value) + QStringLiteral("\n") + tip;
            } else {
                tip = QString::fromStdString(row.value) +
                      QStringLiteral("\nEditable - type a value and press Enter");
            }
            value->setToolTip(tip);
        } else {
            value->setFlags(Qt::ItemIsEnabled);
            // Read-only rows use the palette's own disabled colour, so they stay
            // distinguishable AND legible under any theme (UI spec 14/19).
            value->setForeground(
                properties_->palette().color(QPalette::Disabled, QPalette::Text));
            // The full text, because the cell elides. A diagnostic the user can
            // only read half of is a diagnostic that has not been delivered.
            //
            // A value row that is read-only BECAUSE an expression drives it
            // says so, and names the expression: a number whose source is
            // invisible is a number the user cannot check.
            if (row.label == "Value" && row.parameterId != kInvalidObjectId) {
                const Parameter* driver = document_->parameters().findById(row.parameterId);
                value->setToolTip(
                    QString::fromStdString(row.value) + QStringLiteral("\n") +
                    QString::fromStdString(
                        DescribeValueSource(driver != nullptr ? driver->expression()
                                                             : std::string{})));
            } else {
                value->setToolTip(QString::fromStdString(row.value));
            }
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

void MainWindow::showScriptPort(quint16 port) {
    // THE TITLE BAR, because it survives everything else this window does:
    // the status bar is rewritten by the next command, and a message box has
    // to be dismissed and is then gone. A socket that can create, modify and
    // save files should be visible for as long as it is open.
    setWindowTitle(windowTitle() +
                   QStringLiteral("  [script socket 127.0.0.1:%1]").arg(port));
    statusBar()->showMessage(
        QStringLiteral("Listening for scripts on 127.0.0.1:%1 -- loopback only").arg(port),
        8000);
}

void MainWindow::refreshAll() {
    // Provenance is pruned HERE, not only in onRecomputeRequested (round 4,
    // R3R4-M2). It was unreachable from any public entry point, so no test
    // could see it and a reviewer replaced the whole body with `return;`
    // without failing anything -- a Major closed by a fix nothing could
    // observe. Every path that rebuilds the shell from the document now prunes,
    // which is also the more honest rule: an entry describing a sketch the
    // document no longer has is stale the moment the document changes, not the
    // moment someone recomputes.
    pruneProvenance();
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
    refreshCommandStates();
    // M12: the sketch canvas and the constraint panel are part of "the shell
    // rebuilt from the document" too.
    //
    // Leaving them out was a real defect, and one no data-level test could see:
    // Ctrl+Z in sketch mode undid the geometry in the document and the canvas
    // went on drawing it, because Undo routes through onRecomputeRequested ->
    // refreshAll and never told the canvas anything. Every path that rebuilds
    // the shell now rebuilds these two as well.
    if (inSketchMode()) {
        rebuildConstraintPanel();
        if (sketchCanvas_ != nullptr) sketchCanvas_->update();
        updateSketchStatus();
    }
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

    // EVERY selection change re-arms the commands, and this is the one place
    // every selection change goes through.
    //
    // Without it the enabled state only ever caught up when some OTHER command
    // happened to refresh it. Finish Sketch does, which is why the usual route
    // to a sketch worked -- but clicking a sketch in the TREE did not, so after
    // File > Open (which selects nothing) "Edit Selected Sketch" stayed greyed
    // out no matter what the user clicked. Reported against a document whose
    // only content WAS a sketch, where the tree was the only way in.
    refreshCommandStates();

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
    const auto field = static_cast<PropertyField>(item->data(kFieldRole).toInt());

    // A checkbox row carries its answer in the CHECK STATE, not the text. The
    // text beside it is a label, and reading that instead would hand
    // ApplyPropertyEdit the word the row displayed a moment ago rather than
    // what the user just clicked.
    const std::string typed = field == PropertyField::Reversed
                                  ? (item->checkState() == Qt::Checked ? "1" : "0")
                                  : item->text().toStdString();

    // EVERY decision is in ApplyPropertyEdit, which is Qt-free and unit tested.
    // What is left here is what only a widget can do: show the result. That
    // split is the M6.14 lesson applied on purpose rather than after the fact.
    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(*document_, parameterId, field, typed);

    // Immediate feedback, no modal dialog (UI spec 7).
    statusLeft_->setText(QString::fromStdString(outcome.status));

    if (outcome.applied) {
        rejectedEditParameter_ = kInvalidObjectId;
        rejectedEditField_ = PropertyField::None;
        rejectedEditText_.clear();
        rejectedEditDetail_.clear();
        onRecomputeRequested();
        return;
    }

    // Refused: keep what was typed, and carry the caret rendering into the
    // rebuilt row's tooltip. The document is unchanged.
    rejectedEditParameter_ = parameterId;
    rejectedEditField_ = field;
    rejectedEditText_ = outcome.rejectedText;
    rejectedEditDetail_ = outcome.detail;
    updatingWidgets_ = true;
    rebuildProperties();
    updatingWidgets_ = false;
}

bool MainWindow::typeIntoPropertyRow(const std::string& label, const std::string& text) {
    if (properties_ == nullptr) return false;
    for (int row = 0; row < properties_->rowCount(); ++row) {
        const QTableWidgetItem* key = properties_->item(row, 0);
        QTableWidgetItem* value = properties_->item(row, 1);
        if (key == nullptr || value == nullptr) continue;
        if (!key->text().endsWith(QString::fromStdString(label))) continue;
        // The WIDGET's own flag, not the row's `editable` field: this exists to
        // catch the case where the model says editable and the cell is not.
        if ((value->flags() & Qt::ItemIsEditable) == 0) return false;
        value->setText(QString::fromStdString(text));
        return true; // setText fires cellChanged -> onPropertyCommitted
    }
    return false;
}

std::string MainWindow::displayedPropertyTooltip(const std::string& label) const {
    for (int row = 0; row < properties_->rowCount(); ++row) {
        const QTableWidgetItem* name = properties_->item(row, 0);
        const QTableWidgetItem* value = properties_->item(row, 1);
        if (name == nullptr || value == nullptr) continue;
        const QString text = name->text();
        const int slash = text.lastIndexOf(QStringLiteral(" / "));
        const QString bare = slash < 0 ? text : text.mid(slash + 3);
        if (bare == QString::fromStdString(label)) return value->toolTip().toStdString();
    }
    return {};
}

bool MainWindow::hasPropertyRow(const std::string& label) const {
    for (int row = 0; row < properties_->rowCount(); ++row) {
        const QTableWidgetItem* name = properties_->item(row, 0);
        if (name == nullptr) continue;
        const QString text = name->text();
        const int slash = text.lastIndexOf(QStringLiteral(" / "));
        const QString bare = slash < 0 ? text : text.mid(slash + 3);
        if (bare == QString::fromStdString(label)) return true;
    }
    return false;
}

QString MainWindow::editPropertyByLabel(const std::string& label, const QString& text) {
    for (int row = 0; row < properties_->rowCount(); ++row) {
        const QTableWidgetItem* name = properties_->item(row, 0);
        QTableWidgetItem* value = properties_->item(row, 1);
        if (name == nullptr || value == nullptr) continue;
        const QString labelText = name->text();
        const int slash = labelText.lastIndexOf(QStringLiteral(" / "));
        const QString bare = slash < 0 ? labelText : labelText.mid(slash + 3);
        if (bare != QString::fromStdString(label)) continue;
        if ((value->flags() & Qt::ItemIsEditable) == 0)
            return QStringLiteral("the %1 row is not editable")
                .arg(QString::fromStdString(label));
        // setText emits cellChanged, which is the SAME signal typing emits --
        // so this drives the real commit path rather than a parallel one.
        value->setText(text);
        return statusLeft_->text();
    }
    return QStringLiteral("no property row is labelled %1").arg(QString::fromStdString(label));
}

ObjectId MainWindow::selectedFeatureBody(std::size_t* indexOut) const {
    if (indexOut != nullptr) *indexOut = static_cast<std::size_t>(-1);
    if (document_ == nullptr || selectedId_ == kInvalidObjectId) return kInvalidObjectId;
    for (const auto& body : document_->bodies())
        for (std::size_t i = 0; i < body->features().size(); ++i)
            if (body->features()[i]->id() == selectedId_) {
                if (indexOut != nullptr) *indexOut = i;
                return body->id();
            }
    return kInvalidObjectId;
}

void MainWindow::refreshCommandStates() {
    if (document_ == nullptr) return;
    // Enabled FROM THE MODEL. An Undo item that is always enabled and silently
    // does nothing is a lie the user can click, and this project has shipped
    // that shape before in other clothes.
    if (undoAction_ != nullptr) {
        const std::string label = document_->nextUndoLabel();
        undoAction_->setEnabled(document_->undoDepth() > 0);
        undoAction_->setText(label.empty()
                                 ? QStringLiteral("&Undo")
                                 : QStringLiteral("&Undo %1").arg(QString::fromStdString(label)));
    }
    if (redoAction_ != nullptr) {
        const std::string label = document_->nextRedoLabel();
        redoAction_->setEnabled(document_->redoDepth() > 0);
        redoAction_->setText(label.empty()
                                 ? QStringLiteral("&Redo")
                                 : QStringLiteral("&Redo %1").arg(QString::fromStdString(label)));
    }
    const ObjectId body = selectedFeatureBody();
    if (suppressAction_ != nullptr) suppressAction_->setEnabled(body != kInvalidObjectId);
    if (rollbackAction_ != nullptr) rollbackAction_->setEnabled(body != kInvalidObjectId);
    if (rollForwardAction_ != nullptr)
        rollForwardAction_->setEnabled(!document_->bodies().empty());

    // DISABLED IN SKETCH MODE, and that is what frees the Del key.
    //
    // This action is ApplicationShortcut so it reaches the native 3D view, and
    // an application-scoped shortcut outranks the canvas's own widget-scoped
    // Del -- so while a sketch was open, Del deleted a document OBJECT instead
    // of the selected geometry. A disabled QAction does not consume its
    // shortcut, so gating it here hands Del back to the sketch, where it
    // belongs while one is open.
    if (deleteObjectAction_ != nullptr)
        deleteObjectAction_->setEnabled(!inSketchMode() && selectedId_ != kInvalidObjectId);
    // Enabled by what was PICKED, not by what is selected in the tree. The
    // viewer records the face under the last click; anything else -- a curved
    // face, empty space, a tree selection -- leaves the button off, and the
    // command still explains itself if it is reached by the menu.
    if (sketchOnFaceAction_ != nullptr) {
        const PickedFace& face = viewer_ != nullptr ? viewer_->pickedFace() : PickedFace{};
        sketchOnFaceAction_->setEnabled(!inSketchMode() && face.isFace && face.planar);
    }
    const bool haveSketch = selectedSketch() != kInvalidObjectId;
    const bool haveTail = currentTail() != kInvalidObjectId;
    if (insertPadAction_ != nullptr) insertPadAction_->setEnabled(haveSketch);
    // A pocket needs BOTH: a profile to cut with and a solid to cut into.
    if (insertPocketAction_ != nullptr) insertPocketAction_->setEnabled(haveSketch && haveTail);
    if (insertFilletAction_ != nullptr) insertFilletAction_->setEnabled(haveTail);
    if (insertChamferAction_ != nullptr) insertChamferAction_->setEnabled(haveTail);

    // M12: sketch commands are enabled FROM THE MODEL too. A "Horizontal"
    // button that is clickable with no sketch open is the same lie as an
    // always-enabled Undo.
    const bool sketching = inSketchMode();
    if (editSketchAction_ != nullptr) editSketchAction_->setEnabled(haveSketch);
    if (finishSketchAction_ != nullptr) finishSketchAction_->setEnabled(sketching);
    for (QAction* action : sketchModeActions_)
        if (action != nullptr) action->setEnabled(sketching);
}

ObjectId MainWindow::selectedSketch() const {
    if (document_ == nullptr) return kInvalidObjectId;
    for (const Sketch* sketch : document_->sketches())
        if (sketch->id() == selectedId_) return sketch->id();
    return kInvalidObjectId;
}

ObjectId MainWindow::currentTail() const {
    // The presenter already computes exactly this -- the ACTIVE, unconsumed
    // solid -- and asking it twice would be two places to keep in step.
    if (presenter_ == nullptr) return kInvalidObjectId;
    const std::vector<ObjectId> solids = presenter_->displayableSolids();
    return solids.empty() ? kInvalidObjectId : solids.back();
}

QString MainWindow::describeCreatedFeature(const char* what, ObjectId featureId,
                                           const DocumentRecomputeReport& report) const {
    // The feature's OWN diagnostic, by id. A document-level "something failed"
    // would send the user looking through the tree for which thing.
    for (const RecomputeItemReport& item : report.items) {
        if (item.id != featureId) continue;
        if (item.status == RecomputeStatus::Success) break;
        return QStringLiteral("%1 could not be built: %2")
            .arg(QString::fromLatin1(what))
            .arg(QString::fromStdString(item.message));
    }
    return QStringLiteral("%1 created").arg(QString::fromLatin1(what));
}

// A name no other row in the tree already has (M17.15).
//
// Every feature command used to hand its type name straight over: two pockets
// were both called "Pocket", three fillets all "Fillet". The tree is how a
// user picks what to delete, dimension or roll back to, and two rows reading
// the same thing make that a guess.
//
// It stopped being cosmetic the moment the owner deleted one of two identical
// "Pocket" rows and lost their undo history: the middle link of a chain cannot
// be removed reversibly, the tail can, and nothing on screen told them apart.
//
// Numbered from 1 and skipping what is taken, so deleting Pocket2 and adding
// another gives Pocket2 back rather than climbing forever.
std::string MainWindow::uniqueObjectName(const std::string& base) const {
    std::set<std::string> taken;
    // PARAMETERS TOO, and this half is not cosmetic: an expression names a
    // parameter by NAME (`#PocketDepth`), and findByName answers with the
    // first match. Two parameters called PocketDepth means every expression
    // mentioning it binds to whichever was created first -- and the panel
    // shows two identical rows, so the user cannot see which one they are
    // editing either.
    for (const auto& parameter : document_->parameters().items())
        taken.insert(parameter->name());
    for (const Sketch* sketch : document_->sketches()) taken.insert(sketch->name());
    for (const auto& body : document_->bodies())
        for (const auto& feature : body->features()) taken.insert(feature->name());

    for (int suffix = 1; suffix < 10000; ++suffix) {
        std::string candidate = base + std::to_string(suffix);
        if (taken.count(candidate) == 0) return candidate;
    }
    // Unreachable for any document a person made. Returning the bare base is
    // still a usable name; it is only the uniqueness that is given up.
    return base;
}

QString MainWindow::insertPadFromSelection() {
    const ObjectId sketch = selectedSketch();
    if (sketch == kInvalidObjectId) {
        const QString message = QStringLiteral("Select a sketch to pad");
        statusLeft_->setText(message);
        return message;
    }
    if (document_->bodies().empty()) document_->addBody("Body001");
    Body& body = *document_->bodies().front();
    document_->beginTransaction("Insert Pad");
    Parameter& length =
        document_->addParameter(uniqueObjectName("PadLength"), 20.0, UnitType::Millimeter);
    PadFeature& pad = document_->addPadFeature(body, uniqueObjectName("Pad"), sketch, length.id());
    document_->commitTransaction();
    const ObjectId created = pad.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Pad", created, report);
    if (message.startsWith(QStringLiteral("Pad created")))
        message += QStringLiteral("; edit its Length in the panel");
    sketchMessage_.clear();
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::insertPocketFromSelection() {
    const ObjectId sketch = selectedSketch();
    const ObjectId base = currentTail();
    if (sketch == kInvalidObjectId || base == kInvalidObjectId) {
        const QString message = QStringLiteral("Select a sketch, with a solid to cut into");
        statusLeft_->setText(message);
        return message;
    }
    Body& body = *document_->bodies().front();
    // Measured BEFORE the pocket exists, so the comparison below is against the
    // solid the user was actually looking at.
    const double volumeBefore = document_->massProperties().volumeMm3;
    document_->beginTransaction("Insert Pocket");
    Parameter& depth =
        document_->addParameter(uniqueObjectName("PocketDepth"), 10.0, UnitType::Millimeter);
    PocketFeature& pocket = document_->addPocketFeature(body, uniqueObjectName("Pocket"), base, sketch, depth.id());
    document_->commitTransaction();
    const ObjectId created = pocket.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Pocket", created, report);
    if (message.startsWith(QStringLiteral("Pocket created"))) {
        message += QStringLiteral("; edit its Depth in the panel");
        // A CUT THAT REMOVED NOTHING, said out loud (M17.8, ADR-M17-031).
        //
        // Reported by the owner: a pocket from a sketch on a side face "did not
        // work". It did not fail either -- the boolean succeeded, the result
        // was the base unchanged, and the feature was Valid. A command that
        // reports success and changes nothing leaves the user with no message
        // to read and nothing to act on.
        //
        // Said HERE and not in PocketFeature, because a cut that removes
        // nothing is LEGAL in the model (ADR-M8-002, and M8's release gate
        // pins a pocket whose tool lands inside the hole of an annulus).
        // Failing it in Core would overturn a deliberate decision to make one
        // common mistake legible. The shell is where "what just happened"
        // belongs, and it can say so without changing what is allowed.
        const double volumeAfter = document_->massProperties().volumeMm3;
        if (volumeBefore > 0.0 && volumeAfter >= volumeBefore * (1.0 - 1e-9)) {
            message = QStringLiteral(
                "Pocket created, but it removed no material -- its tool is entirely outside "
                "the solid. A sketch made ON A FACE has its normal pointing out of the part, "
                "so type a NEGATIVE Depth to cut inward.");
        }
    }
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

// The edge selection the CURRENT PICK implies, and what to tell the user.
//
// The face picked in the 3D view is turned into a QUERY here, at the moment
// the command runs -- not remembered as a face. SelectionForPickedFace makes
// every judgement (is it flat, is it the outermost one in its direction) and
// this only has to find the faces to judge against.
//
// With nothing picked the answer is every edge, which is what Fillet and
// Chamfer have always done and what every existing file says.
MainWindow::DressSelection MainWindow::selectionForDress(ObjectId baseFeatureId) const {
    DressSelection chosen;
    chosen.selection = AllEdgesSelection();
    chosen.words = DescribeEdgeSelection(chosen.selection);

    if (viewer_ == nullptr || !viewer_->pickedFace().isFace) return chosen;

    // Every face of the solid about to be dressed -- the base's own shape,
    // which is what the query will be answered against.
    const ISolidFeature* solid = nullptr;
    for (const auto& body : document_->bodies())
        for (const auto& feature : body->features())
            if (feature->id() == baseFeatureId)
                solid = dynamic_cast<const ISolidFeature*>(feature.get());
    if (solid == nullptr || !solid->currentShape().isValid()) return chosen;

    const EdgeSelectionPick pick =
        SelectionForPickedFace(viewer_->pickedFace(), FacesOf(solid->currentShape()));
    if (!pick.ok) {
        // The pick is REFUSED, not silently ignored: a user who clicked a face
        // and got every edge rounded would reasonably conclude the click did
        // not register.
        chosen.refusal = QString::fromStdString(pick.message);
        return chosen;
    }
    chosen.selection = pick.selection;
    chosen.words = pick.message;
    return chosen;
}

QString MainWindow::insertFilletOnTail() {
    const ObjectId base = currentTail();
    if (base == kInvalidObjectId) {
        const QString message = QStringLiteral("No solid to fillet");
        statusLeft_->setText(message);
        return message;
    }
    // WHICH EDGES, decided before anything is created, so a refused pick costs
    // the user nothing to undo.
    const DressSelection chosen = selectionForDress(base);
    if (!chosen.refusal.isEmpty()) {
        statusLeft_->setText(chosen.refusal);
        return chosen.refusal;
    }
    Body& body = *document_->bodies().front();
    document_->beginTransaction("Insert Fillet");
    Parameter& radius =
        document_->addParameter(uniqueObjectName("FilletRadius"), 2.0, UnitType::Millimeter);
    FilletFeature& fillet = document_->addFilletFeature(body, uniqueObjectName("Fillet"), base, radius.id());
    document_->setFeatureEdgeSelection(fillet.id(), chosen.selection);
    document_->commitTransaction();
    const ObjectId created = fillet.id();
    // The FEATURE's own diagnostic (ADR-M17-022), which Fillet and Chamfer were
    // still missing after Pad, Pocket and Revolve got it. A radius too large
    // for the geometry fails in the kernel, and this command answered "Fillet
    // created" regardless -- a success message over a solid that did not
    // change, which is the exact defect that ADR fixed for the others.
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Fillet", created, report);
    if (message.startsWith(QStringLiteral("Fillet created")))
        message += QStringLiteral(" on %1 -- edit its Radius in the panel")
                       .arg(QString::fromStdString(chosen.words));
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::insertChamferOnTail() {
    const ObjectId base = currentTail();
    if (base == kInvalidObjectId) {
        const QString message = QStringLiteral("No solid to chamfer");
        statusLeft_->setText(message);
        return message;
    }
    // WHICH EDGES, decided before anything is created, so a refused pick costs
    // the user nothing to undo.
    const DressSelection chosen = selectionForDress(base);
    if (!chosen.refusal.isEmpty()) {
        statusLeft_->setText(chosen.refusal);
        return chosen.refusal;
    }
    Body& body = *document_->bodies().front();
    document_->beginTransaction("Insert Chamfer");
    Parameter& distance =
        document_->addParameter(uniqueObjectName("ChamferDistance"), 2.0,
                                UnitType::Millimeter);
    ChamferFeature& chamfer =
        document_->addChamferFeature(body, uniqueObjectName("Chamfer"), base, distance.id());
    document_->setFeatureEdgeSelection(chamfer.id(), chosen.selection);
    document_->commitTransaction();
    const ObjectId created = chamfer.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Chamfer", created, report);
    if (message.startsWith(QStringLiteral("Chamfer created")))
        message += QStringLiteral(" on %1 -- edit its Distance in the panel")
                       .arg(QString::fromStdString(chosen.words));
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

void MainWindow::onInsertPadRequested() { insertPadFromSelection(); }
void MainWindow::onInsertPocketRequested() { insertPocketFromSelection(); }

void MainWindow::onInsertRevolveRequested() {
    const ObjectId sketchId = selectedSketch();
    if (sketchId == kInvalidObjectId) {
        statusLeft_->setText(QStringLiteral("Select a sketch to revolve"));
        return;
    }
    const Sketch* sketch = document_->findSketch(sketchId);
    if (sketch == nullptr) return;

    // THE AXIS. A single construction line is the draughtsman's centreline and
    // needs no question asked. Anything else is a real choice -- a profile may
    // legitimately be revolved about one of its OWN edges (a rectangle about
    // its side is a cylinder) -- so the user is asked rather than guessed at.
    SketchEntityId axis = obviousRevolveAxis(sketchId);
    if (axis == kInvalidSketchEntityId) {
        QStringList labels;
        std::vector<SketchEntityId> ids;
        for (const SketchEntity& entity : sketch->entities()) {
            const auto* line = std::get_if<SketchLine>(&entity.geometry);
            if (line == nullptr) continue;
            ids.push_back(entity.id);
            labels << QStringLiteral("Line #%1  (%2, %3) - (%4, %5)%6")
                          .arg(static_cast<qulonglong>(ToObjectId(entity.id)))
                          .arg(line->start.x)
                          .arg(line->start.y)
                          .arg(line->end.x)
                          .arg(line->end.y)
                          .arg(entity.construction ? QStringLiteral("  [construction]")
                                                   : QString());
        }
        if (ids.empty()) {
            statusLeft_->setText(
                QStringLiteral("That sketch has no line to revolve about. Draw the axis and "
                               "mark it Construction (Q)."));
            return;
        }
        bool accepted = false;
        const QString chosen = QInputDialog::getItem(
            this, QStringLiteral("Revolve"),
            QStringLiteral("Which line is the axis?\n"
                           "Tip: mark the axis Construction (Q) and it is chosen for you."),
            labels, 0, false, &accepted);
        if (!accepted) return;
        const int index = labels.indexOf(chosen);
        if (index < 0) return;
        axis = ids[static_cast<std::size_t>(index)];
    }

    bool accepted = false;
    // DEGREES IN. The parameter is stored in radians because that is what the
    // solver and the kernel read (roadmap 7), and the conversion happens once,
    // here, where the number enters.
    const double degrees = QInputDialog::getDouble(
        this, QStringLiteral("Revolve"), QStringLiteral("Angle in degrees."), 360.0, 0.001,
        360.0, 3, &accepted);
    if (!accepted) return;
    insertRevolveFromSelection(axis, degrees);
}

SketchEntityId MainWindow::obviousRevolveAxis(ObjectId sketchId) const {
    const Sketch* sketch = document_ != nullptr ? document_->findSketch(sketchId) : nullptr;
    if (sketch == nullptr) return kInvalidSketchEntityId;
    SketchEntityId found = kInvalidSketchEntityId;
    for (const SketchEntity& entity : sketch->entities()) {
        if (!entity.construction) continue;
        if (!std::holds_alternative<SketchLine>(entity.geometry)) continue;
        // EXACTLY one. Two construction lines is not an obvious answer, and
        // picking the first would be a guess wearing a convention's clothes.
        if (found != kInvalidSketchEntityId) return kInvalidSketchEntityId;
        found = entity.id;
    }
    return found;
}

QString MainWindow::insertRevolveFromSelection(SketchEntityId axisEntityId,
                                               double angleDegrees) {
    const ObjectId sketchId = selectedSketch();
    if (sketchId == kInvalidObjectId) {
        const QString message = QStringLiteral("Select a sketch to revolve");
        statusLeft_->setText(message);
        return message;
    }
    const Sketch* sketch = document_->findSketch(sketchId);
    if (sketch == nullptr || sketch->findEntity(axisEntityId) == nullptr) {
        const QString message = QStringLiteral("That axis is not in the selected sketch");
        statusLeft_->setText(message);
        return message;
    }

    if (document_->bodies().empty()) document_->addBody("Body001");
    Body& body = *document_->bodies().front();
    document_->beginTransaction("Insert Revolve");
    Parameter& angle = document_->addParameter(uniqueObjectName("RevolveAngle"),
                                               angleDegrees * 3.14159265358979323846 / 180.0,
                                               UnitType::Radian);
    RevolveFeature& revolve =
        document_->addRevolveFeature(body, uniqueObjectName("Revolve"), sketchId, axisEntityId,
                                     angle.id());
    document_->commitTransaction();
    const ObjectId created = revolve.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    // SAYS WHAT IT STORED, and whether it worked. The panel shows the parameter
    // in radians, so a user who typed 360 and then reads 6.2832 should have
    // been told why.
    QString message = describeCreatedFeature("Revolve", created, report);
    if (message.startsWith(QStringLiteral("Revolve created")))
        message = QStringLiteral(
                      "Revolve created at %1 deg; its RevolveAngle is in radians in the panel")
                      .arg(angleDegrees);
    sketchMessage_.clear();
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}
void MainWindow::onInsertFilletRequested() { insertFilletOnTail(); }
void MainWindow::onInsertChamferRequested() { insertChamferOnTail(); }

QString MainWindow::undoCommand() {
    if (!document_->undo()) {
        const QString message = QStringLiteral("Nothing to undo");
        statusLeft_->setText(message);
        return message;
    }
    onRecomputeRequested();
    const QString message = QStringLiteral("Undone");
    reportSketchOrPlainStatus(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::redoCommand() {
    if (!document_->redo()) {
        const QString message = QStringLiteral("Nothing to redo");
        statusLeft_->setText(message);
        return message;
    }
    onRecomputeRequested();
    const QString message = QStringLiteral("Redone");
    reportSketchOrPlainStatus(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::toggleSuppressSelected() {
    std::size_t index = 0;
    if (selectedFeatureBody(&index) == kInvalidObjectId) {
        const QString message = QStringLiteral("Select a feature to suppress");
        statusLeft_->setText(message);
        return message;
    }
    const bool suppressed = !document_->isFeatureActive(selectedId_);
    document_->setSuppressed(selectedId_, !suppressed);
    const ObjectId keep = selectedId_;
    onRecomputeRequested();
    selectObject(keep);
    const QString message = suppressed ? QStringLiteral("Feature unsuppressed")
                                       : QStringLiteral("Feature suppressed");
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::rollbackToSelected() {
    std::size_t index = 0;
    const ObjectId body = selectedFeatureBody(&index);
    if (body == kInvalidObjectId) {
        const QString message = QStringLiteral("Select a feature to roll back to");
        statusLeft_->setText(message);
        return message;
    }
    // "To selected" means the selected feature is the LAST one evaluated, which
    // is what a user means by "show me the model at this step".
    document_->setRollbackPosition(body, index + 1);
    const ObjectId keep = selectedId_;
    onRecomputeRequested();
    selectObject(keep);
    const QString message =
        QStringLiteral("Rolled back to step %1").arg(static_cast<int>(index + 1));
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::rollForwardToEnd() {
    if (document_->bodies().empty()) {
        const QString message = QStringLiteral("Nothing to roll forward");
        statusLeft_->setText(message);
        return message;
    }
    for (const auto& body : document_->bodies())
        document_->setRollbackPosition(body->id(), Body::kNoRollback);
    const ObjectId keep = selectedId_;
    onRecomputeRequested();
    selectObject(keep);
    const QString message = QStringLiteral("Rolled forward to the end");
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

void MainWindow::onUndoRequested() { undoCommand(); }
void MainWindow::onRedoRequested() { redoCommand(); }
void MainWindow::onSuppressRequested() { toggleSuppressSelected(); }
void MainWindow::onRollbackRequested() { rollbackToSelected(); }
void MainWindow::onRollForwardRequested() { rollForwardToEnd(); }

void MainWindow::onRecomputeRequested() {
    const bool ok = presenter_->recomputeForDisplay();
    // pruneProvenance now runs inside refreshAll, below -- see the note there.
    const ObjectId keep = selectedId_;
    refreshAll();
    selectObject(keep); // an edit must not silently change the selection

    // refreshAll() has already reported whatever state the document is now in
    // (reportHealth), so success only needs to say so without overwriting a
    // failure message that is still true.
    if (ok) statusLeft_->setText(selectionSummary());
}

namespace {
// The filter used by both dialogs, so Open and Save cannot disagree about what
// an EP3D file is called.
const char* kDocumentFilter = "EP3D documents (*.ep3d);;All files (*)";
} // namespace

MainWindow::~MainWindow() = default;

void MainWindow::onNewRequested() {
    statusLeft_->setText(newDocumentCommand());
}

void MainWindow::onDeleteObjectRequested() {
    statusLeft_->setText(deleteSelectedObjectCommand());
}

QString MainWindow::newDocumentCommand() {
    if (document_ == nullptr) return QStringLiteral("No document");
    if (inSketchMode()) finishSketchCommand();

    auto fresh = std::make_unique<PartDocument>("Untitled");
    // The kernel and the solver are the APPLICATION'S (ADR-M3-003 /
    // ADR-M5-003) and a new document arrives with neither, exactly as a loaded
    // one does. Carried across from the document being replaced.
    fresh->setGeometryKernel(document_->geometryKernel());
    fresh->setSketchSolver(document_->sketchSolver());

    ownedDocument_ = std::move(fresh);
    document_ = ownedDocument_.get();
    presenter_->setDocument(*document_);
    if (sketchCanvas_ != nullptr) sketchCanvas_->setSketch(document_, kInvalidObjectId);
    selectedId_ = kInvalidObjectId;
    // NO PATH. The next Save must ask where, or the new document would
    // overwrite whatever the last one was saved as.
    documentPath_.clear();
    setWindowTitle(QStringLiteral("EP3D - Untitled"));

    onRecomputeRequested();
    refreshAll();
    return QStringLiteral("New document");
}

QString MainWindow::deleteSelectedObjectCommand() {
    if (document_ == nullptr || selectedId_ == kInvalidObjectId)
        return QStringLiteral("Select something in the model tree to delete");
    // A sketch being EDITED cannot be deleted out from under the canvas.
    if (inSketchMode() && selectedId_ == editingSketch_)
        return QStringLiteral("Finish the sketch before deleting it");

    const ObjectId victim = selectedId_;
    // WHETHER THIS CAN BE UNDONE IS READ FROM THE MODEL, not guessed from the
    // type (M17.15).
    //
    // Core already decides, and decides in three ways: a parameter, a frame, a
    // connector or an UNCONSUMED feature is recorded and comes back; a feature
    // something downstream consumes, a sketch, a body or a material cannot be
    // replayed, so the whole history is CLEARED rather than left offering an
    // undo that would do the wrong thing.
    //
    // The comment in removeObject says the clearing "is observable ... so a UI
    // can tell the user the history ended rather than offering an undo that
    // lies". No UI ever did. This command warned about sketches alone, so
    // deleting a consumed pad, a body or a material wiped every undo step the
    // user had and said "Deleted".
    const std::size_t undoBefore = document_->undoDepth();
    if (!document_->removeObject(victim))
        return QStringLiteral("That cannot be deleted");
    const std::size_t undoAfter = document_->undoDepth();
    const bool recorded = undoAfter > undoBefore;
    const bool historyCleared = undoAfter == 0 && undoBefore > 0;

    selectedId_ = kInvalidObjectId;
    onRecomputeRequested();
    refreshAll();
    // SAYS WHAT IS LEFT BROKEN. Deleting a sketch a Pad was built on does not
    // delete the Pad -- it leaves it with nothing to extrude, and the user
    // should hear that from the command rather than discover it in the tree.
    QString message;
    if (recorded) {
        message = QStringLiteral("Deleted -- Ctrl+Z brings it back");
    } else if (historyCleared) {
        // The SECOND loss is the one that stings, and it is the one nobody was
        // being told about: not only is this delete permanent, every earlier
        // step just became permanent too.
        message = QStringLiteral("Deleted -- this cannot be undone, and it cleared the earlier "
                                 "undo history as well");
    } else {
        message = QStringLiteral("Deleted -- this cannot be undone");
    }
    for (const std::unique_ptr<Body>& body : document_->bodies())
        for (const std::unique_ptr<Feature>& feature : body->features())
            if (feature->state() == ComputeState::Failed) {
                message += QStringLiteral(" -- something downstream now fails; check the tree");
                break;
            }
    return message;
}

void MainWindow::onOpenRequested() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open"), documentPath_,
                                                      QString::fromLatin1(kDocumentFilter));
    if (path.isEmpty()) return; // cancelled; nothing said, nothing changed
    statusLeft_->setText(openDocumentFile(path));
}

void MainWindow::onSaveRequested() {
    // Save with no path yet IS Save As. Silently writing somewhere chosen for
    // the user is how files go missing.
    if (documentPath_.isEmpty()) {
        onSaveAsRequested();
        return;
    }
    statusLeft_->setText(saveDocumentFile(documentPath_));
}

void MainWindow::onSaveAsRequested() {
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save As"), documentPath_,
                                                QString::fromLatin1(kDocumentFilter));
    if (path.isEmpty()) return;
    // The extension is ADDED, not assumed: a file called "bracket" that is an
    // EP3D document is a file the Open dialog will not show by default.
    if (!path.endsWith(QStringLiteral(".ep3d"), Qt::CaseInsensitive))
        path += QStringLiteral(".ep3d");
    statusLeft_->setText(saveDocumentFile(path));
}

std::vector<const Sketch*> MainWindow::openedSketches() const {
    return document_ != nullptr ? document_->sketches() : std::vector<const Sketch*>{};
}

const Sketch* MainWindow::openedSketchById(ObjectId id) const {
    return document_ != nullptr ? document_->findSketch(id) : nullptr;
}

std::size_t MainWindow::openedDocumentFeatureCount() const {
    if (document_ == nullptr || document_->bodies().empty()) return 0;
    return document_->bodies().front()->features().size();
}

std::size_t MainWindow::openedDocumentParameterCount() const {
    return document_ != nullptr ? document_->parameters().items().size() : 0;
}

QString MainWindow::saveDocumentFile(const QString& path) {
    if (document_ == nullptr) return QStringLiteral("No document to save");
    // FINISH THE SKETCH FIRST. Sketch mode is a view state, but leaving it
    // makes what is on screen match what is in the file.
    if (inSketchMode()) finishSketchCommand();

    const SaveResult saved = savePartDocumentToFile(*document_, path.toStdString());
    if (!saved) {
        // NAMES the reason. "Could not save" leaves a user staring at a
        // document they now cannot trust.
        return QStringLiteral("Could not save: %1").arg(QString::fromStdString(saved.message));
    }
    documentPath_ = path;
    setWindowTitle(QStringLiteral("EP3D - %1").arg(QFileInfo(path).fileName()));
    return QStringLiteral("Saved to %1").arg(path);
}

QString MainWindow::openDocumentFile(const QString& path) {
    LoadResult loaded = loadPartDocumentFromFile(path.toStdString());
    if (!loaded) {
        // NOTHING CHANGED. The loader never returns a half-restored document,
        // so the one on screen is still exactly what it was.
        return QStringLiteral("Could not open: %1").arg(QString::fromStdString(loaded.message));
    }

    // The kernel and the solver are the APPLICATION'S, not the file's -- they
    // are injected (ADR-M3-003 / ADR-M5-003) and a loaded document arrives with
    // neither. Carried across from the document being replaced, which is where
    // this window got them in the first place.
    if (document_ != nullptr) {
        loaded.document->setGeometryKernel(document_->geometryKernel());
        loaded.document->setSketchSolver(document_->sketchSolver());
    }

    if (inSketchMode()) finishSketchCommand();
    // ADOPTED, then pointed at. The previous document is freed only if THIS
    // window owned it; the one passed to the constructor belongs to its own
    // owner and outlives us.
    ownedDocument_ = std::move(loaded.document);
    document_ = ownedDocument_.get();
    presenter_->setDocument(*document_);
    if (sketchCanvas_ != nullptr) sketchCanvas_->setSketch(document_, kInvalidObjectId);
    selectedId_ = kInvalidObjectId;

    onRecomputeRequested();
    refreshAll();
    onFitAllRequested();
    documentPath_ = path;
    setWindowTitle(QStringLiteral("EP3D - %1").arg(QFileInfo(path).fileName()));
    return QStringLiteral("Opened %1").arg(path);
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
                    : document_->addParameter(uniqueObjectName("PadLength"), 20.0,
                                              UnitType::Millimeter)
                          .id();
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

std::vector<std::string> MainWindow::treeRows() const {
    std::vector<std::string> rows;
    if (tree_ == nullptr) return rows;
    const std::function<void(QTreeWidgetItem*, int)> visit = [&](QTreeWidgetItem* item,
                                                                int depth) {
        rows.push_back(std::string(static_cast<std::size_t>(depth) * 2, ' ') +
                       item->text(0).toStdString());
        for (int i = 0; i < item->childCount(); ++i) visit(item->child(i), depth + 1);
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) visit(tree_->topLevelItem(i), 0);
    return rows;
}

bool MainWindow::wireframeMenuChecked() const {
    return solidWireframeAction_ != nullptr && solidWireframeAction_->isChecked();
}

QString MainWindow::setSolidDisplayCommand(bool wireframe) {
    if (viewer_ == nullptr) return QStringLiteral("No 3D view");
    viewer_->setSolidDisplay(wireframe ? OcctViewWidget::SolidDisplay::Wireframe
                                       : OcctViewWidget::SolidDisplay::Shaded);
    // Read BACK from the viewer, never assumed. A menu that ticked itself and
    // a view that did something else is the same defect the sketch toolbar
    // shipped once already, in different clothes.
    const bool nowWireframe =
        viewer_->solidDisplay() == OcctViewWidget::SolidDisplay::Wireframe;
    if (solidWireframeAction_ != nullptr) solidWireframeAction_->setChecked(nowWireframe);
    if (solidShadedAction_ != nullptr) solidShadedAction_->setChecked(!nowWireframe);

    const QString message = nowWireframe
                                ? QStringLiteral("Solids drawn as wireframe")
                                : QStringLiteral("Solids drawn shaded");
    statusLeft_->setText(message);
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


// =============================================================================
// M12 -- sketch mode
// =============================================================================

void MainWindow::buildSketchUi() {
    // --- The Sketch menu ----------------------------------------------------
    QMenu* sketch = menuBar()->addMenu(QStringLiteral("&Sketch"));
    // Icons are built from the ACTIVE palette, once, here -- after the
    // application has applied its theme. A QIcon is a baked pixmap set, so a
    // theme switched at runtime would leave these behind; that is a known
    // limitation and the shell has no runtime theme switch.
    const QPalette iconPalette = palette();
    const auto sketchIcon = [&iconPalette](ui::SketchIcon which) {
        return ui::MakeSketchIcon(which, iconPalette);
    };

    newSketchAction_ = sketch->addAction(sketchIcon(ui::SketchIcon::NewSketch),
                                         QStringLiteral("&New Sketch"));
    newSketchAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    connect(newSketchAction_, &QAction::triggered, this, &MainWindow::onNewSketchRequested);
    sketchOnFaceAction_ = sketch->addAction(sketchIcon(ui::SketchIcon::SketchOnFace),
                                           QStringLiteral("New Sketch on Selected &Face"));
    connect(sketchOnFaceAction_, &QAction::triggered, this,
            &MainWindow::onSketchOnFaceRequested);

    // Onto the MODEL toolbar, and wired HERE rather than in buildToolbar().
    //
    // buildToolbar() runs first, before this action exists -- reaching for it
    // there dereferenced a null QAction and took the window down before it
    // drew anything. Inserted at the FRONT, because clicking a face is where
    // modelling on an existing solid starts: pick a face, draw on it, pad it.
    // The separator goes in before the same anchor, so it lands between this
    // button and Pad.
    if (modelToolBar_ != nullptr) {
        sketchOnFaceAction_->setIcon(sketchIcon(ui::SketchIcon::SketchOnFace));
        sketchOnFaceAction_->setIconText(QStringLiteral("On Face"));
        sketchOnFaceAction_->setToolTip(
            QStringLiteral("New Sketch on Face\nClick a flat face in the 3D view, then this.\n"
                           "The sketch is placed on that face's plane -- it stays there if the "
                           "model changes, and does not follow the face."));
        QAction* const before = modelToolBar_->actions().value(0);
        modelToolBar_->insertAction(before, sketchOnFaceAction_);
        modelToolBar_->insertSeparator(before);
    }
    editSketchAction_ = sketch->addAction(sketchIcon(ui::SketchIcon::EditSketch),
                                          QStringLiteral("&Edit Selected Sketch"));
    editSketchAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(editSketchAction_, &QAction::triggered, this, &MainWindow::onEditSketchRequested);
    finishSketchAction_ = sketch->addAction(sketchIcon(ui::SketchIcon::FinishSketch),
                                            QStringLiteral("&Finish Sketch"));
    finishSketchAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Return")));
    connect(finishSketchAction_, &QAction::triggered, this, &MainWindow::onFinishSketchRequested);

    // --- The sketch toolbar -------------------------------------------------
    //
    // A SEPARATE toolbar, shown only in sketch mode. todo 14 asks for
    // "contextual toolbars"; the cheapest honest version of that is a bar that
    // is not there when its commands would not work.
    sketchToolBar_ = addToolBar(QStringLiteral("Sketch"));
    // ICON ONLY. Eighteen labelled buttons is a bar nobody can scan; the same
    // eighteen as icons is a CAD toolbar. The words are not lost -- every
    // action keeps its text for the Sketch menu, and every tooltip spells out
    // what the command does AND what it needs selected, which is more than the
    // label ever said.
    sketchToolBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    sketchToolBar_->setIconSize(QSize(ui::size::kSketchToolbarIcon,
                                      ui::size::kSketchToolbarIcon));
    sketchToolBar_->setMovable(false);

    struct ToolEntry {
        SketchTool tool;
        const char* label;
        const char* shortcut;
        const char* tip;
        ui::SketchIcon icon;
    };
    static const ToolEntry kTools[] = {
        {SketchTool::Select, "Select", "S", "Select geometry; click again to deselect",
         ui::SketchIcon::Select},
        {SketchTool::Line, "Line", "L", "Draw connected lines; Esc ends the chain",
         ui::SketchIcon::Line},
        {SketchTool::Rectangle, "Rectangle", "R",
         "Draw a rectangle as 4 lines plus horizontal/vertical constraints",
         ui::SketchIcon::Rectangle},
        {SketchTool::Circle, "Circle", "C", "Click the centre, then a point on the rim",
         ui::SketchIcon::Circle},
        {SketchTool::Arc, "Arc", "A", "Click centre, start, then end (swept counter-clockwise)",
         ui::SketchIcon::Arc},
        {SketchTool::Point, "Point", "P", "Place a point", ui::SketchIcon::Point},
        // M17.17. Placed after the tool each is a variant of, so the bar reads
        // as pairs -- corner rectangle then centre rectangle, and so on.
        // Shortcuts are Shift+letter for the same reason: the variant sits
        // next to its sibling on the keyboard too.
        {SketchTool::CenterRectangle, "Centre Rectangle", "Shift+R",
         "Click the centre, then a corner.\nThe same four lines a corner "
         "rectangle makes -- the centre is where you click, not a constraint",
         ui::SketchIcon::CenterRectangle},
        {SketchTool::ThreePointCircle, "3-Point Circle", "Shift+C",
         "Click three points the circle passes through.\nA point that lands on "
         "existing geometry earns a Point-on-object constraint",
         ui::SketchIcon::ThreePointCircle},
        {SketchTool::ThreePointArc, "3-Point Arc", "Shift+A",
         "Click both ends, then a point the arc passes through.\nThe third click "
         "is what decides which way round it goes",
         ui::SketchIcon::ThreePointArc},
        {SketchTool::TangentArc, "Tangent Arc", "Shift+T",
         "Click the free END of a line or arc, then where the arc should stop.\n"
         "It leaves that end SMOOTHLY, and stays smooth when you drag it.\n"
         "Keeps going, so you can chain a run of them",
         ui::SketchIcon::TangentArc},
        {SketchTool::Ellipse, "Ellipse", "Shift+E",
         "Click the centre, then the end of the LONG axis, then the width.\n"
         "The long axis is second because an ellipse's rotation is measured to "
         "it.\nThe third click's distance ACROSS the axis is the width, so "
         "sliding\nalong it changes nothing",
         ui::SketchIcon::Ellipse},
        {SketchTool::EllipticalArc, "Elliptical Arc", "Shift+G",
         "The same three clicks, then a fourth for where it stops.\n"
         "It starts at the long axis and sweeps counter-clockwise",
         ui::SketchIcon::EllipticalArc},
        {SketchTool::Spline, "Spline", "Shift+N",
         "Click each point the curve should go through.\n"
         "Double-click to finish, or click the first point again to close it.\n"
         "Only its two ENDS can carry a constraint -- the points between\n"
         "them are free, and the DOF readout counts every one of them",
         ui::SketchIcon::Spline},
        {SketchTool::Slot, "Slot", "Shift+S",
         "Click both end centres, then click to set the width.\nTwo sides and two "
         "round ends, held square at every corner.\nThe width comes from how far the "
         "third click is ACROSS the slot, so sliding along it changes nothing",
         ui::SketchIcon::Slot},
        {SketchTool::Polygon, "Polygon", "Shift+P",
         "Click the centre, then a corner.\nBuilt on a construction circle with "
         "equal sides, so it stays regular",
         ui::SketchIcon::Polygon},
    };
    QMenu* tools = sketch->addMenu(QStringLiteral("&Tools"));
    for (const ToolEntry& entry : kTools) {
        const SketchTool tool = entry.tool;
        QAction* action =
            sketchToolBar_->addAction(sketchIcon(entry.icon), QString::fromLatin1(entry.label));
        // The tooltip carries the NAME as well, because an icon-only bar has
        // nowhere else to say it.
        action->setToolTip(QStringLiteral("%1 (%2)\n%3")
                               .arg(QString::fromLatin1(entry.label))
                               .arg(QString::fromLatin1(entry.shortcut))
                               .arg(QString::fromLatin1(entry.tip)));
        action->setShortcut(QKeySequence(QString::fromLatin1(entry.shortcut)));
        // SCOPED TO THE CANVAS. These are single letters, and a window-scoped
        // single-letter shortcut steals the keystroke from every line editor in
        // the window: typing `#Width / 2` into a dimension or a property cell
        // would fire Horizontal on the `h`. Qt processes shortcuts BEFORE the
        // focus widget sees the key, so the editor never gets it.
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        sketchCanvas_->addAction(action);
        action->setCheckable(true);
        // WHICH tool this is, carried as data rather than inferred from the
        // button's text. With an icon-only bar the text is no longer what the
        // user sees, and matching on it was a coupling waiting to break the
        // moment a label was reworded.
        action->setData(QVariant::fromValue<int>(static_cast<int>(tool)));
        if (tool == SketchTool::Select) action->setChecked(true);
        connect(action, &QAction::triggered, this, [this, tool]() {
            if (sketchCanvas_ == nullptr) return;
            sketchCanvas_->setTool(tool);
            // Read BACK from the canvas rather than assuming `tool` took: the
            // canvas is the one that knows, and Esc changes it without any
            // button being pressed.
            syncSketchToolButtons();
            updateSketchStatus();
        });
        tools->addAction(action);
        sketchModeActions_.push_back(action);
    }

    sketchToolBar_->addSeparator();

    struct CommandEntry {
        SketchEditKind kind;
        const char* label;
        const char* shortcut;
        const char* tip;
        bool dimension;
        ui::SketchIcon icon;
    };
    static const CommandEntry kCommands[] = {
        {SketchEditKind::AddCoincident, "Coincident", "K", "Join the 2 selected points", false,
         ui::SketchIcon::Coincident},
        {SketchEditKind::AddHorizontal, "Horizontal", "H", "Make the selected lines horizontal",
         false, ui::SketchIcon::Horizontal},
        {SketchEditKind::AddVertical, "Vertical", "V", "Make the selected lines vertical", false,
         ui::SketchIcon::Vertical},
        {SketchEditKind::AddFix, "Fix", "F", "Pin the selected points where they are", false,
         ui::SketchIcon::Fix},
        // M13. Shortcuts follow the same one-letter scheme, skipping the ones
        // M12 already took (S/L/R/C/A/P for tools, K/H/V/F/D for commands).
        {SketchEditKind::AddParallel, "Parallel", "G", "Make the 2 selected lines parallel",
         false, ui::SketchIcon::Parallel},
        {SketchEditKind::AddPerpendicular, "Perpendicular", "N",
         "Make the 2 selected lines perpendicular", false, ui::SketchIcon::Perpendicular},
        {SketchEditKind::AddEqual, "Equal", "E",
         "Equal length (2 lines) or equal radius (2 circles/arcs)", false,
         ui::SketchIcon::Equal},
        {SketchEditKind::AddConcentric, "Concentric", "O",
         "Make the 2 selected circles/arcs share a centre", false, ui::SketchIcon::Concentric},
        {SketchEditKind::AddMidpoint, "Midpoint", "M",
         "Put the selected point at the selected line's midpoint", false,
         ui::SketchIcon::Midpoint},
        {SketchEditKind::AddPointOnObject, "On object", "B",
         "Put the selected point on the selected line, circle or arc", false,
         ui::SketchIcon::PointOnObject},
        {SketchEditKind::AddSymmetric, "Symmetric", "",
         "Mirror 2 selected points about a selected line", false, ui::SketchIcon::Symmetric},
        {SketchEditKind::AddTangent, "Tangent", "T",
         "Make the 2 selected entities touch: a line and a curve, or two curves", false,
         ui::SketchIcon::Tangent},
        {SketchEditKind::None, "Dimension", "D",
         "Dimension the selection: 1 line = length, 1 circle = diameter, 1 arc = radius, "
         "2 points = distance, 2 lines = angle",
         true, ui::SketchIcon::Dimension},
        {SketchEditKind::AddRadius, "Radius", "",
         "Force a RADIUS dimension on the selected circle or arc", true, ui::SketchIcon::Radius},
        {SketchEditKind::AddMajorAxis, "Major axis", "",
         "Dimension the LONG semi-axis of a selected ellipse", false,
         ui::SketchIcon::MajorAxisDimension},
        {SketchEditKind::AddMinorAxis, "Minor axis", "",
         "Dimension the SHORT semi-axis of a selected ellipse", false,
         ui::SketchIcon::MinorAxisDimension},
        {SketchEditKind::AddDiameter, "Diameter", "",
         "Force a DIAMETER dimension on the selected circle or arc", true,
         ui::SketchIcon::Diameter},
        // The two legs of a point-to-point distance. Their own commands rather
        // than an inference: two selected points can mean three different
        // measurements, and picking one for the user would be a silent guess.
        {SketchEditKind::AddHorizontalDistance, "Horizontal Distance", "",
         "Dimension the HORIZONTAL gap between the 2 selected points", true,
         ui::SketchIcon::HorizontalDistance},
        {SketchEditKind::AddVerticalDistance, "Vertical Distance", "",
         "Dimension the VERTICAL gap between the 2 selected points", true,
         ui::SketchIcon::VerticalDistance},
        {SketchEditKind::AddPointLineDistance, "Distance to Line", "",
         "Dimension the PERPENDICULAR gap between the selected point and line", true,
         ui::SketchIcon::PointLineDistance},
    };
    QMenu* constrain = sketch->addMenu(QStringLiteral("&Constrain and Dimension"));
    for (const CommandEntry& entry : kCommands) {
        const SketchEditKind kind = entry.kind;
        const bool dimension = entry.dimension;
        QAction* action =
            sketchToolBar_->addAction(sketchIcon(entry.icon), QString::fromLatin1(entry.label));
        action->setToolTip(entry.shortcut[0] != 0
                               ? QStringLiteral("%1 (%2)\n%3")
                                     .arg(QString::fromLatin1(entry.label))
                                     .arg(QString::fromLatin1(entry.shortcut))
                                     .arg(QString::fromLatin1(entry.tip))
                               : QStringLiteral("%1\n%2")
                                     .arg(QString::fromLatin1(entry.label))
                                     .arg(QString::fromLatin1(entry.tip)));
        if (entry.shortcut[0] != 0) {
            action->setShortcut(QKeySequence(QString::fromLatin1(entry.shortcut)));
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            sketchCanvas_->addAction(action);
        }
        connect(action, &QAction::triggered, this,
                [this, kind, dimension]() { (void)applySketchCommand(kind, dimension); });
        constrain->addAction(action);
        sketchModeActions_.push_back(action);
    }

    // THE ROW BREAK, marked where the meaning changes (M17.13).
    //
    // Everything above is what you DRAW and how you CONSTRAIN it; everything
    // below is what you DO to what you drew. The split used to be "the
    // separator nearest the middle", which is a fact about arithmetic rather
    // than about the commands -- it landed wherever the count happened to put
    // it, and moved every time a button was added.
    sketchToolbarRowBreak_ = sketchToolBar_->addSeparator();
    QAction* originPoint = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::OriginPoint),
                                                     QStringLiteral("Origin Point"));
    originPoint->setToolTip(
        QStringLiteral("Origin Point\nAdd a fixed point at (0,0) and select it,\n"
                       "so you can dimension from the origin"));
    connect(originPoint, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        const QString status = sketchCanvas_->addOriginPoint();
        if (!status.isEmpty()) {
            sketchMessage_ = status;
            updateSketchStatus();
        }
        rebuildConstraintPanel();
    });
    sketchModeActions_.push_back(originPoint);

    trimAction_ = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Trim),
                                           QStringLiteral("Trim"));
    trimAction_->setToolTip(
        QStringLiteral("Trim\nClick the part of a line you want to remove.\n"
                       "It is cut back to the nearest crossing. Esc leaves trim mode."));
    trimAction_->setCheckable(true);
    connect(trimAction_, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        sketchCanvas_->setTrimming(!sketchCanvas_->trimming());
        // Read BACK, never assumed: the canvas decides, and a button that
        // showed its own guess is how the toolbar and the canvas came to
        // disagree about the active tool once already.
        trimAction_->setChecked(sketchCanvas_->trimming());
        sketchMessage_ = sketchCanvas_->trimming()
                             ? QStringLiteral("Trim: click the piece of a line to remove.")
                             : QStringLiteral("Trim off.");
        updateSketchStatus();
    });
    sketchModeActions_.push_back(trimAction_);

    extendAction_ = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Extend),
                                             QStringLiteral("Extend"));
    extendAction_->setToolTip(
        QStringLiteral("Extend\nClick near the end of a line to stretch it\n"
                       "to the first thing beyond it. Esc leaves extend mode."));
    extendAction_->setCheckable(true);
    connect(extendAction_, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        sketchCanvas_->setExtending(!sketchCanvas_->extending());
        syncSketchToolButtons();
        sketchMessage_ = sketchCanvas_->extending()
                             ? QStringLiteral("Extend: click near the end of a line to stretch.")
                             : QStringLiteral("Extend off.");
        updateSketchStatus();
    });
    sketchModeActions_.push_back(extendAction_);

    QAction* reference = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::ReferenceDimension),
                                                  QStringLiteral("Reference"));
    reference->setToolTip(
        QStringLiteral("Reference dimension\nPick a dimension, then this: it MEASURES the "
                       "geometry instead of driving it.\nIt takes no degree of freedom and "
                       "cannot conflict -- which is how you show a size you are not "
                       "controlling.\nDrawn in brackets, as on a drawing."));
    connect(reference, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        sketchMessage_ = sketchCanvas_->toggleDimensionDriven();
        updateSketchStatus();
        rebuildConstraintPanel();
    });
    sketchModeActions_.push_back(reference);

    dimensionToolAction_ = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::DimensionTool),
                                                    QStringLiteral("Dimension"));
    dimensionToolAction_->setToolTip(
        QStringLiteral("Dimension (D)\nClick the geometry to measure, then click where the "
                       "dimension line goes.\nThe KIND is read from what you picked: a line is "
                       "a length, two points a distance, a circle a diameter.\nEsc leaves the "
                       "mode."));
    dimensionToolAction_->setCheckable(true);
    dimensionToolAction_->setShortcut(QKeySequence(QStringLiteral("D")));
    dimensionToolAction_->setShortcutContext(Qt::WidgetShortcut);
    connect(dimensionToolAction_, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        sketchCanvas_->setDimensioning(!sketchCanvas_->dimensioning());
        syncSketchToolButtons();
        sketchMessage_ = sketchCanvas_->dimensioning()
                             ? QStringLiteral("Dimension: click the geometry to measure.")
                             : QStringLiteral("Dimension off.");
        updateSketchStatus();
    });
    sketchModeActions_.push_back(dimensionToolAction_);

    useReferenceAction_ = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::UseReference),
                                                   QStringLiteral("Use"));
    useReferenceAction_->setToolTip(
        QStringLiteral("Use projected geometry\nClick a projected reference edge, corner or hole\n"
                       "to turn it into real sketch geometry you can pad, constrain and "
                       "dimension.\nThe reference itself stays, so you can use it again. "
                       "Esc leaves the mode."));
    useReferenceAction_->setCheckable(true);
    connect(useReferenceAction_, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        sketchCanvas_->setUseReference(!sketchCanvas_->useReference());
        syncSketchToolButtons();
        sketchMessage_ =
            sketchCanvas_->useReference()
                ? QStringLiteral("Use: click a projected edge, corner or hole to convert it.")
                : QStringLiteral("Use off.");
        updateSketchStatus();
    });
    sketchModeActions_.push_back(useReferenceAction_);

    QAction* mirror = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Mirror),
                                               QStringLiteral("Mirror"));
    mirror->setToolTip(
        QStringLiteral("Mirror\nSelect what to mirror, then the line to mirror it across\n"
                       "LAST. The copies stay tied to the originals."));
    connect(mirror, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        const QString status = sketchCanvas_->applyMirror();
        if (!status.isEmpty()) {
            sketchMessage_ = status;
            updateSketchStatus();
        }
        rebuildConstraintPanel();
    });
    sketchModeActions_.push_back(mirror);

    QAction* split = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Split),
                                              QStringLiteral("Split"));
    split->setToolTip(
        QStringLiteral("Split\nSelect what to cut FIRST, then the things that cross it.\n"
                       "Every piece is kept and stays joined to the next.\n"
                       "Constraints move to the piece they still describe; a split that\n"
                       "would change what one of them SAYS is refused and names it."));
    connect(split, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        const QString status = sketchCanvas_->applySplit();
        if (!status.isEmpty()) {
            sketchMessage_ = status;
            updateSketchStatus();
        }
        rebuildConstraintPanel();
    });
    sketchModeActions_.push_back(split);

    QAction* fillet = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Fillet),
                                               QStringLiteral("Fillet"));
    fillet->setToolTip(
        QStringLiteral("Fillet\nSelect the 2 lines that meet at a corner,\n"
                       "then give the radius. The arc stays tangent to both."));
    connect(fillet, &QAction::triggered, this, &MainWindow::onFilletRequested);
    sketchModeActions_.push_back(fillet);

    QAction* chamfer = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Chamfer),
                                                QStringLiteral("Chamfer"));
    chamfer->setToolTip(
        QStringLiteral("Chamfer\nSelect the 2 lines that meet at a corner,\n"
                       "then give the setback along each."));
    connect(chamfer, &QAction::triggered, this, &MainWindow::onChamferRequested);
    sketchModeActions_.push_back(chamfer);

    QAction* offset = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Offset),
                                               QStringLiteral("Offset"));
    offset->setToolTip(
        QStringLiteral("Offset\nCopy the selected line, circle or arc at a distance,\n"
                       "with the constraints that keep it there.\n"
                       "A negative distance offsets the other way."));
    connect(offset, &QAction::triggered, this, &MainWindow::onOffsetRequested);
    sketchModeActions_.push_back(offset);

    QAction* transform = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Transform),
                                                  QStringLiteral("Transform"));
    transform->setToolTip(
        QStringLiteral("Transform\nMove, turn or resize the selection -- and optionally leave\n"
                       "a copy behind. A copy keeps the constraints that live INSIDE\n"
                       "the selection and gets its own dimensions; ones that reach\n"
                       "outside are counted and left with the original.\n"
                       "In place it only moves things: the sketch's constraints still\n"
                       "hold, so a pinned sketch will pull them back and say so."));
    connect(transform, &QAction::triggered, this, &MainWindow::onTransformRequested);
    sketchModeActions_.push_back(transform);

    QAction* construction = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::Construction),
                                                     QStringLiteral("Construction"));
    construction->setToolTip(
        QStringLiteral("Construction (Q)\nSwitch the selected geometry to or from construction:\n"
                       "drawn, snapped to and dimensioned, but not part of the solid"));
    construction->setShortcut(QKeySequence(QStringLiteral("Q")));
    construction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    sketchCanvas_->addAction(construction);
    connect(construction, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        const QString status = sketchCanvas_->toggleConstruction();
        if (!status.isEmpty()) {
            sketchMessage_ = status;
            updateSketchStatus();
        }
    });
    sketchModeActions_.push_back(construction);

    QAction* deleteGeometry = sketchToolBar_->addAction(
        sketchIcon(ui::SketchIcon::DeleteGeometry), QStringLiteral("Delete"));
    deleteGeometry->setShortcut(QKeySequence(QStringLiteral("Del")));
    deleteGeometry->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    sketchCanvas_->addAction(deleteGeometry);
    deleteGeometry->setToolTip(
        QStringLiteral("Delete (Del)\nDelete the selected geometry and every constraint on it,\n"
                       "or the constraint whose badge is picked on the canvas"));
    connect(deleteGeometry, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        // Not deleteSelection(): this action owns the Del key on the canvas, so
        // it has to make the same decision the canvas itself would.
        const QString status = sketchCanvas_->deleteSelectionOrHighlightedConstraint();
        if (!status.isEmpty()) {
            sketchMessage_ = status;
            updateSketchStatus();
        }
        rebuildConstraintPanel();
    });
    sketchModeActions_.push_back(deleteGeometry);

    QAction* autoPlace = sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::AutoPlaceDimensions),
                                                   QStringLiteral("Auto-place Dimensions"));
    autoPlace->setToolTip(QStringLiteral("Auto-place Dimensions\nPut every dimension back "
                                         "where the layout would put it"));
    connect(autoPlace, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ == nullptr) return;
        const QString status = sketchCanvas_->autoPlaceAllDimensions();
        if (!status.isEmpty()) {
            sketchMessage_ = status;
            updateSketchStatus();
        }
    });
    sketchModeActions_.push_back(autoPlace);
    constrain->addAction(autoPlace);

    QAction* fitSketch =
        sketchToolBar_->addAction(sketchIcon(ui::SketchIcon::FitSketch),
                                  QStringLiteral("Fit Sketch"));
    fitSketch->setToolTip(QStringLiteral("Fit Sketch\nZoom so the whole sketch is visible"));
    connect(fitSketch, &QAction::triggered, this, [this]() {
        if (sketchCanvas_ != nullptr) sketchCanvas_->fitView();
    });
    sketchModeActions_.push_back(fitSketch);

    // --- The constraint manager (roadmap 6.3) -------------------------------
    constraints_ = new QTableWidget(this);
    constraints_->setColumnCount(4);
    constraints_->setHorizontalHeaderLabels({QStringLiteral("Constraint"), QStringLiteral("On"),
                                             QStringLiteral("Value"), QStringLiteral("Status")});
    constraints_->verticalHeader()->setVisible(false);
    constraints_->setSelectionBehavior(QAbstractItemView::SelectRows);
    constraints_->setSelectionMode(QAbstractItemView::SingleSelection);
    constraints_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    constraints_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    constraints_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    constraints_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    constraints_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    connect(constraints_, &QTableWidget::cellDoubleClicked, this,
            &MainWindow::onConstraintRowActivated);
    // SELECTION, not activation: roadmap 8.2 point 2 wants picking a listed
    // constraint to show WHICH geometry it is about, and a user diagnosing a
    // conflict clicks once. Double-click stays the dimension editor.
    connect(constraints_, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::onConstraintRowHighlighted);

    // Roadmap 6.3 asks for a delete control ON the panel. The menu entry below
    // does the same work, but a command reachable only through Constrain > ...
    // is not the "select the offending row and throw it away" the section
    // describes -- and throwing one away is the whole recovery path from an
    // over-constrained sketch.
    deleteConstraintButton_ = new QPushButton(QStringLiteral("Delete Constraint"), this);
    deleteConstraintButton_->setToolTip(
        QStringLiteral("Delete the selected constraint\n"
                       "Use this to remove a constraint marked AT FAULT"));
    deleteConstraintButton_->setEnabled(false);
    connect(deleteConstraintButton_, &QPushButton::clicked, this,
            [this]() { deleteSelectedConstraintRow(); });

    auto* constraintPanel = new QWidget(this);
    auto* constraintLayout = new QVBoxLayout(constraintPanel);
    constraintLayout->setContentsMargins(0, 0, 0, 0);
    constraintLayout->addWidget(constraints_);
    constraintLayout->addWidget(deleteConstraintButton_);

    constraintDock_ = new QDockWidget(QStringLiteral("Constraints"), this);
    constraintDock_->setWidget(constraintPanel);
    constraintDock_->setFeatures(QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, constraintDock_);

    dimensionFormatAction_ =
        constrain->addAction(QStringLiteral("Dimension &Format..."));
    connect(dimensionFormatAction_, &QAction::triggered, this,
            &MainWindow::onDimensionFormatRequested);
    sketchModeActions_.push_back(dimensionFormatAction_);

    deleteConstraintAction_ = constrain->addAction(QStringLiteral("Delete Selected Constraint"));
    connect(deleteConstraintAction_, &QAction::triggered, this,
            [this]() { deleteSelectedConstraintRow(); });
    sketchModeActions_.push_back(deleteConstraintAction_);

    // --- TWO ROWS, because one row of 36 icons is not scannable -------------
    //
    // The comment where this bar is created still says "eighteen buttons", and
    // it was right when it was written. The bar has since doubled, and Qt's
    // answer to a toolbar wider than its window is to hide the overflow behind
    // a small chevron -- so on any ordinary window width the last third of the
    // sketch tools were not visible at all. The owner went looking for Use and
    // could not find it, which is the correct reaction to a button that is not
    // on screen.
    //
    // Split HERE, after the bar is fully built, rather than by routing each
    // command to one of two bars at its own call site: twenty-odd call sites
    // each choosing a row is twenty-odd chances for a new command to land in
    // the wrong one, and the rows would drift as commands were added. The split
    // point is the separator nearest the middle, so it lands on a boundary the
    // toolbar's author already thought was one.
    {
        const QList<QAction*> all = sketchToolBar_->actions();
        const int splitAt = all.indexOf(sketchToolbarRowBreak_);
        if (splitAt > 0) {
            addToolBarBreak();
            sketchToolBarSecond_ = addToolBar(QStringLiteral("Sketch (2)"));
            sketchToolBarSecond_->setIconSize(sketchToolBar_->iconSize());
            sketchToolBarSecond_->setToolButtonStyle(Qt::ToolButtonIconOnly);
            sketchToolBarSecond_->setMovable(false);
            // The separator itself is dropped: it was marking the boundary that
            // is now a row break, and a leading separator on the second row is
            // a gap with nothing on either side of it.
            for (int i = splitAt; i < all.size(); ++i) {
                sketchToolBar_->removeAction(all[i]);
                if (i > splitAt) sketchToolBarSecond_->addAction(all[i]);
            }
        }
    }

    // Hidden until a sketch is open.
    sketchToolBar_->setVisible(false);
    if (sketchToolBarSecond_ != nullptr) sketchToolBarSecond_->setVisible(false);
    constraintDock_->setVisible(false);
    refreshCommandStates();
}

bool MainWindow::inSketchMode() const noexcept { return editingSketch_ != kInvalidObjectId; }

void MainWindow::enterSketchMode(ObjectId sketchId) {
    editingSketch_ = sketchId;
    sketchCanvas_->setSketch(document_, sketchId);
    centralStack_->setCurrentWidget(sketchCanvas_);
    sketchToolBar_->setVisible(true);
    if (sketchToolBarSecond_ != nullptr) sketchToolBarSecond_->setVisible(true);
    constraintDock_->setVisible(true);
    sketchCanvas_->setFocus(Qt::OtherFocusReason);
    rebuildConstraintPanel();
    updateSketchStatus();
    refreshCommandStates();
}

QString MainWindow::newSketchCommand() {
    if (document_ == nullptr) return QStringLiteral("No document");
    // Counting sketches gave the same name twice after one was deleted; the
    // tree is how a user picks which to edit.
    const std::string name = uniqueObjectName("Sketch");
    Sketch& created = document_->addSketch(name);
    enterSketchMode(created.id());
    // Every new sketch starts with a real, fixed origin point.
    //
    // The canvas has always DRAWN a marker at (0,0), so a user reasonably
    // expects to be able to select it and measure from it -- and could not,
    // because there was nothing there. Materialising it costs one entity and
    // removes a whole class of "why can't I dimension this".
    if (sketchCanvas_ != nullptr) sketchCanvas_->addOriginPoint();
    refreshAll();
    // The caveat is part of the message, not a footnote somewhere else: the
    // undo stack still holds the PREVIOUS edit, so a user who presses Ctrl+Z
    // expecting to remove this sketch would undo something they did earlier.
    const QString message =
        QStringLiteral("%1 created and open for drawing. Note: creating a sketch is not yet "
                       "undoable -- what you draw inside it is.")
            .arg(QString::fromStdString(name));
    sketchMessage_ = message;
    updateSketchStatus();
    return message;
}

QString MainWindow::sketchOnFaceCommand() {
    if (document_ == nullptr) return QStringLiteral("No document");

    // The DECISION is PlanSketchOnFace's, in the Qt-free layer where a test can
    // reach every branch of it (M17_FACE_*). What is left here is what only a
    // shell can do: ask the viewer what was picked, and show the answer.
    const FaceSketchPlan plan =
        PlanSketchOnFace(viewer_ != nullptr ? viewer_->pickedFace() : PickedFace{});
    if (!plan.ok) {
        const QString message = QString::fromStdString(plan.message);
        statusLeft_->setText(message);
        return message;
    }

    const std::string name = uniqueObjectName("Sketch");
    Sketch& created = document_->addSketch(name, plan.frame);
    // The face's boundary, projected, as a tracing underlay (M17.6). Added
    // BEFORE the sketch is opened, so the canvas has it the first time it
    // paints rather than a frame later.
    //
    // Not undoable, and consistent with the sketch itself: creating a sketch is
    // not an undo step (see newSketchCommand), and an underlay that could be
    // undone out from under the sketch it belongs to would leave a face sketch
    // that no longer knows what it was made on.
    document_->addSketchReferences(created.id(), plan.reference.geometry);

    // MAKE IT FOLLOW THE FACE (M17.14, ADR-M17-036), when the pick can be
    // said as a query. The frame above is still set, and is what the sketch
    // uses until the first recompute re-derives it -- so a sketch that cannot
    // be tracked is exactly the frozen-plane sketch M17.5 shipped, rather than
    // a broken one.
    //
    // The two conditions come from the same pick, so this cannot disagree with
    // what the user clicked: `createdBy` is the feature the viewer resolved
    // from the face's provenance, and `extremeTowards`/`facing` narrow it to
    // the one face.
    QString tracking;
    if (viewer_ != nullptr && viewer_->pickedFace().createdBy != 0) {
        FaceQuery query;
        query.createdBy = static_cast<ObjectId>(viewer_->pickedFace().createdBy);
        query.facing = viewer_->pickedFace().normal;
        query.extremeTowards = viewer_->pickedFace().normal;
        if (document_->setSketchTrackedFace(created.id(), query)) {
            tracking = QStringLiteral(" It follows %1, so it moves when the model does.")
                           .arg(QString::fromStdString(DescribeFaceQuery(query)));
        } else {
            // Said out loud. A sketch that silently did NOT get tracking is one
            // the user will expect to move and will find has not.
            tracking = QStringLiteral(" It could not be made to follow that face, so it stays "
                                      "on this plane.");
        }
    }

    enterSketchMode(created.id());
    // Same as a plain new sketch: a real origin point to dimension from. The
    // origin of a face sketch is the part origin dropped onto the face's
    // plane, so measuring from it means something on this plane too.
    if (sketchCanvas_ != nullptr) sketchCanvas_->addOriginPoint();
    refreshAll();

    const QString message = QStringLiteral("%1 created on the picked face. %2.%3")
                                .arg(QString::fromStdString(name),
                                     QString::fromStdString(plan.message), tracking);
    sketchMessage_ = message;
    updateSketchStatus();
    return message;
}

QString MainWindow::editSelectedSketchCommand() {
    const ObjectId sketchId = selectedSketch();
    if (sketchId == kInvalidObjectId) {
        const QString message = QStringLiteral("Select a sketch in the model tree to edit");
        statusLeft_->setText(message);
        return message;
    }
    enterSketchMode(sketchId);
    const Sketch* sketch = document_->findSketch(sketchId);
    const QString message =
        QStringLiteral("Editing %1 -- %2 entities, %3 constraints")
            .arg(QString::fromStdString(sketch != nullptr ? sketch->name() : std::string()))
            .arg(sketch != nullptr ? sketch->entities().size() : 0)
            .arg(sketch != nullptr ? sketch->constraints().size() : 0);
    sketchMessage_ = message;
    updateSketchStatus();
    return message;
}

QString MainWindow::finishSketchCommand() {
    if (!inSketchMode()) {
        const QString message = QStringLiteral("No sketch is open");
        statusLeft_->setText(message);
        return message;
    }
    const ObjectId finished = editingSketch_;
    editingSketch_ = kInvalidObjectId;
    sketchCanvas_->setSketch(document_, kInvalidObjectId);
    centralStack_->setCurrentWidget(viewer_);
    sketchToolBar_->setVisible(false);
    if (sketchToolBarSecond_ != nullptr) sketchToolBarSecond_->setVisible(false);
    constraintDock_->setVisible(false);
    // Selecting the finished sketch is what makes "Insert > Pad" the obvious
    // next step: the command it needs is already armed.
    selectObject(finished);
    refreshAll();
    const Sketch* sketch = document_->findSketch(finished);
    QString message = QStringLiteral("Sketch closed");
    if (sketch != nullptr) {
        const SketchStatusLine line = DescribeSketchStatus(*sketch);
        message = QStringLiteral("%1 closed -- [%2] %3. Insert > Pad to extrude it.")
                      .arg(QString::fromStdString(sketch->name()))
                      .arg(QString::fromStdString(line.badge))
                      .arg(QString::fromStdString(line.text));
    }
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

void MainWindow::rebuildConstraintPanel() {
    if (constraints_ == nullptr) return;
    const Sketch* sketch =
        (document_ != nullptr && editingSketch_ != kInvalidObjectId)
            ? document_->findSketch(editingSketch_)
            : nullptr;

    // Which constraint was selected, BY ID. The panel is rebuilt on every
    // recompute, and clearing the rows drops the selection -- so without this
    // the highlight would blink off whenever anything solved, which is exactly
    // when a user is staring at it. Row INDEX would not do: a delete or a new
    // constraint renumbers the rows underneath.
    const SketchConstraintId previous =
        sketchCanvas_ != nullptr ? sketchCanvas_->highlightedConstraint()
                                 : kInvalidSketchConstraintId;

    constraints_->setRowCount(0);
    if (sketch == nullptr) {
        if (sketchCanvas_ != nullptr)
            sketchCanvas_->setHighlightedConstraint(kInvalidSketchConstraintId);
        if (deleteConstraintButton_ != nullptr) deleteConstraintButton_->setEnabled(false);
        return;
    }

    const std::vector<ConstraintRow> rows = ConstraintRowsFor(*document_, *sketch);
    constraints_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const ConstraintRow& row = rows[i];
        const auto makeItem = [&](const std::string& text) {
            auto* item = new QTableWidgetItem(QString::fromStdString(text));
            item->setData(kIdRole, QVariant::fromValue<qulonglong>(ToObjectId(row.id)));
            item->setToolTip(QString::fromStdString(text));
            return item;
        };
        constraints_->setItem(i, 0, makeItem(row.kind));
        constraints_->setItem(i, 1, makeItem(row.elements));
        constraints_->setItem(i, 2, makeItem(row.parameter));
        // TEXT, not a colour (A06). "OK" and "AT FAULT" are readable in a
        // monochrome screenshot and by a reader who cannot distinguish red.
        constraints_->setItem(i, 3, makeItem(row.offending ? "AT FAULT" : "OK"));
    }

    // Put the selection back on the SAME constraint, if it still exists. A
    // constraint that was just deleted is gone on purpose, and the highlight
    // goes with it rather than sliding onto whatever took its row.
    int restored = -1;
    if (previous != kInvalidSketchConstraintId) {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (rows[static_cast<std::size_t>(i)].id == previous) restored = i;
    }
    // Only the restore is needed. Clearing is NOT done here on purpose:
    // setRowCount(0) above already emitted itemSelectionChanged with an empty
    // selection, which ran onConstraintRowHighlighted() and dropped both the
    // ring and the button. An explicit else-branch here was written first and
    // then deleted -- a mutation showed nothing could tell it from its absence,
    // which is the definition of code that cannot be trusted to be right.
    if (restored >= 0) constraints_->setCurrentCell(restored, 0);
}

void MainWindow::updateSketchStatus() {
    if (!inSketchMode() || sketchCanvas_ == nullptr) return;
    const SketchStatusLine line = sketchCanvas_->statusLine();
    // BADGE AND SENTENCE ALWAYS, prompt-or-message after them.
    //
    // The first version put the command's message in the WHOLE status line, and
    // the constraint state vanished the moment anything was done -- which is
    // precisely when a user wants to know whether the sketch is still
    // under-constrained. Roadmap 8.2 point 1 asks for the state to exist as
    // text; a text that is overwritten by the next command does not.
    const QString tail =
        sketchMessage_.isEmpty() ? sketchCanvas_->promptText() : sketchMessage_;
    const QString text = QStringLiteral("[%1] %2   |   %3")
                             .arg(QString::fromStdString(line.badge))
                             .arg(QString::fromStdString(line.text))
                             .arg(tail);
    statusLeft_->setText(text);
    statusLeft_->setToolTip(QString::fromStdString(line.detail));
}

namespace {

// Buttons only: separators are actions too, and counting them would make the
// icon totals disagree with what is on screen.
std::vector<QAction*> ToolbarButtons(const QToolBar* bar) {
    std::vector<QAction*> buttons;
    if (bar == nullptr) return buttons;
    for (QAction* action : bar->actions())
        if (!action->isSeparator()) buttons.push_back(action);
    return buttons;
}

} // namespace

std::vector<QAction*> MainWindow::sketchToolbarButtons() const {
    // BOTH ROWS, as one list. The split into two rows is a layout decision;
    // every readback and every test that walks "the sketch toolbar" must go on
    // seeing all of it, or splitting the bar would silently halve what the
    // smoke test checks -- and the check that every icon is distinct is exactly
    // the one that must not start looking at half the icons.
    std::vector<QAction*> buttons = ToolbarButtons(sketchToolBar_);
    const std::vector<QAction*> second = ToolbarButtons(sketchToolBarSecond_);
    buttons.insert(buttons.end(), second.begin(), second.end());
    return buttons;
}

int MainWindow::modelToolbarButtonCount() const {
    return static_cast<int>(ToolbarButtons(modelToolBar_).size());
}
int MainWindow::modelToolbarButtonsWithIcons() const {
    int count = 0;
    for (const QAction* action : ToolbarButtons(modelToolBar_))
        if (!action->icon().isNull()) ++count;
    return count;
}
std::string MainWindow::modelToolbarLabel(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(modelToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return std::string();
    // iconText(), which is what the BAR shows -- text() is the menu's wording
    // and carries an & mnemonic, so "Pocket" does not appear in it at all.
    return buttons[static_cast<std::size_t>(index)]->iconText().toStdString();
}
bool MainWindow::modelToolbarButtonEnabled(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(modelToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return false;
    return buttons[static_cast<std::size_t>(index)]->isEnabled();
}
unsigned long long MainWindow::modelToolbarIconFingerprint(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(modelToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return 0;
    const QIcon icon = buttons[static_cast<std::size_t>(index)]->icon();
    if (icon.isNull()) return 0;
    const QImage image = icon.pixmap(QSize(ui::size::kToolbarIcon, ui::size::kToolbarIcon))
                             .toImage()
                             .convertToFormat(QImage::Format_RGBA8888);
    unsigned long long hash = 1469598103934665603ULL; // FNV-1a
    for (int y = 0; y < image.height(); ++y) {
        const uchar* line = image.constScanLine(y);
        for (int x = 0; x < image.bytesPerLine(); ++x) {
            hash ^= static_cast<unsigned long long>(line[x]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

int MainWindow::mainToolbarButtonCount() const {
    return static_cast<int>(ToolbarButtons(mainToolBar_).size());
}
int MainWindow::mainToolbarButtonsWithIcons() const {
    int count = 0;
    for (const QAction* action : ToolbarButtons(mainToolBar_))
        if (!action->icon().isNull()) ++count;
    return count;
}
std::string MainWindow::mainToolbarLabel(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(mainToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return std::string();
    return buttons[static_cast<std::size_t>(index)]->text().toStdString();
}
unsigned long long MainWindow::mainToolbarIconFingerprint(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(mainToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return 0;
    const QIcon icon = buttons[static_cast<std::size_t>(index)]->icon();
    if (icon.isNull()) return 0;
    const QImage image = icon.pixmap(QSize(ui::size::kToolbarIcon, ui::size::kToolbarIcon))
                             .toImage()
                             .convertToFormat(QImage::Format_RGBA8888);
    unsigned long long hash = 1469598103934665603ULL; // FNV-1a
    for (int y = 0; y < image.height(); ++y) {
        const uchar* line = image.constScanLine(y);
        for (int x = 0; x < image.bytesPerLine(); ++x) {
            hash ^= static_cast<unsigned long long>(line[x]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

std::string MainWindow::checkedSketchToolLabel() const {
    for (const QAction* action : sketchToolbarButtons()) {
        if (!action->isCheckable() || !action->isChecked()) continue;
        return action->text().toStdString();
    }
    return std::string();
}

bool MainWindow::extendButtonChecked() const {
    return extendAction_ != nullptr && extendAction_->isChecked();
}

bool MainWindow::editSketchEnabled() const {
    return editSketchAction_ != nullptr && editSketchAction_->isEnabled();
}

bool MainWindow::trimButtonChecked() const {
    return trimAction_ != nullptr && trimAction_->isChecked();
}

int MainWindow::sketchToolbarButtonCount() const {
    return static_cast<int>(sketchToolbarButtons().size());
}

int MainWindow::sketchToolbarButtonsWithIcons() const {
    int count = 0;
    for (const QAction* action : sketchToolbarButtons())
        if (!action->icon().isNull()) ++count;
    return count;
}

unsigned long long MainWindow::sketchToolbarIconFingerprint(int index) const {
    const std::vector<QAction*> buttons = sketchToolbarButtons();
    if (index < 0 || index >= static_cast<int>(buttons.size())) return 0;
    const QIcon icon = buttons[static_cast<std::size_t>(index)]->icon();
    if (icon.isNull()) return 0;

    // Rendered at a SIZE, then hashed. Hashing the QIcon itself would compare
    // handles rather than pictures, and two different handles can carry the
    // same drawing.
    const QImage image =
        icon.pixmap(QSize(ui::size::kSketchToolbarIcon, ui::size::kSketchToolbarIcon))
            .toImage()
            .convertToFormat(QImage::Format_RGBA8888);
    unsigned long long hash = 1469598103934665603ULL; // FNV-1a
    for (int y = 0; y < image.height(); ++y) {
        const uchar* line = image.constScanLine(y);
        for (int x = 0; x < image.bytesPerLine(); ++x) {
            hash ^= static_cast<unsigned long long>(line[x]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

std::string MainWindow::sketchToolbarTooltip(int index) const {
    const std::vector<QAction*> buttons = sketchToolbarButtons();
    if (index < 0 || index >= static_cast<int>(buttons.size())) return std::string();
    return buttons[static_cast<std::size_t>(index)]->toolTip().toStdString();
}

int MainWindow::displayedConstraintRowCount() const {
    return constraints_ != nullptr ? constraints_->rowCount() : 0;
}

std::string MainWindow::displayedConstraintText(int row) const {
    if (constraints_ == nullptr || row < 0 || row >= constraints_->rowCount()) return std::string();
    std::string text;
    for (int column = 0; column < constraints_->columnCount(); ++column) {
        const QTableWidgetItem* item = constraints_->item(row, column);
        if (item == nullptr) continue;
        if (!text.empty()) text += " | ";
        text += item->text().toStdString();
    }
    return text;
}

std::string MainWindow::displayedSketchStatus() const {
    return statusLeft_ != nullptr ? statusLeft_->text().toStdString() : std::string();
}

int MainWindow::selectedConstraintRow() const {
    if (constraints_ == nullptr || constraints_->selectedItems().isEmpty()) return -1;
    return constraints_->currentRow();
}

unsigned long long MainWindow::displayedConstraintId(int row) const {
    if (constraints_ == nullptr || row < 0 || row >= constraints_->rowCount()) return 0;
    const QTableWidgetItem* item = constraints_->item(row, 0);
    return item != nullptr ? item->data(kIdRole).toULongLong() : 0;
}

bool MainWindow::selectConstraintRow(int row) {
    if (constraints_ == nullptr || row < 0 || row >= constraints_->rowCount()) return false;
    constraints_->setCurrentCell(row, 0);
    return true;
}

QString MainWindow::deleteSelectedConstraintRow() {
    if (constraints_ == nullptr || sketchCanvas_ == nullptr)
        return QStringLiteral("No constraint panel");
    const int row = constraints_->currentRow();
    const QTableWidgetItem* item = row >= 0 ? constraints_->item(row, 0) : nullptr;
    if (item == nullptr) {
        const QString message = QStringLiteral("Select a constraint row to delete");
        statusLeft_->setText(message);
        return message;
    }
    const auto id = static_cast<SketchConstraintId>(
        static_cast<ObjectId>(item->data(kIdRole).toULongLong()));
    const QString status = sketchCanvas_->deleteConstraint(id);
    sketchMessage_ = status;
    updateSketchStatus();
    rebuildConstraintPanel();
    return status;
}

void MainWindow::onTransformRequested() {
    if (sketchCanvas_ == nullptr || !inSketchMode()) return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Transform"));
    auto* form = new QFormLayout(&dialog);

    auto* mode = new QComboBox(&dialog);
    mode->addItem(QStringLiteral("Move"));
    mode->addItem(QStringLiteral("Rotate"));
    mode->addItem(QStringLiteral("Scale"));
    form->addRow(QStringLiteral("What"), mode);

    auto* du = new QDoubleSpinBox(&dialog);
    du->setRange(-1.0e6, 1.0e6);
    du->setDecimals(3);
    du->setValue(10.0);
    auto* dv = new QDoubleSpinBox(&dialog);
    dv->setRange(-1.0e6, 1.0e6);
    dv->setDecimals(3);
    dv->setValue(0.0);
    form->addRow(QStringLiteral("dX (mm)"), du);
    form->addRow(QStringLiteral("dY (mm)"), dv);

    auto* angle = new QDoubleSpinBox(&dialog);
    angle->setRange(-3600.0, 3600.0);
    angle->setDecimals(3);
    angle->setValue(90.0);
    form->addRow(QStringLiteral("Angle (deg, CCW)"), angle);

    auto* factor = new QDoubleSpinBox(&dialog);
    factor->setRange(0.001, 1000.0);
    factor->setDecimals(4);
    factor->setValue(2.0);
    form->addRow(QStringLiteral("Scale"), factor);

    auto* keep = new QCheckBox(QStringLiteral("Leave a copy behind"), &dialog);
    form->addRow(QString(), keep);

    // ONLY THE ROW THAT APPLIES. Three number boxes where two are ignored is a
    // dialog that invites the user to fill in a value nothing reads.
    const auto showRelevant = [&]() {
        const int which = mode->currentIndex();
        form->setRowVisible(du, which == 0);
        form->setRowVisible(dv, which == 0);
        form->setRowVisible(angle, which == 1);
        form->setRowVisible(factor, which == 2);
    };
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [&showRelevant](int) { showRelevant(); });
    showRelevant();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    SketchTransform request;
    request.kind = mode->currentIndex() == 0   ? SketchTransformKind::Move
                   : mode->currentIndex() == 1 ? SketchTransformKind::Rotate
                                               : SketchTransformKind::Scale;
    request.deltaMm = Vec2{du->value(), dv->value()};
    // DEGREES IN THE DIALOG, radians in the model -- converted once, here, at
    // the boundary where the user's unit stops being the model's.
    request.angleRad = angle->value() * 3.14159265358979323846 / 180.0;
    request.factor = factor->value();
    request.keepACopy = keep->isChecked();

    const QString status = sketchCanvas_->applyTransform(request);
    if (!status.isEmpty()) {
        sketchMessage_ = status;
        updateSketchStatus();
    }
    rebuildConstraintPanel();
}

void MainWindow::onFilletRequested() {
    if (sketchCanvas_ == nullptr || !inSketchMode()) return;
    bool accepted = false;
    const double radius = QInputDialog::getDouble(
        this, QStringLiteral("Fillet"), QStringLiteral("Radius in mm."), 5.0, 0.001, 1.0e6, 3,
        &accepted);
    if (!accepted) return;
    const QString status = sketchCanvas_->applyFillet(radius);
    if (!status.isEmpty()) {
        sketchMessage_ = status;
        sketchMessageSticky_ = !status.contains(QStringLiteral("Rounded"));
        updateSketchStatus();
    }
    rebuildConstraintPanel();
}

void MainWindow::onChamferRequested() {
    if (sketchCanvas_ == nullptr || !inSketchMode()) return;
    bool accepted = false;
    const double first = QInputDialog::getDouble(
        this, QStringLiteral("Chamfer"),
        QStringLiteral("Setback along the FIRST selected line, in mm."), 5.0, 0.001, 1.0e6, 3,
        &accepted);
    if (!accepted) return;
    // ASKED SEPARATELY, and defaulted to the first: an equal chamfer is the
    // common case, and offering it as the default costs one Enter instead of
    // making the user type the same number twice.
    const double second = QInputDialog::getDouble(
        this, QStringLiteral("Chamfer"),
        QStringLiteral("Setback along the SECOND selected line, in mm."), first, 0.001, 1.0e6, 3,
        &accepted);
    if (!accepted) return;

    const QString status = sketchCanvas_->applyChamfer(first, second);
    if (!status.isEmpty()) {
        sketchMessage_ = status;
        updateSketchStatus();
    }
    rebuildConstraintPanel();
}

void MainWindow::onOffsetRequested() {
    if (sketchCanvas_ == nullptr || !inSketchMode()) return;
    // ASKED FOR, not remembered. A remembered distance is the shape of "why did
    // that jump 50 mm" -- the last value is invisible, and the command is rare
    // enough that typing it is not the cost.
    bool accepted = false;
    const double distance = QInputDialog::getDouble(
        this, QStringLiteral("Offset"),
        QStringLiteral("Distance in mm.\n"
                       "Negative offsets the other way (right of a line's direction,\n"
                       "or inward for a circle or arc)."),
        10.0, -1.0e6, 1.0e6, 3, &accepted);
    if (!accepted) return;

    const QString status = sketchCanvas_->applyOffset(distance);
    if (!status.isEmpty()) {
        sketchMessage_ = status;
        updateSketchStatus();
    }
    rebuildConstraintPanel();
}

void MainWindow::onConstraintPickedOnCanvas(qulonglong constraintId) {
    if (constraints_ == nullptr) return;
    // Move the PANEL to what was clicked on the canvas, so the two views never
    // disagree about which constraint is being looked at. Setting the row emits
    // itemSelectionChanged, which puts the highlight back through
    // onConstraintRowHighlighted -- harmless, because setHighlightedConstraint
    // returns early when the id has not changed, so there is no loop.
    if (constraintId == 0) {
        constraints_->clearSelection();
        constraints_->setCurrentCell(-1, -1);
        onConstraintRowHighlighted();
        return;
    }
    for (int i = 0; i < constraints_->rowCount(); ++i) {
        if (displayedConstraintId(i) != constraintId) continue;
        constraints_->setCurrentCell(i, 0);
        // Scrolled into view: a row selected below the fold is a selection the
        // user cannot see, which is the same as no feedback at all.
        constraints_->scrollToItem(constraints_->item(i, 0));
        return;
    }
}

void MainWindow::onConstraintRowHighlighted() {
    if (constraints_ == nullptr) return;
    const int row = constraints_->currentRow();
    const QTableWidgetItem* item =
        (row >= 0 && !constraints_->selectedItems().isEmpty()) ? constraints_->item(row, 0)
                                                               : nullptr;
    const SketchConstraintId id =
        item != nullptr ? static_cast<SketchConstraintId>(
                              static_cast<ObjectId>(item->data(kIdRole).toULongLong()))
                        : kInvalidSketchConstraintId;
    if (sketchCanvas_ != nullptr) sketchCanvas_->setHighlightedConstraint(id);
    // The button follows the selection: an always-enabled delete button that
    // answers "select a row first" is a control that looks available and is
    // not.
    if (deleteConstraintButton_ != nullptr)
        deleteConstraintButton_->setEnabled(id != kInvalidSketchConstraintId);
    if (deleteConstraintAction_ != nullptr && inSketchMode())
        deleteConstraintAction_->setEnabled(id != kInvalidSketchConstraintId);
}

bool MainWindow::constraintDeleteButtonEnabled() const {
    return deleteConstraintButton_ != nullptr && deleteConstraintButton_->isEnabled();
}

std::string MainWindow::constraintDeleteButtonText() const {
    return deleteConstraintButton_ != nullptr
               ? deleteConstraintButton_->text().toStdString()
               : std::string();
}

QString MainWindow::clickConstraintDeleteButton() {
    if (deleteConstraintButton_ == nullptr) return QStringLiteral("No delete button");
    if (!deleteConstraintButton_->isEnabled())
        return QStringLiteral("Select a constraint row to delete");
    deleteConstraintButton_->click();
    return sketchMessage_;
}

void MainWindow::onConstraintRowActivated(int row, int column) {
    Q_UNUSED(column);
    if (constraints_ == nullptr || sketchCanvas_ == nullptr) return;
    const QTableWidgetItem* item = constraints_->item(row, 0);
    if (item == nullptr) return;
    onDimensionActivated(item->data(kIdRole).toULongLong());
}

void MainWindow::onDimensionActivated(qulonglong constraintId) {
    if (document_ == nullptr || sketchCanvas_ == nullptr || !inSketchMode()) return;
    const Sketch* sketch = document_->findSketch(editingSketch_);
    if (sketch == nullptr) return;
    const auto id = static_cast<SketchConstraintId>(static_cast<ObjectId>(constraintId));
    const std::string current = DimensionEditText(*document_, *sketch, id);
    if (current.empty()) return; // not a dimension: nothing to edit

    bool accepted = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("Edit dimension"),
        QStringLiteral("Value, or an expression such as #d1 / 2.\n"
                       "Angles are in degrees; lengths in mm."),
        QLineEdit::Normal, QString::fromStdString(current), &accepted);
    if (!accepted) return;

    const QString status = sketchCanvas_->commitDimensionText(id, typed);
    sketchMessage_ = status;
    updateSketchStatus();
    rebuildConstraintPanel();
}

QString MainWindow::applyDimensionFormat(SketchConstraintId constraintId,
                                        const QString& prefix, const QString& suffix,
                                        double plusDisplay, double minusDisplay) {
    if (sketchCanvas_ == nullptr) return QStringLiteral("No sketch canvas");
    const QString status = sketchCanvas_->commitDimensionFormat(constraintId, prefix, suffix,
                                                                plusDisplay, minusDisplay);
    sketchMessage_ = status;
    updateSketchStatus();
    rebuildConstraintPanel();
    return status;
}

void MainWindow::onDimensionFormatRequested() {
    if (sketchCanvas_ == nullptr || constraints_ == nullptr || !inSketchMode()) return;
    const int row = constraints_->currentRow();
    const QTableWidgetItem* item = row >= 0 ? constraints_->item(row, 0) : nullptr;
    if (item == nullptr) {
        sketchMessage_ = QStringLiteral("Select a dimension in the Constraints panel first.");
        updateSketchStatus();
        return;
    }
    const auto id = static_cast<SketchConstraintId>(
        static_cast<ObjectId>(item->data(kIdRole).toULongLong()));

    QString prefix;
    QString suffix;
    double plus = 0.0;
    double minus = 0.0;
    if (!sketchCanvas_->dimensionFormatOf(id, &prefix, &suffix, &plus, &minus)) {
        sketchMessage_ = QStringLiteral("That row is a constraint, not a dimension.");
        updateSketchStatus();
        return;
    }

    // Four fields, not a coded string. A syntax like `2x|50|REF +0.1/-0.05`
    // would be one more thing to learn and to get wrong, and the tolerance
    // fields being NUMERIC is what stops "0,1" reaching the model.
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Dimension format"));
    auto* form = new QFormLayout(&dialog);

    auto* prefixEdit = new QLineEdit(prefix, &dialog);
    prefixEdit->setPlaceholderText(QStringLiteral("e.g. 2x "));
    auto* suffixEdit = new QLineEdit(suffix, &dialog);
    suffixEdit->setPlaceholderText(QStringLiteral("e.g.  REF"));
    auto* plusSpin = new QDoubleSpinBox(&dialog);
    auto* minusSpin = new QDoubleSpinBox(&dialog);
    for (QDoubleSpinBox* spin : {plusSpin, minusSpin}) {
        spin->setDecimals(4);
        spin->setRange(0.0, 1000.0);
        spin->setSingleStep(0.05);
    }
    plusSpin->setValue(plus);
    minusSpin->setValue(minus);

    form->addRow(QStringLiteral("Prefix"), prefixEdit);
    form->addRow(QStringLiteral("Suffix"), suffixEdit);
    form->addRow(QStringLiteral("Upper tolerance +"), plusSpin);
    form->addRow(QStringLiteral("Lower tolerance -"), minusSpin);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;
    applyDimensionFormat(id, prefixEdit->text(), suffixEdit->text(), plusSpin->value(),
                         minusSpin->value());
}

QString MainWindow::applySketchCommand(SketchEditKind kind, bool dimension) {
    if (sketchCanvas_ == nullptr) return QString();
    const QString status = dimension ? sketchCanvas_->applyDimension(kind)
                                     : sketchCanvas_->applyConstraint(kind);
    // A refusal is REPORTED, and it STAYS. A constraint command that silently
    // does nothing is exactly the failure roadmap 8 is written against -- and a
    // message erased by the next mouse move is silent in every way that
    // matters.
    if (!status.isEmpty()) {
        sketchMessage_ = status;
        sketchMessageSticky_ = !sketchCanvas_->lastCommandApplied();
        updateSketchStatus();
    }
    rebuildConstraintPanel();
    return status;
}

std::string MainWindow::displayedSketchMessage() const {
    return sketchMessage_.toStdString();
}

void MainWindow::onSketchDocumentChanged(const QString& status) {
    refreshAll();
    rebuildConstraintPanel();
    sketchMessage_ = status;
    // Something actually happened, so whatever refusal was on screen has been
    // answered and goes back to being transient.
    sketchMessageSticky_ = false;
    updateSketchStatus();
}

void MainWindow::reportSketchOrPlainStatus(const QString& message) {
    // In sketch mode a message goes where the PROMPT goes, so the constraint
    // badge survives it; outside sketch mode it owns the line.
    if (inSketchMode()) {
        sketchMessage_ = message;
        updateSketchStatus();
        return;
    }
    if (statusLeft_ != nullptr) statusLeft_->setText(message);
}

void MainWindow::syncSketchToolButtons() {
    if (sketchToolBar_ == nullptr || sketchCanvas_ == nullptr) return;
    // Trim is a mode the canvas can leave on its own (Esc), so its button is
    // read back here with the rest rather than set where it is clicked.
    if (trimAction_ != nullptr) trimAction_->setChecked(sketchCanvas_->trimming());
    if (extendAction_ != nullptr) extendAction_->setChecked(sketchCanvas_->extending());
    if (useReferenceAction_ != nullptr)
        useReferenceAction_->setChecked(sketchCanvas_->useReference());
    if (dimensionToolAction_ != nullptr)
        dimensionToolAction_->setChecked(sketchCanvas_->dimensioning());
    const int active = static_cast<int>(sketchCanvas_->tool());
    for (QAction* action : sketchToolBar_->actions()) {
        if (!action->isCheckable()) continue;
        // Trim is checkable but is NOT one of the drawing tools -- it carries
        // no tool data, and this loop would read that as "not the active tool"
        // and switch it off the instant it was switched on. It is synced above,
        // from the canvas, like everything else here.
        if (action == trimAction_ || action == extendAction_ ||
            action == useReferenceAction_ || action == dimensionToolAction_)
            continue;
        const QVariant which = action->data();
        action->setChecked(which.isValid() && which.toInt() == active);
    }
}

void MainWindow::onSketchPresentationChanged() {
    // The tool can change without any button being clicked -- Esc leaves the
    // tool (ADR-M17-002), and switching tools discards a half-drawn shape. When
    // the checked state was only updated inside the button's own handler, Esc
    // put the canvas back on the arrow while the toolbar went on showing
    // Rectangle pressed: the model was right and the screen was wrong, which is
    // the one defect class this shell is built to keep catching.
    syncSketchToolButtons();
    updateSketchStatus();
    // Shown once -- UNLESS it is a refusal.
    //
    // An ordinary report ("added 1 entity") should give way to the prompt on
    // the next repaint, or the line gets stuck describing something the user
    // has moved on from. A REFUSAL is the opposite: it is the only explanation
    // of why nothing happened, and clearing it on the next repaint meant the
    // first twitch of the mouse erased it. It stays until the user does
    // something else.
    if (!sketchMessageSticky_) sketchMessage_.clear();
}

void MainWindow::onNewSketchRequested() { newSketchCommand(); }
void MainWindow::onSketchOnFaceRequested() { sketchOnFaceCommand(); }
void MainWindow::onEditSketchRequested() { editSelectedSketchCommand(); }
void MainWindow::onFinishSketchRequested() { finishSketchCommand(); }

} // namespace paramcad
