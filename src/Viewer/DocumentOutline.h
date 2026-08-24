#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Sketch/ISketchSolver.h"
#include "Core/Sketch/SketchTypes.h"
#include <set>
#include <string>
#include <vector>

namespace paramcad {

class PartDocument;

// The UI state vocabulary (UI spec 11), defined ONCE so the Model Tree, the
// Property Panel and the status bar cannot drift into describing the same
// object differently.
//
// Hover/Preselected and Disabled are deliberately absent: they are transient
// widget states owned by the widget, not properties of a document object.
// Mixing them in here is how a semantic model starts carrying presentation
// state.
enum class OutlineState {
    Normal,     // no computed state of its own (a container or a grouping row)
    Valid,      // computed successfully and current
    Dirty,      // needs recompute
    Failed,     // its own recompute ran and reported failure
    Blocked,    // a prerequisite failed, so this never ran
    Suppressed, // deliberately excluded from recompute
    Hidden      // computed normally, deliberately not drawn (VIEW state)
};

// What kind of document object a row represents. The tree needs this for
// icons and grouping; it is NOT a type discriminator for behaviour -- code that
// needs behaviour asks for a capability (ADR-M3-007).
enum class OutlineKind {
    Document, Parameter, Sketch, Constraint, Solid, MassProperties, Material,
    Frame, Connector, Other
};

struct OutlineNode {
    ObjectId id{kInvalidObjectId};
    std::string name;
    std::string typeLabel;   // human-facing: "Sketch", "Pad", "Box", ...
    OutlineKind kind{OutlineKind::Other};
    OutlineState state{OutlineState::Normal};
    std::string diagnostic;  // non-empty only when something is wrong
    std::vector<OutlineNode> children;
};

// One editable or read-only row in the Property Panel.
//
// `unitLabel` is separate from `value` on purpose: UI spec 8 makes a wrong or
// missing unit a Critical defect, and a formatter that bakes the unit into the
// value string cannot be checked for that.
// Which editable thing a row writes to (M11.3).
//
// A parameter now has TWO editable aspects -- its literal value and the
// expression that may drive it -- and the commit handler cannot tell them apart
// from the id alone. Naming the field is what keeps the panel from guessing.
enum class PropertyField {
    None,       // not editable
    Value,      // a plain number
    Expression, // expression text; empty clears it
    // A CHECKBOX over the SIGN of the same parameter (M17.8, ADR-M17-031).
    //
    // Not a second stored value. A pad or pocket already carries its direction
    // in the sign of its length: negative builds on the other side of the
    // sketch plane. A separate `reversed` flag would be a second truth about
    // one fact, and the two would eventually disagree -- a stored flag saying
    // "reversed" over a length the user had since typed a minus sign into is a
    // feature that points the way neither of them asked for.
    //
    // So this row READS the sign and WRITES the sign. There is one fact, shown
    // in the form a user can act on: "-5 mm deep" is an odd thing to read, and
    // a ticked Reversed box is not.
    Reversed,
    // The object's own NAME (M17.16, ADR-M17-039). `parameterId` on such a row
    // carries the OBJECT's id, not a parameter's -- the field has always meant
    // "what to write", and a name is written to the thing itself.
    Name
};

struct PropertyRow {
    std::string group;      // "General", "Geometry", "Material", ...
    std::string label;
    std::string value;      // already formatted for display
    std::string unitLabel;  // "mm", "kg", "kg/m^3", or empty when unitless
    bool editable{false};
    ObjectId parameterId{kInvalidObjectId}; // set when editable: what to write
    double numericValue{0.0};
    // Defaulted, so every existing 7-argument aggregate initialisation of this
    // struct keeps compiling and keeps meaning what it meant.
    PropertyField field{PropertyField::None};
};

// Builds the tree and property views of a document. Free of Qt and of OCCT, so
// everything the UI decides -- what to show, in what state, with what
// diagnostic -- is unit-testable without a display (UI spec 20).
class DocumentOutline {
public:
    explicit DocumentOutline(const PartDocument& document) noexcept : document_(&document) {}

    // The document tree: Part -> Parameters / Sketches / Solids / MassProperties.
    //
    // `hiddenIds` is view state supplied by the caller (the presenter owns it);
    // this class stays free of any notion of a viewer.
    OutlineNode build(const std::set<ObjectId>& hiddenIds = {}) const;

    // Properties of one object, or an empty vector if the id is not something
    // this panel can describe. Accepts a SketchConstraintId's underlying
    // ObjectId too: constraint ids come from the same generator, so they are
    // document-unique and a selected constraint row can be described here
    // exactly like any other object.
    std::vector<PropertyRow> propertiesOf(ObjectId id) const;

    // Properties of ONE THING PICKED ON THE SKETCH CANVAS (M26.7): a line, an
    // arc, a circle, a point, a spline, an ellipse -- or one endpoint of any
    // of them.
    //
    // A SEPARATE ENTRY POINT from propertiesOf(id), even though entity ids come
    // from the same generator and would be findable by id alone. A canvas pick
    // is a SketchElementRef, and the sub-element is half of what was picked:
    // "Line1" and "Line1's end" are different answers to "what did I click",
    // and an id-only lookup would have to throw that half away before it began.
    //
    // Empty when the ref names nothing this sketch holds.
    //
    // READ-ONLY, and deliberately. Sketch geometry is what the solver WRITES;
    // a cell that took a typed coordinate would be overwritten by the next
    // solve whenever a constraint disagreed with it, and a field that silently
    // reverts is the thing this project refuses to ship (ADR-M17-042 says the
    // same about a driven dimension). Moving geometry is what dragging and
    // dimensions are for.
    std::vector<PropertyRow> propertiesOfSketchElement(ObjectId sketchId,
                                                       const SketchElementRef& ref) const;

    // Human-facing solve status, e.g. "Solved", "Over-constrained". Spec 18
    // requires the status to be readable as TEXT: a status conveyed only by a
    // row colour is unreadable in greyscale, to a colour-blind user, and in a
    // screenshot attached to a bug report.
    static const char* solveStatusLabel(SketchSolveStatus status) noexcept;

    // Marker text for a state, used alongside colour so state is never conveyed
    // by colour alone (UI spec 11/19).
    static const char* stateMarker(OutlineState state) noexcept;
    static const char* stateLabel(OutlineState state) noexcept;

private:
    const PartDocument* document_;
};

} // namespace paramcad
