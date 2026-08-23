#include "Kernel/Occt/OcctProvenance.h"

#include "Kernel/Occt/OcctShape.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>

#include <memory>

namespace paramcad {

KernelShape WithProvenance(const KernelShape& result, const KernelShape& base,
                           std::uint64_t tag) {
    const auto* made = dynamic_cast<const OcctShape*>(result.handle());
    if (made == nullptr || made->shape().IsNull()) return result; // foreign or empty: nothing to say

    // What the base already accounted for, both as a face set and as a history
    // to carry forward.
    TopTools_IndexedMapOfShape baseFaces;
    ShapeProvenance provenance;
    if (const auto* consumed = dynamic_cast<const OcctShape*>(base.handle())) {
        if (!consumed->shape().IsNull())
            TopExp::MapShapes(consumed->shape(), TopAbs_FACE, baseFaces);
        provenance = consumed->provenance();
    }

    // Everything the result has that the base did not. For a Pad this is every
    // face; for a Pocket it is the walls and floor of the cut; for a Fillet it
    // is the rounded strips.
    //
    // A face the operation MODIFIED -- trimmed by the cut, say -- is a
    // different TShape from the base's and therefore counts as new. That is
    // the right answer for this question: a user asking for "the faces the
    // pocket made" means the surfaces that appeared, and a trimmed top face is
    // not one of them... but neither is it the untouched original. Counting it
    // as the pocket's is the lesser error, and it is the one that makes
    // "fillet what the pocket made" pick the cut rather than nothing.
    TopTools_IndexedMapOfShape created;
    for (TopExp_Explorer it(made->shape(), TopAbs_FACE); it.More(); it.Next())
        if (!baseFaces.Contains(it.Current())) created.Add(it.Current());

    // An operation that created NO new face records nothing rather than an
    // empty entry: a tag present with nothing under it and a tag absent are
    // the same claim, and one of them would need explaining at every read.
    if (!created.IsEmpty()) provenance[tag] = std::move(created);

    return KernelShape{std::make_shared<OcctShape>(made->shape(), std::move(provenance))};
}

} // namespace paramcad
