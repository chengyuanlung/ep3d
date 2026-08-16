#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Core/Sketch/SketchTypes.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace paramcad {

// What M7 proposes to do to a Sketch, before it does any of it (spec 15).
//
// Reconstruction is staged rather than applied while interpreting source data,
// so that the whole proposal -- every Parameter, every constraint, every item
// skipped and why -- can be inspected and validated before a single document
// object exists. Interpreting and mutating in one pass would make "no
// half-created Parameters remain" (spec 16) a property of error handling
// scattered through the analysis, instead of a property of the design.
//
// A plan names NO document object it has not been given. It refers to native
// geometry by SketchEntityId, which already exists, and to Parameters it has
// not created yet by PlanParameterSlot, which exists only while the plan does.

// --- Provenance (spec 8, spec 26) -------------------------------------------
//
// The three categories spec 3 says "must not be silently mixed". They are
// carried on every planned item rather than inferred later, because after the
// constraint is a native SketchConstraint there is nothing left to infer it
// from: a Length constraint the source stated and a Length constraint M7 chose
// are the same object.
enum class ReconstructionOrigin {
    // The source stated this dimension and it resolves to exactly one piece of
    // native geometry (spec 8, Class A).
    ExplicitSource,
    // M7 recognised a geometric relation from the geometry itself, under a
    // documented rule and tolerance (spec 8, Class C).
    InferredGeometric,
    // M7 chose a placement so the sketch has a determinate solution. Not a
    // measurement and never presented as one (spec 8, Class D; ADR-M7-008).
    InferredPlacement
};

const char* ReconstructionOriginName(ReconstructionOrigin origin) noexcept;

// --- Why something was NOT reconstructed (spec 18) --------------------------
//
// M7 prefers "not reconstructed" to "confidently wrong", so these are the
// normal outcome for real drawings, not an error path. Each is a distinct
// reason because each calls for a different action by the user.
enum class ReconstructionSkipReason {
    NoTargetGeometry,          // nothing native matches the dimension's points
    AmbiguousTarget,           // more than one does, and nothing chooses between them
    ValueDisagreesWithGeometry,// the stated value is not what the geometry measures
    TextOverride,              // the drawing displays a number the geometry does not encode
    UnsupportedKind,           // a dimension kind M7.1 does not reconstruct
    InvalidValue,              // non-finite, zero, negative, or below the validity floor
    NameCollision              // no deterministic free name could be formed
};

const char* ReconstructionSkipReasonName(ReconstructionSkipReason reason) noexcept;

struct ReconstructionSkip {
    ReconstructionSkipReason reason{ReconstructionSkipReason::NoTargetGeometry};
    // Why, in words a user can act on. Spec 18 requires a structured
    // diagnostic, not silence, and a reason code alone does not tell someone
    // WHICH of their dimensions was dropped.
    std::string detail;
    // Optional source metadata (a DXF handle, say) for provenance only. Never
    // identity, and correctness never depends on it (spec 7).
    std::string sourceRef;
};

// --- Plan-local Parameter handles -------------------------------------------
//
// A planned dimensional constraint must bind to a Parameter that does not
// exist yet, so the plan needs its own way to say "that one". This is it.
//
// A distinct type, not a bare index, so the compiler rejects passing an
// ObjectId where a slot belongs and vice versa -- the two are never
// interchangeable, and the whole point of spec 7 is that one of them must never
// become the other. A slot exists only while the plan does: apply maps each to
// the real ObjectId the document hands back, nothing outside the plan ever sees
// one, and none is ever persisted, serialized or used as native identity.
enum class PlanParameterSlot : std::size_t {};

inline constexpr PlanParameterSlot kInvalidPlanParameterSlot{
    static_cast<std::size_t>(-1)};

struct PlannedParameter {
    // Deterministic and user-readable (spec 9). Unique within the plan AND
    // against the document the plan was built for, checked at build time.
    std::string name;
    double value{0.0};
    UnitType unit{UnitType::Millimeter};
    ReconstructionOrigin origin{ReconstructionOrigin::ExplicitSource};
    // Which source item produced it, for the provenance report (spec 26).
    std::string sourceRef;
};

struct PlannedConstraint {
    // The constraint exactly as it will be created, EXCEPT that a dimensional
    // kind's parameterId is left invalid: the Parameter has no ObjectId yet.
    // Apply fills it from `parameter` below. Storing the rest natively means
    // the plan cannot describe a constraint the model could not hold.
    SketchConstraintData data{};

    // Set if and only if IsDimensional(data). Checked when the plan is
    // validated, so a dimensional constraint with no parameter -- the exact
    // shape of "reports success but controls nothing" -- cannot be applied.
    PlanParameterSlot parameter{kInvalidPlanParameterSlot};

    ReconstructionOrigin origin{ReconstructionOrigin::ExplicitSource};
    std::string sourceRef;
};

struct ReconstructionPlan {
    // The document AND the sketch this plan was built for. A plan is not
    // portable: its SketchEntityIds mean nothing anywhere else, and applying it
    // elsewhere is the "silently targets wrong geometry" Critical of spec 34.
    //
    // BOTH are needed, and the sketch id alone is not enough. `ObjectId` is a
    // PROCESS-LOCAL counter starting at 1, so two documents loaded in one
    // session carry overlapping ids by construction -- independent review
    // demonstrated a plan built for document A applying cleanly to document B
    // and driving B's 250 mm edge with A's Width=100, with no diagnostic. An
    // earlier revision of this comment claimed the case was covered; it was
    // checking an id space that is not unique across documents.
    // ROUND 2 correction: the comment above was still describing an id space
    // that cannot do this job. `ObjectId` starts at 1 in EVERY process, so two
    // parts saved in two sessions BOTH carry documentId 1 and sketchId 2 --
    // review demonstrated a plan for a 100x50 plate applying cleanly to a
    // 103x80 bracket, ok=1, no diagnostic, the bracket's edge now driven by
    // the plate's Width. The ids remain as a cheap first pass; what actually
    // separates two documents is the FINGERPRINT below.
    ObjectId documentId{kInvalidObjectId};
    ObjectId sketchId{kInvalidObjectId};

    // A hash of the sketch's entity ids AND their geometry at analysis time.
    // Two different drawings collide only if every entity id and every
    // coordinate matches, which is the definition of "the same sketch".
    // Deliberately NOT a tolerance: this answers "is this the same document",
    // and the 5% agreement band answers "does this dimension describe this
    // line" -- round 2 found the second standing in for the first, which is
    // how two parts 3% apart passed as identical.
    std::uint64_t fingerprint{0};

    std::vector<PlannedParameter> parameters;
    std::vector<PlannedConstraint> constraints;
    std::vector<ReconstructionSkip> skipped;

    bool empty() const noexcept { return parameters.empty() && constraints.empty(); }

    const PlannedParameter* find(PlanParameterSlot slot) const noexcept;
};

// Whether this plan can be applied at all, checked BEFORE anything is created
// (spec 15: analyze, build, VALIDATE, apply).
//
// This is the last point at which rejecting costs nothing. Everything it checks
// is a defect that would otherwise become a half-built document or, worse, a
// document that solves and is wrong:
//   - a dimensional constraint bound to no Parameter, or to a slot that is not
//     in this plan
//   - a non-dimensional constraint carrying a Parameter slot
//   - a Parameter value that is non-finite or below the dimension floor
//   - a length-like constraint bound to a Parameter that is not a length
//   - two planned Parameters with the same name
struct PlanValidation {
    bool ok{false};
    std::string message;
    explicit operator bool() const noexcept { return ok; }
};

PlanValidation ValidatePlan(const ReconstructionPlan& plan);

} // namespace paramcad
