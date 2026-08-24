#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"

#include "Core/Geometry/MathTypes.h"

#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// ONE APPEARANCE OF A PART INSIDE AN ASSEMBLY (M23, ADR-M23-002).
//
// An instance is three things and no more:
//
//   * WHICH PART -- a file path and the name of a body inside it. Stored as a
//     sentence, not as geometry, for exactly the reason ImportFeature stores a
//     path (ADR-M22-003): the part file is the source of truth, so a part that
//     was edited shows up here on the next rebuild, and a part that went away
//     stops the instance by name instead of leaving a copy nobody can trace
//     back to anything;
//   * WHERE -- a ReferenceFrame, by id. NOT a Transform3D of its own. An
//     instance's placement IS a frame, so it gets the frame hierarchy's cycle
//     refusal, its composed-never-stored world transform (ADR-M10-002), its
//     undo record and its dirty propagation for free, and a sub-assembly in
//     M26 is a frame parented to a frame rather than a second way of saying
//     where something is;
//   * WHAT CAME OUT -- the placed solid, and whether it is current.
//
// The same file can be instanced any number of times: each instance re-reads
// it, so three copies of a bracket are three loads. That is the cost of "the
// file is the truth" and it is paid deliberately -- a cache would be a second
// thing that has to be right about when the file changed, which is the shape
// of defect this project keeps finding.
//
// NOT here, deliberately: a material. The part brings its own, and an assembly
// that could override it would be a second place a density lives.
class PartInstance final : public IRecomputable {
public:
    PartInstance(std::string name, std::string sourcePath, std::string bodyName,
                 ObjectId frameId);
    // Restore constructor (deserialization): keeps the persisted id and state.
    PartInstance(ObjectId id, std::string name, ComputeState state, std::string sourcePath,
                 std::string bodyName, ObjectId frameId);

    ObjectId id() const noexcept override { return id_; }
    static std::string_view typeName() noexcept { return "Instance"; }

    const std::string& name() const noexcept { return name_; }
    const std::string& sourcePath() const noexcept { return sourcePath_; }
    // Which body in that file. EMPTY means "the only one" -- and a file with
    // several bodies and no name given is REFUSED rather than resolved to the
    // first, because the first is an order, and order is not identity
    // (ADR-M4-004).
    const std::string& bodyName() const noexcept { return bodyName_; }
    ObjectId frameId() const noexcept { return frameId_; }

    const KernelShape& currentShape() const noexcept { return currentShape_; }
    ComputeState currentState() const noexcept { return state_; }

    // THE MATE CONNECTORS THIS INSTANCE BRINGS IN (M24, roadmap §21).
    //
    // Defined in the PART and reused by every instance of it, which is the
    // whole reason connector-based mating beats referencing topology: define
    // "the shaft axis" once on the motor, and every motor in every assembly
    // has it.
    //
    // Read out of the part file on each rebuild, alongside the solid, and held
    // as a transform in the PART's own coordinates -- where it lands in the
    // assembly is that composed with wherever this instance ended up, computed
    // on demand and never stored (ADR-M10-002).
    struct MateConnector {
        std::string name;
        Transform3D localTransform;
    };
    const std::vector<MateConnector>& connectors() const noexcept { return connectors_; }
    // The named one, or nullptr. A name that no longer resolves is a loud
    // failure at the mate rather than a quiet fallback here.
    const MateConnector* findConnector(const std::string& name) const noexcept;

    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    friend class AssemblyDocument;

    void setName(std::string name) { name_ = std::move(name); }

    ObjectId id_;
    std::string name_;
    std::string sourcePath_;
    std::string bodyName_;
    ObjectId frameId_;
    ComputeState state_ = ComputeState::Dirty;
    KernelShape currentShape_;
    std::vector<MateConnector> connectors_;
};

} // namespace paramcad
