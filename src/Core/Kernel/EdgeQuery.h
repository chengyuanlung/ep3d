#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Kernel/FaceGeometry.h"
#include "Core/Kernel/FaceQuery.h"

#include <string>
#include <variant>
#include <vector>

namespace paramcad {

// Which edges a fillet or a chamfer dresses (M17.12, ADR-M17-034).
//
// A QUERY, RE-EVALUATED EVERY REBUILD -- never a stored edge, and never an
// index into one. This is the whole architecture, and it is Onshape's rather
// than SolidWorks': a fillet does not remember "edge 7", it remembers a
// sentence about which edges it wants, and that sentence is answered again
// against whatever geometry the model currently has.
//
// The difference shows the moment a parameter changes. An index points at
// whatever now happens to be seventh -- which after a rebuild is a different
// edge, or none, and the fillet either fails or silently moves. A description
// like "the topmost face's edges" still means the same thing when the part
// gets taller, because it never described a position in the first place.
//
// That is also why NONE of these hold coordinates from the moment they were
// made. A plane at z = 20 is an index wearing better clothes: make the pad
// 30 mm tall and it matches nothing. Every query here is phrased so that the
// answer moves with the model.

// Every edge of the solid. What Fillet and Chamfer did before there was any
// choice (ADR-M8-006), and still the default.
struct AllEdges {};

// The edges of the face that lies furthest along `direction` and faces that
// way -- "the top face's edges" for +Z, "the bottom" for -Z, and so on.
//
// EXTREME rather than "the face at this height": a part that grows keeps its
// top face, and this query keeps meaning the top face. It is the shape of
// nearly every real fillet a user asks for on a prismatic part.
struct EdgesOfExtremeFace {
    Vec3 direction{0.0, 0.0, 1.0};
};

// Every straight edge parallel to `direction` -- "all the vertical edges".
//
// Survives any change that does not rotate the part, because being parallel to
// Z is not a position either.
struct EdgesParallelTo {
    Vec3 direction{0.0, 0.0, 1.0};
};

// The edges of every face a given feature CREATED (M17.13, ADR-M17-035).
//
// The query Onshape's `qCreatedBy` is, and the only one here that describes
// PROVENANCE rather than shape. That makes it the only one that survives the
// geometry moving: "what the pocket cut" is still what the pocket cut after
// the pocket has been made deeper, wider, or put somewhere else entirely.
//
// It is also the only way to name an INNER face. "The outermost face towards
// +Z" cannot say "the pocket floor" -- the top face is outermost and the floor
// is not -- so before this, a pocket's own edges could not be selected at all.
struct EdgesCreatedBy {
    ObjectId featureId{kInvalidObjectId};
};

// The edges of the ONE face a face query narrows to (M17.14, ADR-M17-036).
//
// This is where the two vocabularies meet, and it is what COMPOSITION looks
// like here. `EdgesCreatedBy{pocket}` names the pocket's floor AND its four
// walls; `EdgesOfExtremeFace{+Z}` names the part's own top. Neither can say
// "the pocket floor" -- but a FaceQuery is a conjunction, so
// `{createdBy: pocket, extremeTowards: +Z}` says exactly that, and this
// carries it into an edge selection.
//
// It narrows to ONE face on purpose. A query matching several is refused
// where it is resolved, because "which of these four walls" is a question the
// user has to answer, not one this code should pick from.
struct EdgesOfFace {
    FaceQuery face;
};

using EdgeQuery = std::variant<AllEdges, EdgesOfExtremeFace, EdgesParallelTo, EdgesCreatedBy,
                               EdgesOfFace>;

// A feature holds a LIST, so a user can take a face's edges AND the vertical
// ones. The union is what gets dressed; an edge named twice is dressed once.
using EdgeSelection = std::vector<EdgeQuery>;

// The default: everything, which is what every file written before M17.12 says
// by having no selection at all.
inline EdgeSelection AllEdgesSelection() { return EdgeSelection{AllEdges{}}; }

inline bool IsAllEdges(const EdgeSelection& selection) {
    return selection.size() == 1 && std::holds_alternative<AllEdges>(selection.front());
}

// What the query says, in words a user can check against the part in front of
// them. Every surface that shows a selection uses this, so the property panel,
// the status bar and any future dialog cannot describe the same query
// differently.
std::string DescribeEdgeSelection(const EdgeSelection& selection);

// Turning a PICKED FACE into a query (M17.12, ADR-M17-034).
//
// The user clicks a face; what gets stored is a sentence. The only sentence
// available today is "the outermost face in this direction", so a pick that
// does not describe such a face is REFUSED rather than stored as something
// else -- a query that names a different face than the one under the cursor
// would dress the wrong edges on the next rebuild, and look deliberate.
//
// `faces` is every face of the solid, which is how "outermost" is decided. It
// is passed in rather than fetched so this stays free of any kernel.
struct EdgeSelectionPick {
    bool ok{false};
    std::string message; // always set, on success and refusal alike
    EdgeSelection selection;
};

EdgeSelectionPick SelectionForPickedFace(const FacePlane& picked,
                                         const std::vector<FacePlane>& faces);

} // namespace paramcad
