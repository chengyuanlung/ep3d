#pragma once

#include "Core/Assembly/PartInstance.h"
#include "Core/Document/DocumentBase.h"
#include "Core/Geometry/MathTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace paramcad {

// THE SECOND DOCUMENT TYPE (M23, ADR-M23-002).
//
// What an assembly holds is INSTANCES: appearances of parts, each somewhere.
// It holds no sketches, no features and no bodies, and that is not an
// omission -- an assembly does not model shape, it arranges shapes that were
// modelled elsewhere.
//
// Almost nothing here is new machinery. Identity, the dependency graph, undo,
// reference frames, connectors, naming and the recompute engine all come from
// DocumentBase, which M23 created for exactly this reason (ADR-M23-001): the
// plan's P3 says the cost of a second document type is paid once or paid for
// ever. What is left below is the part that really is about assemblies, and it
// is short enough to read in one sitting -- which is the evidence that P3 was
// honoured rather than a claim about it.
//
// NOT here yet, and named rather than half-built: mates (M24), degrees of
// freedom and interference (M25), sub-assemblies and exploded states (M26).
// An instance today is placed by setting its transform directly, which is what
// Onshape calls a fixed instance dragged by hand.
class AssemblyDocument final : public DocumentBase {
public:
    explicit AssemblyDocument(std::string name);
    // Restore constructor (deserialization): keeps the persisted document id.
    AssemblyDocument(ObjectId id, std::string name);

    DocumentType type() const noexcept override { return DocumentType::Assembly; }

    // --- Instances -----------------------------------------------------------
    //
    // `sourcePath` names an .ep3d file and `bodyName` a body inside it. An
    // empty body name means "the only one", and a file with several bodies and
    // no name given is REFUSED at recompute time with the names it does have --
    // taking the first would make an instance mean something different after
    // the part gained a body, which is position-as-identity (ADR-M4-004).
    //
    // Every instance gets its own ReferenceFrame, parented to Origin, named
    // "<instance> origin". That frame IS the placement: there is no second
    // transform anywhere, so an instance cannot be in two places.
    PartInstance& addInstance(std::string name, std::string sourcePath,
                              std::string bodyName = {});
    // Restore path: the frame already exists and is passed in, because the
    // loader restores frames before instances and re-deriving one here would
    // give the file's frame and the instance's frame different ids.
    PartInstance& restoreInstance(ObjectId id, std::string name, ComputeState state,
                                  std::string sourcePath, std::string bodyName,
                                  ObjectId frameId);

    std::vector<const PartInstance*> instances() const;
    const PartInstance* findInstance(ObjectId id) const noexcept;
    const PartInstance* findInstanceNamed(const std::string& name) const noexcept;

    // Moves an instance. Goes through the frame, so it is undoable, it dirties
    // the instance through an ordinary graph edge, and a sub-assembly's
    // children will follow it in M26 without anything here changing.
    // False if the id is not an instance.
    bool setInstanceTransform(ObjectId instanceId, const Transform3D& placement);
    // Where the instance sits relative to its parent, and in the assembly's
    // world. Identity for an unknown id, exactly as worldTransform is.
    Transform3D instanceTransform(ObjectId instanceId) const noexcept;
    Transform3D instanceWorldTransform(ObjectId instanceId) const noexcept;

    bool removeObject(ObjectId id) override;

protected:
    void requireUnusedIdHook(ObjectId id, const char* who) const override;
    std::string ownObjectName(ObjectId id) const override;
    void applyOwnName(ObjectId id, const std::string& name) override;
    bool ownNameIsTaken(const std::string& name, ObjectId except) const override;
    void applyOwnDelta(const UndoDelta& delta, bool forward) override;

private:
    PartInstance* findInstanceForEdit(ObjectId id) noexcept;
    // The frame an instance is placed by, created and wired in one place so
    // add and restore cannot disagree about what an instance's frame is.
    void wireInstance(PartInstance& instance);

    std::vector<std::unique_ptr<PartInstance>> instances_;
};

} // namespace paramcad
