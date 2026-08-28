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
    ImportFeature(std::string name, std::string path, ObjectId materialId,
                  ObjectId thicknessParameterId = kInvalidObjectId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    ImportFeature(ObjectId id, std::string name, ComputeState state, std::string path,
                  ObjectId materialId, ObjectId thicknessParameterId = kInvalidObjectId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Import"; }

    const std::string& path() const noexcept { return path_; }

    // A THICKNESS, WHEN THE FILE HAS NO SOLID IN IT (M59).
    //
    // M57 established the honest fact about IGES: most of it carries trimmed
    // surfaces and no volume anywhere, because that is what the format was
    // built for. Until now this feature could only say so. With a thickness
    // set, the surfaces are read, sewn into a skin and given that thickness --
    // which is how a supplier's surface model becomes a part, and it is the
    // commonest thing anybody does with a surface in mechanical CAD.
    //
    // kInvalidObjectId means the M22 behaviour and everything made before this:
    // a file with no solid is refused. That stays the default because it is the
    // right answer for STEP, where a missing solid means something went wrong
    // rather than that the format works this way.
    //
    // THE SOLID IS STILL PREFERRED. When the file HAS a solid, the solid is
    // what is used, thickness or no thickness -- a file that gained a proper
    // solid should stop being approximated by an offset the moment it does,
    // without anybody having to notice and clear a field.
    ObjectId thicknessParameterId() const noexcept { return thicknessParameterId_; }

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
    ObjectId thicknessParameterId_;
    KernelShape currentShape_;
};

} // namespace paramcad
