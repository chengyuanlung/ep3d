#pragma once

#include "Core/Document/ObjectId.h"
#include <set>
#include <vector>

namespace paramcad {

class PartDocument;

// What the viewer is allowed to know about a document (ADR-M4-006).
//
// The viewer NEVER owns semantic objects: it holds a non-owning PartDocument*
// and, per displayed solid, an ObjectId. Everything below is a read of document
// state or a call through PartDocument's existing facade -- the viewer is never
// on the recompute path, and refresh is a consequence of recompute rather than
// a participant in it.
//
// This class is deliberately free of Qt AND of OCCT so the document-facing half
// of the viewer is testable without a display: the widget layer owns the
// AIS/V3d objects and asks this for what to show.
class DocumentPresenter {
public:
    // document must outlive this presenter (same non-owning contract
    // PartDocument::setGeometryKernel uses for the kernel, ADR-M3-003).
    explicit DocumentPresenter(PartDocument& document) noexcept : document_(&document) {}

    PartDocument& document() const noexcept { return *document_; }

    // ObjectIds of the features that currently hold a valid runtime shape, in
    // document order. A feature whose ComputeState is not Valid is omitted:
    // its retained shape is stale (ADR-M3-004/006), and showing stale geometry
    // as if it were current is the display-layer version of the defect
    // ADR-M3-006 fixed for mass properties.
    std::vector<ObjectId> displayableSolids() const;

    // Recomputes and reports whether the display should be rebuilt. The viewer
    // calls this rather than touching the graph itself.
    bool recomputeForDisplay();

    // Visibility is VIEW state, not document state (UI spec 11 separates Hidden
    // from Suppressed for exactly this reason): hiding a solid changes what is
    // drawn, never what is computed or saved. It therefore lives here and never
    // reaches PartDocument.
    void setHidden(ObjectId id, bool hidden);
    bool isHidden(ObjectId id) const;
    void toggleHidden(ObjectId id) { setHidden(id, !isHidden(id)); }

private:
    PartDocument* document_;
    std::set<ObjectId> hidden_;
};

} // namespace paramcad
