#pragma once

#include "Core/Assembly/Mate.h"
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

    // --- Grounding (M24) -----------------------------------------------------
    //
    // A mate says where something is RELATIVE to something else, so a chain of
    // them has to start somewhere that is not relative to anything. That is
    // what grounding is: this instance stays where it was put, and everything
    // mated to it follows.
    //
    // Nothing is grounded by default, and an assembly whose mates reach no
    // ground is REFUSED with the names -- because "somewhere" is not an answer
    // and putting the first instance in the list there would make the answer
    // depend on the order things were typed.
    bool setInstanceGrounded(ObjectId instanceId, bool grounded);
    bool isInstanceGrounded(ObjectId instanceId) const noexcept;
    // The restore-path twin: grounds without recording an undo step.
    bool restoreInstanceGrounded(ObjectId instanceId);

    // --- Mates (M24, ADR-M24-002) --------------------------------------------
    //
    // Each side is an instance and a connector NAME on the part it brings in.
    // `value` is the mate's one remaining freedom -- radians for a revolute,
    // millimetres for a slider -- and must be zero for a Fastened mate, which
    // has no freedom to spend it on.
    //
    // Throws when an id is not an instance, when both sides are the SAME
    // instance (a thing cannot be mated to itself), or when a Fastened mate is
    // handed a value.
    Mate& addMate(std::string name, MateType type, ObjectId leadingInstanceId,
                  std::string leadingConnector, ObjectId followingInstanceId,
                  std::string followingConnector, double value = 0.0);
    Mate& restoreMate(ObjectId id, std::string name, MateType type, ObjectId leadingInstanceId,
                      std::string leadingConnector, ObjectId followingInstanceId,
                      std::string followingConnector, double value);

    std::vector<const Mate*> mates() const;
    const Mate* findMate(ObjectId id) const noexcept;
    const Mate* findMateNamed(const std::string& name) const noexcept;

    // Drives the mate's one freedom. This is what "turn the hinge" means: it
    // is an ordinary undoable edit, and the next rebuild moves everything
    // downstream of it. False if the id is not a mate; refused for Fastened.
    bool setMateValue(ObjectId mateId, double value);

    // WHERE A MATE CONNECTOR ACTUALLY IS, in the assembly (M24).
    //
    // The instance's placement composed with the connector's place on the
    // part. Composed on demand, never stored -- which is what makes "the two
    // connectors are the same place" a thing that can be MEASURED rather than
    // assumed, and that measurement is the half of the hinge gate a picture
    // cannot check.
    //
    // `found` is set false when the instance or the connector name does not
    // resolve, because identity is not a place and returning one would be a
    // lie a caller cannot distinguish from an answer.
    Transform3D mateConnectorWorldTransform(ObjectId instanceId, const std::string& connectorName,
                                            bool* found = nullptr) const noexcept;

    // --- What the solve concluded (M24, ADR-M24-004) --------------------------
    //
    // Filled by the last recompute. Empty message and true means the mates were
    // satisfiable and every instance was placed; false names what stopped it.
    struct MateSolveReport {
        bool ok = true;
        std::string message;
        // Per instance, the degrees of freedom the mates LEFT it (roadmap
        // §20.3 requires this be readable per instance, not as one number for
        // the assembly: "this assembly is under-constrained" is not something
        // a user can act on).
        struct InstanceFreedom {
            ObjectId instanceId = kInvalidObjectId;
            int rotational = 0;
            int translational = 0;
            std::string describedBy; // the mate that decided it, or "ground"
        };
        std::vector<InstanceFreedom> freedoms;
    };
    const MateSolveReport& mateSolveReport() const noexcept { return solveReport_; }

    DocumentRecomputeReport recompute() override;

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

    Mate* findMateForEdit(ObjectId id) noexcept;
    // The rules a mate has to pass before it exists at all. Shared by add and
    // restore so the two doors cannot come to disagree about what a legal mate
    // is -- which is how a document that saves cleanly stops loading.
    void requireMatable(ObjectId leadingInstanceId, ObjectId followingInstanceId, MateType type,
                        double value, const char* who) const;
    // Places every instance the mates reach, starting from the grounded ones.
    // Returns false and fills `solveReport_` when it cannot.
    bool solveMates();

    std::vector<std::unique_ptr<PartInstance>> instances_;
    std::vector<std::unique_ptr<Mate>> mates_;
    std::vector<ObjectId> groundedInstances_;
    MateSolveReport solveReport_;
};

} // namespace paramcad
