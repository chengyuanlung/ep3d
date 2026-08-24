#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// A solid read from a STEP file (M22).
//
// IT STORES THE PATH, NOT THE GEOMETRY, and that is the decision worth
// defending. Writing the imported topology into the .ep3d would break the rule
// the whole file format is built on (ADR-M4-004): no topology, no index, no
// kernel representation ever crosses into the document. What is stored is the
// SENTENCE -- "the solid in that file" -- and it is answered again on every
// rebuild, exactly as a face query is.
//
// The consequences are real and are the point:
//
//   * the file has to still be there. A missing one FAILS LOUDLY rather than
//     falling back to the last shape, because a part that quietly kept working
//     after its source vanished is a part nobody can reproduce;
//   * re-exporting the source updates the model on the next rebuild, which is
//     what a user who fixed the source expects;
//   * the .ep3d stays small and stays diffable.
//
// The alternative -- embedding the geometry -- makes the document
// self-contained, which is a real advantage and a different feature. It would
// need somewhere to put a blob, which this format does not have.
//
// Like a Box and a Pad, an import builds from nothing: it is a legal chain
// BASE, and everything downstream treats it as one.
class ImportFeature final : public Feature,
                            public IRecomputable,
                            public ISolidFeature,
                            public IMaterialReferencing {
public:
    ImportFeature(std::string name, std::string path, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    ImportFeature(ObjectId id, std::string name, ComputeState state, std::string path,
                  ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Import"; }

    const std::string& path() const noexcept { return path_; }

    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;

    // Real execution: read the file through the injected kernel. Commits only
    // on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    std::string path_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
