#pragma once

#include "Core/Document/DocumentBase.h"
#include "Core/Drawing/BomTable.h"
#include "Core/Drawing/Revision.h"
#include "Core/Drawing/Annotation.h"
#include "Core/Drawing/HoleTable.h"
#include "Core/Drawing/SheetEdits.h"
#include "Core/Electrical/SchematicObjects.h"
#include "Core/Drawing/DimensionStyle.h"
#include "Core/Drawing/DrawingDimension.h"
#include "Core/Drawing/DrawingEntity.h"
#include "Core/Drawing/SheetFrame.h"
#include "Core/Drawing/TitleBlock.h"
#include "Core/Drawing/DrawingTables.h"
#include "Core/Drawing/DrawingView.h"
#include "Core/Drawing/Hatch.h"
#include "Core/Drawing/Sheet.h"
#include "Core/Drawing/SheetPage.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paramcad {

// THE THIRD DOCUMENT TYPE (M32, roadmap §24).
//
// A drawing is a SHEET OF PAPER with views of other documents on it and
// annotation over the top. It is the first document whose contents have a
// physical size, and the first whose whole purpose is to LEAVE the program.
//
// WHY A THIRD TYPE RATHER THAN A MODE OF A PART:
//
// A drawing references documents, it does not contain them. Its geometry is
// two-dimensional and measured in paper millimetres, not model millimetres.
// Its objects are layers and views and annotations, none of which a part has.
// Every one of those would have become an "if this document is really a
// drawing" branch inside PartDocument, and the P3 lift M23 paid for exists
// precisely so a new type costs a subclass rather than a flag.
//
// WHAT IS ON A SHEET IS TWO DIFFERENT KINDS OF THING, and keeping them apart
// is the load-bearing decision of this whole block:
//
//   * DERIVED -- the curves a view projects out of a 3D model. No ObjectId,
//     not registered, not undoable, thrown away and rebuilt whenever the
//     model changes. A user cannot drag one, delete one or put it on another
//     layer, because it is not a thing they made.
//   * AUTHORED -- the lines, circles, text and dimensions a user draws on the
//     sheet. ObjectId, layer, linetype, undo, selection. These arrive in M33,
//     ported from EasyCad, whose entity model had already made these choices
//     and proved them against DXF.
//
// They are separate lists of separate types, not one list with a flag. A flag
// would turn "can this be deleted / dragged / renamed / saved" into four
// questions somebody has to remember to ask, and the day one of them is
// forgotten a projected edge becomes editable and the drawing stops matching
// the model.
class DrawingDocument final : public DocumentBase {
public:
    explicit DrawingDocument(std::string name);
    DrawingDocument(ObjectId id, std::string name);

    DocumentType type() const noexcept override { return DocumentType::Drawing; }

    // --- The paper -----------------------------------------------------------
    // THE CURRENT PAGE'S PAPER. Every reader that existed before M44 asks this
    // and keeps working, because a one-page drawing has exactly one answer.
    const Sheet& sheet() const noexcept { return currentPage().paper(); }

    // --- THE PAGES (M44) -----------------------------------------------------
    //
    // A drawing file is a SET of pages: the general arrangement, then the
    // details. Each has its own paper, frame and title block, because that is
    // what ISO 5457 and 7200 describe -- a page, not a document.
    //
    // Up to here the title block's Sheet row could only say 1 / 1, and that is
    // the kind of half-truth a drawing carries into a workshop: a reader who
    // sees 1 / 1 believes there is no second page.
    SheetPage& addSheetPage(std::string name);
    // THE LOADER'S RAW PATH, like every other restore here: no undo record and
    // no validation, because the loader has done both.
    //
    // A drawing is CONSTRUCTED with one page, so a file that carries its own
    // has to take that one away first. Doing it the other way -- constructing
    // with none -- would give every accessor a no-page case, and a case that
    // cannot happen is a case nobody maintains.
    void clearSheetPagesForRestore();
    void restoreCurrentSheet(ObjectId sheetId) noexcept { currentPageId_ = sheetId; }
    SheetPage& restoreSheetPage(ObjectId id, std::string name, Sheet paper,
                                FrameMargins margins, double zoneTargetMm,
                                bool frameVisible);
    std::vector<const SheetPage*> sheetPages() const;
    const SheetPage* findSheetPage(ObjectId id) const noexcept;
    ObjectId currentSheetId() const noexcept;
    bool setCurrentSheet(ObjectId sheetId);
    // REFUSED while anything is on it, and refused for the last page -- a
    // drawing with no paper is not a drawing, and silently taking the objects
    // with it throws away work nobody asked to lose (the rule M41's datums
    // settled).
    bool removeSheetPage(ObjectId sheetId);
    std::size_t objectsOnSheet(ObjectId sheetId) const;

    // "2 / 3", DERIVED from where the page sits in the file. A stored copy is
    // the first thing to go stale when a page is inserted, and it is stale in
    // the one place a reader trusts absolutely.
    std::string sheetNumberOf(ObjectId sheetId) const;
    // The same two numbers as digits, for the title block's Sheet row -- which
    // is where a reader learns there IS a second page.
    int currentSheetNumber() const noexcept;
    int sheetCount() const noexcept;

    // WHICH PAGE AN OBJECT IS ON, and moving it to another. ONE switchboard:
    // eight kinds of object sit on paper, and eight copies of this walk would
    // be eight chances for one of them to be forgotten.
    // Is an object with this sheetId on the page being looked at? The one
    // question the painter asks about every object it is handed.
    bool isOnCurrentSheet(ObjectId sheetId) const noexcept;
    ObjectId sheetOfObject(ObjectId objectId) const;
    bool setObjectSheet(ObjectId objectId, ObjectId sheetId);

    // WHY THIS DRAWING CANNOT BE WRITTEN, or empty when it can.
    //
    // Today it is one rule -- every object has to be on a page that exists --
    // and it is asked by the saver AND the loader, so what one refuses the
    // other refuses (ADR-M3-008). Ownership would have made the rule
    // unnecessary; a page holding its own objects cannot disagree with them.
    // This is the boundary check that buys most of the same safety without
    // rewriting forty methods that walk the document's lists.
    std::string whyDrawingRefused() const;
    // Through the facade, so every change is one undo step and the views that
    // sit on the paper are told it moved.
    bool setSheetSize(SheetSize size);
    bool setSheetOrientation(SheetOrientation orientation);
    bool setSheetScale(const DrawingScale& scale);
    bool setSheetCustomSize(double widthMm, double heightMm);
    // The load path: writes the sheet with NO undo record, because opening a
    // file is not something the user did (ADR-M9-001). `customWidthMm` and
    // `customHeightMm` are read only for a Custom sheet, and are PORTRAIT
    // millimetres as `setCustomSize` stores them.
    void restoreSheet(SheetSize size, SheetOrientation orientation, DrawingScale scale,
                      double customWidthMm, double customHeightMm, ProjectionAngle angle);

    // --- Layers (the DXF table model) ----------------------------------------
    //
    // Every drawing has layer "0" from the moment it is constructed, and it
    // can be neither renamed nor deleted. That is AutoCAD's rule and it is
    // kept because a DXF without layer 0 is a DXF other programs refuse.
    Layer& addLayer(std::string name, int color = 7,
                    std::string linetype = kContinuousLinetypeName);
    Layer& restoreLayer(ObjectId id, std::string name, int color, std::string linetype, bool on,
                        bool frozen, bool locked, int lineweight);
    std::vector<const Layer*> layers() const;
    const Layer* findLayer(ObjectId id) const noexcept;
    const Layer* findLayerNamed(const std::string& name) const noexcept;

    // WHICH LAYER NEW GEOMETRY LANDS ON. Document state, not presentation: a
    // drawing reopened on another machine must put new lines where the last
    // person left the current layer, or two people drawing the same detail
    // put it on two different layers.
    ObjectId currentLayerId() const noexcept { return currentLayerId_; }
    bool setCurrentLayer(ObjectId layerId);
    // The load path, for the same reason -- and it does NOT apply the frozen
    // or locked refusal: a file may legitimately have been left with a locked
    // layer current, and refusing it here would silently move the current
    // layer somewhere the user did not put it.
    void restoreCurrentLayer(ObjectId layerId);
    void restoreCurrentDimensionStyle(ObjectId styleId);
    void restoreGeneralToleranceClass(GeneralToleranceClass klass) noexcept {
        generalTolerance_ = klass;
    }

    bool setLayerColor(ObjectId layerId, int color);
    bool setLayerLinetype(ObjectId layerId, std::string linetype);
    bool setLayerOn(ObjectId layerId, bool on);
    bool setLayerFrozen(ObjectId layerId, bool frozen);
    bool setLayerLocked(ObjectId layerId, bool locked);
    bool setLayerLineweight(ObjectId layerId, int lineweight);

    // --- Linetypes -----------------------------------------------------------
    Linetype& addLinetype(std::string name, std::string description,
                          std::vector<double> pattern);
    Linetype& restoreLinetype(ObjectId id, std::string name, std::string description,
                              std::vector<double> pattern);
    std::vector<const Linetype*> linetypes() const;
    const Linetype* findLinetype(ObjectId id) const noexcept;
    const Linetype* findLinetypeNamed(const std::string& name) const noexcept;

    // --- Views ---------------------------------------------------------------
    //
    // THROWS on a refusal, matching addInstance: a view with no source file
    // can never build, so it is refused at the door rather than left as a
    // permanent failure in the tree whose cause is a blank field.
    DrawingView& addView(std::string name, std::string sourcePath, std::string bodyName,
                         ViewDirection direction, Vec2 positionMm);
    // A CHILD VIEW: the same model, seen from another side, LINED UP with its
    // parent (M32.3).
    //
    // It takes no position of its own. Where it sits is COMPOSED from its
    // parent's place plus an offset along the alignment axis -- exactly as an
    // instance's placement is composed from its frame (ADR-M10-002) -- so
    // moving the parent moves the children and nothing had to be told.
    //
    // Refused when the child is not square to the parent: an isometric beside
    // a front view is not aligned to anything, and inventing a side for it to
    // sit on would put it somewhere the user cannot predict.
    DrawingView& addProjectedView(std::string name, ObjectId parentViewId,
                                  ViewDirection direction, double offsetMm);

    DrawingView& restoreView(ObjectId id, std::string name, ComputeState state,
                             std::string sourcePath, std::string bodyName,
                             ViewDirection direction, Vec2 positionMm, DrawingScale scale,
                             bool ownScale, bool showHidden, bool showTangent,
                             ObjectId parentViewId, double alignmentOffsetMm);

    // --- Section views (M38) --------------------------------------------------
    //
    // A section is an ordinary projected view of a solid with a half-space
    // taken out of it. The cut line is a SENTENCE on the parent -- two points
    // in the parent's model millimetres and which way the arrows point -- so
    // turning the parent turns the cut with it.
    DrawingView& addSectionView(std::string name, ObjectId parentViewId, Vec2 fromMm,
                                Vec2 toMm, int arrowSide, double offsetMm);
    bool setSectionCut(ObjectId viewId, Vec2 fromMm, Vec2 toMm, int arrowSide);

    // --- Broken views (M50) ---------------------------------------------------
    //
    // A long part on a short sheet. NOT a kind of view and NOT a cut: the span
    // is a mapping from model millimetres onto paper, applied in
    // viewPointToSheetMm and undone in sheetPointToViewMm -- so a dimension
    // across the break reads the true length by construction rather than by a
    // rule somebody has to remember. See BreakFold.h.
    bool setBreakSpan(ObjectId viewId, double fromMm, double toMm, bool horizontal,
                      double gapMm);
    bool clearBreakSpan(ObjectId viewId);
    // Why this break cannot be drawn on this view, or empty when it can --
    // asked against the view's own extent, so a break past the end of the part
    // is refused rather than drawn across empty paper.
    std::string whyBreakRefused(ObjectId viewId, const BreakSpan& span) const;
    // HOW BIG A GAP TO LEAVE, in the MODEL millimetres BreakSpan is measured
    // in, for this view's scale.
    //
    // The gap is a drafting artefact and not a feature of the part, so what
    // matters is how wide it is ON THE PAPER -- about six millimetres, near
    // enough to see and not so wide the two halves stop reading as one part.
    // Left as a model-space number it is 1.5 mm of paper at 1:2 and 0.6 at
    // 1:10, which is a break nobody can see, on exactly the long parts breaks
    // are for.
    //
    // Derived here, where the scale is known, rather than folded into
    // BreakFold -- which is arithmetic on the span alone and has no business
    // knowing what a sheet is.
    double suggestedBreakGapMm(ObjectId viewId) const noexcept;
    // WHAT A VIEW ACTUALLY DRAWS: its curves, cut at the break's lips.
    //
    // ONE PATH, whether the view is broken or not -- a painter that asked
    // "is it broken?" and took a different route would be the second place
    // this decision lives, and the day the two disagree the drawing shows
    // material the break removed.
    std::vector<ProjectedCurve> drawableCurves(ObjectId viewId) const;

    // --- Detail views (M49) ---------------------------------------------------
    //
    // A detail is the parent's own projection, cropped to a circle and drawn
    // bigger. The circle is a SENTENCE on the parent -- a centre and a radius
    // in the parent's model millimetres -- so turning or editing the parent
    // carries the detail with it (see DrawingView::DetailFrame).
    //
    // The SCALE is asked for, because it is the whole point of the view.
    DrawingView& addDetailView(std::string name, ObjectId parentViewId, Vec2 centreMm,
                               double radiusMm, DrawingScale scale, double offsetMm);
    bool setDetailFrame(ObjectId viewId, Vec2 centreMm, double radiusMm);

    // WHICH LETTER THIS VIEW CARRIES: "A" for the first, "B" for the second.
    //
    // DERIVED from the order they were made, and empty for a view that is
    // neither a section nor a detail. The mark on the parent and the title
    // under the view have to carry the SAME letter, and that is the classic
    // "two things that must agree" trap -- so neither is typed and both ask
    // here.
    //
    // ONE SEQUENCE FOR BOTH KINDS (M49). Sections and details drawing from
    // separate pools would put "SECTION A-A" and "DETAIL A" on one sheet: two
    // things called A, a line and a circle on the parent both marked A, and a
    // reader who looks up A finding whichever they see first. Nothing about
    // either view looks wrong.
    std::string viewLetterOf(ObjectId viewId) const;
    // The same sequence, asked about a section -- which is what most callers
    // want to know, and empty for anything else.
    std::string sectionLetterOf(ObjectId viewId) const;

    // WHAT IS WRITTEN UNDER A VIEW: "A-A" for a section, its name otherwise,
    // with its scale after it when it does not follow the sheet's.
    //
    // Here rather than in the painter because the painter is not a place a
    // test can look. The screen and the plot share one renderer (M35), but
    // that renderer is still Qt, and the only way to check the caption was to
    // read it off a screenshot. A section titled by its NAME instead of its
    // letter -- a real mutation that survived -- draws a caption that looks
    // entirely reasonable and does not match the line on the parent.
    std::string viewLabelText(ObjectId viewId) const;

    // THE CUT FACE TO FILL, IN SHEET MILLIMETRES, and the pattern to fill it
    // with. Empty loops for a view that is not a section or has no cut face.
    //
    // A hatch is ANNOTATION: its pitch is a paper measurement, so a 1:10
    // section is not filled solid. That means the region has to be converted
    // out of model millimetres before it is hatched, and converting it in the
    // painter put the one line that decides this where nothing could test it.
    HatchRegion sectionHatchRegionMm(ObjectId viewId) const;
    // The angle and offset alternate with the section's letter, so two
    // sections on one sheet are told apart -- and derived from the letter, so
    // they survive a reopen.
    HatchStyle sectionHatchStyle(ObjectId viewId) const;

    // WHERE A VIEW ACTUALLY SITS, base or child. The one reader, so a
    // renderer, a plot and the "does it fit" check cannot disagree.
    Vec2 viewPositionMm(ObjectId viewId) const;

    // Why this view cannot be projected off that one, or empty when it can.
    std::string whyViewCannotBeProjectedFrom(ObjectId parentViewId,
                                             ViewDirection direction) const;

    // WHICH VIEWS ARE BEHIND THEIR MODELS. A drawing does not watch the disk;
    // it answers when asked, and the shell turns that into "3 views are out of
    // date -- update?" rather than silently rebuilding everything.
    std::vector<ObjectId> staleViews() const;
    std::vector<const DrawingView*> views() const;
    const DrawingView* findView(ObjectId id) const noexcept;
    // The view itself, for a test that has to put a projection into it. There
    // is no other way to place two candidate points exactly where an anchor's
    // rules have to decide between them (see DrawingView::setProjectionForTesting).
    DrawingView* viewForTesting(ObjectId id) noexcept { return findViewForEdit(id); }
    const DrawingView* findViewNamed(const std::string& name) const noexcept;

    bool setViewPosition(ObjectId viewId, Vec2 positionMm);
    bool setViewDirection(ObjectId viewId, ViewDirection direction);
    bool setViewScale(ObjectId viewId, const DrawingScale& scale);
    bool clearViewScale(ObjectId viewId);
    bool setSheetProjectionAngle(ProjectionAngle angle);
    bool setViewAlignmentOffsetMm(ObjectId viewId, double offsetMm);
    bool setViewShowsHiddenLines(ObjectId viewId, bool show);
    bool setViewShowsTangentEdges(ObjectId viewId, bool show);

    // Why this view cannot be placed there, or empty when it can. The sheet
    // is a finite piece of paper and a view outside it is a view nobody
    // prints -- said here rather than discovered at plot time.
    std::string whyViewCannotSitAt(Vec2 positionMm) const;

    // --- Authored geometry (M33) ---------------------------------------------
    //
    // What a user DRAWS on the sheet, as against what a view projects. The
    // distinction this document was built around in M32.1, now with the other
    // half in it.
    //
    // NEW GEOMETRY LANDS ON THE CURRENT LAYER, always. A caller that named a
    // layer per entity would be a caller that could put something on a frozen
    // one, and the user would have drawn a line that appears not to have been
    // drawn.
    DrawingEntity& addEntity(DrawShape shape);
    DrawingEntity& restoreEntity(ObjectId id, DrawShape shape, ObjectId layerId, int color,
                                 std::string linetype, int lineweight);
    std::vector<const DrawingEntity*> entities() const;
    const DrawingEntity* findEntity(ObjectId id) const noexcept;

    // MOVE, COPY, ROTATE, SCALE, MIRROR -- all one operation, because they
    // are. EasyCad had five commands over one `ApplyTransform`, and so does
    // this: what differs is the matrix the shell builds, not what happens to
    // the geometry.
    // TRIM, EXTEND, FILLET, CHAMFER, OFFSET, ARRAY -- all one operation here,
    // for the same reason move and copy are (M40).
    //
    // What each of them produces is: some entities go, some shapes arrive.
    // Written as six document methods they would be six copies of the same
    // remove-and-add, each with its own idea of which layer the new geometry
    // lands on and whether the whole thing is one undo step -- and the way
    // anybody would find out they disagreed is an undo that half-worked.
    //
    // ONE UNDO STEP. A trim that could be half-undone would leave a drawing
    // with the old line back and the new pieces still there, sitting exactly
    // on top of each other.
    //
    // Returns the ids of what was made; empty on refusal, with `why` set.
    std::vector<ObjectId> applySheetEdit(const std::vector<ObjectId>& consumed,
                                         const SheetEditResult& made, const std::string& label,
                                         std::string* why = nullptr);

    bool transformEntities(const std::vector<ObjectId>& ids, const Matrix2D& transform);
    // ...and the copying half, which is the same transform applied to clones.
    std::vector<ObjectId> copyEntities(const std::vector<ObjectId>& ids,
                                       const Matrix2D& transform);

    bool setEntityLayer(ObjectId entityId, ObjectId layerId);
    bool setEntityColor(ObjectId entityId, int color);
    bool setEntityLinetype(ObjectId entityId, std::string linetype);
    bool setEntityLineweight(ObjectId entityId, int lineweight);

    // WHAT IS UNDER THE CURSOR, nearest first, within `apertureMm`. Entities
    // on invisible or locked layers are NOT offered: a lock that still let
    // things be picked would be a lock in name only.
    std::vector<ObjectId> entitiesNear(Vec2 point, double apertureMm) const;
    // Window (fully inside) or crossing (touching) -- two different rules,
    // and a package with only one of them surprises everybody who has met the
    // other.
    std::vector<ObjectId> entitiesInWindow(const Box2D& window, bool crossing) const;

    // The layer an entity is drawn with, resolved through ByLayer. ONE reader,
    // so the canvas, a plot and a DXF write cannot disagree about what colour
    // something is.
    int resolvedColorOf(const DrawingEntity& entity) const;
    std::string resolvedLinetypeOf(const DrawingEntity& entity) const;
    int resolvedLineweightOf(const DrawingEntity& entity) const;
    bool isEntityVisible(const DrawingEntity& entity) const;
    // A DIMENSION IS ON A LAYER TOO, and it obeys the same two rules -- so it
    // asks the same two functions rather than a copy of them. A dimension
    // carries no colour of its own yet, which is why it always resolves
    // ByLayer.
    int resolvedColorOfDimension(const DrawingDimension& dimension) const;
    // A component's colour is its layer's, resolved by the SAME rule an entity
    // and a dimension use -- a third copy of "ByLayer means ask the layer" is
    // exactly what resolvedColorOnLayer exists to prevent.
    int resolvedColorOnLayerForTesting(ObjectId layerId) const {
        return resolvedColorOnLayer(kColorByLayer, layerId);
    }
    bool isDimensionVisible(const DrawingDimension& dimension) const;

    // --- Schematic (M36) ------------------------------------------------------
    //
    // A SCHEMATIC IS A DRAWING. Same paper, frame, title block, layers and plot
    // path; what makes it electrical is symbols with PINS and wires that
    // connect them (see SymbolLibrary.h for why this is not a fourth document
    // type).
    SymbolPlacement& addSymbol(std::string tag, std::string symbolName, Vec2 positionMm);
    SymbolPlacement& restoreSymbol(ObjectId id, std::string tag, std::string symbolName,
                                   Vec2 positionMm, double rotationRad, bool mirrored,
                                   ObjectId layerId);
    std::vector<const SymbolPlacement*> symbols() const;
    const SymbolPlacement* findSymbol(ObjectId id) const noexcept;
    const SymbolPlacement* findSymbolTagged(const std::string& tag) const noexcept;
    bool setSymbolPosition(ObjectId symbolId, Vec2 at);
    bool setSymbolRotation(ObjectId symbolId, double radians);
    bool setSymbolMirrored(ObjectId symbolId, bool mirrored);

    WireEntity& addWire(std::vector<Vec2> pointsMm);
    WireEntity& restoreWire(ObjectId id, std::vector<Vec2> pointsMm, ObjectId layerId,
                            std::string label);
    std::vector<const WireEntity*> wires() const;
    const WireEntity* findWire(ObjectId id) const noexcept;
    bool setWirePoints(ObjectId wireId, std::vector<Vec2> pointsMm);
    bool setWireLabel(ObjectId wireId, std::string label);

    // WHAT IS CONNECTED TO WHAT, right now. DERIVED, every time -- a stored
    // netlist is a schematic stating a circuit the drawing no longer shows,
    // and what gets built is the netlist.
    //
    // A net's NAME comes off the wires that make it up (see WireEntity). Two
    // wires in one net carrying different labels is a contradiction, and it is
    // REPORTED rather than resolved: picking one would rename a wire somebody
    // has already crimped a ferrule for, and picking silently would hide that
    // the schematic says two things.
    Netlist netlist() const;
    // Nets whose wires disagree about the name, as the names involved.
    std::vector<std::string> conflictingNetNames() const;
    // Assigns W1, W2, ... to every UNLABELLED net, writing the name onto its
    // wires. One undo step for the whole sheet, because it is one thing the
    // user did.
    std::size_t numberNets(const std::string& prefix = "W");

    // --- The parts list (M35.6) ----------------------------------------------
    //
    // A BOM IS A VIEW OF AN ASSEMBLY, in the way a projected view is a view of
    // a body: it names a file (ADR-M22-003) and its ROWS ARE COUNTED ON
    // DEMAND. A list that kept its own quantities is a drawing stating a bill
    // of materials the assembly no longer has -- and the wrong number is the
    // one that gets ordered.
    BomTable& addBomTable(std::string name, std::string sourcePath, Vec2 positionMm);
    BomTable& restoreBomTable(ObjectId id, std::string name, std::string sourcePath,
                              Vec2 positionMm, BomDepth depth, std::vector<BomColumn> columns,
                              double rowHeightMm, bool growsUpward, long long sourceStamp);
    std::vector<const BomTable*> bomTables() const;
    const BomTable* findBomTable(ObjectId id) const noexcept;
    bool setBomPosition(ObjectId tableId, Vec2 at);
    bool setBomDepth(ObjectId tableId, BomDepth depth);
    bool setBomColumns(ObjectId tableId, std::vector<BomColumn> columns);
    bool setBomRowHeightMm(ObjectId tableId, double rowHeightMm);
    bool setBomGrowsUpward(ObjectId tableId, bool upward);

    // WHAT THE LIST SAYS RIGHT NOW. Read from the file every time it is asked
    // for -- THE one counter, so the canvas, a plot and a DXF write cannot
    // disagree about how many bolts there are.
    BomContents countBom(const BomTable& table) const;

    // --- THE REVISION HISTORY (M48) ------------------------------------------
    //
    // The drawing's own list, and the one place in this document where a
    // letter is STORED rather than derived -- Revision.h says why at length.
    // In one line: a balloon's number points at a row that exists now, and a
    // revision letter is a fact other people's paperwork already cites.
    const std::vector<Revision>& revisions() const noexcept { return revisions_; }
    // The letter to offer next, derived from the last one. This is the half
    // that MUST be derived: a hand-typed next letter is how a drawing gets two
    // Rev Cs, or an I.
    std::string nextRevisionLetter() const;
    // WHAT THE DRAWING IS AT. Empty when it has never been issued -- which is
    // not the same as Rev A, and printing A on an unissued drawing is a claim
    // nobody made.
    std::string currentRevision() const;
    std::string whyRevisionRefused(const Revision& revision) const;
    // Refuses per whyRevisionRefused, and records one undo step.
    bool addRevision(Revision revision);
    bool removeRevision(const std::string& letter);
    // For the loader and for undo: puts the history back exactly, including
    // where in it a row sat.
    void restoreRevision(Revision revision, std::size_t at);

    // THE TABLE THAT SHOWS IT. Holds no rows: they are the history above,
    // asked for at every repaint, so a table cannot show an issue this drawing
    // does not have and cannot miss one it does.
    RevisionTable& addRevisionTable(std::string name, Vec2 positionMm);
    RevisionTable& restoreRevisionTable(ObjectId id, std::string name, Vec2 positionMm,
                                        double widthMm, double rowHeightMm);
    std::vector<const RevisionTable*> revisionTables() const;
    const RevisionTable* findRevisionTable(ObjectId id) const noexcept;
    bool setRevisionTablePosition(ObjectId tableId, Vec2 at);

    // WHAT A TITLE BLOCK FIELD PRINTS, on this drawing. THE one caller of
    // TitleBlock::valueOf in a running program, because it is the only thing
    // that knows all three derived facts at once: which page this is, how many
    // there are, and what the drawing is issued at. A painter that asked the
    // block directly would have to supply them itself, and the one it would
    // get wrong is the revision.
    std::string titleBlockValue(const TitleBlockField& field) const;

    // --- THE HOLE TABLE (M39.4) ---------------------------------------------
    //
    // Every hole in the part a view is of, tagged and measured from a datum
    // the drawing states. Added against a VIEW rather than a file, so the
    // table and the tags drawn in the view cannot describe different parts.
    HoleTable& addHoleTable(std::string name, ObjectId viewId, Vec2 positionMm, Vec2 datumMm);
    HoleTable& restoreHoleTable(ObjectId id, std::string name, ObjectId viewId, Vec2 positionMm,
                                Vec2 datumMm, std::vector<HoleColumn> columns,
                                double rowHeightMm);
    std::vector<const HoleTable*> holeTables() const;
    const HoleTable* findHoleTable(ObjectId id) const noexcept;
    bool setHoleTablePosition(ObjectId id, Vec2 positionMm);
    bool setHoleTableDatum(ObjectId id, Vec2 datumMm);
    bool removeHoleTable(ObjectId id);

    // THE ROWS, COUNTED NOW. Never stored, for the reason a parts list's
    // quantities are not: a table holding its own copy is a drawing stating
    // hole positions the part no longer has.
    HoleTableContents holesOf(const HoleTable& table) const;

    // --- THE SYMBOLS (M41) ---------------------------------------------------
    //
    // Surface finish, feature control frame and datum, added the same way and
    // through the same door, because they are one object with three bodies.
    Annotation& addAnnotation(AnnotationBody body, DimensionAnchor anchor, Vec2 positionMm);
    Annotation& restoreAnnotation(ObjectId id, AnnotationBody body, DimensionAnchor anchor,
                                  Vec2 positionMm, ObjectId layerId);
    std::vector<const Annotation*> annotations() const;
    const Annotation* findAnnotation(ObjectId id) const noexcept;
    bool setAnnotationPosition(ObjectId id, Vec2 positionMm);
    bool setAnnotationBody(ObjectId id, AnnotationBody body);

    // WHICH LETTER THIS DATUM IS: "A" for the first placed, "B" for the second.
    //
    // DERIVED from the order they were placed, and empty for an annotation that
    // is not a datum. The symbol on the face and every frame that refers to it
    // must carry the SAME letter, which is the trap M38's section letters were
    // built to avoid -- so neither is typed and both ask here.
    std::string datumLetterOf(ObjectId annotationId) const;

    // WHAT THE SYMBOL SAYS, with its datums' current letters resolved. Empty
    // when the annotation cannot be drawn -- ask whyAnnotationRefused.
    std::string annotationText(ObjectId annotationId) const;
    // Why it cannot be drawn, or empty when it can. A frame naming a datum
    // that has been deleted lands here.
    std::string whyAnnotationRefused(ObjectId annotationId) const;

    // HOW MANY FRAMES STILL REFER TO THIS DATUM.
    //
    // Deleting a datum that frames still name is REFUSED, and this is what the
    // refusal counts. The alternatives are worse: cascading the delete throws
    // away drafting work the user did not ask to lose, and letting the frames
    // dangle leaves a document that cannot be saved -- a drawing a user cannot
    // get out of, because of a delete nobody warned them about.
    std::size_t framesReferringToDatum(ObjectId datumId) const;

    // WHERE THE LEADER LANDS, in sheet millimetres, or nothing when the symbol
    // has lost what it pointed at.
    //
    // A DANGLING LEADER IS NOT THE SAME AS AN UNDRAWABLE BODY, and the two are
    // deliberately separate: the specification is still perfectly good, it is
    // the attachment that has gone. So this does NOT stop a save -- a
    // dimension that dangles does not either -- but the reader has to be told,
    // because a finish symbol that quietly stays put beside a face that has
    // moved says the wrong surface has to be ground.
    std::optional<Vec2> annotationLeaderTipMm(ObjectId annotationId) const;
    // Which lists are counting a file that has changed since. The same
    // question a view answers, through the same content hash (M32.4).
    std::vector<ObjectId> staleBomTables() const;
    // Re-reads the stamp, so a counted list stops being stale. Not undoable:
    // it changes nothing a user did.
    bool markBomCounted(ObjectId tableId);

    // --- The frame and the title block (M35) ---------------------------------
    //
    // BOTH ARE DERIVED FROM THE SHEET WHERE THEY CAN BE. The frame is two
    // rectangles and a ring of zone labels, all of them a function of the
    // paper; the title block's scale, size and projection rows are read from
    // the sheet and cannot be typed. What is stored is only what a user
    // actually decides: the margins, and the text of the free fields.
    //
    // If either were entities on the paper, resizing the sheet would leave the
    // old border and the old size printed in the corner -- and it would look
    // completely plausible.
    const TitleBlock& titleBlock() const noexcept { return titleBlock_; }
    const FrameMargins& frameMargins() const noexcept {
        return currentPage().frameMargins();
    }
    double frameZoneTargetMm() const noexcept { return currentPage().frameZoneTargetMm(); }
    bool isFrameVisible() const noexcept { return currentPage().isFrameVisible(); }

    // WHERE THE FRAME IS, right now, on this paper. Asked for, never stored.
    SheetFrameGeometry frame() const noexcept;
    // The title block's bottom-left corner: hard against the frame's
    // bottom-right, which is where ISO 7200 puts it and where a reader's eye
    // goes first. Derived, so a resize moves it.
    Vec2 titleBlockOriginMm() const noexcept;

    bool setTitleBlockField(const std::string& label, std::string value);
    bool addTitleBlockField(std::string label, TitleBlockSource source);
    bool removeTitleBlockField(const std::string& label);
    bool setTitleBlockSize(double widthMm, double rowHeightMm);
    bool setTitleBlockVisible(bool visible);
    bool setFrameMargins(const FrameMargins& margins);
    bool setFrameZoneTargetMm(double zoneTargetMm);
    bool setFrameVisible(bool visible);
    // The loader's raw path, like every other restore here: no undo record,
    // no validation, because the loader has already done both.
    void restoreTitleBlock(TitleBlock block);
    void restoreFrame(const FrameMargins& margins, double zoneTargetMm, bool visible);

    // --- Dimension styles (M34) ----------------------------------------------
    //
    // Every drawing has ISO-25 from the moment it is constructed and it cannot
    // be deleted -- the same rule layer "0" follows, and for the same reason: a
    // dimension has to have a style.
    DimensionStyle& addDimensionStyle(std::string name);
    DimensionStyle& restoreDimensionStyle(ObjectId id, std::string name);
    std::vector<const DimensionStyle*> dimensionStyles() const;
    const DimensionStyle* findDimensionStyle(ObjectId id) const noexcept;
    const DimensionStyle* findDimensionStyleNamed(const std::string& name) const noexcept;
    // Editing a style through the facade, so every change is one undo step and
    // the drawing knows to repaint.
    bool editDimensionStyle(ObjectId styleId, const DimensionStyle& to);
    ObjectId currentDimensionStyleId() const noexcept { return currentStyleId_; }
    bool setCurrentDimensionStyle(ObjectId styleId);

    // --- Dimensions ------------------------------------------------------------
    DrawingDimension& addDimension(DimensionKind kind, DimensionAnchor first,
                                   DimensionAnchor second, Vec2 linePositionMm);
    DrawingDimension& restoreDimension(ObjectId id, DimensionKind kind, DimensionAnchor first,
                                       DimensionAnchor second, LinearDirection direction,
                                       Vec2 linePositionMm, ObjectId styleId, ObjectId layerId,
                                       std::string textOverride);
    std::vector<const DrawingDimension*> dimensions() const;
    const DrawingDimension* findDimension(ObjectId id) const noexcept;
    bool setDimensionDirection(ObjectId dimensionId, LinearDirection direction);
    bool setDimensionLinePosition(ObjectId dimensionId, Vec2 at);
    bool setDimensionTextOverride(ObjectId dimensionId, std::string text);
    bool setDimensionStyleOf(ObjectId dimensionId, ObjectId styleId);

    // --- Tolerances (M37) ----------------------------------------------------
    bool setDimensionTolerance(ObjectId dimensionId, DimensionTolerance tolerance);
    // WHAT UNMARKED SIZES MEAN. A property of the SHEET, like the projection
    // angle -- see GeneralToleranceEdit.
    GeneralToleranceClass generalToleranceClass() const noexcept { return generalTolerance_; }
    bool setGeneralToleranceClass(GeneralToleranceClass klass);
    // The note the drawing prints beside its title block, or empty when no
    // class is stated -- and empty is a real answer, not a missing one.
    std::string generalToleranceNote() const;

    // THE TOLERANCE PART OF WHAT A DIMENSION READS, alone -- so the canvas can
    // set it in smaller type without a second opinion about what it says.
    //
    // dimensionText() is BUILT FROM THIS, which is what stops the two
    // disagreeing: there is one rule and one place it lives.
    std::string dimensionToleranceText(const DrawingDimension& dimension) const;
    // Whether the dimension text is drawn in a box (a Basic dimension).
    bool dimensionIsBasic(const DrawingDimension& dimension) const noexcept;
    // Nothing when the fit is one this build cannot compute -- and the
    // dimension then says so on the paper rather than printing a size with no
    // tolerance where a fit was asked for.
    std::optional<Deviations> dimensionFit(const DrawingDimension& dimension) const;

    // WHAT A DIMENSION CURRENTLY READS -- resolved, never stored.
    //
    // THE one place an anchor becomes a coordinate. A canvas, a plot, a DXF
    // write and a "is anything dangling" check all ask here, so none of them
    // can disagree about what the drawing says.
    DimensionMeasurement measure(const DrawingDimension& dimension) const;
    // The text it shows: the override if there is one, otherwise the
    // measurement through its style.
    std::string dimensionText(const DrawingDimension& dimension) const;

    // A POINT INSIDE A VIEW, PUT ON THE PAPER (M35).
    //
    // THE one multiplication. Projected curves are in MODEL millimetres
    // (ProjectedGeometry.h) and everything that draws or exports them needs
    // them in SHEET millimetres, which is the view's position plus the model
    // point times the effective scale.
    //
    // It was written out three times before this existed -- in the canvas, in
    // resolveAnchor, and in the DXF writer -- which is the shape of defect
    // this project keeps finding: several places doing the same arithmetic,
    // each correct on its own, and nothing that makes them agree. It is also
    // why a dimension can read true size: the multiplication happens here and
    // `measure` divides this exact factor back out.
    //
    // An unknown view gives the point back unchanged, because a caller with no
    // view has a sheet point already.
    Vec2 viewPointToSheetMm(ObjectId viewId, Vec2 modelMm) const noexcept;
    // THE OTHER HALF OF THE SAME MAPPING (M50). Sheet millimetres back to the
    // view's own model millimetres: the position undone, the scale undone, and
    // -- the part that matters -- the break UNFOLDED.
    //
    // Everything that MEASURES comes through here. Without it, measuring off
    // the paper would give the folded length: a 600 mm bar broken in the
    // middle would dimension as 203, and every other number on the drawing
    // would agree with it.
    Vec2 sheetPointToViewMm(ObjectId viewId, Vec2 sheetMm) const noexcept;
    double viewScaleFactor(ObjectId viewId) const noexcept;

    // WHAT A DIMENSION ON THIS SELECTION WOULD MEASURE (M34).
    //
    // ONE rule about which snap points a pick turns into anchors, so the menu,
    // the toolbar and a script all put the same dimension on the same pick --
    // rather than three hand-copied readings of "a circle means its centre and
    // its rim", which is precisely the shape of defect this project keeps
    // finding.
    //
    // It refuses rather than guesses: a diameter asked for on a line has no
    // sensible answer, and inventing one would put a number on the drawing
    // that measures something the user did not point at.
    struct DimensionProposal {
        bool ok = false;
        std::string why;
        DimensionAnchor first;
        DimensionAnchor second;
        Vec2 linePositionMm{};
    };
    DimensionProposal proposeDimension(DimensionKind kind,
                                       const std::vector<ObjectId>& entityIds) const;

    // WHICH DIMENSIONS HAVE LOST WHAT THEY MEASURED. Answered when asked, like
    // staleViews -- and it has to be visible, because a dangling dimension is
    // the one failure a drawing must never hide.
    std::vector<ObjectId> danglingDimensions() const;

    // --- DocumentBase --------------------------------------------------------
    DocumentRecomputeReport recompute() override;
    bool removeOwnObject(ObjectId id) override;

protected:
    void forEachOwnNamed(const std::function<void(const NamedSlot&)>& visit) override;
    void applyOwnDelta(const UndoDelta& delta, bool forward) override;

private:
    DrawingView* findViewForEdit(ObjectId id) noexcept;
    DrawingEntity* findEntityForEdit(ObjectId id) noexcept;
    DrawingDimension* findDimensionForEdit(ObjectId id) noexcept;
    // EVERY CONTAINER WHOSE OBJECTS SIT ON A PAGE, WRITTEN DOWN ONCE (M48).
    //
    // Four functions used to enumerate these by hand -- how many objects are
    // on a page, which page an object is on, moving one between pages, and the
    // rule the saver and loader share about pages that are not there. Six
    // containers, four copies, kept in step by whoever remembered.
    //
    // M48 needed a seventh (the revision table) and that is what makes it
    // worth fixing rather than continuing: forget one copy and the table is on
    // a page for the purpose of being drawn and on no page for the purpose of
    // being counted, so deleting that page is allowed and takes the table with
    // it. Nothing throws. The drawing simply comes back without its history.
    //
    // Static, and taking `self`, so the const and non-const callers share ONE
    // list rather than the usual pair of overloads -- which would be the same
    // defect one level down.
    template <class Self, class Fn>
    static void eachPagedList(Self& self, Fn&& fn) {
        fn(self.views_, "a view");
        fn(self.entities_, "a line");
        fn(self.dimensions_, "a dimension");
        fn(self.annotations_, "a symbol");
        fn(self.bomTables_, "a parts list");
        fn(self.holeTables_, "a hole table");
        fn(self.revisionTables_, "a revision table");
    }

    RevisionTable* findRevisionTableForEdit(ObjectId id) noexcept;
    BomTable* findBomTableForEdit(ObjectId id) noexcept;
    HoleTable* findHoleTableForEdit(ObjectId id) noexcept;
    Annotation* findAnnotationForEdit(ObjectId id) noexcept;
    SymbolPlacement* findSymbolForEdit(ObjectId id) noexcept;
    WireEntity* findWireForEdit(ObjectId id) noexcept;
    DimensionStyle* findDimensionStyleForEdit(ObjectId id) noexcept;
    // Where an anchor actually is, in SHEET millimetres, or nothing when it
    // has lost what it pointed at.
    std::optional<Vec2> resolveAnchor(const DimensionAnchor& anchor) const;
    // What ByLayer means, in ONE place. Both an entity and a dimension route
    // through here, so the canvas cannot end up drawing the two by different
    // rules.
    int resolvedColorOnLayer(int ownColor, ObjectId layerId) const;
    bool layerIsVisible(ObjectId layerId) const;
    Layer* findLayerForEdit(ObjectId id) noexcept;
    // The seeded table every drawing starts with: layer 0 and CONTINUOUS.
    // Written through the restore path so a new document carries no undo
    // history (ADR-M9-001) -- "Undo" on a fresh drawing must not delete the
    // layer everything is on.
    void seedTables();

    // THE PAGES. Never empty: a drawing is constructed with one, and the last
    // one cannot be deleted.
    std::vector<std::unique_ptr<SheetPage>> pages_;
    ObjectId currentPageId_ = kInvalidObjectId;

    // The current page, as the five old members were. Const and mutable, so a
    // reader stays const and only the editors ask for the other one.
    const SheetPage& currentPage() const noexcept;
    SheetPage& currentPageForEdit() noexcept;
    Sheet& paperForEdit() noexcept { return currentPageForEdit().paperForEdit(); }
    TitleBlock& blockForEdit() noexcept { return titleBlock_; }
    TitleBlock titleBlock_;
    std::vector<std::unique_ptr<Layer>> layers_;
    std::vector<std::unique_ptr<Linetype>> linetypes_;
    std::vector<std::unique_ptr<DrawingView>> views_;
    std::vector<std::unique_ptr<DrawingEntity>> entities_;
    std::vector<std::unique_ptr<DimensionStyle>> dimensionStyles_;
    std::vector<std::unique_ptr<DrawingDimension>> dimensions_;
    ObjectId currentStyleId_{kInvalidObjectId};
    std::vector<std::unique_ptr<BomTable>> bomTables_;
    std::vector<std::unique_ptr<HoleTable>> holeTables_;
    std::vector<std::unique_ptr<RevisionTable>> revisionTables_;
    // THE HISTORY ITSELF. Not objects with ids: a revision is a row of text,
    // not something on the paper that can be picked, moved or put on a layer.
    // Giving it an id would invite a second thing to point at it, and the
    // pointer would then be the copy that goes stale.
    std::vector<Revision> revisions_;
    std::vector<std::unique_ptr<Annotation>> annotations_;
    std::vector<std::unique_ptr<SymbolPlacement>> symbols_;
    std::vector<std::unique_ptr<WireEntity>> wires_;

    GeneralToleranceClass generalTolerance_ = GeneralToleranceClass::None;



    ObjectId currentLayerId_{kInvalidObjectId};
};

} // namespace paramcad
