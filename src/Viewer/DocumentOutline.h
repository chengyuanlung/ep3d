#pragma once

#include "Core/Document/ObjectId.h"
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
enum class OutlineKind { Document, Parameter, Sketch, Solid, MassProperties, Material, Other };

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
struct PropertyRow {
    std::string group;      // "General", "Geometry", "Material", ...
    std::string label;
    std::string value;      // already formatted for display
    std::string unitLabel;  // "mm", "kg", "kg/m^3", or empty when unitless
    bool editable{false};
    ObjectId parameterId{kInvalidObjectId}; // set when editable: what to write
    double numericValue{0.0};
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
    // this panel can describe.
    std::vector<PropertyRow> propertiesOf(ObjectId id) const;

    // Marker text for a state, used alongside colour so state is never conveyed
    // by colour alone (UI spec 11/19).
    static const char* stateMarker(OutlineState state) noexcept;
    static const char* stateLabel(OutlineState state) noexcept;

private:
    const PartDocument* document_;
};

} // namespace paramcad
