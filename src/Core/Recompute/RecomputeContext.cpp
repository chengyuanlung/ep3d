#include "Core/Recompute/RecomputeContext.h"

#include "Core/Document/PartDocument.h"

#include <stdexcept>

namespace paramcad {

PartDocument& RecomputeContext::part() const {
    // See the header. This cannot fail through any path that exists: features
    // and sketches are owned by a PartDocument and nothing else creates them.
    // It throws rather than returning something, because a node that is
    // recomputing inside the wrong kind of document has no correct answer to
    // give and a null check at each of the thirty-odd call sites would be
    // thirty places to get it wrong.
    auto* asPart = dynamic_cast<PartDocument*>(&document);
    if (asPart == nullptr)
        throw std::runtime_error("this node is recomputing in a document that is not a Part");
    return *asPart;
}

} // namespace paramcad
