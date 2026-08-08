#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Recompute/RecomputeTypes.h"

namespace paramcad {

struct RecomputeContext;

// Optional capability interface for document nodes that execute work during a
// document recompute pass. Not every Core object is recomputable: Parameters
// participate as dirty-source nodes without implementing this interface
// (ADR-011), and test doubles implement it directly (spec 16).
class IRecomputable {
public:
    virtual ~IRecomputable() = default;

    virtual ObjectId id() const noexcept = 0;
    virtual RecomputeResult recompute(const RecomputeContext& context) = 0;
};

} // namespace paramcad
