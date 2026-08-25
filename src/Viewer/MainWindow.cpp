#include "Viewer/MainWindow.h"
#include "Core/Feature/TransformFeatures.h"
#include "Core/Feature/ImportFeature.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Feature/ShellFeature.h"
#include "Core/Feature/LoftFeature.h"
#include "Core/Feature/SweepFeature.h"
#include <set>
#include <stdexcept>

#include "Cli/SketchScript.h"
#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/AssemblyStates.h"
#include "Core/Assembly/Instance.h"
#include "Core/Assembly/Mate.h"
#include "Core/Document/DocumentBase.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Core/Serialization/DocumentJson.h"
#include "Viewer/AssemblyOutline.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"
#include "Viewer/DrawingOutline.h"
#include "Viewer/DrawingCanvas.h"
#include "Core/Electrical/SymbolLibrary.h"
#include "Core/Export/DxfWriter.h"
#include "Viewer/DrawingPlot.h"
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
#include <QFile>
#include <QFileDialog>
#include "Core/Serialization/PartDocumentSerializer.h"
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
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
    drawingCanvas_ = new DrawingCanvasWidget(this);
    centralStack_->addWidget(sketchCanvas_);
    centralStack_->addWidget(drawingCanvas_);
    centralStack_->setCurrentWidget(viewer_);
    setCentralWidget(centralStack_);

    // --- The drawing canvas talks back (M33/M34) ----------------------------
    //
    // THREE SIGNALS, THREE COMMANDS. The canvas reports what the pointer did;
    // every change to the document goes through a command on this window, so
    // there is one path in and the undo stack sees all of it.
    connect(drawingCanvas_, &DrawingCanvasWidget::toolFinished, this,
            [this](DrawingTool tool, std::vector<Vec2> points) {
                drawShapeCommand(tool, points);
                setDrawingToolCommand(DrawingTool::None);
            });
    connect(drawingCanvas_, &DrawingCanvasWidget::objectPicked, this, [this](ObjectId id) {
        selectObject(id);
    });
    connect(drawingCanvas_, &DrawingCanvasWidget::dimensionDropped, this,
            [this](ObjectId id, Vec2 at) { moveDimensionCommand(id, at); });

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
    resizeDocks({treeDock_}, {ui::size::kModelTreeMinWidth + 100}, Qt::Horizontal);

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
    // RUN A SCRIPT (M26.6). The same vocabulary the CLI and the socket take,
    // reachable from the window -- so an .ep3ds is something a user can open
    // rather than something they need a terminal for.
    QAction* runScript = file->addAction(QStringLiteral("&Run Script..."));
    runScript->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    runScript->setToolTip(
        QStringLiteral("Run an .ep3ds script against THIS document.\n"
                       "It adds to what is already open -- File > New first to start empty.\n"
                       "Each command is its own undo step, as it is over the script socket."));
    connect(runScript, &QAction::triggered, this, &MainWindow::onRunScriptRequested);
    file->addSeparator();
    QAction* quit = file->addAction(QStringLiteral("E&xit"));
    connect(quit, &QAction::triggered, this, &QWidget::close);

    // --- Assembly (M28) -----------------------------------------------------
    //
    // ITS OWN MENU, not entries mixed into Insert. Insert builds features on a
    // part and every one of its items is disabled on an assembly; putting
    // "Insert Part" among them would put the one enabled item in a list of
    // fourteen greyed ones, which reads as a mistake rather than as a menu.
    //
    // The whole menu is disabled on a part, so it is visible and inert rather
    // than appearing and vanishing -- a menu that comes and goes teaches a user
    // that the application is unpredictable.
    assemblyMenu_ = menuBar()->addMenu(QStringLiteral("&Assembly"));
    insertInstanceAction_ = assemblyMenu_->addAction(QStringLiteral("Insert &Part..."));
    insertInstanceAction_->setToolTip(
        QStringLiteral("Add a part or sub-assembly to this assembly.\n"
                       "It is placed at the origin; move it, ground it, or mate it."));
    connect(insertInstanceAction_, &QAction::triggered, this,
            &MainWindow::onInsertInstanceRequested);

    assemblyMenu_->addSeparator();
    groundInstanceAction_ = assemblyMenu_->addAction(QStringLiteral("&Ground / Unground"));
    groundInstanceAction_->setToolTip(
        QStringLiteral("Pin the selected instance where it is.\n"
                       "A mate solve needs something that does not move."));
    connect(groundInstanceAction_, &QAction::triggered, this,
            &MainWindow::onGroundInstanceRequested);

    patternInstanceAction_ = assemblyMenu_->addAction(QStringLiteral("Pattern &Instance..."));
    patternInstanceAction_->setToolTip(
        QStringLiteral("Repeat the selected instance in a row.\n"
                       "The copies are ordinary instances, not a stored feature: to change "
                       "the count, delete them and pattern again."));
    connect(patternInstanceAction_, &QAction::triggered, this,
            &MainWindow::onPatternInstanceRequested);

    assemblyMenu_->addSeparator();
    addMateAction_ = assemblyMenu_->addAction(QStringLiteral("Add &Mate..."));
    addMateAction_->setToolTip(
        QStringLiteral("Mate the two selected instances by naming a connector on each.\n"
                       "The TYPE is yours to choose -- a mate picked for you is a "
                       "constraint you did not ask for."));
    connect(addMateAction_, &QAction::triggered, this, &MainWindow::onAddMateRequested);

    driveMateAction_ = assemblyMenu_->addAction(QStringLiteral("Dri&ve Mate..."));
    driveMateAction_->setToolTip(
        QStringLiteral("Set the selected mate's free value and hold it there.\n"
                       "This is how a mechanism is moved without dragging it."));
    connect(driveMateAction_, &QAction::triggered, this, &MainWindow::onDriveMateRequested);

    limitMateAction_ = assemblyMenu_->addAction(QStringLiteral("&Limit Mate..."));
    limitMateAction_->setToolTip(
        QStringLiteral("Stop the selected mate at a lower and an upper bound.\n"
                       "A value outside them is held at the nearer end, not refused."));
    connect(limitMateAction_, &QAction::triggered, this, &MainWindow::onLimitMateRequested);

    deleteMateAction_ = assemblyMenu_->addAction(QStringLiteral("Delete M&ate"));
    deleteMateAction_->setToolTip(
        QStringLiteral("Delete the selected mate.\n"
                       "The instances it held stay -- a mate holds them, it does not own them."));
    connect(deleteMateAction_, &QAction::triggered, this, &MainWindow::onDeleteMateRequested);

    // --- Relations (M31, §20.5) ---------------------------------------------
    //
    // THEIR OWN GROUP, after the mates. A relation is not a mate and the menu
    // has to say so: a mate decides where two parts are, a relation decides
    // that two numbers a mate solve was free to choose move together.
    assemblyMenu_->addSeparator();
    addRelationAction_ = assemblyMenu_->addAction(QStringLiteral("Add &Relation..."));
    addRelationAction_->setToolTip(
        QStringLiteral("Couple the selected mates so one drives the other:\n"
                       "a gear, a rack and pinion, a screw or a linear ratio.\n"
                       "Select TWO mates -- or one, for a screw."));
    connect(addRelationAction_, &QAction::triggered, this, &MainWindow::onAddRelationRequested);

    relationRatioAction_ = assemblyMenu_->addAction(QStringLiteral("Relation Rat&io..."));
    relationRatioAction_->setToolTip(
        QStringLiteral("Change the selected relation's ratio.\n"
                       "Turns per turn for a gear; MILLIMETRES PER TURN for a screw or a "
                       "rack."));
    connect(relationRatioAction_, &QAction::triggered, this,
            &MainWindow::onRelationRatioRequested);

    reverseRelationAction_ = assemblyMenu_->addAction(QStringLiteral("Re&verse Relation"));
    reverseRelationAction_->setToolTip(
        QStringLiteral("Turn the driven end the other way.\n"
                       "Two meshing gears turn opposite ways; this is how that is said."));
    connect(reverseRelationAction_, &QAction::triggered, this,
            &MainWindow::onReverseRelationRequested);

    deleteRelationAction_ = assemblyMenu_->addAction(QStringLiteral("Delete Relati&on"));
    deleteRelationAction_->setToolTip(
        QStringLiteral("Delete the selected relation.\n"
                       "The mates it coupled stay, and the freedom it was driving is the "
                       "solve's again."));
    connect(deleteRelationAction_, &QAction::triggered, this,
            &MainWindow::onDeleteRelationRequested);

    // --- Drawings (M32.4, §24) ----------------------------------------------
    //
    // ITS OWN MENU, beside Assembly. A drawing is a third document type, not a
    // mode of either of the others -- and a menu that buried its commands
    // under "Model" would be saying otherwise.
    drawingMenu_ = menuBar()->addMenu(QStringLiteral("&Drawing"));
    newDrawingAction_ = drawingMenu_->addAction(QStringLiteral("&New Drawing"));
    newDrawingAction_->setToolTip(
        QStringLiteral("Start a drawing: a sheet of paper to put views of a model on."));
    connect(newDrawingAction_, &QAction::triggered, this, &MainWindow::onNewDrawingRequested);

    drawingMenu_->addSeparator();
    addBaseViewAction_ = drawingMenu_->addAction(QStringLiteral("Add &View..."));
    addBaseViewAction_->setToolTip(
        QStringLiteral("Put a view of a model on the sheet.\n"
                       "Hidden lines are worked out; the view stays a view of that FILE, so "
                       "editing the model updates it."));
    connect(addBaseViewAction_, &QAction::triggered, this, &MainWindow::onAddBaseViewRequested);

    addProjectedViewAction_ = drawingMenu_->addAction(QStringLiteral("&Project View..."));
    addProjectedViewAction_->setToolTip(
        QStringLiteral("Project another side off the selected view.\n"
                       "It stays LINED UP with its parent, which is what lets a reader carry "
                       "a measurement between them."));
    connect(addProjectedViewAction_, &QAction::triggered, this,
            &MainWindow::onAddProjectedViewRequested);

    updateViewsAction_ = drawingMenu_->addAction(QStringLiteral("&Update Views"));
    updateViewsAction_->setToolTip(
        QStringLiteral("Redraw the views whose models have changed.\n"
                       "Only those -- redrawing the rest would re-run hidden-line removal for "
                       "nothing."));
    connect(updateViewsAction_, &QAction::triggered, this, &MainWindow::onUpdateViewsRequested);

    drawingMenu_->addSeparator();
    sheetSetupAction_ = drawingMenu_->addAction(QStringLiteral("&Sheet..."));
    sheetSetupAction_->setToolTip(
        QStringLiteral("Paper size, which way round, scale, and the projection angle.\n"
                       "The angle decides which side every projected view falls on."));
    connect(sheetSetupAction_, &QAction::triggered, this, &MainWindow::onSheetSetupRequested);

    addLayerAction_ = drawingMenu_->addAction(QStringLiteral("Add &Layer..."));
    addLayerAction_->setToolTip(QStringLiteral("A new layer, with a colour and a linetype."));
    connect(addLayerAction_, &QAction::triggered, this, &MainWindow::onAddDrawingLayerRequested);

    // --- The frame and the title block (M35) --------------------------------
    titleBlockAction_ = drawingMenu_->addAction(QStringLiteral("&Title Block..."));
    titleBlockAction_->setToolTip(
        QStringLiteral("What this drawing IS: its title, its number, who drew it.\n"
                       "The scale, the size and the projection are filled in from the "
                       "sheet and cannot be typed."));
    connect(titleBlockAction_, &QAction::triggered, this, &MainWindow::onTitleBlockRequested);
    frameAction_ = drawingMenu_->addAction(QStringLiteral("&Frame..."));
    frameAction_->setToolTip(
        QStringLiteral("The border and its margins, in millimetres.\n"
                       "The binding edge is wider because that is the edge it is filed on."));
    connect(frameAction_, &QAction::triggered, this, &MainWindow::onFrameRequested);

    // --- Schematic (M36) -----------------------------------------------------
    drawingMenu_->addSeparator();
    placeSymbolAction_ = drawingMenu_->addAction(QStringLiteral("Place &Component..."));
    placeSymbolAction_->setToolTip(
        QStringLiteral("An IEC symbol with PINS -- which is what makes it a component\n"
                       "rather than a picture of one. Its tag is chosen from the symbol's "
                       "kind: R for a resistor, K for a contactor, X for a terminal."));
    connect(placeSymbolAction_, &QAction::triggered, this,
            &MainWindow::onPlaceSymbolRequested);
    drawWireAction_ = drawingMenu_->addAction(QStringLiteral("Draw &Wire"));
    drawWireAction_->setCheckable(true);
    drawWireAction_->setToolTip(
        QStringLiteral("Two clicks. A wire is its own kind of object, not a line on a "
                       "layer:\nmoving a line to another layer must not be able to change "
                       "the circuit."));
    connect(drawWireAction_, &QAction::triggered, this, [this](bool on) {
        setDrawingToolCommand(on ? DrawingTool::Wire : DrawingTool::None);
    });
    turnSymbolAction_ = drawingMenu_->addAction(QStringLiteral("T&urn Component"));
    turnSymbolAction_->setToolTip(QStringLiteral("A quarter turn, anticlockwise."));
    connect(turnSymbolAction_, &QAction::triggered, this,
            [this] { turnSelectedSymbolCommand(1.5707963267948966); });
    numberNetsAction_ = drawingMenu_->addAction(QStringLiteral("&Number Nets"));
    numberNetsAction_->setToolTip(
        QStringLiteral("W1, W2, ... left to right, then bottom to top -- the order the "
                       "sheet is read.\nA net that already has a name keeps it."));
    connect(numberNetsAction_, &QAction::triggered, this, [this] { numberNetsCommand(); });

    addBomAction_ = drawingMenu_->addAction(QStringLiteral("&Parts List..."));
    addBomAction_->setToolTip(
        QStringLiteral("Counts an assembly, and keeps counting it.\n"
                       "The quantities are read from the assembly every time the list is "
                       "drawn -- there is no stored copy to go out of date."));
    connect(addBomAction_, &QAction::triggered, this, &MainWindow::onAddBomRequested);
    recountBomAction_ = drawingMenu_->addAction(QStringLiteral("Re-&count Parts Lists"));
    recountBomAction_->setToolTip(
        QStringLiteral("Marks every list as looked at. What they SHOW was never out of "
                       "date; this clears the flag that says the assembly has changed "
                       "since anybody checked."));
    connect(recountBomAction_, &QAction::triggered, this,
            [this] { recountBomCommand(); });

    exportDxfAction_ = drawingMenu_->addAction(QStringLiteral("&Export DXF..."));
    exportDxfAction_->setToolTip(
        QStringLiteral("R12, which every CAD program reads.\n"
                       "Views are flattened -- the curves go out where they sit on the "
                       "paper and stop following the model. Anything the format cannot "
                       "carry is listed after the export."));
    connect(exportDxfAction_, &QAction::triggered, this, &MainWindow::onExportDxfRequested);

    plotPdfAction_ = drawingMenu_->addAction(QStringLiteral("&Plot to PDF..."));
    plotPdfAction_->setToolTip(
        QStringLiteral("One page, at TRUE SIZE: a printed A3 measures A3 under a rule.\n"
                       "The drawing's scale is already in the views, so the page is 1:1."));
    connect(plotPdfAction_, &QAction::triggered, this, &MainWindow::onPlotPdfRequested);

    // --- Sheet geometry (M33) -----------------------------------------------
    //
    // CHECKABLE, because a tool is a MODE: the button stays pressed while the
    // pointer means "draw", and a user who cannot see which mode they are in
    // has to find out by clicking.
    drawingMenu_->addSeparator();
    drawLineAction_ = drawingMenu_->addAction(QStringLiteral("Draw &Line"));
    drawLineAction_->setCheckable(true);
    drawLineAction_->setToolTip(QStringLiteral("Two clicks. Snaps to what is already drawn."));
    connect(drawLineAction_, &QAction::triggered, this, [this](bool on) {
        setDrawingToolCommand(on ? DrawingTool::Line : DrawingTool::None);
    });
    drawCircleAction_ = drawingMenu_->addAction(QStringLiteral("Draw &Circle"));
    drawCircleAction_->setCheckable(true);
    drawCircleAction_->setToolTip(QStringLiteral("Centre, then a point on the rim."));
    connect(drawCircleAction_, &QAction::triggered, this, [this](bool on) {
        setDrawingToolCommand(on ? DrawingTool::Circle : DrawingTool::None);
    });
    drawRectangleAction_ = drawingMenu_->addAction(QStringLiteral("Draw &Rectangle"));
    drawRectangleAction_->setCheckable(true);
    drawRectangleAction_->setToolTip(
        QStringLiteral("Two opposite corners. Drawn as four lines, so each side can be "
                       "dimensioned and trimmed on its own."));
    connect(drawRectangleAction_, &QAction::triggered, this, [this](bool on) {
        setDrawingToolCommand(on ? DrawingTool::Rectangle : DrawingTool::None);
    });

    // --- Dimensions (M34) ---------------------------------------------------
    //
    // Six entries, not one "Dimension" that guesses: a circle can honestly
    // take a radius OR a diameter, and a tool that picked for the user would
    // be picking what the drawing SAYS about the part.
    drawingMenu_->addSeparator();
    linearDimensionAction_ = drawingMenu_->addAction(QStringLiteral("&Dimension"));
    linearDimensionAction_->setToolTip(
        QStringLiteral("The true distance between what is selected.\n"
                       "It measures the geometry, so it follows when the geometry moves."));
    connect(linearDimensionAction_, &QAction::triggered, this, [this] {
        onAddDimensionRequested(DimensionKind::Linear, LinearDirection::Aligned);
    });
    horizontalDimensionAction_ = drawingMenu_->addAction(QStringLiteral("&Horizontal Dimension"));
    horizontalDimensionAction_->setToolTip(
        QStringLiteral("Across only -- the X distance, whatever angle the geometry sits at."));
    connect(horizontalDimensionAction_, &QAction::triggered, this, [this] {
        onAddDimensionRequested(DimensionKind::Linear, LinearDirection::Horizontal);
    });
    verticalDimensionAction_ = drawingMenu_->addAction(QStringLiteral("&Vertical Dimension"));
    verticalDimensionAction_->setToolTip(QStringLiteral("Up and down only -- the Y distance."));
    connect(verticalDimensionAction_, &QAction::triggered, this, [this] {
        onAddDimensionRequested(DimensionKind::Linear, LinearDirection::Vertical);
    });
    radiusDimensionAction_ = drawingMenu_->addAction(QStringLiteral("&Radius"));
    radiusDimensionAction_->setToolTip(QStringLiteral("Of the selected circle or arc."));
    connect(radiusDimensionAction_, &QAction::triggered, this, [this] {
        onAddDimensionRequested(DimensionKind::Radius, LinearDirection::Aligned);
    });
    diameterDimensionAction_ = drawingMenu_->addAction(QStringLiteral("Dia&meter"));
    diameterDimensionAction_->setToolTip(
        QStringLiteral("Of the selected circle or arc. A hole is dimensioned by its "
                       "diameter, because that is the drill."));
    connect(diameterDimensionAction_, &QAction::triggered, this, [this] {
        onAddDimensionRequested(DimensionKind::Diameter, LinearDirection::Aligned);
    });
    angularDimensionAction_ = drawingMenu_->addAction(QStringLiteral("An&gle"));
    angularDimensionAction_->setToolTip(
        QStringLiteral("Between two selected lines, in degrees, about where they cross."));
    connect(angularDimensionAction_, &QAction::triggered, this, [this] {
        onAddDimensionRequested(DimensionKind::Angular, LinearDirection::Aligned);
    });

    dimensionTextAction_ = drawingMenu_->addAction(QStringLiteral("Dimension &Text..."));
    dimensionTextAction_->setToolTip(
        QStringLiteral("Show something else instead of the number.\n"
                       "It still MEASURES -- an override changes the text, never the size."));
    connect(dimensionTextAction_, &QAction::triggered, this,
            &MainWindow::onDimensionTextRequested);
    dimensionStyleAction_ = drawingMenu_->addAction(QStringLiteral("Dimension St&yle..."));
    dimensionStyleAction_->setToolTip(
        QStringLiteral("Text height, arrows, decimals and suffix, in paper millimetres.\n"
                       "Every dimension using the style follows."));
    connect(dimensionStyleAction_, &QAction::triggered, this,
            &MainWindow::onDimensionStyleRequested);

    drawingMenu_->addSeparator();
    deleteDrawingObjectAction_ = drawingMenu_->addAction(QStringLiteral("&Delete"));
    deleteDrawingObjectAction_->setToolTip(
        QStringLiteral("Delete the selected view or layer.\n"
                       "Deleting a view takes the views projected off it -- in one undo step."));
    connect(deleteDrawingObjectAction_, &QAction::triggered, this,
            &MainWindow::onDeleteDrawingObjectRequested);

    // --- The three state mechanisms (M30, §49) ------------------------------
    //
    // THREE SEPARATE GROUPS, because they are three separate mechanisms. §49
    // warns against merging them into one "view state", and a menu that listed
    // them together would be the first step in doing exactly that.
    assemblyMenu_->addSeparator();
    capturePositionAction_ = assemblyMenu_->addAction(QStringLiteral("&Capture Position..."));
    capturePositionAction_->setToolTip(
        QStringLiteral("Remember where everything is: every mate's value, and where the "
                       "instances no mate places are sitting."));
    connect(capturePositionAction_, &QAction::triggered, this,
            &MainWindow::onCaptureNamedPositionRequested);

    applyPositionAction_ = assemblyMenu_->addAction(QStringLiteral("App&ly Position"));
    applyPositionAction_->setToolTip(
        QStringLiteral("Move the assembly back to the selected position.\n"
                       "One undo step, because it is one thing you chose."));
    connect(applyPositionAction_, &QAction::triggered, this,
            &MainWindow::onApplyNamedPositionRequested);

    assemblyMenu_->addSeparator();
    addExplodeViewAction_ = assemblyMenu_->addAction(QStringLiteral("New &Exploded View..."));
    connect(addExplodeViewAction_, &QAction::triggered, this,
            &MainWindow::onAddExplodeViewRequested);
    addExplodeStepAction_ = assemblyMenu_->addAction(QStringLiteral("Add Explode &Step..."));
    addExplodeStepAction_->setToolTip(
        QStringLiteral("Move the selected instance out, as the next step of the selected "
                       "view.\nSteps can be previewed one at a time."));
    connect(addExplodeStepAction_, &QAction::triggered, this,
            &MainWindow::onAddExplodeStepRequested);
    showExplodeAction_ = assemblyMenu_->addAction(QStringLiteral("Sho&w Exploded View"));
    showExplodeAction_->setCheckable(true);
    showExplodeAction_->setToolTip(
        QStringLiteral("Draw the selected exploded view.\n"
                       "Nothing moves: an explosion is a picture, not an edit."));
    connect(showExplodeAction_, &QAction::triggered, this,
            &MainWindow::onShowExplodeViewRequested);
    explodePreviewAction_ = assemblyMenu_->addAction(QStringLiteral("Explode &Up To..."));
    explodePreviewAction_->setToolTip(
        QStringLiteral("Show only the first N steps -- the view's own rollback bar."));
    connect(explodePreviewAction_, &QAction::triggered, this,
            &MainWindow::onExplodePreviewRequested);

    assemblyMenu_->addSeparator();
    interferenceAction_ = assemblyMenu_->addAction(QStringLiteral("Check &Interference"));
    interferenceAction_->setToolTip(
        QStringLiteral("Find every pair of instances that overlap, and by how much."));
    connect(interferenceAction_, &QAction::triggered, this,
            &MainWindow::onCheckInterferenceRequested);

    assemblyMenu_->addSeparator();
    deleteInstanceAction_ = assemblyMenu_->addAction(QStringLiteral("&Delete Instance"));
    deleteInstanceAction_->setToolTip(
        QStringLiteral("Delete the selected instance.\n"
                       "Any mate that names it goes with it, and the message says how many."));
    connect(deleteInstanceAction_, &QAction::triggered, this,
            &MainWindow::onDeleteInstanceRequested);

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

    QMenu* exchange = menuBar()->addMenu(QStringLiteral("&Exchange"));
    exportAction_ = exchange->addAction(QStringLiteral("&Export Current Solid..."));
    connect(exportAction_, &QAction::triggered, this, &MainWindow::onExportRequested);
    importAction_ = exchange->addAction(QStringLiteral("&Import STEP..."));
    connect(importAction_, &QAction::triggered, this, &MainWindow::onImportRequested);

    QMenu* insert = menuBar()->addMenu(QStringLiteral("&Insert"));
    insertPadAction_ = insert->addAction(QStringLiteral("&Pad from Selected Sketch"));
    connect(insertPadAction_, &QAction::triggered, this, &MainWindow::onInsertPadRequested);
    insertPocketAction_ = insert->addAction(QStringLiteral("P&ocket from Selected Sketch"));
    connect(insertPocketAction_, &QAction::triggered, this, &MainWindow::onInsertPocketRequested);
    insertRevolveAction_ = insert->addAction(QStringLiteral("&Revolve Selected Sketch"));
    connect(insertRevolveAction_, &QAction::triggered, this,
            &MainWindow::onInsertRevolveRequested);
    insertSweepAction_ = insert->addAction(QStringLiteral("S&weep Two Selected Sketches"));
    connect(insertSweepAction_, &QAction::triggered, this, &MainWindow::onInsertSweepRequested);
    insertLoftAction_ = insert->addAction(QStringLiteral("&Loft Through Selected Sketches"));
    connect(insertLoftAction_, &QAction::triggered, this, &MainWindow::onInsertLoftRequested);
    insert->addSeparator();
    insertShellAction_ = insert->addAction(QStringLiteral("S&hell, Open at Picked Face"));
    connect(insertShellAction_, &QAction::triggered, this, &MainWindow::onInsertShellRequested);
    insertHoleAction_ = insert->addAction(QStringLiteral("&Hole at Selected Sketch Points"));
    connect(insertHoleAction_, &QAction::triggered, this, &MainWindow::onInsertHoleRequested);
    insert->addSeparator();
    insertUnionAction_ = insert->addAction(QStringLiteral("&Union the Two Solids"));
    connect(insertUnionAction_, &QAction::triggered, this, &MainWindow::onInsertUnionRequested);
    insertSubtractAction_ = insert->addAction(QStringLiteral("S&ubtract the Second Solid"));
    connect(insertSubtractAction_, &QAction::triggered, this,
            &MainWindow::onInsertSubtractRequested);
    insertIntersectAction_ = insert->addAction(QStringLiteral("&Intersect the Two Solids"));
    connect(insertIntersectAction_, &QAction::triggered, this,
            &MainWindow::onInsertIntersectRequested);
    insert->addSeparator();
    insertCircularPatternAction_ =
        insert->addAction(QStringLiteral("Circular &Pattern about the Origin"));
    connect(insertCircularPatternAction_, &QAction::triggered, this,
            &MainWindow::onInsertCircularPatternRequested);
    insertCurvePatternAction_ =
        insert->addAction(QStringLiteral("Pattern &Along Selected Sketch"));
    connect(insertCurvePatternAction_, &QAction::triggered, this,
            &MainWindow::onInsertCurvePatternRequested);
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
    // M19-M22, in the order a part is usually built: make material, take it
    // away, copy it, exchange it.
    insertSweepAction_->setIcon(icon(ui::SketchIcon::Sweep));
    insertSweepAction_->setIconText(QStringLiteral("Sweep"));
    insertSweepAction_->setToolTip(
        QStringLiteral("Sweep\nDrag the first selected sketch along the second.\n"
                       "Select TWO sketches -- profile first, path second, in tree order."));
    model->addAction(insertSweepAction_);

    insertLoftAction_->setIcon(icon(ui::SketchIcon::Loft));
    insertLoftAction_->setIconText(QStringLiteral("Loft"));
    insertLoftAction_->setToolTip(
        QStringLiteral("Loft\nSkin through two or more selected sketches.\n"
                       "The ORDER is the shape, and it is the order in the tree."));
    model->addAction(insertLoftAction_);

    insertShellAction_->setIcon(icon(ui::SketchIcon::Shell));
    insertShellAction_->setIconText(QStringLiteral("Shell"));
    insertShellAction_->setToolTip(
        QStringLiteral("Shell\nHollow the solid, leaving a wall.\n"
                       "Click the face to leave OPEN in the 3D view first."));
    model->addAction(insertShellAction_);

    insertHoleAction_->setIcon(icon(ui::SketchIcon::Hole));
    insertHoleAction_->setIconText(QStringLiteral("Hole"));
    insertHoleAction_->setToolTip(
        QStringLiteral("Hole\nDrill a bore at every point of the selected sketch.\n"
                       "It goes all the way through until you give it a Depth."));
    model->addAction(insertHoleAction_);

    model->addSeparator();
    insertUnionAction_->setIcon(icon(ui::SketchIcon::Union));
    insertUnionAction_->setIconText(QStringLiteral("Union"));
    insertUnionAction_->setToolTip(
        QStringLiteral("Union\nJoin the body's two separate solids into one.\n"
                       "Pad into the same body twice to make two."));
    model->addAction(insertUnionAction_);

    insertSubtractAction_->setIcon(icon(ui::SketchIcon::Subtract));
    insertSubtractAction_->setIconText(QStringLiteral("Subtract"));
    insertSubtractAction_->setToolTip(
        QStringLiteral("Subtract\nRemove the later solid from the earlier one."));
    model->addAction(insertSubtractAction_);

    insertIntersectAction_->setIcon(icon(ui::SketchIcon::Intersect));
    insertIntersectAction_->setIconText(QStringLiteral("Intersect"));
    insertIntersectAction_->setToolTip(
        QStringLiteral("Intersect\nKeep only what both solids occupy.\n"
                       "Two solids that do not overlap are refused, not emptied."));
    model->addAction(insertIntersectAction_);

    model->addSeparator();
    insertCircularPatternAction_->setIcon(icon(ui::SketchIcon::CircularPattern));
    insertCircularPatternAction_->setIconText(QStringLiteral("Ring"));
    insertCircularPatternAction_->setToolTip(
        QStringLiteral("Circular Pattern\nCopies around the origin's Z axis.\n"
                       "The step is PER COPY, so four at 90 deg is a full ring."));
    model->addAction(insertCircularPatternAction_);

    insertCurvePatternAction_->setIcon(icon(ui::SketchIcon::CurvePattern));
    insertCurvePatternAction_->setIconText(QStringLiteral("Along"));
    insertCurvePatternAction_->setToolTip(
        QStringLiteral("Pattern Along a Curve\nCopies spaced along the selected sketch's "
                       "curve.\nEvenly by ARC LENGTH, not by parameter."));
    model->addAction(insertCurvePatternAction_);

    model->addSeparator();
    exportAction_->setIcon(icon(ui::SketchIcon::ExportModel));
    exportAction_->setIconText(QStringLiteral("Export"));
    exportAction_->setToolTip(
        QStringLiteral("Export\nWrite the current solid as STEP or STL.\n"
                       "The format comes from the extension you type."));
    model->addAction(exportAction_);

    importAction_->setIcon(icon(ui::SketchIcon::ImportModel));
    importAction_->setIconText(QStringLiteral("Import"));
    importAction_->setToolTip(
        QStringLiteral("Import\nBring in a STEP file as a feature.\n"
                       "It stores the PATH: edit that file and the model follows."));
    model->addAction(importAction_);

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

    // --- The ASSEMBLY toolbar (M30.2) ---------------------------------------
    //
    // ITS OWN BAR, shown only for an assembly while the part bar is hidden.
    //
    // The menus could afford to leave part items visible-and-disabled: a menu
    // is opened deliberately, and a greyed row still says the command exists.
    // A TOOLBAR is always in view, and an open assembly showing seventeen
    // greyed part buttons and no assembly buttons is what a screenshot of M30
    // actually looked like.
    //
    // KNOWN COSMETIC ISSUE, measured and named rather than left to be noticed:
    // the break gives this bar its own row, so the hidden bar's row stays as a
    // blank strip and the visible bar sits about 49 px lower than a part's
    // does. Two ways round it were tried and are both worse -- sharing a row
    // leaves Qt collapsing it, so the bar reports visible and paints nothing,
    // and one bar with swapped contents did not restore the part actions after
    // the swap back. A blank strip is the cheapest of the three wrongs, and it
    // is the only one a user can still work through.
    addToolBarBreak();
    QToolBar* assembly = addToolBar(QStringLiteral("Assembly"));
    assembly->setIconSize(QSize(ui::size::kToolbarIcon, ui::size::kToolbarIcon));
    assembly->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    assembly->setMovable(false);

    const auto addAssembly = [&](QAction* action, ui::SketchIcon which,
                                 const char* shortLabel) {
        if (action == nullptr) return;
        action->setIcon(icon(which));
        action->setIconText(QString::fromLatin1(shortLabel));
        assembly->addAction(action);
    };
    addAssembly(insertInstanceAction_, ui::SketchIcon::InsertInstance, "Insert");
    addAssembly(groundInstanceAction_, ui::SketchIcon::GroundInstance, "Ground");
    assembly->addSeparator();
    addAssembly(addMateAction_, ui::SketchIcon::AddMate, "Mate");
    addAssembly(driveMateAction_, ui::SketchIcon::DriveMate, "Drive");
    addAssembly(limitMateAction_, ui::SketchIcon::LimitMate, "Limit");
    addAssembly(addRelationAction_, ui::SketchIcon::AddRelation, "Relation");
    assembly->addSeparator();
    addAssembly(patternInstanceAction_, ui::SketchIcon::AssemblyPattern, "Pattern");
    assembly->addSeparator();
    addAssembly(capturePositionAction_, ui::SketchIcon::NamedPosition, "Position");
    addAssembly(showExplodeAction_, ui::SketchIcon::ExplodeView, "Explode");
    assembly->addSeparator();
    addAssembly(interferenceAction_, ui::SketchIcon::Interference, "Interference");
    assemblyToolBar_ = assembly;

    // --- The DRAWING toolbar (M32.4) ----------------------------------------
    //
    // A third bar for the same reason there is a second: a toolbar is always
    // in view, and a drawing showing the part bar would be seventeen greyed
    // buttons and none of the ones that work.
    addToolBarBreak();
    QToolBar* drawing = addToolBar(QStringLiteral("Drawing"));
    drawing->setIconSize(QSize(ui::size::kToolbarIcon, ui::size::kToolbarIcon));
    drawing->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    drawing->setMovable(false);
    const auto addDrawing = [&](QAction* action, ui::SketchIcon which, const char* shortLabel) {
        if (action == nullptr) return;
        action->setIcon(icon(which));
        action->setIconText(QString::fromLatin1(shortLabel));
        drawing->addAction(action);
    };
    addDrawing(addBaseViewAction_, ui::SketchIcon::BaseView, "View");
    addDrawing(addProjectedViewAction_, ui::SketchIcon::ProjectedView, "Project");
    addDrawing(updateViewsAction_, ui::SketchIcon::UpdateViews, "Update");
    drawing->addSeparator();
    addDrawing(sheetSetupAction_, ui::SketchIcon::SheetSetup, "Sheet");
    addDrawing(titleBlockAction_, ui::SketchIcon::TitleBlock, "Title");
    addDrawing(addLayerAction_, ui::SketchIcon::DrawingLayer, "Layer");
    drawing->addSeparator();
    addDrawing(drawLineAction_, ui::SketchIcon::Line, "Line");
    addDrawing(drawCircleAction_, ui::SketchIcon::Circle, "Circle");
    addDrawing(drawRectangleAction_, ui::SketchIcon::Rectangle, "Rect");
    addDrawing(drawWireAction_, ui::SketchIcon::Wire, "Wire");
    addDrawing(placeSymbolAction_, ui::SketchIcon::Component, "Part");
    drawing->addSeparator();
    addDrawing(linearDimensionAction_, ui::SketchIcon::LinearDimension, "Dim");
    addDrawing(radiusDimensionAction_, ui::SketchIcon::RadiusDimension, "Radius");
    addDrawing(diameterDimensionAction_, ui::SketchIcon::DiameterDimension, "Dia");
    addDrawing(angularDimensionAction_, ui::SketchIcon::AngularDimension, "Angle");
    addDrawing(dimensionStyleAction_, ui::SketchIcon::DimensionStyleIcon, "Style");
    drawingToolBar_ = drawing;
    drawingToolBar_->setVisible(false);
    // Hidden until an assembly is open, as the sketch bar is hidden until a
    // sketch is.
    assemblyToolBar_->setVisible(false);
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
    // EXTENDED since M26.1, because a loft takes several sketches and a sweep
    // takes two. `selectedId_` stays the FIRST of the selection, which is what
    // every single-input command already means by "the selection" -- so
    // nothing that worked before behaves differently, and the commands that
    // need more ask selectedSketches() for it.
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            &MainWindow::onTreeSelectionChanged);

    treeDock_ = new QDockWidget(QStringLiteral("Model Tree"), this);
    treeDock_->setWidget(tree_);
    treeDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::LeftDockWidgetArea, treeDock_);

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

OutlineNode MainWindow::buildOutline() const {
    // THREE BUILDERS, ONE TREE. The widget, the state markers, the selection
    // and the property panel are shared; only what goes into the nodes
    // differs -- which is what P3 bought and why a third document type costs
    // a builder rather than a second tree.
    if (const auto* assembly = dynamic_cast<const AssemblyDocument*>(document_))
        return AssemblyOutline(*assembly).build(hiddenIds());
    if (const auto* drawing = dynamic_cast<const DrawingDocument*>(document_))
        return DrawingOutline(*drawing).build(hiddenIds());
    return DocumentOutline(part()).build(hiddenIds());
}

DrawingDocument* AsDrawing(DocumentBase* document) noexcept {
    return dynamic_cast<DrawingDocument*>(document);
}

// =============================================================================
// Assembly commands (M28)
// =============================================================================

namespace {

// The assembly, or null. Written once here for the same reason partOrNull()
// exists: five commands asking the same question five different ways is five
// chances to ask it wrongly.
AssemblyDocument* AsAssembly(DocumentBase* document) noexcept {
    return dynamic_cast<AssemblyDocument*>(document);
}

} // namespace

ObjectId MainWindow::selectedInstance() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return kInvalidObjectId;
    return assembly->findInstance(selectedId_) != nullptr ? selectedId_ : kInvalidObjectId;
}

void MainWindow::adoptAssemblyForTesting(const QString& name) {
    // The same adoption File > Open performs, without a file: the window owns
    // the document, points everything at it, and the kernel and solver are
    // carried across because they are the APPLICATION'S (ADR-M3-003).
    auto fresh = std::make_unique<AssemblyDocument>(name.toStdString());
    if (document_ != nullptr) {
        fresh->setGeometryKernel(document_->geometryKernel());
        fresh->setSketchSolver(document_->sketchSolver());
    }
    if (inSketchMode()) finishSketchCommand();
    ownedDocument_ = std::move(fresh);
    document_ = ownedDocument_.get();
    presenter_->setDocument(*document_);
    if (sketchCanvas_ != nullptr) sketchCanvas_->setSketch(partOrNull(), kInvalidObjectId);
    selectedId_ = kInvalidObjectId;
    documentPath_.clear();
    refreshAll();
}

std::size_t MainWindow::instanceCountForTesting() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    return assembly == nullptr ? 0 : assembly->instances().size();
}

std::size_t MainWindow::mateCountForTesting() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    return assembly == nullptr ? 0 : assembly->mates().size();
}

std::vector<std::string> MainWindow::instanceNamesForTesting() const {
    std::set<std::string> unique;
    if (const AssemblyDocument* assembly = AsAssembly(document_))
        for (const Instance* instance : assembly->instances()) unique.insert(instance->name());
    return {unique.begin(), unique.end()};
}

std::vector<Vec3> MainWindow::instancePlacesForTesting() const {
    std::vector<Vec3> places;
    if (const AssemblyDocument* assembly = AsAssembly(document_))
        for (const Instance* instance : assembly->instances())
            places.push_back(assembly->instanceWorldTransform(instance->id()).translation);
    return places;
}

void MainWindow::selectFirstInstanceForTesting() {
    if (const AssemblyDocument* assembly = AsAssembly(document_))
        if (!assembly->instances().empty()) selectObject(assembly->instances().front()->id());
}

std::vector<ObjectId> MainWindow::selectedInstances() const {
    std::vector<ObjectId> found;
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr || tree_ == nullptr) return found;
    std::set<ObjectId> chosen;
    for (const QTreeWidgetItem* item : tree_->selectedItems())
        chosen.insert(static_cast<ObjectId>(item->data(0, kIdRole).toULongLong()));
    // IN DOCUMENT ORDER, for the reason selectedSketches() gives: Qt does not
    // keep click order, and a mate whose LEADING instance depended on an order
    // nobody can see would place the wrong part. Document order is the order
    // the instances were inserted in, which is what the tree shows.
    for (const Instance* instance : assembly->instances())
        if (chosen.count(instance->id()) != 0) found.push_back(instance->id());
    return found;
}

void MainWindow::selectInstancesForTesting(const std::vector<ObjectId>& ids) {
    if (tree_ == nullptr) return;
    updatingWidgets_ = true;
    tree_->clearSelection();
    const std::function<void(QTreeWidgetItem*)> mark = [&](QTreeWidgetItem* item) {
        const ObjectId id = static_cast<ObjectId>(item->data(0, kIdRole).toULongLong());
        for (const ObjectId wanted : ids)
            if (wanted == id) item->setSelected(true);
        for (int i = 0; i < item->childCount(); ++i) mark(item->child(i));
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) mark(tree_->topLevelItem(i));
    updatingWidgets_ = false;
    refreshCommandStates();
}

void MainWindow::selectNamedPositionForTesting(const QString& name) {
    if (const AssemblyDocument* assembly = AsAssembly(document_))
        if (const NamedPosition* found =
                assembly->findNamedPositionNamed(name.toStdString()))
            selectObject(found->id());
}

void MainWindow::fitAllForTesting() { onFitAllRequested(); }

std::size_t MainWindow::undoDepthForTesting() const {
    return document_ != nullptr ? document_->undoDepth() : 0;
}

bool MainWindow::recomputeForTesting() {
    return document_ != nullptr && document_->recompute().success;
}

bool MainWindow::mateIsDrivenForTesting() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr || assembly->mates().empty()) return false;
    return assembly->mates().front()->isDriven();
}

double MainWindow::mateValueForTesting() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr || assembly->mates().empty()) return 0.0;
    return assembly->mates().front()->value();
}

void MainWindow::selectFirstMateForTesting() {
    if (const AssemblyDocument* assembly = AsAssembly(document_))
        if (!assembly->mates().empty()) selectObject(assembly->mates().front()->id());
}

std::vector<ObjectId> MainWindow::allInstancesForTesting() const {
    std::vector<ObjectId> ids;
    if (const AssemblyDocument* assembly = AsAssembly(document_))
        for (const Instance* instance : assembly->instances()) ids.push_back(instance->id());
    return ids;
}

Vec3 MainWindow::instanceWorldPlaceForTesting(ObjectId instanceId) const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return Vec3{};
    return assembly->instanceWorldTransform(instanceId).translation;
}

int MainWindow::instanceFreedomForTesting(ObjectId instanceId) const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return -1;
    for (const auto& freedom : assembly->mateSolveReport().freedoms)
        if (freedom.instanceId == instanceId)
            return freedom.rotational + freedom.translational;
    return -1;
}

QString MainWindow::driveSelectedMateForTesting(double value) {
    selectFirstMateForTesting();
    return driveSelectedMate(value);
}

QString MainWindow::driveMateForTesting(ObjectId mateId, double value) {
    selectObject(mateId);
    return driveSelectedMate(value);
}

QString MainWindow::createRelationForTesting(RelationType type, const QString& driverMateName,
                                            const QString& drivenMateName, double ratio,
                                            bool reversed) {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return QStringLiteral("not an assembly");
    const Mate* driver = assembly->findMateNamed(driverMateName.toStdString());
    const Mate* driven = assembly->findMateNamed(drivenMateName.toStdString());
    if (driver == nullptr || driven == nullptr) return QStringLiteral("no such mate");
    // THROUGH THE SELECTION, so the enabling rule is exercised too: a command
    // reachable only from a test is a command nobody can click.
    selectInstancesForTesting(driver->id() == driven->id()
                                  ? std::vector<ObjectId>{driver->id()}
                                  : std::vector<ObjectId>{driver->id(), driven->id()});
    return createRelationCommand(type, driver->id(), driven->id(), ratio, reversed);
}

void MainWindow::selectRelationForTesting(const QString& name) {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return;
    const Relation* relation = assembly->findRelationNamed(name.toStdString());
    if (relation == nullptr) return;
    selectObject(relation->id());
}

std::size_t MainWindow::relationCountForTesting() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    return assembly == nullptr ? 0u : assembly->relations().size();
}

bool MainWindow::addRelationEnabledForTesting() const {
    return addRelationAction_ != nullptr && addRelationAction_->isEnabled();
}

std::vector<ObjectId> MainWindow::allMatesForTesting() const {
    std::vector<ObjectId> ids;
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return ids;
    for (const Mate* mate : assembly->mates()) ids.push_back(mate->id());
    return ids;
}

std::vector<std::string> MainWindow::relationNamesForTesting() const {
    std::vector<std::string> names;
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return names;
    for (const Relation* relation : assembly->relations()) names.push_back(relation->name());
    return names;
}

std::string MainWindow::objectNameForTesting(ObjectId id) const {
    return document_ == nullptr ? std::string() : document_->objectName(id);
}

QString MainWindow::limitSelectedMateForTesting(double minimum, double maximum) {
    selectFirstMateForTesting();
    return limitSelectedMate(minimum, maximum);
}

ObjectId MainWindow::selectedNamedPosition() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return kInvalidObjectId;
    return assembly->findNamedPosition(selectedId_) != nullptr ? selectedId_ : kInvalidObjectId;
}

ObjectId MainWindow::selectedExplodeView() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return kInvalidObjectId;
    return assembly->findExplodeView(selectedId_) != nullptr ? selectedId_ : kInvalidObjectId;
}

QString MainWindow::captureNamedPositionCommand(const QString& name) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Only an assembly has positions."));
    if (name.trimmed().isEmpty()) return say(QStringLiteral("A position needs a name."));
    if (assembly->findNamedPositionNamed(name.toStdString()) != nullptr)
        return say(QStringLiteral("There is already a position called %1.").arg(name));

    document_->beginTransaction("Capture position " + name.toStdString());
    assembly->captureNamedPosition(name.toStdString());
    if (!document_->commitTransaction())
        return say(QStringLiteral("The document refused that position."));
    refreshAll();
    // WHAT IT CAPTURED, said plainly. §49: a named position is the mate values
    // PLUS the transforms of instances no mate places -- and that second half
    // is the one that is easy to forget exists until a hand-placed part comes
    // back somewhere else.
    return say(QStringLiteral("Captured %1: every mate's value, and where the instances no "
                              "mate places are sitting.")
                   .arg(name));
}

QString MainWindow::applySelectedNamedPosition() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId positionId = selectedNamedPosition();
    if (assembly == nullptr || positionId == kInvalidObjectId)
        return say(QStringLiteral("Select a named position to apply."));
    const NamedPosition* position = assembly->findNamedPosition(positionId);
    const QString name =
        position != nullptr ? QString::fromStdString(position->name()) : QStringLiteral("it");

    // ONE TRANSACTION, so it is ONE undo step. A position is one thing the user
    // chose; without this, undoing "back to Open" would walk back one mate at a
    // time and stop somewhere that was never any position at all.
    document_->beginTransaction("Apply position " + name.toStdString());
    const bool applied = assembly->applyNamedPosition(positionId);
    if (!applied || !document_->commitTransaction())
        return say(QStringLiteral("%1 could not be applied.").arg(name));
    (void)document_->recompute();
    refreshAll();
    return say(QStringLiteral("Moved to %1").arg(name));
}

QString MainWindow::addExplodeViewCommand(const QString& name) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Only an assembly explodes."));
    if (name.trimmed().isEmpty()) return say(QStringLiteral("An exploded view needs a name."));
    if (assembly->findExplodeViewNamed(name.toStdString()) != nullptr)
        return say(QStringLiteral("There is already a view called %1.").arg(name));

    document_->beginTransaction("Add exploded view " + name.toStdString());
    const ExplodeView& made = assembly->addExplodeView(name.toStdString());
    const ObjectId madeId = made.id();
    if (!document_->commitTransaction())
        return say(QStringLiteral("The document refused that view."));
    refreshAll();
    selectObject(madeId);
    return say(QStringLiteral("Added %1. Select an instance and add a step to it.").arg(name));
}

QString MainWindow::addExplodeStepCommand(ObjectId viewId, const Vec3& offsetMm) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Only an assembly explodes."));
    const ExplodeView* view = assembly->findExplodeView(viewId);
    if (view == nullptr) return say(QStringLiteral("Select an exploded view first."));
    const ObjectId instanceId = selectedInstance();
    if (instanceId == kInvalidObjectId)
        return say(QStringLiteral("Select the instance this step moves."));

    const Instance* instance = assembly->findInstance(instanceId);
    const std::string stepName =
        (instance != nullptr ? instance->name() : std::string("Step")) + " out";

    document_->beginTransaction("Add explode step");
    bool added = false;
    try {
        added = assembly->addExplodeStep(viewId, stepName, instanceId, offsetMm);
    } catch (const std::exception& problem) {
        document_->commitTransaction();
        return say(QStringLiteral("That step was refused: %1")
                       .arg(QString::fromUtf8(problem.what())));
    }
    if (!added || !document_->commitTransaction())
        return say(QStringLiteral("That step was refused."));
    refreshAll();
    return say(QStringLiteral("Added step %1 of %2")
                   .arg(view->steps().size())
                   .arg(QString::fromStdString(view->name())));
}

QString MainWindow::showExplodeView(ObjectId viewId) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Only an assembly explodes."));

    // PRESENTATION ONLY -- no transaction, nothing recorded, nothing to undo.
    // The model is not moved by looking at it exploded, which is the whole
    // difference between an exploded view and dragging the parts apart.
    presenter_->setShownExplodeView(viewId);
    refreshAll();
    if (viewId == kInvalidObjectId)
        return say(QStringLiteral("Showing the assembly as it is."));
    const ExplodeView* view = assembly->findExplodeView(viewId);
    return say(QStringLiteral("Showing %1. Nothing has moved -- this is a picture.")
                   .arg(view != nullptr ? QString::fromStdString(view->name())
                                        : QStringLiteral("that view")));
}

QString MainWindow::setExplodePreviewCommand(ObjectId viewId, std::size_t stepsShown) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Only an assembly explodes."));
    const ExplodeView* view = assembly->findExplodeView(viewId);
    if (view == nullptr) return say(QStringLiteral("Select an exploded view first."));

    // THE VIEW'S OWN ROLLBACK BAR (§49 point 2). The position is stored on the
    // view, so it survives a save -- and it CLAMPS rather than erroring, which
    // is EvaluationCut's whole contract: a shortened list with an old position
    // means "all of it" without anyone having to go and fix it.
    document_->beginTransaction("Preview explode steps");
    const bool set = assembly->setExplodePreview(viewId, stepsShown);
    if (!set || !document_->commitTransaction())
        return say(QStringLiteral("That preview position was refused."));
    refreshAll();
    return say(QStringLiteral("Showing %1 of %2 step%3")
                   .arg(std::min(stepsShown, view->steps().size()))
                   .arg(view->steps().size())
                   .arg(view->steps().size() == 1 ? "" : "s"));
}

QString MainWindow::showHideSelectedInstance() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    const AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId instanceId = selectedInstance();
    if (assembly == nullptr || instanceId == kInvalidObjectId)
        return say(QStringLiteral("Select an instance to hide."));

    // PURE PRESENTATION (A02). It never touches the document, so it records no
    // undo step and cannot be saved -- and that second half is a real limit,
    // not an oversight: §49's DISPLAY STATE is a NAMED set of what is hidden,
    // and naming one that cannot be persisted would be a feature that forgets
    // itself. Where a named display state should live is a decision this
    // milestone does not make.
    presenter_->toggleHidden(instanceId);
    refreshAll();
    const Instance* instance = assembly->findInstance(instanceId);
    const QString name =
        instance != nullptr ? QString::fromStdString(instance->name()) : QStringLiteral("it");
    return say(presenter_->isHidden(instanceId)
                   ? QStringLiteral("%1 hidden. It is still there and still solved.").arg(name)
                   : QStringLiteral("%1 shown").arg(name));
}

QString MainWindow::checkInterferenceCommand() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Only an assembly can interfere."));

    const AssemblyDocument::InterferenceReport report = assembly->checkInterference();
    if (!report.ok)
        return say(QStringLiteral("Interference could not be checked: %1")
                       .arg(QString::fromStdString(report.message)));
    if (report.overlaps.empty()) {
        // WHAT WAS ACTUALLY CHECKED. "No interference" over an assembly where
        // three instances failed to build is a clean bill of health for a part
        // that was never looked at, and the report already says so.
        return say(report.message.empty()
                       ? QStringLiteral("No interference.")
                       : QStringLiteral("No interference. %1")
                             .arg(QString::fromStdString(report.message)));
    }
    // NAMED PAIRS, not a count. "3 interferences" tells a user nothing they can
    // act on; which two parts, and by how much, is the whole of what they need.
    QStringList lines;
    for (const AssemblyDocument::Interference& overlap : report.overlaps) {
        const Instance* first = assembly->findInstance(overlap.firstInstanceId);
        const Instance* second = assembly->findInstance(overlap.secondInstanceId);
        lines << QStringLiteral("%1 and %2 overlap by %3 mm^3")
                     .arg(first != nullptr ? QString::fromStdString(first->name())
                                           : QStringLiteral("?"),
                          second != nullptr ? QString::fromStdString(second->name())
                                            : QStringLiteral("?"))
                     .arg(overlap.volumeMm3, 0, 'f', 3);
    }
    return say(lines.join(QStringLiteral("; ")));
}

ObjectId MainWindow::selectedMate() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return kInvalidObjectId;
    return assembly->findMate(selectedId_) != nullptr ? selectedId_ : kInvalidObjectId;
}

std::vector<std::string> MainWindow::connectorsOfInstance(ObjectId instanceId) const {
    std::vector<std::string> names;
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return names;
    const Instance* instance = assembly->findInstance(instanceId);
    if (instance == nullptr) return names;
    // ITS PART'S connectors, re-read on every rebuild. The shell lists what the
    // part offers; it does not invent connectors, because a connector belongs
    // to the part that has the feature (§21) and an assembly cannot write into
    // a part file.
    for (const Instance::MateConnector& connector : instance->connectors())
        names.push_back(connector.name);
    return names;
}

QString MainWindow::createMateCommand(MateType type, ObjectId leadingInstance,
                                      const QString& leadingConnector,
                                      ObjectId followingInstance,
                                      const QString& followingConnector) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Only an assembly can hold mates."));

    const Instance* leading = assembly->findInstance(leadingInstance);
    const Instance* following = assembly->findInstance(followingInstance);
    if (leading == nullptr || following == nullptr)
        return say(QStringLiteral("A mate needs two instances."));
    if (leadingInstance == followingInstance)
        return say(QStringLiteral("A mate holds two different instances; that is one."));

    // NAMED CONNECTORS THAT EXIST, checked here rather than deep in the solve.
    // A mate that resolves to nothing fails at solve time naming an id, which
    // is a message about the wrong thing.
    if (leading->findConnector(leadingConnector.toStdString()) == nullptr)
        return say(QStringLiteral("%1 has no connector called %2")
                       .arg(QString::fromStdString(leading->name()), leadingConnector));
    if (following->findConnector(followingConnector.toStdString()) == nullptr)
        return say(QStringLiteral("%1 has no connector called %2")
                       .arg(QString::fromStdString(following->name()), followingConnector));

    std::string name = std::string(toString(type));
    for (int suffix = 2; assembly->findMateNamed(name) != nullptr; ++suffix)
        name = std::string(toString(type)) + " " + std::to_string(suffix);

    document_->beginTransaction("Add mate " + name);
    assembly->addMate(name, type, leadingInstance, leadingConnector.toStdString(),
                      followingInstance, followingConnector.toStdString());
    if (!document_->commitTransaction())
        return say(QStringLiteral("The document refused that mate."));

    // SOLVED, then reported. A mate that cannot be solved -- because nothing is
    // grounded, or because it conflicts -- is something the user has to hear
    // about now, and the solver already says which.
    (void)document_->recompute();
    refreshAll();
    const AssemblyDocument::MateSolveReport& report = assembly->mateSolveReport();
    if (!report.ok)
        return say(QStringLiteral("%1 added, but the assembly will not solve: %2")
                       .arg(QString::fromStdString(name),
                            QString::fromStdString(report.message)));
    return say(QStringLiteral("Mated %1 / %2 to %3 / %4 (%5)")
                   .arg(QString::fromStdString(leading->name()), leadingConnector,
                        QString::fromStdString(following->name()), followingConnector,
                        QString::fromUtf8(std::string(toString(type)).c_str())));
}

QString MainWindow::deleteSelectedMate() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId mateId = selectedMate();
    if (assembly == nullptr || mateId == kInvalidObjectId)
        return say(QStringLiteral("Select a mate to delete."));
    const Mate* mate = assembly->findMate(mateId);
    const QString name =
        mate != nullptr ? QString::fromStdString(mate->name()) : QStringLiteral("it");

    document_->beginTransaction("Delete mate");
    const bool removed = document_->removeObject(mateId);
    if (!removed || !document_->commitTransaction())
        return say(QStringLiteral("%1 could not be deleted.").arg(name));
    (void)document_->recompute();
    selectedId_ = kInvalidObjectId;
    refreshAll();
    // NOTHING ELSE GOES. A mate holds two instances; it does not own them, so
    // deleting it frees them rather than taking them with it -- which is the
    // opposite of deleting an instance, and worth saying so nobody has to guess.
    return say(QStringLiteral("Deleted %1. The instances it held are free again.").arg(name));
}

QString MainWindow::driveSelectedMate(double value) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId mateId = selectedMate();
    if (assembly == nullptr || mateId == kInvalidObjectId)
        return say(QStringLiteral("Select a mate to drive."));
    const Mate* mate = assembly->findMate(mateId);
    if (mate == nullptr) return say(QStringLiteral("That mate is gone."));
    if (FreedomOf(mate->type()).total() == 0)
        return say(QStringLiteral("A %1 mate leaves nothing to drive.")
                       .arg(QString::fromUtf8(std::string(toString(mate->type())).c_str())));

    // NOT IF A RELATION ALREADY DECIDES IT (M31). The relation would overwrite
    // the number on the very next solve, so accepting the value would report
    // "driven to 0.5" over a part that did not move -- and the user would have
    // no way to tell which of the two things they should have changed.
    for (std::size_t c = 0; c < kMateComponentCount; ++c) {
        if (!FreedomOf(mate->type()).free[c]) continue;
        const Relation* by = assembly->relationDriving(mateId, static_cast<MateComponent>(c));
        if (by == nullptr) continue;
        return say(QStringLiteral("%1 is driven by the relation %2. Change its ratio, or "
                                  "delete it, to move this.")
                       .arg(QString::fromStdString(mate->name()),
                            QString::fromStdString(by->name())));
    }

    document_->beginTransaction("Drive mate");
    // DRIVEN, and said so in the model -- not merely given a value. A value set
    // on a mate the solver is free to move is a value the next solve overwrites,
    // which is the defect examples/four-bar.ep3ds shipped with (ADR-M25-006).
    const bool set = assembly->setMateValue(mateId, value) &&
                     assembly->setMateDriven(mateId, true);
    if (!set || !document_->commitTransaction())
        return say(QStringLiteral("That mate would not take that value."));
    (void)document_->recompute();
    refreshAll();
    const AssemblyDocument::MateSolveReport& report = assembly->mateSolveReport();
    if (!report.ok)
        return say(QStringLiteral("Driven, but the assembly will not solve: %1")
                       .arg(QString::fromStdString(report.message)));
    return say(QStringLiteral("%1 driven to %2")
                   .arg(QString::fromStdString(mate->name()))
                   .arg(value, 0, 'f', 3));
}

QString MainWindow::limitSelectedMate(double minimum, double maximum) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId mateId = selectedMate();
    if (assembly == nullptr || mateId == kInvalidObjectId)
        return say(QStringLiteral("Select a mate to limit."));
    if (minimum > maximum)
        return say(QStringLiteral("A limit's lower bound cannot be above its upper one."));
    const Mate* mate = assembly->findMate(mateId);
    if (mate == nullptr) return say(QStringLiteral("That mate is gone."));

    // THE FIRST FREE COMPONENT, which is the one a single-freedom mate means.
    const MateFreedom freedom = FreedomOf(mate->type());
    int component = -1;
    for (int i = 0; i < 6; ++i)
        if (freedom.free[i]) { component = i; break; }
    if (component < 0)
        return say(QStringLiteral("A %1 mate has no freedom to limit.")
                       .arg(QString::fromUtf8(std::string(toString(mate->type())).c_str())));

    document_->beginTransaction("Limit mate");
    const bool set = assembly->setMateLimit(mateId, static_cast<MateComponent>(component),
                                            minimum, maximum);
    if (!set || !document_->commitTransaction())
        return say(QStringLiteral("That limit was refused."));
    (void)document_->recompute();
    refreshAll();
    // CLAMPED, NOT REFUSED (§22). A driven value outside its limits is held at
    // the limit, so a user who types 200 on a 0..90 hinge sees 90 and the arm
    // at 90 -- not an error and not a hinge bent past its stop.
    return say(QStringLiteral("%1 limited to %2 .. %3. A value outside that is held at the "
                              "nearer end rather than refused.")
                   .arg(QString::fromStdString(mate->name()))
                   .arg(minimum, 0, 'f', 3)
                   .arg(maximum, 0, 'f', 3));
}

// =============================================================================
// Relations (M31, roadmap §20.5)
// =============================================================================

std::vector<ObjectId> MainWindow::selectedMates() const {
    std::vector<ObjectId> found;
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr || tree_ == nullptr) return found;
    std::set<ObjectId> chosen;
    for (const QTreeWidgetItem* item : tree_->selectedItems())
        chosen.insert(static_cast<ObjectId>(item->data(0, kIdRole).toULongLong()));
    // IN DOCUMENT ORDER, for the reason selectedInstances() gives: Qt does not
    // keep click order, and a relation whose DRIVER depended on an order
    // nobody can see would gear the train backwards.
    for (const Mate* mate : assembly->mates())
        if (chosen.count(mate->id()) != 0) found.push_back(mate->id());
    return found;
}

ObjectId MainWindow::selectedRelation() const {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return kInvalidObjectId;
    return assembly->findRelation(selectedId_) != nullptr ? selectedId_ : kInvalidObjectId;
}

QString MainWindow::createRelationCommand(RelationType type, ObjectId driverMate,
                                          ObjectId drivenMate, double ratio, bool reversed) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Relations belong to an assembly."));
    const Mate* driver = assembly->findMate(driverMate);
    const Mate* driven = assembly->findMate(drivenMate);
    if (driver == nullptr || driven == nullptr)
        return say(QStringLiteral("Select the mate to drive from, and the one to drive."));

    // WHICH FREEDOM, derived by the one rule (§20.5). A gear takes each mate's
    // first free ROTATION, a rack and pinion a rotation and a translation --
    // and a mate that has not got the freedom the type needs is refused here,
    // by name, rather than coupled to a component the solve pins to zero.
    const auto pick = [&](const Mate& mate, bool rotation,
                          CoupledFreedom& into) -> QString {
        const std::size_t c = FirstFreeComponentOfKind(FreedomOf(mate.type()), rotation);
        if (c >= kMateComponentCount)
            return QStringLiteral("%1 is a %2 mate, which has nothing to %3.")
                .arg(QString::fromStdString(mate.name()),
                     QString::fromUtf8(std::string(toString(mate.type())).c_str()),
                     rotation ? QStringLiteral("turn") : QStringLiteral("slide"));
        into.mateId = mate.id();
        into.component = static_cast<MateComponent>(c);
        return {};
    };
    CoupledFreedom from;
    CoupledFreedom to;
    if (const QString bad = pick(*driver, RelationDriverIsRotation(type), from); !bad.isEmpty())
        return say(bad);
    if (const QString bad = pick(*driven, RelationDrivenIsRotation(type), to); !bad.isEmpty())
        return say(bad);

    // ASKED BEFORE IT IS DONE, because addRelation THROWS on a refusal -- it
    // treats one as a programming error from a UI that should have known. So
    // this is the UI knowing.
    if (const std::string why = assembly->whyRelationIsRefused(type, from, to); !why.empty())
        return say(QStringLiteral("That relation was refused: %1")
                       .arg(QString::fromStdString(why)));

    const std::string name = document_->unusedNameLike(std::string(toString(type)));
    document_->beginTransaction("Add relation");
    Relation* made = nullptr;
    try {
        made = &assembly->addRelation(name, type, from, to, ratio, reversed);
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(QStringLiteral("That relation was refused: %1")
                       .arg(QString::fromUtf8(error.what())));
    }
    // THE MATE IS NO LONGER DRIVEN BY HAND. Its freedom now belongs to the
    // relation, and a tree row still saying "driven" would be the model
    // claiming two authorities for one number. Released inside the SAME
    // transaction, so it is one undo step, and said out loud below -- a
    // silent state change is the thing this is avoiding, not the change.
    //
    // NOT FOR A SCREW, where the driver and the driven end are the same mate:
    // there, driving the rotation by hand is exactly the point.
    const bool released = driven->id() != driver->id() && driven->isDriven() &&
                          assembly->setMateDriven(driven->id(), false);
    if (!document_->commitTransaction()) return say(QStringLiteral("That relation was refused."));
    (void)document_->recompute();
    selectedId_ = made->id();
    refreshAll();

    const bool perTurn = type == RelationType::Screw || type == RelationType::RackAndPinion;
    const QString sentence =
        QStringLiteral("%1 now drives %2 at %3 %4%5")
            .arg(QString::fromStdString(driver->name()),
                 QString::fromStdString(driven->name()))
            .arg(ratio, 0, 'f', 3)
            .arg(perTurn ? QStringLiteral("mm per turn") : QStringLiteral("per turn"),
                 reversed ? QStringLiteral(", reversed") : QString());
    const QString full =
        released ? sentence + QStringLiteral(". %1 is no longer driven by hand")
                                  .arg(QString::fromStdString(driven->name()))
                 : sentence;
    const AssemblyDocument::MateSolveReport& report = assembly->mateSolveReport();
    if (!report.ok)
        return say(QStringLiteral("%1, but the assembly will not solve: %2")
                       .arg(full, QString::fromStdString(report.message)));
    return say(full);
}

QString MainWindow::setSelectedRelationRatio(double ratio) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId id = selectedRelation();
    if (assembly == nullptr || id == kInvalidObjectId)
        return say(QStringLiteral("Select a relation."));
    const Relation* relation = assembly->findRelation(id);
    if (relation == nullptr) return say(QStringLiteral("That relation is gone."));

    document_->beginTransaction("Relation ratio");
    const bool set = assembly->setRelationRatio(id, ratio);
    if (!set || !document_->commitTransaction())
        return say(QStringLiteral("That ratio was refused."));
    (void)document_->recompute();
    refreshAll();
    const bool perTurn = relation->type() == RelationType::Screw ||
                         relation->type() == RelationType::RackAndPinion;
    return say(QStringLiteral("%1 is now %2 %3")
                   .arg(QString::fromStdString(relation->name()))
                   .arg(ratio, 0, 'f', 3)
                   .arg(perTurn ? QStringLiteral("mm per turn") : QStringLiteral("per turn")));
}

QString MainWindow::reverseSelectedRelation() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId id = selectedRelation();
    if (assembly == nullptr || id == kInvalidObjectId)
        return say(QStringLiteral("Select a relation."));
    const Relation* relation = assembly->findRelation(id);
    if (relation == nullptr) return say(QStringLiteral("That relation is gone."));

    const bool wanted = !relation->reversed();
    document_->beginTransaction("Reverse relation");
    const bool set = assembly->setRelationReversed(id, wanted);
    if (!set || !document_->commitTransaction())
        return say(QStringLiteral("That was refused."));
    (void)document_->recompute();
    refreshAll();
    return say(QStringLiteral("%1 now turns %2")
                   .arg(QString::fromStdString(relation->name()),
                        wanted ? QStringLiteral("the other way")
                               : QStringLiteral("the same way as its driver")));
}

QString MainWindow::deleteSelectedRelation() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId id = selectedRelation();
    if (assembly == nullptr || id == kInvalidObjectId)
        return say(QStringLiteral("Select a relation to delete."));
    const Relation* relation = assembly->findRelation(id);
    const QString name =
        relation != nullptr ? QString::fromStdString(relation->name()) : QStringLiteral("it");

    document_->beginTransaction("Delete relation");
    const bool removed = document_->removeObject(id);
    if (!removed || !document_->commitTransaction())
        return say(QStringLiteral("%1 could not be deleted.").arg(name));
    (void)document_->recompute();
    selectedId_ = kInvalidObjectId;
    refreshAll();
    // NOTHING ELSE GOES, and the freedom goes back to the solve -- which is
    // the opposite of deleting the MATE, and worth saying so nobody guesses.
    return say(QStringLiteral("Deleted %1. The freedom it was driving is the solve's again.")
                   .arg(name));
}

QString MainWindow::clearLimitOnSelectedMate() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId mateId = selectedMate();
    if (assembly == nullptr || mateId == kInvalidObjectId)
        return say(QStringLiteral("Select a mate."));
    const Mate* mate = assembly->findMate(mateId);
    if (mate == nullptr) return say(QStringLiteral("That mate is gone."));
    const MateFreedom freedom = FreedomOf(mate->type());
    int component = -1;
    for (int i = 0; i < 6; ++i)
        if (freedom.free[i]) { component = i; break; }
    if (component < 0) return say(QStringLiteral("That mate has no limit to clear."));

    document_->beginTransaction("Clear mate limit");
    assembly->clearMateLimit(mateId, static_cast<MateComponent>(component));
    if (!document_->commitTransaction()) return say(QStringLiteral("That was refused."));
    (void)document_->recompute();
    refreshAll();
    return say(QStringLiteral("%1 moves freely again").arg(QString::fromStdString(mate->name())));
}

QString MainWindow::insertInstanceCommand(const QString& sourcePath, const QString& bodyName) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return say(QStringLiteral("Only an assembly can hold instances."));
    if (sourcePath.isEmpty()) return say(QStringLiteral("Insert needs a part file."));

    // A NAME THE TREE CAN SHOW, derived from the file rather than asked for.
    // Onshape does the same: the instance is named after what it is, and
    // renaming it is one edit in the property panel afterwards. Asking first
    // would put a dialog in front of the commonest action in an assembly.
    const QString base = QFileInfo(sourcePath).completeBaseName();
    std::string name = base.isEmpty() ? std::string("Part") : base.toStdString();
    if (assembly->findInstanceNamed(name) != nullptr) {
        // ...AND MADE UNIQUE, because five of the same part is the ordinary
        // case in an assembly, not an error.
        for (int suffix = 2;; ++suffix) {
            const std::string candidate = name + " " + std::to_string(suffix);
            if (assembly->findInstanceNamed(candidate) == nullptr) {
                name = candidate;
                break;
            }
        }
    }

    document_->beginTransaction("Insert " + name);
    const Instance& made =
        assembly->addInstance(name, sourcePath.toStdString(), bodyName.toStdString());
    const ObjectId madeId = made.id();
    if (!document_->commitTransaction())
        return say(QStringLiteral("The document refused that instance."));

    // RECOMPUTED, then reported -- an instance whose source will not load is a
    // failure the user has to hear about now rather than discover later.
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(madeId);
    if (!report.success) {
        const Instance* placed = assembly->findInstance(madeId);
        return say(QStringLiteral("%1 was inserted but did not build: %2")
                       .arg(QString::fromStdString(name))
                       .arg(placed != nullptr && placed->currentState() == ComputeState::Failed
                                ? QStringLiteral("its source could not be read")
                                : QStringLiteral("see the model tree")));
    }
    return say(QStringLiteral("Inserted %1 from %2")
                   .arg(QString::fromStdString(name), QFileInfo(sourcePath).fileName()));
}

QString MainWindow::placeSelectedInstance(const Vec3& whereMm) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId instanceId = selectedInstance();
    if (assembly == nullptr || instanceId == kInvalidObjectId)
        return say(QStringLiteral("Select an instance to move."));

    // §19: this moves the INSTANCE, never the part. The transform lives on the
    // instance's own frame, so the part file it came from is untouched and
    // every other instance of it stays where it is.
    Transform3D placement = assembly->instanceTransform(instanceId);
    placement.translation = whereMm;
    document_->beginTransaction("Move instance");
    const bool moved = assembly->setInstanceTransform(instanceId, placement);
    if (!moved || !document_->commitTransaction())
        return say(QStringLiteral("That instance could not be moved."));
    (void)document_->recompute();
    refreshAll();
    return say(QStringLiteral("Moved to (%1, %2, %3) mm")
                   .arg(whereMm.x, 0, 'f', 2)
                   .arg(whereMm.y, 0, 'f', 2)
                   .arg(whereMm.z, 0, 'f', 2));
}

QString MainWindow::toggleGroundSelectedInstance() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId instanceId = selectedInstance();
    if (assembly == nullptr || instanceId == kInvalidObjectId)
        return say(QStringLiteral("Select an instance to ground."));

    const bool wasGrounded = assembly->isInstanceGrounded(instanceId);
    document_->beginTransaction(wasGrounded ? "Unground instance" : "Ground instance");
    const bool changed = assembly->setInstanceGrounded(instanceId, !wasGrounded);
    if (!changed || !document_->commitTransaction())
        return say(QStringLiteral("That instance could not be grounded."));
    (void)document_->recompute();
    refreshAll();
    const Instance* instance = assembly->findInstance(instanceId);
    const QString name =
        instance != nullptr ? QString::fromStdString(instance->name()) : QStringLiteral("it");
    return say(wasGrounded ? QStringLiteral("%1 is free to move again").arg(name)
                           : QStringLiteral("%1 is grounded").arg(name));
}

QString MainWindow::patternSelectedInstance(int count, const Vec3& stepMm) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId instanceId = selectedInstance();
    if (assembly == nullptr || instanceId == kInvalidObjectId)
        return say(QStringLiteral("Select an instance to pattern."));
    if (count < 2)
        return say(QStringLiteral("A pattern needs at least 2, counting the original."));

    document_->beginTransaction("Pattern instance");
    std::vector<ObjectId> copies;
    try {
        copies = assembly->addInstancePattern(instanceId, count, stepMm);
    } catch (const std::exception& problem) {
        document_->commitTransaction();
        return say(QStringLiteral("That pattern was refused: %1")
                       .arg(QString::fromUtf8(problem.what())));
    }
    if (!document_->commitTransaction())
        return say(QStringLiteral("The document refused that pattern."));
    (void)document_->recompute();
    refreshAll();
    // WHAT IT COSTS TO CHANGE, said now. The copies are ordinary instances and
    // not a stored feature (ADR-M26-003), so editing the count afterwards means
    // deleting them and doing it again -- and a user should hear that before
    // they build a row of forty.
    return say(QStringLiteral("Made %1 cop%2. They are ordinary instances: to change the "
                              "count, delete them and pattern again.")
                   .arg(copies.size())
                   .arg(copies.size() == 1 ? "y" : "ies"));
}

QString MainWindow::deleteSelectedInstance() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId instanceId = selectedInstance();
    if (assembly == nullptr || instanceId == kInvalidObjectId)
        return say(QStringLiteral("Select an instance to delete."));

    const Instance* instance = assembly->findInstance(instanceId);
    const QString name =
        instance != nullptr ? QString::fromStdString(instance->name()) : QStringLiteral("it");
    // WHAT ELSE GOES, counted BEFORE the delete. A mate names two instances and
    // cannot outlive either of them, and finding that out afterwards -- from a
    // tree with fewer rows than expected -- is how a user learns not to trust
    // Delete.
    std::size_t matesLost = 0;
    for (const Mate* mate : assembly->mates())
        if (mate->leadingInstanceId() == instanceId || mate->followingInstanceId() == instanceId)
            ++matesLost;

    document_->beginTransaction("Delete instance");
    const bool removed = document_->removeObject(instanceId);
    if (!removed || !document_->commitTransaction())
        return say(QStringLiteral("%1 could not be deleted.").arg(name));
    (void)document_->recompute();
    selectedId_ = kInvalidObjectId;
    refreshAll();
    if (matesLost == 0) return say(QStringLiteral("Deleted %1").arg(name));
    return say(QStringLiteral("Deleted %1 and %2 mate%3 that named it")
                   .arg(name)
                   .arg(matesLost)
                   .arg(matesLost == 1 ? "" : "s"));
}

// --- The slots: the dialogs, and nothing else --------------------------------

void MainWindow::onCaptureNamedPositionRequested() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Capture Position"),
                                               QStringLiteral("Name this position"),
                                               QLineEdit::Normal, QStringLiteral("Open"), &ok);
    if (!ok) return;
    captureNamedPositionCommand(name);
}

void MainWindow::onApplyNamedPositionRequested() { applySelectedNamedPosition(); }

void MainWindow::onAddExplodeViewRequested() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New Exploded View"),
                                               QStringLiteral("Name this view"),
                                               QLineEdit::Normal,
                                               QStringLiteral("Assembly steps"), &ok);
    if (!ok) return;
    addExplodeViewCommand(name);
}

void MainWindow::onAddExplodeStepRequested() {
    const ObjectId viewId = selectedExplodeView();
    if (viewId == kInvalidObjectId) {
        statusLeft_->setText(QStringLiteral("Select the exploded view this step belongs to."));
        return;
    }
    bool ok = false;
    const double along = QInputDialog::getDouble(this, QStringLiteral("Add Explode Step"),
                                                 QStringLiteral("Move out along Z, in mm"),
                                                 40.0, -1e6, 1e6, 3, &ok);
    if (!ok) return;
    addExplodeStepCommand(viewId, Vec3{0.0, 0.0, along});
}

void MainWindow::onShowExplodeViewRequested() {
    const ObjectId viewId = selectedExplodeView();
    // A TOGGLE: showing the view again puts the assembly back as it is.
    showExplodeView(presenter_ != nullptr && presenter_->shownExplodeView() == viewId
                        ? kInvalidObjectId
                        : viewId);
}

void MainWindow::onExplodePreviewRequested() {
    const ObjectId viewId = selectedExplodeView();
    if (viewId == kInvalidObjectId) return;
    bool ok = false;
    const int shown = QInputDialog::getInt(this, QStringLiteral("Explode Up To"),
                                           QStringLiteral("How many steps?"), 1, 0, 999, 1, &ok);
    if (!ok) return;
    setExplodePreviewCommand(viewId, static_cast<std::size_t>(shown));
}

void MainWindow::onCheckInterferenceRequested() { checkInterferenceCommand(); }

void MainWindow::onAddMateRequested() {
    const std::vector<ObjectId> chosen = selectedInstances();
    if (chosen.size() != 2) return;

    // THE DIALOG IS THREE CHOICES, and the type is one of them. Roadmap §20.6
    // is explicit that EP3D must not decide the mate type for the user: every
    // one of the seven is a different machine, and a hinge chosen for you when
    // you wanted a slider is a constraint you did not ask for.
    //
    // Assembled from QInputDialog rather than a designed form, which is a real
    // limitation and is named in the milestone's own notes: there is no
    // red-title "cannot commit yet" state (§10.1) because there is no title to
    // colour. What IS true is that each step can be cancelled and nothing
    // happens, and the command below re-checks every input anyway.
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return;

    static const struct { const char* label; MateType type; } kTypes[] = {
        {"Fastened -- no freedom left", MateType::Fastened},
        {"Revolute -- turns about the shared axis", MateType::Revolute},
        {"Slider -- slides along the shared axis", MateType::Slider},
        {"Cylindrical -- turns and slides", MateType::Cylindrical},
        {"Ball -- turns about a shared point", MateType::Ball},
        {"Planar -- slides and spins in a plane", MateType::Planar},
        {"Parallel -- axes stay parallel", MateType::Parallel},
    };
    QStringList typeLabels;
    for (const auto& entry : kTypes) typeLabels << QString::fromUtf8(entry.label);
    bool ok = false;
    const QString pickedType =
        QInputDialog::getItem(this, QStringLiteral("Add Mate"), QStringLiteral("Mate type"),
                              typeLabels, 1, false, &ok);
    if (!ok) return;
    MateType type = MateType::Revolute;
    for (const auto& entry : kTypes)
        if (pickedType == QString::fromUtf8(entry.label)) type = entry.type;

    const auto pickConnector = [&](ObjectId instanceId, const QString& which) -> QString {
        const std::vector<std::string> names = connectorsOfInstance(instanceId);
        const Instance* instance = assembly->findInstance(instanceId);
        const QString owner =
            instance != nullptr ? QString::fromStdString(instance->name()) : which;
        if (names.empty()) {
            // NAMED, not silent. A part with no connectors cannot be mated, and
            // the fix is in the PART (§21) -- so the message says where to go.
            statusLeft_->setText(
                QStringLiteral("%1 has no mate connectors. Add one to its part first.")
                    .arg(owner));
            return QString();
        }
        QStringList options;
        for (const std::string& name : names) options << QString::fromStdString(name);
        bool chose = false;
        const QString picked = QInputDialog::getItem(
            this, QStringLiteral("Add Mate"),
            QStringLiteral("Connector on %1").arg(owner), options, 0, false, &chose);
        return chose ? picked : QString();
    };

    const QString leading = pickConnector(chosen[0], QStringLiteral("the first"));
    if (leading.isEmpty()) return;
    const QString following = pickConnector(chosen[1], QStringLiteral("the second"));
    if (following.isEmpty()) return;

    createMateCommand(type, chosen[0], leading, chosen[1], following);
}

void MainWindow::onDeleteMateRequested() { deleteSelectedMate(); }

void MainWindow::onAddRelationRequested() {
    const AssemblyDocument* assembly = AsAssembly(document_);
    if (assembly == nullptr) return;
    const std::vector<ObjectId> chosen = selectedMates();
    if (chosen.empty() || chosen.size() > 2) return;

    // THE TYPE IS THE USER'S, the same rule §20.6 sets for a mate: a gear and
    // a rack and pinion are different machines, and one picked for you is a
    // coupling you did not ask for. The list is filtered by ARITY only --
    // one mate selected can only be a screw, two cannot be one -- because
    // that much the selection already decided.
    static const struct { const char* label; RelationType type; bool oneMate; } kTypes[] = {
        {"Gear -- two rotations, in a fixed ratio", RelationType::Gear, false},
        {"Rack and pinion -- a rotation drives a slide", RelationType::RackAndPinion, false},
        {"Screw -- one mate's own turn drives its own travel", RelationType::Screw, true},
        {"Linear -- two slides, in a fixed ratio", RelationType::Linear, false},
    };
    const bool oneMate = chosen.size() == 1;
    QStringList labels;
    std::vector<RelationType> offered;
    for (const auto& entry : kTypes) {
        if (entry.oneMate != oneMate) continue;
        labels << QString::fromLatin1(entry.label);
        offered.push_back(entry.type);
    }
    if (offered.empty()) return;

    bool chose = false;
    const QString picked = offered.size() == 1
                               ? labels.front()
                               : QInputDialog::getItem(this, QStringLiteral("Add Relation"),
                                                       QStringLiteral("Couple them as:"), labels,
                                                       0, false, &chose);
    if (offered.size() > 1 && !chose) return;
    const int which = labels.indexOf(picked);
    if (which < 0) return;
    const RelationType type = offered[static_cast<std::size_t>(which)];

    // THE UNIT IS IN THE PROMPT, because the number means two different things
    // and nothing else on screen would say which.
    const bool perTurn = type == RelationType::Screw || type == RelationType::RackAndPinion;
    bool ok = false;
    const double ratio = QInputDialog::getDouble(
        this, QStringLiteral("Add Relation"),
        perTurn ? QStringLiteral("Millimetres per turn:") : QStringLiteral("Turns per turn:"),
        perTurn ? 4.0 : 1.0, -1000000.0, 1000000.0, 4, &ok);
    if (!ok) return;

    const ObjectId driver = chosen.front();
    const ObjectId driven = chosen.back(); // the same mate when only one was picked
    createRelationCommand(type, driver, driven, ratio);
}

void MainWindow::onRelationRatioRequested() {
    const AssemblyDocument* assembly = AsAssembly(document_);
    const ObjectId id = selectedRelation();
    if (assembly == nullptr || id == kInvalidObjectId) return;
    const Relation* relation = assembly->findRelation(id);
    if (relation == nullptr) return;
    const bool perTurn = relation->type() == RelationType::Screw ||
                         relation->type() == RelationType::RackAndPinion;
    bool ok = false;
    const double ratio = QInputDialog::getDouble(
        this, QStringLiteral("Relation Ratio"),
        perTurn ? QStringLiteral("Millimetres per turn:") : QStringLiteral("Turns per turn:"),
        relation->ratio(), -1000000.0, 1000000.0, 4, &ok);
    if (!ok) return;
    setSelectedRelationRatio(ratio);
}

void MainWindow::onReverseRelationRequested() { reverseSelectedRelation(); }

void MainWindow::onDeleteRelationRequested() { deleteSelectedRelation(); }

void MainWindow::onDriveMateRequested() {
    bool ok = false;
    const double value = QInputDialog::getDouble(
        this, QStringLiteral("Drive Mate"),
        QStringLiteral("Value (degrees for a turn, mm for a slide)"), 0.0, -1e6, 1e6, 3, &ok);
    if (!ok) return;
    driveSelectedMate(value);
}

void MainWindow::onLimitMateRequested() {
    bool ok = false;
    const double lower = QInputDialog::getDouble(this, QStringLiteral("Limit Mate"),
                                                 QStringLiteral("Lower bound"), 0.0, -1e6, 1e6,
                                                 3, &ok);
    if (!ok) return;
    const double upper = QInputDialog::getDouble(this, QStringLiteral("Limit Mate"),
                                                 QStringLiteral("Upper bound"), 90.0, -1e6, 1e6,
                                                 3, &ok);
    if (!ok) return;
    limitSelectedMate(lower, upper);
}

void MainWindow::onInsertInstanceRequested() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Insert Part"), QString(),
        QStringLiteral("EP3D documents (*.ep3d *.ep3da);;All files (*)"));
    if (path.isEmpty()) return; // cancelled; nothing said, nothing changed
    insertInstanceCommand(path);
}

void MainWindow::onGroundInstanceRequested() { toggleGroundSelectedInstance(); }

void MainWindow::onPatternInstanceRequested() {
    bool ok = false;
    const int count = QInputDialog::getInt(this, QStringLiteral("Pattern Instance"),
                                           QStringLiteral("How many, including the original?"),
                                           3, 2, 999, 1, &ok);
    if (!ok) return;
    const double step = QInputDialog::getDouble(this, QStringLiteral("Pattern Instance"),
                                                QStringLiteral("Step along X, in mm"), 50.0,
                                                -1e6, 1e6, 3, &ok);
    if (!ok) return;
    patternSelectedInstance(count, Vec3{step, 0.0, 0.0});
}

void MainWindow::onDeleteInstanceRequested() { deleteSelectedInstance(); }

void MainWindow::rebuildTree() {
    // WHICH BUILDER, by what the document IS (M27). The node type is shared, so
    // everything below this line -- the widget, the state markers, the colours,
    // the selection -- does not know or care which one produced the tree.
    const OutlineNode root = buildOutline();

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
        // WHEN THERE IS NO STATE, THE COLUMN SAYS WHAT THE ROW DOES.
        //
        // A mate, a relation, a named position and an exploded view have no
        // compute state (they are not graph nodes), so that cell is empty --
        // and a relation whose ratio lives only in a tooltip is a ratio nobody
        // reading the tree can see. The rows that DO have a state keep it;
        // their diagnostic stays in the tooltip, where it does not displace
        // "Failed".
        item->setText(1, presentation.label.isEmpty() && !node.diagnostic.empty()
                             ? QString::fromStdString(node.diagnostic)
                             : presentation.label);
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

DocumentType MainWindow::openedDocumentType() const {
    return document_ != nullptr ? document_->type() : DocumentType::Part;
}

bool MainWindow::insertPadEnabled() const {
    return insertPadAction_ != nullptr && insertPadAction_->isEnabled();
}

PartDocument* MainWindow::partOrNull() const noexcept {
    return dynamic_cast<PartDocument*>(document_);
}

PartDocument& MainWindow::part(std::source_location where) const {
    PartDocument* asPart = partOrNull();
    // THROWS rather than returning a reference to nothing. Reaching a part
    // command on an assembly is a programming error -- the menus are what
    // prevent it -- and a null dereference here would report it as a crash
    // with no name on it. RecomputeContext::part() made the same choice for
    // the same reason.
    if (asPart == nullptr)
        throw std::logic_error(std::string("this command needs a part document, and this "
                                           "one is not -- asked from ") +
                               where.file_name() + ":" + std::to_string(where.line()) +
                               " in " + where.function_name());
    return *asPart;
}

void MainWindow::rebuildProperties() {
    // AN ASSEMBLY'S OWN PANEL. An instance, a mate, a named position and an
    // exploded view are described by AssemblyOutline; a part's parameters,
    // sketches and features by DocumentOutline. Neither knows about the other.
    if (const auto* assembly = dynamic_cast<const AssemblyDocument*>(document_)) {
        showPropertyRows(AssemblyOutline(*assembly).propertiesOf(selectedId_));
        return;
    }
    // ...AND A DRAWING'S. A view, a layer and a linetype are described by
    // DrawingOutline. THE SHEET ITSELF ANSWERS TOO, under the document's own
    // id -- selecting the root row is how a user asks "what size is this
    // paper", and a root row that showed nothing would send them hunting for a
    // dialog.
    if (const auto* drawing = dynamic_cast<const DrawingDocument*>(document_)) {
        showPropertyRows(DrawingOutline(*drawing).propertiesOf(selectedId_));
        return;
    }
    const DocumentOutline outline(part());

    // WHAT THE CANVAS HAS PICKED WINS while a sketch is open (M26.7).
    //
    // In sketch mode the model tree is hidden, so `selectedId_` is whatever was
    // last chosen in 3D -- a solid, or nothing. Describing that while the user
    // is clicking lines is answering a question nobody asked, and it left the
    // panel blank for every click inside a sketch.
    //
    // ONE thing only. Two picked elements are a selection for a CONSTRAINT, and
    // a panel that showed the first of them would be quietly describing half of
    // what is highlighted.
    if (inSketchMode() && sketchCanvas_ != nullptr &&
        sketchCanvas_->selection().size() == 1) {
        const std::vector<PropertyRow> picked =
            outline.propertiesOfSketchElement(editingSketch_, sketchCanvas_->selection().front());
        if (!picked.empty()) {
            showPropertyRows(picked);
            return;
        }
    }

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

    showPropertyRows(rows);
}

// The RENDERING half of the panel, shared by both things that fill it: the
// tree's selection and the sketch canvas's. Two copies of this loop would be
// two places for "how a property row looks" to drift apart.
void MainWindow::showPropertyRows(const std::vector<PropertyRow>& rows) {
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
                const Parameter* driver = part().parameters().findById(row.parameterId);
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
    // Reconstruction provenance is a PART's: it maps a sketch to what a
    // DXF import made of it, and an assembly has neither.
    if (partOrNull() == nullptr) return;
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
        it = part().findSketch(it->first) == nullptr ? reconstructionReports_.erase(it)
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
    // AN ASSEMBLY HAS NO MASS PROPERTIES of its own (M27). It has instances,
    // each with a volume of their own, and adding them up is a different
    // number with a different meaning -- so this reports what an assembly IS
    // rather than a total nobody asked for.
    //
    // Found by the M27 gate: this ran on every refresh and threw out of
    // part(), which is the accessor doing its job. Every OTHER part-shaped
    // call in the shell is behind a disabled menu; this one is behind nothing,
    // because a status bar updates itself.
    if (const auto* assembly = dynamic_cast<const AssemblyDocument*>(document_)) {
        const std::size_t instances = assembly->instances().size();
        const std::size_t mates = assembly->mates().size();
        statusRight_->setText(QStringLiteral("%1 instance%2   %3 mate%4")
                                  .arg(instances)
                                  .arg(instances == 1 ? "" : "s")
                                  .arg(mates)
                                  .arg(mates == 1 ? "" : "s"));
        return;
    }
    // A DRAWING COUNTS SHEETS AND VIEWS, not volume. What a status bar says
    // is per document type, and it is the one place in this shell that is
    // behind no menu at all -- so a type it does not know must DEGRADE, never
    // throw.
    //
    // The third type found this the way the second did: `part()` throws, the
    // exception escapes a refresh path, and the program aborts with no message
    // (M27.3). The fix then was to bail at the top of refreshCommandStates;
    // the fix now is that this function no longer calls `part()` at all.
    if (const auto* drawing = dynamic_cast<const DrawingDocument*>(document_)) {
        const std::size_t views = drawing->views().size();
        const std::size_t stale = drawing->staleViews().size();
        QString text = QStringLiteral("%1   %2   %3 view%4")
                           .arg(QString::fromUtf8(
                               std::string(toString(drawing->sheet().size())).c_str()))
                           .arg(QString::fromStdString(drawing->sheet().scale().toString()))
                           .arg(views)
                           .arg(views == 1 ? "" : "s");
        // OUT OF DATE IS WORTH SAYING WITHOUT BEING ASKED. A drawing that
        // quietly shows an old part is the failure this whole block is for.
        if (stale != 0) text += QStringLiteral("   %1 out of date").arg(stale);
        statusRight_->setText(text);
        return;
    }
    const PartDocument* asPart = partOrNull();
    if (asPart == nullptr) {
        statusRight_->clear();
        return;
    }
    const MassProperties& mp = asPart->massProperties();
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
    // NOT a temporary status message.
    //
    // showMessage() writes over the same strip statusLeft_ owns, so for eight
    // seconds after start-up two things were drawing in one place -- and every
    // golden screenshot taken in that window came out with the lines
    // superimposed and unreadable. Two writers to one place is this project's
    // recurring defect in its smallest form; the title bar above already keeps
    // the socket visible for as long as it is open, which is the stronger
    // guarantee anyway.
    statusLeft_->setText(
        QStringLiteral("Listening for scripts on 127.0.0.1:%1 -- loopback only").arg(port));
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
    // Hiding is view state keyed by ObjectId and works for either document
    // type -- but the walk below asks a PART for its tree in order to find out
    // which ids are still real. For an assembly the same question is asked of
    // the assembly's own tree, which buildOutline() already picks.
    if (partOrNull() == nullptr) {
        std::set<ObjectId> hidden;
        const std::function<void(const OutlineNode&)> visitAssembly =
            [&](const OutlineNode& node) {
                if (node.id != kInvalidObjectId && presenter_->isHidden(node.id))
                    hidden.insert(node.id);
                for (const OutlineNode& child : node.children) visitAssembly(child);
            };
        if (const auto* assembly = dynamic_cast<const AssemblyDocument*>(document_))
            visitAssembly(AssemblyOutline(*assembly).build());
        return hidden;
    }
    // Ask the presenter which ids are hidden, without the outline needing any
    // notion of a viewer.
    std::set<ObjectId> hidden;
    const DocumentOutline outline(part());
    const std::function<void(const OutlineNode&)> visit = [&](const OutlineNode& node) {
        if (node.id != kInvalidObjectId && presenter_->isHidden(node.id)) hidden.insert(node.id);
        for (const OutlineNode& child : node.children) visit(child);
    };
    visit(outline.build());
    return hidden;
}

void MainWindow::reportHealth() {
    // THE TREE, from whichever builder the document calls for -- the thing this
    // reads is a failed ROW, and a row is a row whatever produced it. Asking
    // DocumentOutline directly was the last part-shaped call left on the
    // refresh path.
    const OutlineNode root = buildOutline();
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
    // The status bar runs on every refresh with no menu in front of it.
    if (partOrNull() == nullptr) return QString();
    if (selectedId_ == kInvalidObjectId) return QStringLiteral("No selection");
    // A name, not an ObjectId: UI spec 17 requires that no task need knowledge
    // of internal ids or developer terminology, and the status bar was showing
    // "Selected object 12".
    const DocumentOutline outline(part());
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
        ApplyPropertyEdit(part(), parameterId, field, typed);

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

std::string MainWindow::propertyRowValue(const std::string& label) const {
    for (int row = 0; row < properties_->rowCount(); ++row) {
        const QTableWidgetItem* name = properties_->item(row, 0);
        const QTableWidgetItem* value = properties_->item(row, 1);
        if (name == nullptr || value == nullptr) continue;
        const QString text = name->text();
        const int slash = text.lastIndexOf(QStringLiteral(" / "));
        const QString bare = slash < 0 ? text : text.mid(slash + 3);
        if (bare == QString::fromStdString(label)) return value->text().toStdString();
    }
    return std::string();
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
    if (partOrNull() == nullptr) return kInvalidObjectId;
    if (indexOut != nullptr) *indexOut = static_cast<std::size_t>(-1);
    if (document_ == nullptr || selectedId_ == kInvalidObjectId) return kInvalidObjectId;
    for (const auto& body : part().bodies())
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
    // --- AN ASSEMBLY HAS NO PADS (M27) --------------------------------------
    //
    // EARLY, not at the end. Everything below asks a PART-shaped question --
    // is a sketch selected, is there a solid to dress, is there a tail to
    // build on -- and computing those answers for a document that has none of
    // them is how part() came to be reached on every refresh. Undo and Redo
    // above are already settled, and they belong to any document.
    //
    // THIS IS WHAT KEEPS part() FROM BEING REACHED. That accessor throws by
    // design; this is the reason it never has to. Disabling is also the honest
    // form of the refusal -- a command that is offered and then explains
    // itself is worse than one that was never offered, because the refusal
    // arrives after the click.
    // THE ASSEMBLY MENU, enabled from the model exactly as everything else is.
    // Insert Part needs only an assembly; the other three need something
    // selected in it, and offering them with nothing selected would be a
    // command that refuses after the click.
    {
        const bool isDrawing = AsDrawing(document_) != nullptr;
        const bool isAssembly = partOrNull() == nullptr && document_ != nullptr && !isDrawing;
        // ONE BAR OR THE OTHER, never both and never neither -- AND NEITHER
        // WHILE A SKETCH IS OPEN.
        //
        // That last clause is not decoration: M26.2 hides the model toolbar on
        // entering sketch mode, because its commands act on FEATURES and a
        // sketch has none. This rule runs on every refresh, so without the
        // check it turned that bar straight back on and undid M26.2 from a
        // completely different function.
        const bool sketching = inSketchMode();
        if (assemblyToolBar_ != nullptr)
            assemblyToolBar_->setVisible(isAssembly && !sketching);
        if (drawingToolBar_ != nullptr) drawingToolBar_->setVisible(isDrawing && !sketching);
        if (modelToolBar_ != nullptr)
            modelToolBar_->setVisible(!isAssembly && !isDrawing && !sketching);

        // THE CANVAS FOLLOWS THE DOCUMENT. A drawing is looked at as a piece
        // of paper -- no orbit, no perspective -- so it gets its own page in
        // the stack rather than sharing the 3D view's gestures.
        if (drawingCanvas_ != nullptr && centralStack_ != nullptr && !sketching) {
            drawingCanvas_->setDocument(AsDrawing(document_));
            if (isDrawing && centralStack_->currentWidget() != drawingCanvas_)
                centralStack_->setCurrentWidget(drawingCanvas_);
            else if (!isDrawing && centralStack_->currentWidget() == drawingCanvas_)
                centralStack_->setCurrentWidget(viewer_);
            if (isDrawing) drawingCanvas_->update();
        }

        // THE MENU STAYS REACHABLE FROM EVERY DOCUMENT, because "New Drawing"
        // lives in it -- a Drawing menu greyed out on a part is a menu you can
        // never use to make your first drawing. The ITEMS are gated instead.
        //
        // Written out because the first draft said `isDrawing || newDrawingAction_`,
        // which is a pointer in a boolean and therefore always true: the right
        // behaviour by accident, and the next person to read it would have
        // "fixed" it.
        if (drawingMenu_ != nullptr) drawingMenu_->setEnabled(true);
        if (newDrawingAction_ != nullptr) newDrawingAction_->setEnabled(true);
        const bool haveDrawingView = isDrawing && selectedDrawingView() != kInvalidObjectId;
        if (addBaseViewAction_ != nullptr) addBaseViewAction_->setEnabled(isDrawing);
        if (addProjectedViewAction_ != nullptr)
            addProjectedViewAction_->setEnabled(haveDrawingView);
        if (updateViewsAction_ != nullptr)
            updateViewsAction_->setEnabled(isDrawing && staleViewCountForTesting() != 0);
        if (sheetSetupAction_ != nullptr) sheetSetupAction_->setEnabled(isDrawing);
        if (addLayerAction_ != nullptr) addLayerAction_->setEnabled(isDrawing);
        if (deleteDrawingObjectAction_ != nullptr)
            deleteDrawingObjectAction_->setEnabled(isDrawing && selectedId_ != kInvalidObjectId);

        // A DIMENSION TOOL IS ONLY LIVE WHEN IT WOULD SUCCEED. Asking the same
        // proposal the command asks means an enabled button never answers "no
        // dimension: ..." -- and it cannot drift from the command's rule,
        // because there is only the one rule.
        const DrawingDocument* asDrawing = AsDrawing(document_);
        const std::vector<ObjectId> pickedEntities = selectedDrawingEntities();
        const auto canDimension = [&](DimensionKind kind) {
            return asDrawing != nullptr && asDrawing->proposeDimension(kind, pickedEntities).ok;
        };
        const bool linear = canDimension(DimensionKind::Linear);
        if (linearDimensionAction_ != nullptr) linearDimensionAction_->setEnabled(linear);
        if (horizontalDimensionAction_ != nullptr)
            horizontalDimensionAction_->setEnabled(linear);
        if (verticalDimensionAction_ != nullptr) verticalDimensionAction_->setEnabled(linear);
        if (radiusDimensionAction_ != nullptr)
            radiusDimensionAction_->setEnabled(canDimension(DimensionKind::Radius));
        if (diameterDimensionAction_ != nullptr)
            diameterDimensionAction_->setEnabled(canDimension(DimensionKind::Diameter));
        if (angularDimensionAction_ != nullptr)
            angularDimensionAction_->setEnabled(canDimension(DimensionKind::Angular));
        if (dimensionTextAction_ != nullptr)
            dimensionTextAction_->setEnabled(selectedDimension() != kInvalidObjectId);
        if (dimensionStyleAction_ != nullptr) dimensionStyleAction_->setEnabled(isDrawing);
        for (QAction* tool : {drawLineAction_, drawCircleAction_, drawRectangleAction_,
                              titleBlockAction_, frameAction_, plotPdfAction_,
                              exportDxfAction_, addBomAction_, placeSymbolAction_,
                              drawWireAction_})
            if (tool != nullptr) tool->setEnabled(isDrawing);
        // ONLY LIVE WHEN THERE IS SOMETHING TO DO, so an enabled command never
        // does nothing.
        if (numberNetsAction_ != nullptr)
            numberNetsAction_->setEnabled(isDrawing && wireCountForTesting() != 0);
        if (turnSymbolAction_ != nullptr)
            turnSymbolAction_->setEnabled(
                isDrawing && asDrawing != nullptr &&
                asDrawing->findSymbol(selectedId_) != nullptr);
        // ONLY LIVE WHEN THERE IS SOMETHING TO RE-COUNT, so an enabled command
        // never does nothing.
        if (recountBomAction_ != nullptr)
            recountBomAction_->setEnabled(isDrawing && staleBomCountForTesting() != 0);
        const bool haveInstance = selectedInstance() != kInvalidObjectId;
        if (assemblyMenu_ != nullptr) assemblyMenu_->setEnabled(isAssembly);
        if (insertInstanceAction_ != nullptr) insertInstanceAction_->setEnabled(isAssembly);
        if (groundInstanceAction_ != nullptr)
            groundInstanceAction_->setEnabled(isAssembly && haveInstance);
        if (patternInstanceAction_ != nullptr)
            patternInstanceAction_->setEnabled(isAssembly && haveInstance);
        if (deleteInstanceAction_ != nullptr)
            deleteInstanceAction_->setEnabled(isAssembly && haveInstance);

        // A MATE NEEDS TWO INSTANCES SELECTED, which is the selection the
        // command actually consumes -- so the menu offers it exactly when it
        // would work, rather than refusing after the click.
        const bool haveTwo = isAssembly && selectedInstances().size() == 2;
        const bool haveMate = isAssembly && selectedMate() != kInvalidObjectId;
        if (addMateAction_ != nullptr) addMateAction_->setEnabled(haveTwo);
        if (driveMateAction_ != nullptr) driveMateAction_->setEnabled(haveMate);
        if (limitMateAction_ != nullptr) limitMateAction_->setEnabled(haveMate);
        if (deleteMateAction_ != nullptr) deleteMateAction_->setEnabled(haveMate);

        // A RELATION NEEDS THE MATES SELECTED, and how many decides which
        // types are on offer: two for a gear, a rack or a linear ratio, one
        // for a screw. Enabled exactly when the command would work.
        const std::size_t mateCount = isAssembly ? selectedMates().size() : 0u;
        const bool haveRelation = isAssembly && selectedRelation() != kInvalidObjectId;
        if (addRelationAction_ != nullptr)
            addRelationAction_->setEnabled(mateCount == 1 || mateCount == 2);
        if (relationRatioAction_ != nullptr) relationRatioAction_->setEnabled(haveRelation);
        if (reverseRelationAction_ != nullptr) reverseRelationAction_->setEnabled(haveRelation);
        if (deleteRelationAction_ != nullptr) deleteRelationAction_->setEnabled(haveRelation);

        const bool havePosition = isAssembly && selectedNamedPosition() != kInvalidObjectId;
        const bool haveView = isAssembly && selectedExplodeView() != kInvalidObjectId;
        if (capturePositionAction_ != nullptr) capturePositionAction_->setEnabled(isAssembly);
        if (applyPositionAction_ != nullptr) applyPositionAction_->setEnabled(havePosition);
        if (addExplodeViewAction_ != nullptr) addExplodeViewAction_->setEnabled(isAssembly);
        if (addExplodeStepAction_ != nullptr)
            addExplodeStepAction_->setEnabled(haveView || (isAssembly && haveInstance));
        if (showExplodeAction_ != nullptr) {
            showExplodeAction_->setEnabled(haveView);
            // TICKED WHEN IT IS THE ONE BEING DRAWN. The tick is the only place
            // "you are looking at a picture, not the model" is written down.
            showExplodeAction_->setChecked(
                haveView && presenter_ != nullptr &&
                presenter_->shownExplodeView() == selectedExplodeView());
        }
        if (explodePreviewAction_ != nullptr) explodePreviewAction_->setEnabled(haveView);
        if (interferenceAction_ != nullptr) interferenceAction_->setEnabled(isAssembly);
    }

    if (partOrNull() == nullptr) {
        for (QAction* action : partOnlyActions())
            if (action != nullptr) action->setEnabled(false);
        for (QAction* action : sketchModeActions_)
            if (action != nullptr) action->setEnabled(false);
        return;
    }

    const ObjectId body = selectedFeatureBody();
    if (suppressAction_ != nullptr) suppressAction_->setEnabled(body != kInvalidObjectId);
    if (rollbackAction_ != nullptr) rollbackAction_->setEnabled(body != kInvalidObjectId);
    if (rollForwardAction_ != nullptr)
        rollForwardAction_->setEnabled(!part().bodies().empty());

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
    // Revolve takes a SELECTED SKETCH -- its own menu text says so -- and it
    // was never in this function at all, so it sat enabled with nothing
    // selected and refused when pressed. Found by LOOKING at the toolbar,
    // which is the one check a fingerprint of the icons cannot make: every
    // test asked whether the button existed, none asked what it offered.
    if (insertRevolveAction_ != nullptr) insertRevolveAction_->setEnabled(haveSketch);
    // The same rule the commands themselves enforce, so the toolbar cannot
    // offer something the command would refuse (M9's "availability cannot
    // differ between the two surfaces").
    const std::size_t sketchesChosen = selectedSketches().size();
    const bool haveSolid = currentTail() != kInvalidObjectId;
    const bool twoSolids = unconsumedSolids().size() >= 2;
    if (insertSweepAction_ != nullptr) insertSweepAction_->setEnabled(sketchesChosen == 2);
    if (insertLoftAction_ != nullptr) insertLoftAction_->setEnabled(sketchesChosen >= 2);
    if (insertShellAction_ != nullptr) insertShellAction_->setEnabled(haveSolid);
    if (insertHoleAction_ != nullptr) insertHoleAction_->setEnabled(haveSolid && haveSketch);
    if (insertUnionAction_ != nullptr) insertUnionAction_->setEnabled(twoSolids);
    if (insertSubtractAction_ != nullptr) insertSubtractAction_->setEnabled(twoSolids);
    if (insertIntersectAction_ != nullptr) insertIntersectAction_->setEnabled(twoSolids);
    if (insertCircularPatternAction_ != nullptr)
        insertCircularPatternAction_->setEnabled(haveSolid);
    if (insertCurvePatternAction_ != nullptr)
        insertCurvePatternAction_->setEnabled(haveSolid && haveSketch);
    if (exportAction_ != nullptr) exportAction_->setEnabled(haveSolid);
    if (importAction_ != nullptr) importAction_->setEnabled(true);
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

std::vector<QAction*> MainWindow::partOnlyActions() const {
    // ONE LIST, because "which commands need a part" is one fact. Built from
    // the members rather than from a hand-kept name list, so a command that is
    // added and forgotten here is a compile-visible omission rather than a
    // button that quietly works on the wrong document type.
    return {insertPadAction_,      insertPocketAction_,    insertRevolveAction_,
            insertSweepAction_,    insertLoftAction_,      insertShellAction_,
            insertHoleAction_,     insertUnionAction_,     insertSubtractAction_,
            insertIntersectAction_, insertCircularPatternAction_,
            insertCurvePatternAction_, insertFilletAction_, insertChamferAction_,
            exportAction_,         sketchOnFaceAction_,    newSketchAction_,
            editSketchAction_,     finishSketchAction_,    suppressAction_,
            rollbackAction_,       rollForwardAction_,     deleteObjectAction_};
}

ObjectId MainWindow::selectedSketch() const {
    // An assembly has no sketches, so nothing is selected in one.
    if (partOrNull() == nullptr) return kInvalidObjectId;
    if (document_ == nullptr) return kInvalidObjectId;
    for (const Sketch* sketch : part().sketches())
        if (sketch->id() == selectedId_) return sketch->id();
    return kInvalidObjectId;
}

ObjectId MainWindow::currentTail() const {
    // ...and no feature chain to have a tail.
    if (partOrNull() == nullptr) return kInvalidObjectId;
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
    if (partOrNull() == nullptr) return base;
    std::set<std::string> taken;
    // PARAMETERS TOO, and this half is not cosmetic: an expression names a
    // parameter by NAME (`#PocketDepth`), and findByName answers with the
    // first match. Two parameters called PocketDepth means every expression
    // mentioning it binds to whichever was created first -- and the panel
    // shows two identical rows, so the user cannot see which one they are
    // editing either.
    for (const auto& parameter : part().parameters().items())
        taken.insert(parameter->name());
    for (const Sketch* sketch : part().sketches()) taken.insert(sketch->name());
    for (const auto& body : part().bodies())
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
    if (part().bodies().empty()) part().addBody("Body001");
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Pad");
    Parameter& length =
        part().addParameter(uniqueObjectName("PadLength"), 20.0, UnitType::Millimeter);
    PadFeature& pad = part().addPadFeature(body, uniqueObjectName("Pad"), sketch, length.id());
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
    Body& body = *part().bodies().front();
    // Measured BEFORE the pocket exists, so the comparison below is against the
    // solid the user was actually looking at.
    const double volumeBefore = part().massProperties().volumeMm3;
    document_->beginTransaction("Insert Pocket");
    Parameter& depth =
        part().addParameter(uniqueObjectName("PocketDepth"), 10.0, UnitType::Millimeter);
    PocketFeature& pocket = part().addPocketFeature(body, uniqueObjectName("Pocket"), base, sketch, depth.id());
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
        const double volumeAfter = part().massProperties().volumeMm3;
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
    for (const auto& body : part().bodies())
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

// --- M19-M22 features, on the Model toolbar ---------------------------------
//
// Every one of these shipped with "UI: script and API only" recorded against
// it, from M19 to M22. What each command needs is exactly what the script verb
// needs, and the RULES are the script's rules -- a boolean takes the body's two
// unconsumed solids in the order they were drawn, a hole with no depth goes all
// the way through, a ring turns about the world Z. Restating them differently
// here would be two answers to one question.
//
// The shape of every one is Pad's: refuse BEFORE anything is created, so a
// refusal costs the user nothing to undo; one transaction; then the FEATURE's
// own diagnostic rather than "created" regardless (ADR-M17-022).

std::vector<ObjectId> MainWindow::selectedSketches() const {
    if (partOrNull() == nullptr) return {};
    std::vector<ObjectId> found;
    if (document_ == nullptr || tree_ == nullptr) return found;
    std::set<ObjectId> chosen;
    for (const QTreeWidgetItem* item : tree_->selectedItems())
        chosen.insert(static_cast<ObjectId>(item->data(0, kIdRole).toULongLong()));
    // IN DOCUMENT ORDER, walked from the document rather than read out of the
    // selection. Qt does not keep the order things were clicked in, and a
    // command whose answer depended on an order nobody can see would be a
    // command nobody can predict. Document order is a rule the user controls:
    // it is the order the sketches were made in, and it is what the tree shows.
    for (const Sketch* sketch : part().sketches())
        if (chosen.count(sketch->id()) != 0) found.push_back(sketch->id());
    return found;
}

std::vector<ObjectId> MainWindow::unconsumedSolids() const {
    if (partOrNull() == nullptr) return {};
    if (presenter_ == nullptr) return {};
    return presenter_->displayableSolids();
}

MainWindow::PickedFaceQuery MainWindow::selectionForFace() const {
    // Faces belong to a part's solids. An assembly picks instances.
    if (partOrNull() == nullptr) return PickedFaceQuery{};
    PickedFaceQuery out;
    if (viewer_ == nullptr || viewer_->pickedFace().createdBy == 0) {
        out.refusal = QStringLiteral("Click a face in the 3D view first");
        return out;
    }
    // The same three conditions a face sketch uses (ADR-M17-036), from the same
    // pick -- so this cannot disagree with what the user clicked. `createdBy`
    // is provenance and survives the geometry moving; the two directions narrow
    // it to the one face.
    out.query.createdBy = static_cast<ObjectId>(viewer_->pickedFace().createdBy);
    out.query.facing = viewer_->pickedFace().normal;
    out.query.extremeTowards = viewer_->pickedFace().normal;
    out.words = DescribeFaceQuery(out.query);
    return out;
}

QString MainWindow::insertSweepFromSelection() {
    const std::vector<ObjectId> sketches = selectedSketches();
    if (sketches.size() != 2) {
        const QString message =
            QStringLiteral("Select TWO sketches to sweep: the profile and the path, in the "
                           "order they appear in the tree");
        statusLeft_->setText(message);
        return message;
    }
    if (part().bodies().empty()) part().addBody("Body001");
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Sweep");
    SweepFeature& sweep =
        part().addSweepFeature(body, uniqueObjectName("Sweep"), sketches[0], sketches[1]);
    document_->commitTransaction();
    const ObjectId created = sweep.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Sweep", created, report);
    if (message.startsWith(QStringLiteral("Sweep created")))
        message += QStringLiteral("; %1 swept along %2")
                       .arg(QString::fromStdString(document_->objectName(sketches[0])),
                            QString::fromStdString(document_->objectName(sketches[1])));
    sketchMessage_.clear();
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::insertLoftFromSelection() {
    const std::vector<ObjectId> sketches = selectedSketches();
    if (sketches.size() < 2) {
        const QString message =
            QStringLiteral("Select two or more sketches to loft through, in the order they "
                           "appear in the tree -- the order IS the shape");
        statusLeft_->setText(message);
        return message;
    }
    if (part().bodies().empty()) part().addBody("Body001");
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Loft");
    LoftFeature& loft = part().addLoftFeature(body, uniqueObjectName("Loft"), sketches);
    document_->commitTransaction();
    const ObjectId created = loft.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Loft", created, report);
    if (message.startsWith(QStringLiteral("Loft created")))
        message += QStringLiteral(" through %1 sections").arg(sketches.size());
    sketchMessage_.clear();
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::insertShellOnTail() {
    const ObjectId base = currentTail();
    if (base == kInvalidObjectId) {
        const QString message = QStringLiteral("No solid to shell");
        statusLeft_->setText(message);
        return message;
    }
    // WHICH FACE TO OPEN, decided before anything is created.
    const PickedFaceQuery opening = selectionForFace();
    if (!opening.refusal.isEmpty()) {
        const QString message = QStringLiteral("Shell: %1 -- that face becomes the opening")
                                    .arg(opening.refusal);
        statusLeft_->setText(message);
        return message;
    }
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Shell");
    Parameter& thickness =
        part().addParameter(uniqueObjectName("ShellThickness"), 2.0, UnitType::Millimeter);
    ShellFeature& shell = part().addShellFeature(body, uniqueObjectName("Shell"), base,
                                                     FaceSelection{opening.query}, thickness.id());
    document_->commitTransaction();
    const ObjectId created = shell.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Shell", created, report);
    if (message.startsWith(QStringLiteral("Shell created")))
        message += QStringLiteral(", open at %1 -- edit its Thickness in the panel")
                       .arg(QString::fromStdString(opening.words));
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::insertHoleFromSelection() {
    const ObjectId base = currentTail();
    if (base == kInvalidObjectId) {
        const QString message = QStringLiteral("No solid to drill");
        statusLeft_->setText(message);
        return message;
    }
    const ObjectId sketch = selectedSketch();
    if (sketch == kInvalidObjectId) {
        const QString message =
            QStringLiteral("Select a sketch of POINTS to drill -- each point becomes a bore");
        statusLeft_->setText(message);
        return message;
    }
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Hole");
    Parameter& diameter =
        part().addParameter(uniqueObjectName("HoleDiameter"), 5.0, UnitType::Millimeter);
    // ZERO DEPTH MEANS ALL THE WAY THROUGH, which is what a hole usually is and
    // what the feature spells a depth of nought as. The same rule the script's
    // `hole` verb follows when its depth is left out.
    Parameter& depth =
        part().addParameter(uniqueObjectName("HoleDepth"), 0.0, UnitType::Millimeter);
    HoleFeature& hole = part().addHoleFeature(body, uniqueObjectName("Hole"), base, sketch,
                                                  diameter.id(), depth.id());
    document_->commitTransaction();
    const ObjectId created = hole.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Hole", created, report);
    if (message.startsWith(QStringLiteral("Hole created")))
        message += QStringLiteral(
            " through the part -- edit its Diameter, or set a Depth, in the panel");
    sketchMessage_.clear();
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::insertBooleanOnTail(BooleanOperation operation, const char* what) {
    const std::vector<ObjectId> loose = unconsumedSolids();
    if (loose.size() < 2) {
        const QString message =
            QStringLiteral("%1 needs TWO separate solids in the body; there %2 %3. "
                           "Pad into the same body twice to make them.")
                .arg(QString::fromUtf8(what),
                     loose.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                     QString::number(loose.size()));
        statusLeft_->setText(message);
        return message;
    }
    // THE LAST TWO, in the order they were drawn -- so Subtract removes the
    // later one from the earlier, which is the order the tree reads in. The
    // script's boolean verbs take the same two in the same order.
    const ObjectId target = loose[loose.size() - 2];
    const ObjectId tool = loose[loose.size() - 1];
    Body& body = *part().bodies().front();
    document_->beginTransaction(std::string("Insert ") + what);
    BooleanFeature& boolean = part().addBooleanFeature(body, uniqueObjectName(what),
                                                            operation, target, tool);
    document_->commitTransaction();
    const ObjectId created = boolean.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature(what, created, report);
    if (message.startsWith(QString::fromUtf8(what) + QStringLiteral(" created")))
        message += QStringLiteral(" from %1 and %2")
                       .arg(QString::fromStdString(document_->objectName(target)),
                            QString::fromStdString(document_->objectName(tool)));
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::insertCircularPatternOnTail() {
    const ObjectId base = currentTail();
    if (base == kInvalidObjectId) {
        const QString message = QStringLiteral("No solid to pattern");
        statusLeft_->setText(message);
        return message;
    }
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Circular Pattern");
    // THE AXIS IS THE WORLD Z through the origin, which is what a frame at the
    // origin gives -- the same convention the script's `ring` verb uses, and
    // the same +Z a mirror's normal and a linear pattern's direction follow.
    ReferenceFrame& axis = document_->addFrame(uniqueObjectName("RingAxis"));
    Parameter& count =
        part().addParameter(uniqueObjectName("RingCount"), 4.0, UnitType::Unitless);
    // THE STEP IS PER INSTANCE, not a total sweep: four at ninety degrees is a
    // full ring. Four at three-hundred-and-sixty would be four copies on top of
    // each other.
    Parameter& step = part().addParameter(uniqueObjectName("RingStep"),
                                              90.0 * 3.14159265358979323846 / 180.0,
                                              UnitType::Radian);
    CircularPatternFeature& ring = part().addCircularPatternFeature(
        body, uniqueObjectName("Ring"), base, axis.id(), count.id(), step.id());
    document_->commitTransaction();
    const ObjectId created = ring.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Ring", created, report);
    if (message.startsWith(QStringLiteral("Ring created")))
        message += QStringLiteral(
            " -- four at 90 deg about the origin's Z; edit RingCount and RingStep in the panel");
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::insertCurvePatternFromSelection() {
    const ObjectId base = currentTail();
    if (base == kInvalidObjectId) {
        const QString message = QStringLiteral("No solid to pattern");
        statusLeft_->setText(message);
        return message;
    }
    const ObjectId path = selectedSketch();
    if (path == kInvalidObjectId) {
        const QString message =
            QStringLiteral("Select the sketch whose curve the copies follow");
        statusLeft_->setText(message);
        return message;
    }
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Curve Pattern");
    Parameter& count =
        part().addParameter(uniqueObjectName("AlongCount"), 5.0, UnitType::Unitless);
    CurvePatternFeature& along = part().addCurvePatternFeature(
        body, uniqueObjectName("Along"), base, path, count.id());
    document_->commitTransaction();
    const ObjectId created = along.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Along", created, report);
    if (message.startsWith(QStringLiteral("Along created")))
        message += QStringLiteral(" along %1 -- edit AlongCount in the panel")
                       .arg(QString::fromStdString(document_->objectName(path)));
    sketchMessage_.clear();
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
}

QString MainWindow::exportCurrentSolid() {
    const ObjectId base = currentTail();
    if (base == kInvalidObjectId) {
        const QString message = QStringLiteral("No solid to export");
        statusLeft_->setText(message);
        return message;
    }
    const ISolidFeature* solid = nullptr;
    for (const auto& body : part().bodies())
        for (const auto& feature : body->features())
            if (feature->id() == base) solid = dynamic_cast<const ISolidFeature*>(feature.get());
    if (solid == nullptr || solid->currentState() != ComputeState::Valid ||
        !solid->currentShape().isValid()) {
        const QString message = QStringLiteral("That solid has not been built yet -- recompute "
                                               "before exporting");
        statusLeft_->setText(message);
        return message;
    }
    IGeometryKernel* kernel = document_->geometryKernel();
    if (kernel == nullptr) {
        const QString message = QStringLiteral("No geometry kernel configured");
        statusLeft_->setText(message);
        return message;
    }

    // THE FORMAT COMES FROM THE EXTENSION, and the filter is how the dialog
    // says so. There is no format argument anywhere in EP3D (ADR-M22-001): a
    // .step written as STL is a file whose name lies about it.
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export solid"), QString(),
        QStringLiteral("STEP (*.step *.stp);;STL (*.stl)"));
    if (path.isEmpty()) return QStringLiteral("Export cancelled");

    const QString lower = path.toLower();
    IoResult written;
    if (lower.endsWith(QStringLiteral(".stl"))) {
        // The same default deflection the script uses, and said out loud for
        // the same reason: a mesh written at a tolerance nobody chose is a mesh
        // nobody can reproduce.
        written = kernel->exportStl(solid->currentShape(), path.toStdString(), 0.05);
    } else if (lower.endsWith(QStringLiteral(".step")) || lower.endsWith(QStringLiteral(".stp"))) {
        written = kernel->exportStep(solid->currentShape(), path.toStdString());
    } else {
        const QString message =
            QStringLiteral("'%1' has no extension this can write; use .step or .stl").arg(path);
        statusLeft_->setText(message);
        return message;
    }
    const QString message =
        written ? QStringLiteral("Exported %1").arg(QFileInfo(path).fileName())
                : QStringLiteral("Export failed: %1")
                      .arg(QString::fromStdString(written.message));
    statusLeft_->setText(message);
    return message;
}

QString MainWindow::importSolidAsFeature() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import STEP"), QString(),
        QStringLiteral("STEP (*.step *.stp)"));
    if (path.isEmpty()) return QStringLiteral("Import cancelled");

    if (part().bodies().empty()) part().addBody("Body001");
    Body& body = *part().bodies().front();
    document_->beginTransaction("Import");
    // IT STORES THE PATH, NOT THE GEOMETRY (ADR-M22-003). The file is the
    // source of truth and is read again on every rebuild -- so a re-exported
    // source shows up here, and a source that went away stops this feature by
    // name rather than leaving a copy nobody can trace back to anything.
    ImportFeature& brought =
        part().addImportFeature(body, uniqueObjectName("Import"), path.toStdString());
    document_->commitTransaction();
    const ObjectId created = brought.id();
    const DocumentRecomputeReport report = document_->recompute();
    refreshAll();
    selectObject(created);
    QString message = describeCreatedFeature("Import", created, report);
    if (message.startsWith(QStringLiteral("Import created")))
        message += QStringLiteral(" from %1 -- it is re-read on every rebuild")
                       .arg(QFileInfo(path).fileName());
    statusLeft_->setText(message);
    refreshCommandStates();
    return message;
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
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Fillet");
    Parameter& radius =
        part().addParameter(uniqueObjectName("FilletRadius"), 2.0, UnitType::Millimeter);
    FilletFeature& fillet = part().addFilletFeature(body, uniqueObjectName("Fillet"), base, radius.id());
    part().setFeatureEdgeSelection(fillet.id(), chosen.selection);
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
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Chamfer");
    Parameter& distance =
        part().addParameter(uniqueObjectName("ChamferDistance"), 2.0,
                                UnitType::Millimeter);
    ChamferFeature& chamfer =
        part().addChamferFeature(body, uniqueObjectName("Chamfer"), base, distance.id());
    part().setFeatureEdgeSelection(chamfer.id(), chosen.selection);
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
    const Sketch* sketch = part().findSketch(sketchId);
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
    const Sketch* sketch = document_ != nullptr ? part().findSketch(sketchId) : nullptr;
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
    const Sketch* sketch = part().findSketch(sketchId);
    if (sketch == nullptr || sketch->findEntity(axisEntityId) == nullptr) {
        const QString message = QStringLiteral("That axis is not in the selected sketch");
        statusLeft_->setText(message);
        return message;
    }

    if (part().bodies().empty()) part().addBody("Body001");
    Body& body = *part().bodies().front();
    document_->beginTransaction("Insert Revolve");
    Parameter& angle = part().addParameter(uniqueObjectName("RevolveAngle"),
                                               angleDegrees * 3.14159265358979323846 / 180.0,
                                               UnitType::Radian);
    RevolveFeature& revolve =
        part().addRevolveFeature(body, uniqueObjectName("Revolve"), sketchId, axisEntityId,
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
void MainWindow::onInsertSweepRequested() { insertSweepFromSelection(); }
void MainWindow::onInsertLoftRequested() { insertLoftFromSelection(); }
void MainWindow::onInsertShellRequested() { insertShellOnTail(); }
void MainWindow::onInsertHoleRequested() { insertHoleFromSelection(); }
void MainWindow::onInsertUnionRequested() {
    insertBooleanOnTail(BooleanOperation::Union, "Union");
}
void MainWindow::onInsertSubtractRequested() {
    insertBooleanOnTail(BooleanOperation::Subtract, "Subtract");
}
void MainWindow::onInsertIntersectRequested() {
    insertBooleanOnTail(BooleanOperation::Intersect, "Intersect");
}
void MainWindow::onInsertCircularPatternRequested() { insertCircularPatternOnTail(); }
void MainWindow::onInsertCurvePatternRequested() { insertCurvePatternFromSelection(); }
void MainWindow::onExportRequested() { exportCurrentSolid(); }
void MainWindow::onImportRequested() { importSolidAsFeature(); }

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
    const bool suppressed = !part().isFeatureActive(selectedId_);
    part().setSuppressed(selectedId_, !suppressed);
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
    part().setRollbackPosition(body, index + 1);
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
    if (part().bodies().empty()) {
        const QString message = QStringLiteral("Nothing to roll forward");
        statusLeft_->setText(message);
        return message;
    }
    for (const auto& body : part().bodies())
        part().setRollbackPosition(body->id(), Body::kNoRollback);
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

// =============================================================================
// Drawing commands (M32.4, roadmap §24)
// =============================================================================

ObjectId MainWindow::selectedDrawingView() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return kInvalidObjectId;
    return drawing->findView(selectedId_) != nullptr ? selectedId_ : kInvalidObjectId;
}

QString MainWindow::addBaseViewCommand(const QString& sourcePath, const QString& bodyName,
                                       ViewDirection direction, Vec2 positionMm) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Views belong to a drawing."));
    if (sourcePath.isEmpty()) return say(QStringLiteral("A view needs a model file."));
    if (const std::string why = drawing->whyViewCannotSitAt(positionMm); !why.empty())
        return say(QString::fromStdString(why));

    const std::string name =
        document_->unusedNameLike(std::string(toString(direction)));
    document_->beginTransaction("Add view");
    DrawingView* made = nullptr;
    try {
        made = &drawing->addView(name, sourcePath.toStdString(), bodyName.toStdString(),
                                 direction, positionMm);
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(QStringLiteral("That view was refused: %1").arg(QString::fromUtf8(error.what())));
    }
    if (!document_->commitTransaction()) return say(QStringLiteral("That view was refused."));
    (void)document_->recompute();
    selectedId_ = made->id();
    refreshAll();

    // WHETHER IT DREW ANYTHING is the thing worth reporting. A view that was
    // created and could not project is a row in the tree over blank paper, and
    // the message is where the reason belongs.
    if (made->currentState() != ComputeState::Valid)
        return say(QStringLiteral("%1 added, but it will not draw: %2")
                       .arg(QString::fromStdString(made->name()),
                            QString::fromStdString(made->diagnostic())));
    return say(QStringLiteral("%1 view of %2 -- %3 curves")
                   .arg(QString::fromStdString(made->name()), sourcePath)
                   .arg(made->projected().curves.size()));
}

QString MainWindow::addProjectedViewCommand(ViewDirection direction, double offsetMm) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    const ObjectId parent = selectedDrawingView();
    if (drawing == nullptr || parent == kInvalidObjectId)
        return say(QStringLiteral("Select the view to project from."));
    if (const std::string why = drawing->whyViewCannotBeProjectedFrom(parent, direction);
        !why.empty())
        return say(QString::fromStdString(why));

    const std::string name = document_->unusedNameLike(std::string(toString(direction)));
    document_->beginTransaction("Project view");
    DrawingView* made = nullptr;
    try {
        made = &drawing->addProjectedView(name, parent, direction, offsetMm);
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(QStringLiteral("That view was refused: %1")
                       .arg(QString::fromUtf8(error.what())));
    }
    if (!document_->commitTransaction()) return say(QStringLiteral("That view was refused."));
    (void)document_->recompute();
    selectedId_ = made->id();
    refreshAll();
    // WHICH SIDE IT WENT, because that is the convention the reader has to
    // know and the one thing they cannot infer from the button they pressed.
    return say(QStringLiteral("%1 projected from %2 (%3 angle)")
                   .arg(QString::fromStdString(made->name()),
                        QString::fromStdString(drawing->findView(parent)->name()),
                        QString::fromUtf8(
                            std::string(toString(drawing->sheet().projectionAngle())).c_str())));
}

QString MainWindow::updateDrawingViewsCommand() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing has views to update."));
    const std::vector<ObjectId> behind = drawing->staleViews();
    if (behind.empty()) return say(QStringLiteral("Every view is up to date."));

    // ONLY THE ONES THAT ARE BEHIND. Rebuilding everything would re-run
    // hidden-line removal on views nobody's model has touched, which on a
    // sheet of six views is five wasted solves.
    for (const ObjectId one : behind) drawing->markDirty(one);
    (void)document_->recompute();
    refreshAll();
    const std::size_t stillBehind = drawing->staleViews().size();
    if (stillBehind != 0)
        return say(QStringLiteral("Updated %1 view(s); %2 still will not build")
                       .arg(behind.size() - stillBehind)
                       .arg(stillBehind));
    return say(QStringLiteral("Updated %1 view(s)").arg(behind.size()));
}

QString MainWindow::setSheetCommand(SheetSize size, SheetOrientation orientation,
                                    const QString& scale, ProjectionAngle angle) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing has a sheet."));
    DrawingScale parsed;
    if (!ParseDrawingScale(scale.toStdString(), parsed))
        return say(QStringLiteral("'%1' is not a scale like 1:2").arg(scale));

    // ONE UNDO STEP for what the user experienced as one dialog.
    document_->beginTransaction("Sheet setup");
    drawing->setSheetSize(size);
    drawing->setSheetOrientation(orientation);
    drawing->setSheetScale(parsed);
    drawing->setSheetProjectionAngle(angle);
    if (!document_->commitTransaction()) return say(QStringLiteral("That was refused."));

    // A VIEW MAY NOW BE OFF THE PAPER. Said rather than silently moved: where
    // a view sits is the user's, and shuffling it would lose a layout they
    // chose.
    QStringList offSheet;
    for (const DrawingView* view : drawing->views())
        if (!drawing->whyViewCannotSitAt(drawing->viewPositionMm(view->id())).empty())
            offSheet << QString::fromStdString(view->name());
    if (drawingCanvas_ != nullptr) drawingCanvas_->fitSheet();
    refreshAll();
    if (!offSheet.isEmpty())
        return say(QStringLiteral("%1 %2, %3 angle -- but %4 now sits off the paper")
                       .arg(QString::fromUtf8(std::string(toString(size)).c_str()),
                            QString::fromUtf8(std::string(toString(orientation)).c_str()),
                            QString::fromUtf8(std::string(toString(angle)).c_str()),
                            offSheet.join(QStringLiteral(", "))));
    return say(QStringLiteral("%1 %2 at %3, %4 angle")
                   .arg(QString::fromUtf8(std::string(toString(size)).c_str()),
                        QString::fromUtf8(std::string(toString(orientation)).c_str()),
                        QString::fromStdString(parsed.toString()),
                        QString::fromUtf8(std::string(toString(angle)).c_str())));
}

std::vector<ObjectId> MainWindow::selectedDrawingEntities() const {
    std::vector<ObjectId> found;
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr || tree_ == nullptr) return found;
    std::set<ObjectId> chosen;
    for (const QTreeWidgetItem* item : tree_->selectedItems())
        chosen.insert(static_cast<ObjectId>(item->data(0, kIdRole).toULongLong()));
    // IN DOCUMENT ORDER, for the reason selectedInstances() gives: Qt does not
    // keep click order, and an angular dimension whose FIRST arm depended on
    // an order nobody can see would measure the explement half the time.
    for (const paramcad::DrawingEntity* entity : drawing->entities())
        if (chosen.count(entity->id()) != 0) found.push_back(entity->id());
    return found;
}

ObjectId MainWindow::selectedDimension() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return kInvalidObjectId;
    return drawing->findDimension(selectedId_) != nullptr ? selectedId_ : kInvalidObjectId;
}

std::size_t MainWindow::dimensionCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->dimensions().size();
}

std::size_t MainWindow::danglingDimensionCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->danglingDimensions().size();
}

QString MainWindow::dimensionTextForTesting(ObjectId id) const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    const DrawingDimension* dimension = drawing->findDimension(id);
    if (dimension == nullptr) return {};
    return QString::fromStdString(drawing->dimensionText(*dimension));
}

std::size_t MainWindow::drawnDimensionCountForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnDimensionCountForTesting();
}

std::size_t MainWindow::danglingDrawnForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->danglingDrawnForTesting();
}

QString MainWindow::addDimensionCommand(DimensionKind kind, LinearDirection direction) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Dimensions belong to a drawing."));

    // THE DOCUMENT DECIDES WHAT THE PICK MEANS. The window's job is to know
    // what is selected; turning that into two anchors is one rule, and it
    // lives where a script can reach it too.
    const auto proposal = drawing->proposeDimension(kind, selectedDrawingEntities());
    if (!proposal.ok)
        return say(QStringLiteral("No dimension: %1.").arg(QString::fromStdString(proposal.why)));

    document_->beginTransaction("Add dimension");
    DrawingDimension* made = nullptr;
    try {
        made = &drawing->addDimension(kind, proposal.first, proposal.second,
                                      proposal.linePositionMm);
        if (kind == DimensionKind::Linear && direction != LinearDirection::Aligned)
            drawing->setDimensionDirection(made->id(), direction);
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(QStringLiteral("That dimension was refused: %1")
                       .arg(QString::fromUtf8(error.what())));
    }
    if (!document_->commitTransaction())
        return say(QStringLiteral("That dimension was refused."));
    selectedId_ = made->id();
    const QString reads = QString::fromStdString(drawing->dimensionText(*made));
    refreshAll();
    return say(QStringLiteral("Added a dimension reading %1").arg(reads));
}

QString MainWindow::setDimensionDirectionCommand(LinearDirection direction) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Dimensions belong to a drawing."));
    const ObjectId id = selectedDimension();
    if (id == kInvalidObjectId) return say(QStringLiteral("Select a dimension first."));
    if (!drawing->setDimensionDirection(id, direction))
        return say(QStringLiteral("That direction was refused."));
    const QString reads = dimensionTextForTesting(id);
    refreshAll();
    return say(QStringLiteral("It now reads %1").arg(reads));
}

QString MainWindow::setDimensionTextCommand(const QString& text) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Dimensions belong to a drawing."));
    const ObjectId id = selectedDimension();
    if (id == kInvalidObjectId) return say(QStringLiteral("Select a dimension first."));
    if (!drawing->setDimensionTextOverride(id, text.toStdString()))
        return say(QStringLiteral("That text was refused."));
    refreshAll();
    // SAID OUT LOUD, because an override is the one edit that makes a drawing
    // stop showing what it measures -- and a user who forgets they typed it
    // has a drawing that no longer tracks its model.
    return say(text.isEmpty()
                   ? QStringLiteral("Back to the measured size: %1").arg(dimensionTextForTesting(id))
                   : QStringLiteral("Overridden to \"%1\" -- it still measures %2")
                         .arg(text)
                         .arg(QString::number(
                             drawing->measure(*drawing->findDimension(id)).valueMm, 'f', 2)));
}

QString MainWindow::drawShapeCommand(DrawingTool tool, const std::vector<Vec2>& pointsMm) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing has sheet geometry."));
    if (static_cast<int>(pointsMm.size()) < ClicksNeededBy(tool))
        return say(QStringLiteral("That tool needs more points."));

    // ONE PLACE THAT TURNS CLICKS INTO A SHAPE. The canvas knows how many
    // clicks a tool takes and this knows what they mean -- the two halves of
    // the same fact, kept apart on purpose so neither can quietly hold a
    // different idea of the other.
    std::vector<DrawShape> shapes;
    QString what;
    switch (tool) {
        case DrawingTool::None: return say(QStringLiteral("No tool is armed."));
        case DrawingTool::Line:
            if (std::hypot(pointsMm[1].x - pointsMm[0].x, pointsMm[1].y - pointsMm[0].y) < 1e-9)
                return say(QStringLiteral("A line of no length is not a line."));
            shapes.push_back(DrawLine{pointsMm[0], pointsMm[1]});
            what = QStringLiteral("line");
            break;
        case DrawingTool::Circle: {
            const double radius =
                std::hypot(pointsMm[1].x - pointsMm[0].x, pointsMm[1].y - pointsMm[0].y);
            if (!(radius > 1e-9)) return say(QStringLiteral("A circle of no radius is a point."));
            shapes.push_back(DrawCircle{pointsMm[0], radius});
            what = QStringLiteral("circle");
            break;
        }
        case DrawingTool::Wire: {
            // A WIRE IS NOT A LINE. It is its own kind, so that moving it to
            // another layer cannot silently change the circuit while the
            // drawing looks identical -- see SchematicObjects.h.
            if (std::hypot(pointsMm[1].x - pointsMm[0].x, pointsMm[1].y - pointsMm[0].y) < 1e-9)
                return say(QStringLiteral("A wire of no length connects nothing."));
            document_->beginTransaction("Draw wire");
            try {
                drawing->addWire({pointsMm[0], pointsMm[1]});
            } catch (const std::exception& error) {
                document_->abortTransaction();
                return say(QStringLiteral("That wire was refused: %1")
                               .arg(QString::fromUtf8(error.what())));
            }
            if (!document_->commitTransaction())
                return say(QStringLiteral("That wire was refused."));
            refreshAll();
            return say(QStringLiteral("Drew a wire -- %1").arg(netlistSummaryForTesting()));
        }
        case DrawingTool::Rectangle: {
            // FOUR LINES, not a closed polyline -- so each side can be
            // trimmed, dimensioned and put on its own layer, which is what a
            // drafter does to a rectangle about a minute after drawing it.
            const Vec2 a = pointsMm[0];
            const Vec2 b = pointsMm[1];
            if (std::fabs(a.x - b.x) < 1e-9 || std::fabs(a.y - b.y) < 1e-9)
                return say(QStringLiteral("A rectangle with no width or no height is a line."));
            const Vec2 corners[4] = {a, Vec2{b.x, a.y}, b, Vec2{a.x, b.y}};
            for (int i = 0; i < 4; ++i)
                shapes.push_back(DrawLine{corners[i], corners[(i + 1) % 4]});
            what = QStringLiteral("rectangle");
            break;
        }
    }

    // ONE UNDO STEP FOR ONE GESTURE. A rectangle that took four Ctrl+Z to
    // remove would be the shell contradicting what the user did.
    document_->beginTransaction("Draw " + what.toStdString());
    ObjectId last = kInvalidObjectId;
    try {
        for (DrawShape& shape : shapes) last = drawing->addEntity(std::move(shape)).id();
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(QStringLiteral("That was refused: %1").arg(QString::fromUtf8(error.what())));
    }
    if (!document_->commitTransaction()) return say(QStringLiteral("That was refused."));
    selectedId_ = last;
    refreshAll();
    return say(QStringLiteral("Drew a %1").arg(what));
}

void MainWindow::setDrawingToolCommand(DrawingTool tool) {
    if (drawingCanvas_ == nullptr) return;
    drawingCanvas_->setTool(tool);
    if (drawLineAction_ != nullptr)
        drawLineAction_->setChecked(tool == DrawingTool::Line);
    if (drawWireAction_ != nullptr) drawWireAction_->setChecked(tool == DrawingTool::Wire);
    if (drawCircleAction_ != nullptr)
        drawCircleAction_->setChecked(tool == DrawingTool::Circle);
    if (drawRectangleAction_ != nullptr)
        drawRectangleAction_->setChecked(tool == DrawingTool::Rectangle);
    statusLeft_->setText(tool == DrawingTool::None
                             ? QStringLiteral("Select.")
                             : QStringLiteral("Click on the sheet. Esc backs out."));
}

DrawingTool MainWindow::drawingToolForTesting() const {
    return drawingCanvas_ == nullptr ? DrawingTool::None : drawingCanvas_->tool();
}

void MainWindow::pickOnSheetForTesting(Vec2 sheetMm) {
    if (drawingCanvas_ != nullptr) drawingCanvas_->pickForTesting(sheetMm);
}

std::size_t MainWindow::drawingEntityCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->entities().size();
}

QString MainWindow::moveDimensionCommand(ObjectId id, Vec2 toMm) {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    if (!drawing->setDimensionLinePosition(id, toMm)) return {};
    refreshAll();
    return QStringLiteral("Moved a dimension");
}

ObjectId MainWindow::drawingEntityIdForTesting(std::size_t index) const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return kInvalidObjectId;
    const std::vector<const paramcad::DrawingEntity*> all = drawing->entities();
    return index < all.size() ? all[index]->id() : kInvalidObjectId;
}

ObjectId MainWindow::dimensionIdForTesting(std::size_t index) const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return kInvalidObjectId;
    const std::vector<const DrawingDimension*> all = drawing->dimensions();
    return index < all.size() ? all[index]->id() : kInvalidObjectId;
}

bool MainWindow::scaleEntityForTesting(ObjectId id, Vec2 aboutMm, double factor) {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return false;
    const bool changed = drawing->transformEntities({id}, Matrix2D::scaleAbout(aboutMm, factor));
    if (changed) refreshAll();
    return changed;
}

QString MainWindow::setTitleBlockFieldCommand(const QString& label, const QString& value) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing has a title block."));
    if (!drawing->setTitleBlockField(label.toStdString(), value.toStdString())) {
        const TitleBlockField* field = drawing->titleBlock().findField(label.toStdString());
        if (field != nullptr && field->isDerived())
            // SAID, not silently ignored. The row is filled from the sheet, and
            // a user who typed into it needs to know why what they typed did
            // not stick.
            return say(QStringLiteral("%1 is filled in from the sheet -- change the sheet "
                                      "and this follows.")
                           .arg(label));
        return say(QStringLiteral("There is no %1 row in this title block.").arg(label));
    }
    refreshAll();
    return say(QStringLiteral("%1: %2").arg(label, value));
}

QString MainWindow::setFrameMarginsCommand(double bindingMm, double otherMm) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing has a frame."));
    FrameMargins margins;
    margins.bindingMm = bindingMm;
    margins.otherMm = otherMm;
    if (!drawing->setFrameMargins(margins))
        return say(QStringLiteral("Those margins are wider than the paper, so there would be "
                                  "no inside left to draw in."));
    refreshAll();
    return say(QStringLiteral("Frame: %1 mm on the binding edge, %2 mm elsewhere")
                   .arg(bindingMm)
                   .arg(otherMm));
}

QString MainWindow::setFrameVisibleCommand(bool visible) {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    if (!drawing->setFrameVisible(visible)) return {};
    refreshAll();
    const QString said = visible ? QStringLiteral("Frame shown") : QStringLiteral("Frame hidden");
    statusLeft_->setText(said);
    return said;
}

QString MainWindow::setTitleBlockVisibleCommand(bool visible) {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    if (!drawing->setTitleBlockVisible(visible)) return {};
    refreshAll();
    const QString said =
        visible ? QStringLiteral("Title block shown") : QStringLiteral("Title block hidden");
    statusLeft_->setText(said);
    return said;
}

QString MainWindow::plotToPdfCommand(const QString& path) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing can be plotted."));
    const PlotResult plotted = PlotDrawingToPdf(*drawing, path);
    if (!plotted)
        return say(QStringLiteral("Nothing was plotted: %1.")
                       .arg(QString::fromStdString(plotted.why)));
    // THE SIZE IS SAID OUT LOUD, because true size is the whole promise a plot
    // makes: every dimension on the sheet is checkable against a rule only if
    // the paper came out the size it claims.
    return say(QStringLiteral("Plotted %1 x %2 mm at 1:1 to %3")
                   .arg(plotted.widthMm)
                   .arg(plotted.heightMm)
                   .arg(QFileInfo(path).fileName()));
}

QString MainWindow::exportDxfCommand(const QString& path) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing exports to DXF."));
    const DxfWriteResult written = WriteDxfFile(*drawing, path.toStdString());
    if (!written)
        return say(QStringLiteral("Nothing was exported: %1.")
                       .arg(QString::fromStdString(written.why)));

    QString said = QStringLiteral("Exported %1 entities to %2")
                       .arg(written.entities)
                       .arg(QFileInfo(path).fileName());
    // EVERY LOSS, NAMED. Not a count and not a warning icon: a user who is
    // about to send this to a supplier needs to know which parts of their
    // drawing the file cannot carry.
    for (const DxfWriteLoss& loss : written.losses)
        said += QStringLiteral("\n%1: %2")
                    .arg(QString::fromStdString(loss.what),
                         QString::fromStdString(loss.detail));
    return say(said);
}

ObjectId MainWindow::selectedBomTable() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return kInvalidObjectId;
    return drawing->findBomTable(selectedId_) != nullptr ? selectedId_ : kInvalidObjectId;
}

Vec2 MainWindow::defaultBomPositionMm() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return Vec2{0.0, 0.0};
    const Vec2 at = drawing->titleBlockOriginMm();
    return Vec2{at.x, at.y + drawing->titleBlock().heightMm()};
}

QString MainWindow::nextTagFor(const QString& symbolName) const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    const ElectricalSymbol* symbol = BuiltInSymbols().find(symbolName.toStdString());
    // A SYMBOL'S PREFIX SAYS WHAT KIND OF PART IT IS (IEC 81346): R for a
    // resistor, K for a contactor, X for a terminal. A schematic where every
    // part is called "SYMBOL1" is one nobody can cross-reference.
    const QString prefix =
        symbol != nullptr ? QString::fromStdString(symbol->tagPrefix()) : QStringLiteral("U");
    for (int number = 1; number < 10000; ++number) {
        const QString tag = QStringLiteral("-%1%2").arg(prefix).arg(number);
        if (drawing->findSymbolTagged(tag.toStdString()) == nullptr) return tag;
    }
    return {};
}

QString MainWindow::placeSymbolCommand(const QString& symbolName, Vec2 positionMm,
                                       const QString& tag) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Components go on a drawing."));
    if (BuiltInSymbols().find(symbolName.toStdString()) == nullptr)
        return say(QStringLiteral("There is no symbol called %1.").arg(symbolName));

    const QString wanted = tag.isEmpty() ? nextTagFor(symbolName) : tag;
    document_->beginTransaction("Place component");
    SymbolPlacement* made = nullptr;
    try {
        made = &drawing->addSymbol(wanted.toStdString(), symbolName.toStdString(), positionMm);
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(QStringLiteral("That component was refused: %1")
                       .arg(QString::fromUtf8(error.what())));
    }
    if (!document_->commitTransaction())
        return say(QStringLiteral("That component was refused."));
    selectedId_ = made->id();
    refreshAll();
    return say(QStringLiteral("Placed %1 (%2)").arg(wanted, symbolName));
}

QString MainWindow::turnSelectedSymbolCommand(double radians) {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    const SymbolPlacement* symbol = drawing->findSymbol(selectedId_);
    if (symbol == nullptr) {
        statusLeft_->setText(QStringLiteral("Select a component first."));
        return {};
    }
    if (!drawing->setSymbolRotation(symbol->id(), symbol->rotationRad() + radians)) return {};
    refreshAll();
    const QString said = QStringLiteral("Turned %1").arg(QString::fromStdString(symbol->tag()));
    statusLeft_->setText(said);
    return said;
}

QString MainWindow::numberNetsCommand() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Nets belong to a schematic."));
    const std::size_t named = drawing->numberNets();
    refreshAll();
    // WHAT IT DID AND WHAT IS STILL WRONG, together. A command that reported
    // only its success would let a user walk away from a schematic with wires
    // that go nowhere.
    QString said = QStringLiteral("Numbered %1 wire(s)").arg(named);
    const std::size_t dangling = danglingNetCountForTesting();
    if (dangling != 0)
        said += QStringLiteral(" -- %1 net(s) go nowhere").arg(dangling);
    for (const std::string& clash : drawing->conflictingNetNames())
        said += QStringLiteral("\n%1 are the same wire")
                    .arg(QString::fromStdString(clash));
    return say(said);
}

QString MainWindow::topmostWireLabelForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    const WireEntity* highest = nullptr;
    double best = 0.0;
    for (const WireEntity* wire : drawing->wires()) {
        for (const Vec2 point : wire->pointsMm()) {
            if (highest != nullptr && point.y <= best) continue;
            highest = wire;
            best = point.y;
        }
    }
    return highest == nullptr ? QString() : QString::fromStdString(highest->label());
}

QString MainWindow::netlistSummaryForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    const Netlist netlist = drawing->netlist();
    return QStringLiteral("%1 nets, %2 dangling")
        .arg(netlist.nets.size())
        .arg(netlist.danglingNets().size());
}

std::size_t MainWindow::symbolCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->symbols().size();
}

std::size_t MainWindow::wireCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->wires().size();
}

std::size_t MainWindow::netCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->netlist().nets.size();
}

std::size_t MainWindow::danglingNetCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->netlist().danglingNets().size();
}

std::size_t MainWindow::drawnWiresForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnWiresForTesting();
}

std::size_t MainWindow::drawnSymbolsForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnSymbolsForTesting();
}

std::size_t MainWindow::drawnUnknownSymbolsForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnUnknownSymbolsForTesting();
}

std::size_t MainWindow::drawnJunctionsForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnJunctionsForTesting();
}

Box2D MainWindow::drawnSymbolExtentForTesting() const {
    return drawingCanvas_ == nullptr ? Box2D{} : drawingCanvas_->drawnSymbolExtentForTesting();
}

QString MainWindow::addBomTableCommand(const QString& name, const QString& sourcePath,
                                       Vec2 positionMm, BomDepth depth) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing has a parts list."));

    document_->beginTransaction("Add parts list");
    BomTable* made = nullptr;
    try {
        made = &drawing->addBomTable(name.toStdString(), sourcePath.toStdString(), positionMm);
        if (depth != BomDepth::TopLevel) drawing->setBomDepth(made->id(), depth);
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(QStringLiteral("That parts list was refused: %1")
                       .arg(QString::fromUtf8(error.what())));
    }
    if (!document_->commitTransaction())
        return say(QStringLiteral("That parts list was refused."));

    // WHAT IT COUNTED, said out loud -- a list that came up empty because the
    // assembly could not be read looks exactly like one for an assembly with
    // nothing in it.
    const BomContents counted = drawing->countBom(*made);
    selectedId_ = made->id();
    refreshAll();
    if (!counted.ok)
        return say(QStringLiteral("Added %1, but it cannot be counted: %2")
                       .arg(name, QString::fromStdString(counted.why)));
    return say(QStringLiteral("Added %1: %2 lines, %3 parts")
                   .arg(name)
                   .arg(counted.rows.size())
                   .arg(counted.totalQuantity()));
}

QString MainWindow::setBomDepthCommand(BomDepth depth) {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    const ObjectId id = selectedBomTable();
    if (id == kInvalidObjectId) {
        statusLeft_->setText(QStringLiteral("Select a parts list first."));
        return {};
    }
    if (!drawing->setBomDepth(id, depth)) return {};
    refreshAll();
    const QString said =
        depth == BomDepth::Exploded
            ? QStringLiteral("Every part, however deep")
            : QStringLiteral("What this assembly is made of, sub-assemblies as one line each");
    statusLeft_->setText(said);
    return said;
}

QString MainWindow::recountBomCommand() {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    std::size_t counted = 0;
    for (const ObjectId id : drawing->staleBomTables())
        if (drawing->markBomCounted(id)) ++counted;
    refreshAll();
    const QString said = counted == 0
                             ? QStringLiteral("Every parts list is up to date")
                             : QStringLiteral("Re-counted %1 parts list(s)").arg(counted);
    statusLeft_->setText(said);
    return said;
}

std::size_t MainWindow::bomTableCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->bomTables().size();
}

std::size_t MainWindow::staleBomCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->staleBomTables().size();
}

void MainWindow::repaintDrawingForTesting() {
    if (drawingCanvas_ != nullptr) drawingCanvas_->repaint();
}

std::size_t MainWindow::drawnBomRowsForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnBomRowsForTesting();
}

std::size_t MainWindow::drawnUncountedBomsForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnUncountedBomsForTesting();
}

int MainWindow::bomTotalQuantityForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return 0;
    int total = 0;
    for (const BomTable* table : drawing->bomTables())
        total += drawing->countBom(*table).totalQuantity();
    return total;
}

QString MainWindow::titleBlockValueForTesting(const QString& label) const {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return {};
    const TitleBlockField* field = drawing->titleBlock().findField(label.toStdString());
    if (field == nullptr) return {};
    return QString::fromStdString(drawing->titleBlock().valueOf(*field, drawing->sheet()));
}

std::size_t MainWindow::drawnFrameLinesForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnFrameLinesForTesting();
}

std::size_t MainWindow::drawnTitleBlockRowsForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnTitleBlockRowsForTesting();
}

QString MainWindow::addDimensionStyleCommand(const QString& name) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Dimension styles belong to a drawing."));
    document_->beginTransaction("Add dimension style");
    paramcad::DimensionStyle* made = nullptr;
    try {
        made = &drawing->addDimensionStyle(name.toStdString());
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(
            QStringLiteral("That style was refused: %1").arg(QString::fromUtf8(error.what())));
    }
    if (!document_->commitTransaction()) return say(QStringLiteral("That style was refused."));
    drawing->setCurrentDimensionStyle(made->id());
    selectedId_ = made->id();
    refreshAll();
    return say(QStringLiteral("Added dimension style %1, and it is now the current one").arg(name));
}

QString MainWindow::editDimensionStyleCommand(double textHeightMm, double arrowSizeMm,
                                              int decimals, const QString& suffix) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Dimension styles belong to a drawing."));
    // The SELECTED style if one is, otherwise the current one -- which is the
    // one a new dimension would use, so "Style..." with nothing selected edits
    // what the user is about to draw with.
    ObjectId id = drawing->findDimensionStyle(selectedId_) != nullptr
                      ? selectedId_
                      : drawing->currentDimensionStyleId();
    const paramcad::DimensionStyle* was = drawing->findDimensionStyle(id);
    if (was == nullptr) return say(QStringLiteral("There is no style to edit."));

    paramcad::DimensionStyle wanted = *was;
    wanted.setTextHeightMm(textHeightMm);
    wanted.setArrowSizeMm(arrowSizeMm);
    wanted.setDecimals(decimals);
    wanted.setSuffix(suffix.toStdString());
    if (!drawing->editDimensionStyle(id, wanted))
        return say(QStringLiteral("That style change was refused."));
    refreshAll();
    return say(QStringLiteral("Restyled %1 -- every dimension using it followed")
                   .arg(QString::fromStdString(was->name())));
}

QString MainWindow::addDrawingLayerCommand(const QString& name, int color) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return say(QStringLiteral("Only a drawing has layers."));
    document_->beginTransaction("Add layer");
    Layer* made = nullptr;
    try {
        made = &drawing->addLayer(name.toStdString(), color);
    } catch (const std::exception& error) {
        document_->abortTransaction();
        return say(QStringLiteral("That layer was refused: %1")
                       .arg(QString::fromUtf8(error.what())));
    }
    if (!document_->commitTransaction()) return say(QStringLiteral("That layer was refused."));
    selectedId_ = made->id();
    refreshAll();
    return say(QStringLiteral("Added layer %1").arg(name));
}

QString MainWindow::deleteSelectedDrawingObject() {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr || selectedId_ == kInvalidObjectId)
        return say(QStringLiteral("Select something to delete."));
    const QString name = QString::fromStdString(document_->objectName(selectedId_));
    // WHAT ELSE GOES, counted BEFORE the deletion, because afterwards there is
    // nothing left to count.
    std::size_t children = 0;
    for (const DrawingView* view : drawing->views())
        if (view->parentViewId() == selectedId_) ++children;

    if (!document_->removeObject(selectedId_))
        return say(QStringLiteral("%1 cannot be deleted.").arg(name));
    (void)document_->recompute();
    selectedId_ = kInvalidObjectId;
    refreshAll();
    if (children != 0)
        return say(QStringLiteral("Deleted %1 and the %2 view(s) projected from it")
                       .arg(name)
                       .arg(children));
    return say(QStringLiteral("Deleted %1").arg(name));
}

// --- Readbacks ---------------------------------------------------------------

std::size_t MainWindow::drawingViewCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->views().size();
}

std::size_t MainWindow::drawnCurveCountForTesting() const {
    return drawingCanvas_ == nullptr ? 0u : drawingCanvas_->drawnCurveCountForTesting();
}

std::size_t MainWindow::staleViewCountForTesting() const {
    const DrawingDocument* drawing = AsDrawing(document_);
    return drawing == nullptr ? 0u : drawing->staleViews().size();
}

bool MainWindow::drawingCanvasVisibleForTesting() const {
    return drawingCanvas_ != nullptr && centralStack_ != nullptr &&
           centralStack_->currentWidget() == drawingCanvas_;
}

bool MainWindow::drawingToolbarVisible() const {
    return drawingToolBar_ != nullptr && drawingToolBar_->isVisible();
}

void MainWindow::selectDrawingViewForTesting(const QString& name) {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return;
    if (const DrawingView* view = drawing->findViewNamed(name.toStdString()))
        selectObject(view->id());
}

void MainWindow::adoptDrawingForTesting(const QString& name) {
    auto fresh = std::make_unique<DrawingDocument>(name.toStdString());
    fresh->setGeometryKernel(document_->geometryKernel());
    fresh->setSketchSolver(document_->sketchSolver());
    ownedDocument_ = std::move(fresh);
    document_ = ownedDocument_.get();
    presenter_->setDocument(*document_);
    selectedId_ = kInvalidObjectId;
    documentPath_.clear();
    refreshAll();
}

// --- Slots -------------------------------------------------------------------

void MainWindow::onNewDrawingRequested() { newDrawingCommand(); }

void MainWindow::onAddBaseViewRequested() {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Model to draw"), QString(),
        QStringLiteral("EP3D documents (*.ep3d *.ep3da);;All files (*)"));
    if (path.isEmpty()) return;

    static const struct { const char* label; ViewDirection direction; } kDirections[] = {
        {"Front", ViewDirection::Front}, {"Top", ViewDirection::Top},
        {"Right", ViewDirection::Right}, {"Left", ViewDirection::Left},
        {"Back", ViewDirection::Back},   {"Bottom", ViewDirection::Bottom},
        {"Isometric", ViewDirection::Isometric},
    };
    QStringList labels;
    for (const auto& one : kDirections) labels << QString::fromLatin1(one.label);
    bool chose = false;
    const QString picked =
        QInputDialog::getItem(this, QStringLiteral("Add View"), QStringLiteral("Seen from:"),
                              labels, 0, false, &chose);
    if (!chose) return;
    const int which = labels.indexOf(picked);
    if (which < 0) return;

    // PLACED IN THE MIDDLE OF THE PAPER by default, which is where a first
    // view belongs and what saves the user a drag they did not ask for.
    const Vec2 middle{drawing->sheet().widthMm() / 2.0, drawing->sheet().heightMm() / 2.0};
    addBaseViewCommand(path, QString(), kDirections[which].direction, middle);
}

void MainWindow::onAddProjectedViewRequested() {
    const DrawingDocument* drawing = AsDrawing(document_);
    const ObjectId parent = selectedDrawingView();
    if (drawing == nullptr || parent == kInvalidObjectId) return;

    // ONLY THE DIRECTIONS THAT ACTUALLY LINE UP with the selected view. An
    // isometric is not square to a front view, and offering it would be
    // offering a command that refuses itself.
    static const ViewDirection kAll[] = {ViewDirection::Front, ViewDirection::Back,
                                         ViewDirection::Left,  ViewDirection::Right,
                                         ViewDirection::Top,   ViewDirection::Bottom};
    QStringList labels;
    std::vector<ViewDirection> offered;
    for (const ViewDirection one : kAll) {
        if (!drawing->whyViewCannotBeProjectedFrom(parent, one).empty()) continue;
        labels << QString::fromUtf8(std::string(toString(one)).c_str());
        offered.push_back(one);
    }
    if (offered.empty()) return;

    bool chose = false;
    const QString picked = QInputDialog::getItem(this, QStringLiteral("Project View"),
                                                 QStringLiteral("Seen from:"), labels, 0, false,
                                                 &chose);
    if (!chose) return;
    const int which = labels.indexOf(picked);
    if (which < 0) return;

    bool ok = false;
    const double offset = QInputDialog::getDouble(
        this, QStringLiteral("Project View"), QStringLiteral("How far from it (mm):"), 80.0, 1.0,
        10000.0, 1, &ok);
    if (!ok) return;
    addProjectedViewCommand(offered[static_cast<std::size_t>(which)], offset);
}

void MainWindow::onUpdateViewsRequested() { updateDrawingViewsCommand(); }

void MainWindow::onSheetSetupRequested() {
    const DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return;

    static const struct { const char* label; SheetSize size; } kSizes[] = {
        {"A0", SheetSize::A0}, {"A1", SheetSize::A1}, {"A2", SheetSize::A2},
        {"A3", SheetSize::A3}, {"A4", SheetSize::A4},
    };
    QStringList sizes;
    for (const auto& one : kSizes) sizes << QString::fromLatin1(one.label);
    bool chose = false;
    const QString pickedSize =
        QInputDialog::getItem(this, QStringLiteral("Sheet"), QStringLiteral("Paper:"), sizes,
                              sizes.indexOf(QString::fromUtf8(
                                  std::string(toString(drawing->sheet().size())).c_str())),
                              false, &chose);
    if (!chose) return;
    const int sizeIndex = sizes.indexOf(pickedSize);
    if (sizeIndex < 0) return;

    const QStringList orientations{QStringLiteral("Landscape"), QStringLiteral("Portrait")};
    const QString pickedOrientation = QInputDialog::getItem(
        this, QStringLiteral("Sheet"), QStringLiteral("Which way round:"), orientations,
        drawing->sheet().orientation() == SheetOrientation::Portrait ? 1 : 0, false, &chose);
    if (!chose) return;

    const QString scale = QInputDialog::getText(
        this, QStringLiteral("Sheet"), QStringLiteral("Scale (like 1:2):"), QLineEdit::Normal,
        QString::fromStdString(drawing->sheet().scale().toString()), &chose);
    if (!chose) return;

    // THE PROJECTION ANGLE IS ASKED, never defaulted quietly. A reader who
    // cannot tell which convention a drawing is in cannot read it, and the
    // two conventions put every view on opposite sides.
    const QStringList angles{QStringLiteral("First"), QStringLiteral("Third")};
    const QString pickedAngle = QInputDialog::getItem(
        this, QStringLiteral("Sheet"), QStringLiteral("Projection angle:"), angles,
        drawing->sheet().projectionAngle() == ProjectionAngle::Third ? 1 : 0, false, &chose);
    if (!chose) return;

    setSheetCommand(kSizes[sizeIndex].size,
                    pickedOrientation == QStringLiteral("Portrait") ? SheetOrientation::Portrait
                                                                    : SheetOrientation::Landscape,
                    scale,
                    pickedAngle == QStringLiteral("Third") ? ProjectionAngle::Third
                                                           : ProjectionAngle::First);
}

void MainWindow::onAddDimensionRequested(DimensionKind kind, LinearDirection direction) {
    addDimensionCommand(kind, direction);
}

void MainWindow::onDimensionTextRequested() {
    DrawingDocument* drawing = AsDrawing(document_);
    const ObjectId id = selectedDimension();
    if (drawing == nullptr || id == kInvalidObjectId) {
        statusLeft_->setText(QStringLiteral("Select a dimension first."));
        return;
    }
    bool ok = false;
    const QString text = QInputDialog::getText(
        this, QStringLiteral("Dimension Text"),
        QStringLiteral("Text (empty puts back the measured size):"), QLineEdit::Normal,
        QString::fromStdString(drawing->findDimension(id)->textOverride()), &ok);
    if (!ok) return;
    setDimensionTextCommand(text);
}

void MainWindow::onDimensionStyleRequested() {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return;
    const ObjectId id = drawing->findDimensionStyle(selectedId_) != nullptr
                            ? selectedId_
                            : drawing->currentDimensionStyleId();
    const paramcad::DimensionStyle* style = drawing->findDimensionStyle(id);
    if (style == nullptr) return;
    bool ok = false;
    const double height = QInputDialog::getDouble(
        this, QStringLiteral("Dimension Style"), QStringLiteral("Text height (mm on paper):"),
        style->textHeightMm(), 0.5, 50.0, 2, &ok);
    if (!ok) return;
    const double arrow = QInputDialog::getDouble(
        this, QStringLiteral("Dimension Style"), QStringLiteral("Arrow size (mm on paper):"),
        style->arrowSizeMm(), 0.5, 50.0, 2, &ok);
    if (!ok) return;
    const int decimals =
        QInputDialog::getInt(this, QStringLiteral("Dimension Style"),
                             QStringLiteral("Decimal places:"), style->decimals(), 0, 9, 1, &ok);
    if (!ok) return;
    const QString suffix = QInputDialog::getText(
        this, QStringLiteral("Dimension Style"), QStringLiteral("Suffix (e.g. \" mm\"):"),
        QLineEdit::Normal, QString::fromStdString(style->suffix()), &ok);
    if (!ok) return;
    editDimensionStyleCommand(height, arrow, decimals, suffix);
}

void MainWindow::onTitleBlockRequested() {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return;
    // ONE PROMPT PER TYPED ROW, and none for the derived ones -- a dialog that
    // asked for a scale it would then discard would be teaching the user that
    // the program ignores them.
    for (const TitleBlockField& field : drawing->titleBlock().fields()) {
        if (field.isDerived()) continue;
        bool ok = false;
        const QString value = QInputDialog::getText(
            this, QStringLiteral("Title Block"),
            QStringLiteral("%1:").arg(QString::fromStdString(field.label)), QLineEdit::Normal,
            QString::fromStdString(field.value), &ok);
        if (!ok) return;
        setTitleBlockFieldCommand(QString::fromStdString(field.label), value);
    }
}

void MainWindow::onFrameRequested() {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return;
    bool ok = false;
    const double binding = QInputDialog::getDouble(
        this, QStringLiteral("Frame"), QStringLiteral("Binding edge margin (mm):"),
        drawing->frameMargins().bindingMm, 0.0, 200.0, 1, &ok);
    if (!ok) return;
    const double other = QInputDialog::getDouble(
        this, QStringLiteral("Frame"), QStringLiteral("Other margins (mm):"),
        drawing->frameMargins().otherMm, 0.0, 200.0, 1, &ok);
    if (!ok) return;
    setFrameMarginsCommand(binding, other);
}

void MainWindow::onPlotPdfRequested() {
    if (AsDrawing(document_) == nullptr) return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Plot to PDF"), QString(), QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty()) return;
    plotToPdfCommand(path);
}

void MainWindow::onExportDxfRequested() {
    if (AsDrawing(document_) == nullptr) return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export DXF"), QString(), QStringLiteral("DXF R12 (*.dxf)"));
    if (path.isEmpty()) return;
    const QString said = exportDxfCommand(path);
    // A LOSS GETS A DIALOG, not just a status line. The status bar is where a
    // user looks when they are wondering what happened; a loss is something
    // they have to be told before they send the file on.
    if (said.contains(QLatin1Char('\n')))
        QMessageBox::information(this, QStringLiteral("Export DXF"), said);
}

void MainWindow::onPlaceSymbolRequested() {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return;
    QStringList names;
    for (const ElectricalSymbol& symbol : BuiltInSymbols().symbols())
        names << QStringLiteral("%1  -  %2")
                     .arg(QString::fromStdString(symbol.name()),
                          QString::fromStdString(symbol.description()));
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, QStringLiteral("Place Component"),
                                                 QStringLiteral("Symbol:"), names, 0, false,
                                                 &ok);
    if (!ok || chosen.isEmpty()) return;
    const QString symbolName = chosen.section(QStringLiteral("  -  "), 0, 0);
    // THE MIDDLE OF THE FRAME, so a placed component is somewhere the user can
    // see it and drag from -- rather than at the origin, which on a portrait
    // sheet is the bottom-left corner under the frame.
    const SheetFrameGeometry frame = drawing->frame();
    const Vec2 at = frame.ok ? Vec2{(frame.innerMinMm.x + frame.innerMaxMm.x) / 2.0,
                                    (frame.innerMinMm.y + frame.innerMaxMm.y) / 2.0}
                             : Vec2{drawing->sheet().widthMm() / 2.0,
                                    drawing->sheet().heightMm() / 2.0};
    placeSymbolCommand(symbolName, at);
}

void MainWindow::onAddBomRequested() {
    DrawingDocument* drawing = AsDrawing(document_);
    if (drawing == nullptr) return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Parts List: which assembly?"), QString(),
        QStringLiteral("EP3D assembly (*.ep3da)"));
    if (path.isEmpty()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Parts List"),
                                               QStringLiteral("Name:"), QLineEdit::Normal,
                                               QStringLiteral("Parts"), &ok);
    if (!ok || name.isEmpty()) return;
    const QStringList depths{QStringLiteral("What this assembly is made of"),
                             QStringLiteral("Every part, however deep")};
    const QString chosen =
        QInputDialog::getItem(this, QStringLiteral("Parts List"), QStringLiteral("Show:"),
                              depths, 0, false, &ok);
    if (!ok) return;
    // THE LIST SITS ON THE TITLE BLOCK and grows up from it, which is where a
    // reader looks and where it can get longer without running off the sheet.
    addBomTableCommand(name, path, defaultBomPositionMm(),
                       chosen == depths[1] ? BomDepth::Exploded : BomDepth::TopLevel);
}

void MainWindow::onAddDrawingLayerRequested() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Add Layer"),
                                               QStringLiteral("Name:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.isEmpty()) return;
    const int color = QInputDialog::getInt(this, QStringLiteral("Add Layer"),
                                           QStringLiteral("Colour (ACI 1-255):"), 7, 1, 255, 1,
                                           &ok);
    if (!ok) return;
    addDrawingLayerCommand(name, color);
}

void MainWindow::onDeleteDrawingObjectRequested() { deleteSelectedDrawingObject(); }

QString MainWindow::newDrawingCommand() {
    if (document_ == nullptr) return QStringLiteral("No document");
    if (inSketchMode()) finishSketchCommand();

    auto fresh = std::make_unique<DrawingDocument>("Untitled");
    // The kernel is the APPLICATION'S (ADR-M3-003) and a new document arrives
    // without one. A DRAWING needs it more directly than a part does: every
    // view projects through it, so a drawing with no kernel is a drawing that
    // can hold views and draw none of them.
    fresh->setGeometryKernel(document_->geometryKernel());
    fresh->setSketchSolver(document_->sketchSolver());

    ownedDocument_ = std::move(fresh);
    document_ = ownedDocument_.get();
    presenter_->setDocument(*document_);
    if (sketchCanvas_ != nullptr) sketchCanvas_->setSketch(partOrNull(), kInvalidObjectId);
    selectedId_ = kInvalidObjectId;
    documentPath_.clear();
    setWindowTitle(QStringLiteral("EP3D - Untitled drawing"));

    onRecomputeRequested();
    refreshAll();
    return QStringLiteral("New drawing");
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
    if (sketchCanvas_ != nullptr) sketchCanvas_->setSketch(partOrNull(), kInvalidObjectId);
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
    for (const std::unique_ptr<Body>& body : part().bodies())
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
    if (partOrNull() == nullptr) return {};
    return document_ != nullptr ? part().sketches() : std::vector<const Sketch*>{};
}

const Sketch* MainWindow::openedSketchById(ObjectId id) const {
    if (partOrNull() == nullptr) return nullptr;
    return document_ != nullptr ? part().findSketch(id) : nullptr;
}

std::size_t MainWindow::openedDocumentFeatureCount() const {
    if (partOrNull() == nullptr) return 0;
    if (document_ == nullptr || part().bodies().empty()) return 0;
    return part().bodies().front()->features().size();
}

std::size_t MainWindow::openedDocumentParameterCount() const {
    if (partOrNull() == nullptr) return 0;
    return document_ != nullptr ? part().parameters().items().size() : 0;
}

QString MainWindow::saveDocumentFile(const QString& path) {
    if (document_ == nullptr) return QStringLiteral("No document to save");
    // FINISH THE SKETCH FIRST. Sketch mode is a view state, but leaving it
    // makes what is on screen match what is in the file.
    if (inSketchMode()) finishSketchCommand();

    // WHICHEVER KIND IT IS (M27). Save is on Ctrl+S and in the File menu, and
    // neither is disabled for an assembly -- nor should they be: an assembly is
    // a document and saving it is the most ordinary thing to want. This is the
    // matching half of File > Open reading documentType.
    if (const auto* assembly = dynamic_cast<const AssemblyDocument*>(document_)) {
        const SaveResult savedAssembly =
            saveAssemblyDocumentToFile(*assembly, path.toStdString());
        if (!savedAssembly)
            return QStringLiteral("Could not save: %1")
                .arg(QString::fromStdString(savedAssembly.message));
        documentPath_ = path;
        setWindowTitle(QStringLiteral("EP3D - %1").arg(QFileInfo(path).fileName()));
        return QStringLiteral("Saved to %1").arg(path);
    }
    // A DRAWING IS A DOCUMENT TOO, and this was missing.
    //
    // File > Save and Ctrl+S are not disabled for a drawing -- nor should they
    // be -- so hitting either fell through to `part()`, which THROWS. Every
    // drawing built since M32, with its views, dimensions, frame, title block,
    // parts lists and schematics, could be opened and never written back.
    //
    // openDocumentFile has known how to read all three kinds since M27; the
    // two halves were kept by hand and only one of them was finished. That is
    // this project's recurring defect exactly, and the reason the abort was
    // never seen is that the self test built drawings and never saved one.
    if (const auto* drawing = dynamic_cast<const DrawingDocument*>(document_)) {
        const SaveResult savedDrawing =
            saveDrawingDocumentToFile(*drawing, path.toStdString());
        if (!savedDrawing)
            return QStringLiteral("Could not save: %1")
                .arg(QString::fromStdString(savedDrawing.message));
        documentPath_ = path;
        setWindowTitle(QStringLiteral("EP3D - %1").arg(QFileInfo(path).fileName()));
        return QStringLiteral("Saved to %1").arg(path);
    }
    const SaveResult saved = savePartDocumentToFile(part(), path.toStdString());
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
    // WHICH KIND OF DOCUMENT, ASKED OF THE FILE (M27).
    //
    // Not of the extension. ADR-M26-005 settled this for sub-assemblies and the
    // reason is the same here: an extension is a convention and the header is
    // the format. A file renamed by hand still says what it is, and a file that
    // lies about what it is gets refused by name rather than half-loaded.
    std::unique_ptr<DocumentBase> adopted;
    std::string failure;
    const std::optional<DocumentType> kind = docjson::documentTypeOfFile(path.toStdString());
    if (kind == DocumentType::Assembly) {
        AssemblyLoadResult loaded = loadAssemblyDocumentFromFile(path.toStdString());
        if (loaded) adopted = std::move(loaded.document);
        else failure = loaded.message;
    } else if (kind == DocumentType::Drawing) {
        // ASKED OF THE FILE'S OWN HEADER, not of its extension -- the same
        // rule an instance follows about what is in a file it names. A drawing
        // saved as `.ep3d` by hand still opens as a drawing.
        DrawingLoadResult loaded = loadDrawingDocumentFromFile(path.toStdString());
        if (loaded) adopted = std::move(loaded.document);
        else failure = loaded.message;
    } else {
        LoadResult loaded = loadPartDocumentFromFile(path.toStdString());
        if (loaded) adopted = std::move(loaded.document);
        else failure = loaded.message;
    }
    if (adopted == nullptr) {
        // NOTHING CHANGED. Neither loader returns a half-restored document,
        // so the one on screen is still exactly what it was.
        return QStringLiteral("Could not open: %1").arg(QString::fromStdString(failure));
    }

    // The kernel and the solver are the APPLICATION'S, not the file's -- they
    // are injected (ADR-M3-003 / ADR-M5-003) and a loaded document arrives with
    // neither. Carried across from the document being replaced, which is where
    // this window got them in the first place.
    if (document_ != nullptr) {
        adopted->setGeometryKernel(document_->geometryKernel());
        adopted->setSketchSolver(document_->sketchSolver());
    }

    if (inSketchMode()) finishSketchCommand();
    // ADOPTED, then pointed at. The previous document is freed only if THIS
    // window owned it; the one passed to the constructor belongs to its own
    // owner and outlives us.
    ownedDocument_ = std::move(adopted);
    document_ = ownedDocument_.get();
    presenter_->setDocument(*document_);
    // NULL FOR AN ASSEMBLY, which is what the canvas is told rather than being
    // handed something it cannot sketch on.
    if (sketchCanvas_ != nullptr) sketchCanvas_->setSketch(partOrNull(), kInvalidObjectId);
    selectedId_ = kInvalidObjectId;

    onRecomputeRequested();
    refreshAll();
    onFitAllRequested();
    documentPath_ = path;
    setWindowTitle(QStringLiteral("EP3D - %1").arg(QFileInfo(path).fileName()));
    return QStringLiteral("Opened %1").arg(path);
}

void MainWindow::onRunScriptRequested() {
    // The dialog is the only part a test cannot drive, so it is the only part
    // in the slot -- the split onImportDxfRequested established.
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Run Script"), QString(),
        QStringLiteral("EP3D scripts (*.ep3ds);;All files (*)"));
    if (path.isEmpty()) return; // cancelled; nothing said, nothing changed
    runScriptFile(path);
}

QString MainWindow::runScriptFile(const QString& path) {
    const auto say = [this](const QString& message) {
        statusLeft_->setText(message);
        statusLeft_->setToolTip(message);
        return message;
    };
    if (document_ == nullptr) return say(QStringLiteral("There is no document to run against."));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // THE CAUSE, not "could not run" -- the same rule the DXF reader
        // follows. A missing file and an unreadable one are different problems
        // with different fixes.
        return say(QStringLiteral("Could not read %1: %2").arg(path, file.errorString()));
    }
    const QByteArray text = file.readAll();
    file.close();

    // THE SAME INTERPRETER the CLI and the socket use. A second one that
    // understood "nearly the same" vocabulary would be this project's
    // best-known defect shape, reached through the File menu.
    // THE INTERPRETER IS A PART'S. Its vocabulary builds sketches and
    // features, and an assembly has neither -- so this refuses by name rather
    // than throwing out of part(). (The CLI drives assemblies through its own
    // `assembly` verb, against documents it owns.)
    if (partOrNull() == nullptr)
        return say(QStringLiteral("Scripts run against a part, and this document is an "
                                  "assembly. File > New first to start a part."));
    const ScriptOutcome outcome = RunSketchScript(part(), text.toStdString());

    // THE VIEW, whatever happened. A script that fails at line 90 has already
    // built 89 lines' worth of geometry, and leaving the window showing the
    // state before it would be a lie about what the document now holds.
    refreshAll();

    if (!outcome.ok) {
        // WHICH LINE. Finding out where a script goes wrong is most of what
        // running one is for, and a message without the number sends the user
        // back to read the whole file.
        return say(QStringLiteral("%1 stopped at line %2: %3")
                       .arg(QFileInfo(path).fileName())
                       .arg(outcome.failedLine)
                       .arg(QString::fromStdString(outcome.message)));
    }
    // HOW MANY UNDO STEPS IT LEFT, because it is not one. A user who runs a
    // script and then reaches for Ctrl+Z should be told what that will do
    // before they press it, not after.
    return say(QStringLiteral("%1: ran %2 command%3. Each is its own undo step.")
                   .arg(QFileInfo(path).fileName())
                   .arg(outcome.log.size())
                   .arg(outcome.log.size() == 1 ? QString() : QStringLiteral("s")));
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
        part(), name.isEmpty() ? std::string("Imported") : name.toStdString(),
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
        ReconstructSketch(part(), imported.sketchId, read.geometry.dimensions);
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
    if (const Sketch* importedSketch = part().findSketch(imported.sketchId)) {
        if (BuildProfile(*importedSketch)) {
            Body* body = part().bodies().empty() ? &part().addBody("Body001")
                                                     : part().bodies().front().get();
            const Parameter* existing = part().parameters().findByName("PadLength");
            const ObjectId lengthId =
                existing != nullptr
                    ? existing->id()
                    : part().addParameter(uniqueObjectName("PadLength"), 20.0,
                                              UnitType::Millimeter)
                          .id();
            part().addPadFeature(*body, name.toStdString() + "_Pad", imported.sketchId,
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

std::string MainWindow::treeStateFor(const std::string& nameFragment) const {
    std::string found;
    if (tree_ == nullptr) return found;
    const std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
        if (found.empty() &&
            item->text(0).toStdString().find(nameFragment) != std::string::npos)
            found = item->text(1).toStdString();
        for (int i = 0; i < item->childCount(); ++i) visit(item->child(i));
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) visit(tree_->topLevelItem(i));
    return found;
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
        // THE LABEL IS TEXT, AND `&` IS A MNEMONIC MARKER. "H&V Distance"
        // would put an underline under the V and show nothing where the
        // ampersand belongs, so the action text doubles it -- and the TOOLTIP
        // must not, because tooltips do not process mnemonics and would print
        // the doubled one literally. Escaping every label is a no-op for the
        // rest: none of them uses `&`, they carry their accelerator in
        // `shortcut`.
        const QString label = QString::fromLatin1(entry.label);
        QString actionText = label;
        actionText.replace(QLatin1Char('&'), QLatin1String("&&"));
        QAction* action = sketchToolBar_->addAction(sketchIcon(entry.icon), actionText);
        action->setToolTip(entry.shortcut[0] != 0
                               ? QStringLiteral("%1 (%2)\n%3")
                                     .arg(label)
                                     .arg(QString::fromLatin1(entry.shortcut))
                                     .arg(QString::fromLatin1(entry.tip))
                               : QStringLiteral("%1\n%2")
                                     .arg(label)
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
        {SketchEditKind::AddHVDistance, "H&V Distance", "",
         "Dimension BOTH gaps between the 2 selected points at once: one "
         "horizontal and one vertical, as two dimensions in one undo step",
         true, ui::SketchIcon::HVDistance},
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

bool MainWindow::modelToolBarVisible() const {
    return modelToolBar_ != nullptr && modelToolBar_->isVisible();
}
bool MainWindow::modelTreeVisible() const {
    return treeDock_ != nullptr && treeDock_->isVisible();
}
bool MainWindow::constraintPanelOnLeft() const {
    return constraintDock_ != nullptr && dockWidgetArea(constraintDock_) == Qt::LeftDockWidgetArea;
}

void MainWindow::enterSketchMode(ObjectId sketchId) {
    editingSketch_ = sketchId;
    sketchCanvas_->setSketch(partOrNull(), sketchId);
    centralStack_->setCurrentWidget(sketchCanvas_);
    sketchToolBar_->setVisible(true);
    if (sketchToolBarSecond_ != nullptr) sketchToolBarSecond_->setVisible(true);
    // M26.2: the model toolbar and the tree act on FEATURES -- Pad, Revolve,
    // a solid's place in the tree -- and none of that exists to act on while
    // a sketch is open. Leaving them on screen was clutter around the one
    // panel a sketch actually needs, so both go away for as long as the
    // sketch is open, the same way the sketch toolbar itself appears only
    // then.
    modelToolBar_->setVisible(false);
    treeDock_->setVisible(false);
    // The constraint panel moves INTO the column the tree just vacated,
    // rather than piling a second dock onto the properties column on the
    // right. addDockWidget on a dock that is already placed MOVES it -- this
    // is not a second widget, it is the same one changing address -- and the
    // width is reasserted because Qt does not carry a hidden dock's old
    // width over to a widget arriving from a different area.
    addDockWidget(Qt::LeftDockWidgetArea, constraintDock_);
    resizeDocks({constraintDock_}, {ui::size::kModelTreeMinWidth + 100}, Qt::Horizontal);
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
    Sketch& created = part().addSketch(name);
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
    Sketch& created = part().addSketch(name, plan.frame);
    // The face's boundary, projected, as a tracing underlay (M17.6). Added
    // BEFORE the sketch is opened, so the canvas has it the first time it
    // paints rather than a frame later.
    //
    // Not undoable, and consistent with the sketch itself: creating a sketch is
    // not an undo step (see newSketchCommand), and an underlay that could be
    // undone out from under the sketch it belongs to would leave a face sketch
    // that no longer knows what it was made on.
    part().addSketchReferences(created.id(), plan.reference.geometry);

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
        if (part().setSketchTrackedFace(created.id(), query)) {
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
    const Sketch* sketch = part().findSketch(sketchId);
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
    sketchCanvas_->setSketch(partOrNull(), kInvalidObjectId);
    centralStack_->setCurrentWidget(viewer_);
    sketchToolBar_->setVisible(false);
    if (sketchToolBarSecond_ != nullptr) sketchToolBarSecond_->setVisible(false);
    constraintDock_->setVisible(false);
    // Reverses enterSketchMode's swap: the tree comes back, the model
    // toolbar comes back, and the constraint panel returns to the properties
    // column rather than staying parked in the tree's old spot while hidden
    // -- so the NEXT sketch opened starts from the same layout this one did,
    // instead of drifting one dock further left each time a sketch closes.
    modelToolBar_->setVisible(true);
    treeDock_->setVisible(true);
    addDockWidget(Qt::RightDockWidgetArea, constraintDock_);
    // Selecting the finished sketch is what makes "Insert > Pad" the obvious
    // next step: the command it needs is already armed.
    selectObject(finished);
    refreshAll();
    const Sketch* sketch = part().findSketch(finished);
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
            ? part().findSketch(editingSketch_)
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

    const std::vector<ConstraintRow> rows = ConstraintRowsFor(part(), *sketch);
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

int MainWindow::assemblyToolbarButtonCount() const {
    return static_cast<int>(ToolbarButtons(assemblyToolBar_).size());
}
std::string MainWindow::assemblyToolbarLabel(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(assemblyToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return std::string();
    return buttons[static_cast<std::size_t>(index)]->iconText().toStdString();
}
bool MainWindow::assemblyToolbarButtonEnabled(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(assemblyToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return false;
    return buttons[static_cast<std::size_t>(index)]->isEnabled();
}
int MainWindow::drawingToolbarButtonCount() const {
    return static_cast<int>(ToolbarButtons(drawingToolBar_).size());
}

std::string MainWindow::drawingToolbarLabel(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(drawingToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return {};
    return buttons[static_cast<std::size_t>(index)]->iconText().toStdString();
}

namespace {

// WHAT AN ICON ACTUALLY LOOKS LIKE, as one number (M26.1).
//
// Used to prove no two buttons on a bar carry the SAME picture -- "every
// button has an icon" is satisfied by giving them all one, which is exactly
// what a copy-paste slip produces.
//
// ONE COPY, and it was four VARIANTS before M32.4 was about to make it five:
// two hashed pixel by pixel and two byte by byte, at two different sizes.
// Four copies of a hash is bad; four DIFFERENT hashes of one idea is worse,
// because two bars then cannot be compared at all and nothing says so.
//
// Rendered at a size, then hashed. Hashing the QIcon itself would compare
// handles rather than pictures, and two different handles can carry the same
// drawing.
unsigned long long IconFingerprint(const QIcon& icon, int sizePx) {
    if (icon.isNull()) return 0;
    const QImage image = icon.pixmap(QSize(sizePx, sizePx))
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

} // namespace

unsigned long long MainWindow::drawingToolbarIconFingerprint(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(drawingToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return 0;
    return IconFingerprint(buttons[static_cast<std::size_t>(index)]->icon(),
                           ui::size::kToolbarIcon);
}

unsigned long long MainWindow::assemblyToolbarIconFingerprint(int index) const {
    const std::vector<QAction*> buttons = ToolbarButtons(assemblyToolBar_);
    if (index < 0 || index >= static_cast<int>(buttons.size())) return 0;
    return IconFingerprint(buttons[static_cast<std::size_t>(index)]->icon(),
                           ui::size::kToolbarIcon);
}
bool MainWindow::assemblyToolbarVisible() const {
    return assemblyToolBar_ != nullptr && assemblyToolBar_->isVisible();
}
bool MainWindow::modelToolbarVisible() const {
    return modelToolBar_ != nullptr && modelToolBar_->isVisible();
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
    return IconFingerprint(buttons[static_cast<std::size_t>(index)]->icon(),
                           ui::size::kToolbarIcon);
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
    return IconFingerprint(buttons[static_cast<std::size_t>(index)]->icon(),
                           ui::size::kToolbarIcon);
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
    return IconFingerprint(buttons[static_cast<std::size_t>(index)]->icon(),
                           ui::size::kSketchToolbarIcon);
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
    const Sketch* sketch = part().findSketch(editingSketch_);
    if (sketch == nullptr) return;
    const auto id = static_cast<SketchConstraintId>(static_cast<ObjectId>(constraintId));
    const std::string current = DimensionEditText(part(), *sketch, id);
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
    // WHAT WAS PICKED ON THE CANVAS, in the property panel (M26.7).
    // Selection is presentation, so this is the signal that carries it --
    // and without this line the panel only ever answered the model tree,
    // which in sketch mode is not even on screen.
    rebuildProperties();
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
