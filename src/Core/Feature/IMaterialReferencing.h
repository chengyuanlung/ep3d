#pragma once

#include "Core/Document/ObjectId.h"

namespace paramcad {

// A feature that holds a reference to the document's Material.
//
// Introduced after independent review found that removing a Material left every
// referring feature holding the removed id: the document no longer wrote a
// material record, but the features still wrote `materialId`, so the file saved
// cleanly and its own loader then rejected it forever.
//
// The point of a capability interface rather than a list of concrete types is
// ADR-M3-007's rule, now applied to REFERRERS as well as owners (ADR-M4-009):
// `removeObject` must be able to find everything that points at the object
// being removed without naming BoxFeature, PadFeature, and whatever M5 adds.
class IMaterialReferencing {
public:
    virtual ~IMaterialReferencing() = default;

    virtual ObjectId materialId() const noexcept = 0;

private:
    // "Only PartDocument calls it" was a comment, and a comment enforces
    // nothing -- round 4's R1R4-M1 re-pointed a feature's material through a
    // `const PartDocument&` and made the document unsaveable. Now the compiler
    // says it. Access is checked against the STATIC type at the call site, so
    // the concrete overrides are private too; overriding a private virtual is
    // legal and is exactly the NVI shape this wants.
    friend class PartDocument;

    // Called when the referenced Material is removed from the document. The
    // reference becomes kInvalidObjectId -- "no material assigned", an ordinary
    // and valid state (ADR-M3-004: density 0) -- rather than a dangling id.
    virtual void clearMaterialReference() noexcept = 0;

    // Points this feature at a Material, so the graph edge and the
    // mass-properties source are rewired in the same step; a feature that could
    // be re-pointed on its own would leave the graph describing a dependency
    // the document no longer has.
    virtual void setMaterialReference(ObjectId materialId) noexcept = 0;
};

} // namespace paramcad
