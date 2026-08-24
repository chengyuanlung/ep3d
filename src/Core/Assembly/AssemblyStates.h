#pragma once

#include "Core/Assembly/MateFreedom.h"
#include "Core/Document/EvaluationCut.h"
#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"

#include <string>
#include <utility>
#include <vector>

namespace paramcad {

// THREE WAYS TO SHOW ONE ASSEMBLY, AND THEY STAY THREE (M26, ADR-M26-002).
//
// Roadmap §49 separates them and says why, and the reason is not tidiness --
// they capture three different KINDS of thing:
//
//   * a NAMED POSITION is geometry evaluation INPUT: the values of the mates'
//     freedoms, plus where the instances no mate places were put by hand. Feed
//     it back in and the assembly rebuilds to that shape.
//   * an EXPLODED VIEW is a DERIVED display transform: an ordered list of
//     steps that move things apart for a picture. It never changes the model,
//     and re-running the assembly with the explode off gives the same shapes.
//   * a DISPLAY STATE is pure presentation -- what is hidden. It is NOT in
//     this file, and that is the decision: A02 keeps presentation out of Core,
//     and EP3D has no assembly UI to hide anything from yet. Named rather than
//     half-built.
//
// Folding them into one "view state" is what §49 warns against, and it would
// take the document/presentation line (§44) with it.

// A pose the assembly can be put back into (roadmap §49, "Named position").
//
// NOT a configuration: a configuration changes what the model IS, a named
// position only changes where its freedoms are sitting. §49 point 3 is
// explicit that the two must not be merged, and this type holds nothing that
// could define a part.
class NamedPosition {
public:
    struct MateSetting {
        ObjectId mateId = kInvalidObjectId;
        MateValues values{};
    };
    // Instances no mate places. Their absolute transform is part of the pose
    // because nothing else records it -- an assembly that captured only its
    // mates would come back with everything loose in the wrong place.
    struct LooseSetting {
        ObjectId instanceId = kInvalidObjectId;
        Transform3D transform{};
    };

    NamedPosition(std::string name, std::vector<MateSetting> mates,
                  std::vector<LooseSetting> loose);
    NamedPosition(ObjectId id, std::string name, std::vector<MateSetting> mates,
                  std::vector<LooseSetting> loose);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    const std::vector<MateSetting>& mates() const noexcept { return mates_; }
    const std::vector<LooseSetting>& loose() const noexcept { return loose_; }

private:
    friend class AssemblyDocument;
    void setName(std::string name) { name_ = std::move(name); }

    ObjectId id_;
    std::string name_;
    std::vector<MateSetting> mates_;
    std::vector<LooseSetting> loose_;
};

// One step of an exploded view: this instance, moved this far, for the
// picture. Named, because §49 says a step can be named, reordered and deleted
// -- and a list of unnamed displacements is a list nobody can reorder on
// purpose.
struct ExplodeStep {
    std::string name;
    ObjectId instanceId = kInvalidObjectId;
    // A RIGID DISPLACEMENT, so a step can turn a part as well as pull it out
    // -- §49 says "平移／旋轉". Applied ON TOP of where the assembly put the
    // instance, never instead of it.
    Transform3D displacement{};
};

// An exploded view (roadmap §49, "Exploded view").
//
// It has its OWN evaluation position, which is the third appearance of that
// idea and the reason EvaluationCut exists (ADR-M26-001): the panel walks the
// steps one at a time to preview them, exactly as a feature chain's rollback
// bar walks features.
class ExplodeView {
public:
    ExplodeView(std::string name, std::vector<ExplodeStep> steps);
    ExplodeView(ObjectId id, std::string name, std::vector<ExplodeStep> steps,
                std::size_t previewCut);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    const std::vector<ExplodeStep>& steps() const noexcept { return steps_; }

    // How many steps are being shown. `EvaluationCut::kAll` -- the default --
    // is the finished explosion.
    std::size_t previewCut() const noexcept { return preview_.stored(); }
    std::size_t stepsShown() const noexcept { return preview_.effective(steps_.size()); }

    // Everything this view does to `instanceId`, composed in step order, up to
    // the preview position. Identity when no shown step touches it.
    //
    // A QUERY, not stored state: the explosion is derived (§49 calls it
    // "衍生的展示變換"), so asking is the only way to get it and there is
    // nothing to keep in step.
    Transform3D displacementOf(ObjectId instanceId) const noexcept;

private:
    friend class AssemblyDocument;
    void setName(std::string name) { name_ = std::move(name); }
    void setSteps(std::vector<ExplodeStep> steps) { steps_ = std::move(steps); }
    void setPreviewCut(std::size_t cut) noexcept { preview_.set(cut); }

    ObjectId id_;
    std::string name_;
    std::vector<ExplodeStep> steps_;
    EvaluationCut preview_;
};

} // namespace paramcad
