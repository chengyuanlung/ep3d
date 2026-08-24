#pragma once

#include "Core/Document/ObjectId.h"
#include "Viewer/DocumentOutline.h"

#include <set>
#include <string>
#include <vector>

namespace paramcad {

class AssemblyDocument;

// The tree and property views of an ASSEMBLY (M27).
//
// THE SAME NODE TYPE as DocumentOutline produces, and deliberately so. What a
// row IS -- a name, a kind tag, a state, a diagnostic, children -- is the
// shell's vocabulary and belongs to no document type. What goes IN the tree is
// the document type's own business, and the two are different questions:
//
//   * a shared node type means the widget, the state markers, the colours and
//     the selection all work for an assembly the day it exists, with no second
//     renderer to keep in step;
//   * a separate BUILDER means an assembly's tree is instances and mates
//     rather than a part's tree with the part-shaped rows left blank.
//
// This is the shape P3 asked for and ADR-M23-006 deferred: the tree was one of
// the two items not hoisted, because "replacing an abstraction for something
// with no second user is guesswork". The assembly is that second user, and it
// arrived with its needs already known rather than guessed at.
//
// Free of Qt and of OCCT, exactly as DocumentOutline is, so what the UI decides
// stays testable without a display (UI spec 20).
class AssemblyOutline {
public:
    explicit AssemblyOutline(const AssemblyDocument& document) noexcept
        : document_(&document) {}

    // Assembly -> Instances / Mates / Named positions / Exploded views.
    //
    // `hiddenIds` is view state supplied by the caller, on the same non-owning
    // contract DocumentOutline::build uses.
    OutlineNode build(const std::set<ObjectId>& hiddenIds = {}) const;

    // Properties of one thing in an assembly: an instance, a mate, a named
    // position or an exploded view. Empty for an id this panel cannot describe.
    std::vector<PropertyRow> propertiesOf(ObjectId id) const;

private:
    const AssemblyDocument* document_;
};

} // namespace paramcad
