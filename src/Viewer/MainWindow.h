#pragma once

#include "Core/Document/CadDocument.h"
#include "Core/Document/ObjectId.h"
#include "Core/Feature/BooleanFeature.h"
#include "Core/Kernel/EdgeQuery.h"
#include "Core/Kernel/FaceQuery.h"
#include "Core/Recompute/RecomputeTypes.h"
#include "Viewer/DocumentOutline.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Viewer/SketchCanvas.h"
#include <QMainWindow>
#include <QString>
#include <cstddef>
#include <map>
#include <memory>
#include <source_location>
#include <string>
#include <vector>
#include <set>

class QAction;
class QMenu;
class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QLabel;
class QStackedWidget;
class QToolBar;
class QDockWidget;
class QPushButton;

namespace paramcad {

class DocumentBase;
class PartDocument;
class DocumentPresenter;
class OcctViewWidget;
class SketchCanvasWidget;

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
    // Declared, not defaulted inline: this window can OWN a PartDocument (see
    // ownedDocument_), and destroying a unique_ptr needs the complete type,
    // which this header deliberately does not have.
    ~MainWindow();

    // Selected semantic object, or kInvalidObjectId. Exposed so a smoke test
    // can assert Tree/Viewer/Property agreement without a human (UI spec 10).
    ObjectId selectedObjectId() const noexcept { return selectedId_; }

    // Rebuilds tree, properties and viewer from current document state.
    void refreshAll();

    // Says, IN THE WINDOW, that a script socket is open and on which port
    // (M17.28). A GUI application has no console: printing it to stdout tells
    // whoever launched from a terminal and nobody else, and the person looking
    // at the window -- the one who has to decide whether to trust it -- is told
    // nothing at all.
    void showScriptPort(quint16 port);

    // Imports `path` and refreshes the shell. Separated from the menu slot so
    // the whole workflow -- read, import, recompute, redisplay, report -- is
    // reachable without a file dialog, which is the only part of it a test
    // cannot drive. Returns the message shown in the status bar.
    QString importDxfFile(const QString& path);

    // --- M9.5: the history commands, exposed so a smoke test can drive the
    // whole workflow without a human, exactly as importDxfFile is.
    //
    // Each returns the status message it showed, so a test can assert what the
    // USER was told rather than only what the model did -- the M6.14 lesson
    // applied to commands instead of to rows.
    QString undoCommand();
    QString redoCommand();
    QString toggleSuppressSelected();
    QString rollbackToSelected();
    QString rollForwardToEnd();

    // --- M9.5: feature creation in the shell --------------------------------
    //
    // ADR-M8-007 deferred "feature creation dialogs" to M9's edit-transaction
    // work. What arrived is COMMANDS rather than dialogs, and the difference is
    // deliberate (ADR-M9-005): the inputs a Pad needs are a sketch and a
    // length, the tree already selects the sketch, and the property panel
    // already edits the length -- with a preview already on screen. A modal
    // dialog would add a second place to type the same number.
    //
    // Each creates its parameter AND its feature inside one transaction, so the
    // whole command is one undo step and an undo leaves no orphan parameter
    // behind.
    // What a just-created feature ACTUALLY did.
    //
    // "Pad created" was printed whether the feature computed or not, so a
    // profile the kernel could not use -- a rectangle with a circle inside it,
    // say -- reported success and showed nothing new. The status line is the
    // only place a user finds out, and it was saying the opposite of the truth.
    QString describeCreatedFeature(const char* what, ObjectId featureId,
                                   const DocumentRecomputeReport& report) const;

    // --- Files (M17) --------------------------------------------------------
    // Both take a path so a test can drive them; the dialogs live in the slots,
    // which is the split onImportDxfRequested already established.
    // A fresh, empty document -- adopted the same way an opened one is.
    QString newDocumentCommand();
    // Deletes whatever is selected in the model tree: a feature, or a sketch.
    QString deleteSelectedObjectCommand();

    QString saveDocumentFile(const QString& path);
    QString openDocumentFile(const QString& path);
    // Runs an .ep3ds SCRIPT against the document this window is looking at
    // (M26.6). Returns what the user is told; never empty.
    //
    // It ADDS to the open document rather than replacing it -- the same thing
    // `--connect` does, and for the same reason: a script is a sequence of the
    // commands a user could have typed, so running one has to mean what typing
    // them would have meant. `File > New` first is how to get the other
    // behaviour, and it is one click.
    //
    // NOT ONE UNDO STEP, and the message says so. Each command records its own
    // delta, exactly as it does over the socket. Wrapping the whole run in one
    // transaction was the alternative and it is worse: a script that fails at
    // line 90 would throw away the 89 lines that worked, and finding out WHERE
    // a script goes wrong is most of what running one is for.
    QString runScriptFile(const QString& path);

    // --- Assembly commands (M28) --------------------------------------------
    //
    // Each takes its inputs as arguments and returns what the user was told, so
    // the whole workflow is reachable without a dialog -- the split
    // importDxfFile established, and the only reason any of this is testable.
    //
    // The document AS AN ASSEMBLY is checked inside each, the same way part()
    // checks the other direction: these are reached from menu items that are
    // disabled on a part, and the check is what makes "disabled" a guarantee
    // rather than a hope.
    QString insertInstanceCommand(const QString& sourcePath, const QString& bodyName = {});
    // Moves the SELECTED instance to an absolute place in the assembly.
    QString placeSelectedInstance(const Vec3& whereMm);
    QString toggleGroundSelectedInstance();
    // `count` INCLUDING the original, matching addInstancePattern.
    QString patternSelectedInstance(int count, const Vec3& stepMm);
    // Deletes the selected instance -- and says what went with it, because a
    // mate that named it cannot survive it.
    QString deleteSelectedInstance();

    // Which instance the tree has selected, or kInvalidObjectId.
    ObjectId selectedInstance() const;

    // --- Readbacks, so a self test can drive the whole workflow -------------
    //
    // Asked of the document the WINDOW is looking at, which after an Open is
    // not the one its owner still holds -- the same reason openedSketches()
    // exists rather than the test asking the owner.
    void adoptAssemblyForTesting(const QString& name);
    std::size_t instanceCountForTesting() const;
    std::size_t mateCountForTesting() const;
    std::vector<std::string> instanceNamesForTesting() const;
    std::vector<Vec3> instancePlacesForTesting() const;
    void selectFirstInstanceForTesting();
    // WHAT KIND of document this window is looking at (M27). A readback,
    // so a self test can assert that File > Open read the file right.
    DocumentType openedDocumentType() const;
    // The tree AS BUILT, for the same reason: a builder that produced the
    // right nodes and was never called would pass every other check.
    OutlineNode probeOutline() const { return buildOutline(); }
    bool insertPadEnabled() const;
    // Where this document was last saved or opened from, or empty.
    QString documentPath() const { return documentPath_; }
    // Counted off the document the WINDOW is looking at, which after an Open is
    // not the one its owner still holds. Asking the owner would pass even if
    // the window never re-seated.
    std::size_t openedDocumentFeatureCount() const;
    // The sketches of whatever document this window is looking at.
    //
    // BY VALUE, because PartDocument::sketches() builds its vector on the fly.
    // Returning a reference to it bound to a temporary and read back as empty --
    // which looked exactly like "the file lost its sketches".
    std::vector<const Sketch*> openedSketches() const;
    // One sketch of the document this window is looking at, by id.
    const Sketch* openedSketchById(ObjectId id) const;
    std::size_t openedDocumentParameterCount() const;

    QString insertPadFromSelection();
    // Revolve: spins the selected sketch's profile about one of its lines.
    //
    // Split so a test can supply the axis and the angle directly. The
    // interactive half only decides WHICH axis and asks for the angle; the
    // command itself is the same either way, which is what stops the tested
    // path and the used path from drifting apart.
    QString insertRevolveFromSelection(SketchEntityId axisEntityId, double angleDegrees);
    // The axis this sketch offers with no question asked: its single
    // construction line, or an invalid id when the choice is not obvious.
    SketchEntityId obviousRevolveAxis(ObjectId sketchId) const;
    QString insertPocketFromSelection();

    // --- M19-M22 features, on the Model toolbar (M26.1) ---------------------
    //
    // Everything from Sweep to Import shipped with "UI: script and API only"
    // recorded against it. These are the same commands the script verbs are,
    // with the same rules -- restating a rule differently here would be two
    // answers to one question, which is the defect class this project spends
    // its milestones removing.
    //
    // Each one is PUBLIC for the same reason the others are (ADR-M9-005): the
    // selftest drives them and reads the message back, so a command that
    // reported success over a solid that did not change is caught.
    QString insertSweepFromSelection();
    QString insertLoftFromSelection();
    QString insertShellOnTail();
    QString insertHoleFromSelection();
    QString insertBooleanOnTail(BooleanOperation operation, const char* what);
    QString insertCircularPatternOnTail();
    QString insertCurvePatternFromSelection();
    QString exportCurrentSolid();
    QString importSolidAsFeature();
    QString insertFilletOnTail();
    QString insertChamferOnTail();

    // --- M12: sketch mode ---------------------------------------------------
    //
    // Same contract as importDxfFile and the M9.5 history commands: each
    // returns the status line it showed, so a test asserts what the USER was
    // told rather than only what the model did.

    // Creates a sketch on world XY and opens it for drawing.
    //
    // NOT UNDOABLE, and the status line says so. `UndoDelta` has no
    // sketch-existence case: adding one means also defining what an undo does
    // to the features that reference the sketch, which is a Core decision this
    // milestone did not take. Everything the user then DRAWS inside the sketch
    // IS undoable (M12.0 added those deltas) -- it is only the empty sketch's
    // creation that is not.
    QString newSketchCommand();

    // Creates a sketch on the plane of the face last clicked in the 3D view
    // (M17.5, ADR-M17-027), and opens it. Returns the message shown to the
    // user, which on refusal says which of the two things went wrong: nothing
    // was picked, or what was picked is curved.
    //
    // The sketch is placed on that PLANE. It does not track the face, and the
    // success message says so -- see FaceSketch.h for why that limit exists.
    QString sketchOnFaceCommand();

    // Opens the sketch selected in the model tree.
    QString editSelectedSketchCommand();

    // Leaves sketch mode and returns to the 3D view.
    QString finishSketchCommand();

    bool inSketchMode() const noexcept;
    // Which sketch is open, or kInvalidObjectId. A readback, not a handle to
    // edit through: the shell still writes to a sketch through PartDocument's
    // facade (UI spec 20).
    ObjectId editingSketch() const noexcept { return editingSketch_; }

    // M26.2: what a sketch does to the shell around it, readable without a
    // screenshot. The model toolbar (Pad, Revolve, Union, ...) and the tree
    // act on FEATURES and on what is already built -- neither means anything
    // while a sketch is open, and leaving them on screen only crowds the one
    // panel a sketch does need.
    bool modelToolBarVisible() const;
    bool modelTreeVisible() const;
    // The constraint panel moves INTO the column the tree just vacated,
    // rather than sharing the properties column on the right -- a sketch
    // with many constraints needs its own width, not a fight with whatever
    // the property panel is already showing.
    bool constraintPanelOnLeft() const;

    // Switches how SOLIDS are drawn (M17.9). Returns what the user is told;
    // never empty, because a view command that changes nothing visible on an
    // empty document still has to say what it did.
    QString setSolidDisplayCommand(bool wireframe);
    // What the MENU currently claims, which is the half a readback can check
    // against the viewer's own answer.
    bool wireframeMenuChecked() const;

    // The model tree as the WIDGET holds it: one string per row, indented by
    // depth (M17.10). The outline's shape is unit-tested; what only a running
    // window can answer is whether that shape reached the QTreeWidget -- and
    // "the outline nested it" and "the tree shows it nested" are two claims.
    std::vector<std::string> treeRows() const;
    // The 3D view, for readbacks only. The smoke test has to be able to ask
    // what is actually in the scene: "the presenter listed it" and "it is on
    // screen" are different claims, and the gap between two such claims is the
    // defect class this shell exists to catch (M6.14).
    OcctViewWidget* viewer() const noexcept { return viewer_; }
    SketchCanvasWidget* sketchCanvas() const noexcept { return sketchCanvas_; }

    // --- What the SKETCH UI is displaying -----------------------------------
    // Read out of the widgets, never recomputed from the model. M6.14 is the
    // reason: a constraint panel with the right rows and no visible text is a
    // defect only a widget-level question can see.
    int displayedConstraintRowCount() const;
    std::string displayedConstraintText(int row) const;
    // The id carried by a panel row, so a caller can act on the constraint the
    // user is looking at rather than re-deriving which one it must be.
    unsigned long long displayedConstraintId(int row) const;
    std::string displayedSketchStatus() const;
    // Deletes the constraint on the currently highlighted panel row.
    QString deleteSelectedConstraintRow();

    // Opens the prefix / suffix / tolerance editor for one dimension. Exposed
    // with the values pre-supplied so a test can drive the COMMIT path without
    // a modal dialog -- the dialog only collects the four numbers.
    QString applyDimensionFormat(SketchConstraintId constraintId, const QString& prefix,
                                 const QString& suffix, double plusDisplay,
                                 double minusDisplay);
    // Selects a constraint panel row, as clicking it does.
    bool selectConstraintRow(int row);
    // Which row is selected, or -1. Asked of the TABLE: "the canvas and the
    // panel agree" is a claim about two widgets, and re-deriving it from the
    // model would prove only that the model is self-consistent.
    int selectedConstraintRow() const;
    // The panel's own delete button (roadmap 6.3's trash icon). Asked of the
    // WIDGET so a smoke test can tell a reachable button from a menu entry
    // nobody finds: a command that exists only under Constrain > ... is not
    // the "click the row, press delete" the roadmap describes.
    bool constraintDeleteButtonEnabled() const;
    std::string constraintDeleteButtonText() const;
    QString clickConstraintDeleteButton();

    // --- What the sketch TOOLBAR is showing ---------------------------------
    // Asked of the widgets, not of the list they were built from. An action
    // that exists with no icon is an empty button, and every one of these
    // counts would still look right if the icons had never been attached.
    // The MAIN toolbar, asked the same questions as the sketch one. Undo and
    // Redo are exact mirrors of each other by design, which is a distinctness
    // risk a human eye can miss and a fingerprint cannot.
    // The MODEL toolbar: the solid commands, which lived only in the Insert
    // menu. Asked of the widgets for the same reason the others are.
    int modelToolbarButtonCount() const;
    int modelToolbarButtonsWithIcons() const;
    unsigned long long modelToolbarIconFingerprint(int index) const;
    std::string modelToolbarLabel(int index) const;
    bool modelToolbarButtonEnabled(int index) const;

    int mainToolbarButtonCount() const;
    int mainToolbarButtonsWithIcons() const;
    unsigned long long mainToolbarIconFingerprint(int index) const;
    std::string mainToolbarLabel(int index) const;

    // The label of the tool button currently shown as pressed, or empty. Read
    // from the BUTTON, because "which tool is active" is a claim about what the
    // user can see, and the model's answer is exactly the half that was already
    // right when this went wrong.
    std::string checkedSketchToolLabel() const;

    // Runs a constraint or dimension command exactly as its toolbar button
    // does, INCLUDING what happens to the status line.
    //
    // Factored out so a test drives the same path a user does. Calling the
    // canvas directly skips the half that decides whether a refusal is
    // readable, which is the half that was broken.
    QString applySketchCommand(SketchEditKind kind, bool dimension);

    // The status line's message right now, for a test that has to prove a
    // refusal is still readable after the mouse has moved.
    std::string displayedSketchMessage() const;

    // Whether Edit Selected Sketch is OFFERED right now. Asked of the ACTION,
    // because "the command is available" is a claim about the menu, and the
    // model's opinion is exactly the half that was already right.
    bool editSketchEnabled() const;

    // Whether the Trim / Extend buttons are showing as pressed.
    bool trimButtonChecked() const;
    bool extendButtonChecked() const;

    int sketchToolbarButtonCount() const;
    int sketchToolbarButtonsWithIcons() const;
    // A hash of one button's rendered icon, so a test can prove the icons are
    // DISTINCT. "Every button has an icon" is satisfied by giving them all the
    // same one, which is the failure this is here to catch.
    unsigned long long sketchToolbarIconFingerprint(int index) const;
    // The tooltip a button is offering. With an icon-only bar this is the only
    // place the command's NAME appears on screen.
    std::string sketchToolbarTooltip(int index) const;

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

    // What reconstruction did to each imported sketch, kept by the SHELL.
    //
    // Session state, not document state (ADR-M7-017): it is not persisted, and
    // a document reopened later simply has no entry. Keyed by sketch id, so a
    // second import into the same document keeps its own report.
    const ReconstructionReport* reconstructionReportFor(ObjectId sketchId) const;

    // The value the panel is DISPLAYING for a property, read out of the table
    // widget itself rather than recomputed from the model.
    //
    // That distinction is the whole lesson of M6.14: propertiesOf() returned
    // ten correct rows while the user saw ten labels and no values, and every
    // data-level test agreed with the model. A UI assertion has to ask the
    // widget. Empty string when no row carries that label.
    std::string displayedPropertyValue(const std::string& label) const;

    // The TOOLTIP the panel is showing for a property row, read out of the
    // widget. Same reasoning as displayedPropertyValue: after a refused
    // expression the caret rendering lives ONLY in the tooltip, so a test that
    // asks the model cannot tell whether it was ever delivered.
    std::string displayedPropertyTooltip(const std::string& label) const;

    // Types `text` into a property row and commits it, exactly as a user
    // finishing an edit does (M17.16). The readbacks above answer "what does
    // the panel SHOW"; this answers "does typing into it do anything" -- and
    // those are two claims, which is how a Length row that accepted typing and
    // changed nothing survived for a milestone (ADR-M17-027).
    //
    // Returns false when the row is not there or the widget will not let it be
    // edited, so a cell that is read-only in the WIDGET cannot pass as one that
    // merely refused the value.
    bool typeIntoPropertyRow(const std::string& label, const std::string& text);

    // Whether the panel is showing a property row with this label at all.
    // An editable row that is absent is not a row a user can discover.
    bool hasPropertyRow(const std::string& label) const;
    // The VALUE shown against a label, or empty. Asked of the WIDGET, so a
    // panel that built the right rows and painted none of them fails here.
    std::string propertyRowValue(const std::string& label) const;

    // Commit a value into the property row with this label, exactly as typing
    // it and pressing Enter does, and return the resulting status line.
    //
    // Test-only in practice, and the same shape as importDxfFile / undoCommand:
    // the smoke test drives the REAL commit path rather than a parallel one, so
    // what it exercises is what a user gets.
    QString editPropertyByLabel(const std::string& label, const QString& text);

    // NEGATIVE CONTROL for propertyPanelFitsItsPanel().
    //
    // Widens the label column past what leaves the value column readable,
    // samples the guard, and puts the column back. Returns true if the guard
    // correctly reported NOT-fitting.
    //
    // This exists because independent review hard-coded that guard to `return
    // true` and all thirteen viewer smoke tests stayed green: a guard that can
    // only ever say yes is untestable by any amount of well-formed input. The
    // only way to test it is to give it something it must reject.
    bool panelFitGuardCanFail();

private:
    // Session-scoped provenance (ADR-M7-017): never persisted, so a document
    // reopened later simply has no entry.
    //
    // That sentence is only true while every entry's sketch is still in the
    // document. The map had ONLY an insert path (round 2's M3), bounded purely
    // by the shell having no File-Open and no delete-sketch command -- and a
    // LOADED document's sketch ids come from the file, not the generator, so a
    // collision with a live entry is not exotic.
    //
    // WHAT IS ACTUALLY WIRED, corrected in round 4 (R3R4-M2), because the
    // sentence that stood here -- "the two erase paths, called from wherever a
    // sketch or a document leaves" -- was false in both halves:
    //   * `pruneProvenance` is the only path that runs today. It is called
    //     after every recompute and drops entries whose sketch has gone. It is
    //     the backstop, and it is doing all of the work.
    //   * `forgetAllProvenance` has exactly one caller: pruneProvenance's
    //     null-document branch.
    //   * `forgetProvenanceFor` has NO callers. It is kept deliberately, as the
    //     obvious thing for a future delete-sketch command to call, and it is
    //     named here as uncalled so nobody reads it as wired.
    std::map<ObjectId, ReconstructionReport> reconstructionReports_;
    void forgetProvenanceFor(ObjectId sketchId);
    void forgetAllProvenance();
    // Drops entries whose sketch is no longer in the document. The backstop
    // for any future path that removes a sketch without telling this class.
    void pruneProvenance();

private slots:
    void onTreeSelectionChanged();
    void onViewerSelectionChanged(qulonglong objectId);
    void onPropertyCommitted(int row, int column);
    void onRecomputeRequested();
    void onFitAllRequested();
    void onToggleHiddenRequested();
    void onImportDxfRequested();
    void onRunScriptRequested();
    void onInsertInstanceRequested();
    void onGroundInstanceRequested();
    void onPatternInstanceRequested();
    void onDeleteInstanceRequested();
    void onSaveRequested();
    void onSaveAsRequested();
    void onOpenRequested();
    void onNewRequested();
    void onDeleteObjectRequested();
    void onUndoRequested();
    void onRedoRequested();
    void onSuppressRequested();
    void onRollbackRequested();
    void onRollForwardRequested();
    void onInsertPadRequested();
    void onInsertPocketRequested();
    void onInsertRevolveRequested();
    void onInsertSweepRequested();
    void onInsertLoftRequested();
    void onInsertShellRequested();
    void onInsertHoleRequested();
    void onInsertUnionRequested();
    void onInsertSubtractRequested();
    void onInsertIntersectRequested();
    void onInsertCircularPatternRequested();
    void onInsertCurvePatternRequested();
    void onExportRequested();
    void onImportRequested();
    void onInsertFilletRequested();
    void onInsertChamferRequested();
    void onNewSketchRequested();
    void onSketchOnFaceRequested();
    void onEditSketchRequested();
    void onFinishSketchRequested();
    void onSketchDocumentChanged(const QString& status);
    void onSketchPresentationChanged();
    void onDimensionActivated(qulonglong constraintId);
    void onConstraintRowActivated(int row, int column);
    // Offset: asks for a distance, then offsets the selection.
    void onOffsetRequested();
    // Chamfer: asks for the two setbacks, then cuts the corner.
    void onChamferRequested();
    // Fillet: asks for the radius, then rounds the corner.
    void onFilletRequested();
    void onTransformRequested();
    // A badge was clicked on the canvas: move the panel's selection to it.
    void onConstraintPickedOnCanvas(qulonglong constraintId);
    // A row became current: tell the canvas which constraint to ring.
    void onConstraintRowHighlighted();
    void onDimensionFormatRequested();

private:
    void buildMenus();
    void buildToolbar();
    void buildDocks();
    void buildSketchUi();
    void enterSketchMode(ObjectId sketchId);
    void rebuildConstraintPanel();
    // Makes the tool buttons agree with the canvas about which tool is active.
    void syncSketchToolButtons();
    void updateSketchStatus();
    // Puts a one-line message where it belongs for the current mode.
    void reportSketchOrPlainStatus(const QString& message);
    void rebuildTree();
    // THE DOCUMENT AS A PART, checked.
    //
    // Every part-authoring command -- pad, pocket, sketch, pattern -- goes
    // through here, and an assembly has none of them. Written as a checked
    // accessor rather than a cast at each of ninety-three call sites for the
    // reason RecomputeContext::part() already gives: one place asks the
    // question, so one place can answer it wrongly, and it throws rather than
    // returning a null nobody checks.
    //
    // The MENUS are what keep this from being reached on an assembly: every
    // part command is disabled when the document is not one (refreshCommandStates).
    // NAMES ITS CALLER when it throws. "this command needs a part document"
    // with no location is a message that sends a reader through the whole
    // shell looking for which call it was -- which is exactly what it cost
    // the first time. source_location is free at every call site that
    // never throws.
    PartDocument& part(std::source_location where = std::source_location::current()) const;
    // ...and the same question asked WITHOUT insisting, for the code that has
    // to decide what to show rather than what to do.
    PartDocument* partOrNull() const noexcept;

    // The tree, from whichever builder the document type calls for.
    OutlineNode buildOutline() const;
    // Every command that needs a PART. Disabled wholesale when the document
    // is not one, which is what keeps part() from ever being reached.
    std::vector<QAction*> partOnlyActions() const;
    void rebuildProperties();
    // The rendering half, shared by the tree's selection and the canvas's.
    void showPropertyRows(const std::vector<PropertyRow>& rows);
    void clampLabelColumn();
    void updateStatus();
    void reportHealth();
    QString selectionSummary() const;
    // The body that owns the selected object, and the selected feature's index
    // in it. kInvalidObjectId / npos when the selection is not a feature.
    ObjectId selectedFeatureBody(std::size_t* indexOut = nullptr) const;
    // Menu items are enabled from the model, not from a guess: an Undo item
    // that is always enabled and silently does nothing is a lie the user can
    // click.
    void refreshCommandStates();

    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* suppressAction_ = nullptr;
    QAction* rollbackAction_ = nullptr;
    QAction* rollForwardAction_ = nullptr;
    QAction* insertPadAction_ = nullptr;
    QAction* insertPocketAction_ = nullptr;
    QAction* insertRevolveAction_ = nullptr;
    QAction* insertSweepAction_ = nullptr;
    QAction* insertLoftAction_ = nullptr;
    QAction* insertShellAction_ = nullptr;
    QAction* insertHoleAction_ = nullptr;
    QAction* insertUnionAction_ = nullptr;
    QAction* insertSubtractAction_ = nullptr;
    QAction* insertIntersectAction_ = nullptr;
    QAction* insertCircularPatternAction_ = nullptr;
    QAction* insertCurvePatternAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* importAction_ = nullptr;
    QAction* deleteObjectAction_ = nullptr;
    QAction* insertFilletAction_ = nullptr;
    QAction* insertChamferAction_ = nullptr;
    QAction* newSketchAction_ = nullptr;
    QAction* sketchOnFaceAction_ = nullptr;
    QAction* useReferenceAction_ = nullptr;
    QAction* dimensionToolAction_ = nullptr;
    QAction* solidShadedAction_ = nullptr;
    QAction* solidWireframeAction_ = nullptr;
    QAction* editSketchAction_ = nullptr;
    QAction* finishSketchAction_ = nullptr;
    QAction* deleteConstraintAction_ = nullptr;
    QAction* dimensionFormatAction_ = nullptr;
    // Tool and constraint commands, kept so they can be enabled from the model
    // rather than left clickable while no sketch is open.
    std::vector<QAction*> sketchModeActions_;
    // The active chain tail of the first body, or kInvalidObjectId. What a
    // dress feature is applied TO, and what a pocket cuts INTO.
    ObjectId currentTail() const;
    // The selected object if it is a Sketch, else kInvalidObjectId.
    ObjectId selectedSketch() const;
    std::set<ObjectId> hiddenIds() const;

    // THE DOCUMENT, of whichever type (M27).
    //
    // A DocumentBase, because the shell is about to hold two kinds and the
    // alternative -- a second window with its own menu, toolbar, tree,
    // property panel, undo and save -- is four of the five things P3 exists to
    // stop being written twice.
    //
    // Everything that is TRUE OF ANY DOCUMENT goes through this pointer:
    // transactions, undo, recompute, frames, names. Everything that is only
    // true of a PART goes through part() below, which says so.
    DocumentBase* document_;
    // Documents this window LOADED, and therefore owns. The one it was
    // constructed with belongs to whoever built it and is never freed here --
    // which is why this holds only what File > Open brought in.
    // OF WHICHEVER TYPE (M27). File > Open decides which by reading the
    // file's own documentType, so this cannot be the concrete part type.
    std::unique_ptr<DocumentBase> ownedDocument_;
    QString documentPath_;
    DocumentPresenter* presenter_;

    QStackedWidget* centralStack_ = nullptr;
    OcctViewWidget* viewer_ = nullptr;
    SketchCanvasWidget* sketchCanvas_ = nullptr;
    // What a dress command will act on, and what to say about it.
    struct DressSelection {
        EdgeSelection selection;
        std::string words;   // for the status line
        QString refusal;     // non-empty when the pick could not be expressed
    };
    DressSelection selectionForDress(ObjectId baseFeatureId) const;

    // The picked face, said as a QUERY -- which is what a Shell needs and what
    // a face sketch already does with the same pick (ADR-M17-036).
    struct PickedFaceQuery {
        FaceQuery query;
        std::string words;  // for the status line
        QString refusal;    // non-empty when nothing was picked
    };
    PickedFaceQuery selectionForFace() const;

    // Every sketch the tree has selected, IN DOCUMENT ORDER. A loft's order is
    // its shape, so the order has to be one the user can see and control --
    // Qt does not keep the order things were clicked in.
    std::vector<ObjectId> selectedSketches() const;
    // The body's solids that nothing has consumed: what a boolean needs two of.
    std::vector<ObjectId> unconsumedSolids() const;

    // A tree name no other sketch or feature already has.
    std::string uniqueObjectName(const std::string& base) const;

    std::vector<QAction*> sketchToolbarButtons() const;

    QToolBar* sketchToolBar_ = nullptr;
    // The second row of sketch tools. One row of 36 icon-only buttons overflows
    // any ordinary window width, and Qt hides the overflow behind a chevron --
    // so the tools at the end were not on screen at all.
    QToolBar* sketchToolBarSecond_ = nullptr;
    // The separator the two rows are split at -- a MEANING boundary (draw and
    // constrain / modify), not a position.
    QAction* sketchToolbarRowBreak_ = nullptr;
    // A REFUSAL stays on screen until the user does something else; an
    // ordinary status message does not.
    //
    // Both used to be cleared by the next repaint, and the canvas tracks the
    // mouse -- so the first pixel of movement after pressing a command button
    // wiped the reason it refused, and the command looked like it had silently
    // done nothing. That is precisely the failure roadmap 8 is written against,
    // produced by the code that was trying to avoid it.
    bool sketchMessageSticky_ = false;
    QAction* trimAction_ = nullptr;
    QAction* extendAction_ = nullptr;
    QToolBar* mainToolBar_ = nullptr;
    QToolBar* modelToolBar_ = nullptr;
    // The Assembly menu's actions, enabled only when the document is one.
    QAction* insertInstanceAction_ = nullptr;
    QAction* groundInstanceAction_ = nullptr;
    QAction* patternInstanceAction_ = nullptr;
    QAction* deleteInstanceAction_ = nullptr;
    QMenu* assemblyMenu_ = nullptr;

    QDockWidget* treeDock_ = nullptr;
    QDockWidget* constraintDock_ = nullptr;
    QPushButton* deleteConstraintButton_ = nullptr;
    QTableWidget* constraints_ = nullptr;
    // The sketch currently open for drawing, or kInvalidObjectId in 3D mode.
    ObjectId editingSketch_ = kInvalidObjectId;
    // The last thing a sketch command told the user. Sits where the PROMPT
    // goes, so the constraint-state badge is never displaced by it.
    QString sketchMessage_;
    QTreeWidget* tree_ = nullptr;
    QTableWidget* properties_ = nullptr;
    QLabel* statusLeft_ = nullptr;
    QLabel* statusRight_ = nullptr;

    ObjectId selectedId_ = kInvalidObjectId;
    bool updatingWidgets_ = false; // guards against selection feedback loops

    // The last REFUSED property edit, so the panel can put the user's text
    // back in the cell instead of replacing it with the stored value (M11.3).
    //
    // Discarding a rejected expression makes the user retype the whole thing to
    // fix one character, which is the opposite of what a positioned error
    // message is for. Cleared on a successful edit and on selection change.
    ObjectId rejectedEditParameter_ = kInvalidObjectId;
    PropertyField rejectedEditField_ = PropertyField::None;
    std::string rejectedEditText_;
    std::string rejectedEditDetail_;
};

} // namespace paramcad
