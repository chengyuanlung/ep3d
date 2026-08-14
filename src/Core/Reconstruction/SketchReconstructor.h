#pragma once

#include "Core/Import/ImportedGeometry.h"
#include "Core/Reconstruction/ReconstructionPlan.h"
#include "Core/Sketch/SketchTypes.h"
#include <string>
#include <vector>

namespace paramcad {

class PartDocument;

// M7 reconstruction: imported geometry plus imported dimensions become native
// Parameters and SketchConstraints (spec 5).
//
// This is a TRANSFORMATION LAYER. It reads a Sketch that already exists and a
// format-neutral list of dimensions, and it produces normal document objects
// through the documented PartDocument facade -- never by reaching into the
// solver, never by storing a source object inside a constraint. After it has
// run and the document has been saved, nothing about the result depends on the
// DXF file, the parser, or this code having run at all.
//
// Free of any DXF type, so it is testable with hand-built inputs and needs no
// file, no parser and no library present -- the same arrangement SketchImporter
// has for the same reason.

// --- Tolerances (spec 19) ---------------------------------------------------
//
// Separate constants for separate meanings, because they answer different
// questions and the right answers are orders of magnitude apart. Spec 19
// forbids one magic constant for all purposes, and the relationships between
// them are asserted below rather than left as a comment that can rot.

// How close two points must be for reconstruction to treat them as the same
// point -- joining two lines at a corner, or matching a dimension's definition
// point to an endpoint.
//
// This is a RECOGNITION tolerance, not a statement about the geometry: a
// drawing whose corners miss by half a micron was meant to be closed, and the
// solver is what actually closes it. It is deliberately far looser than the
// model's own 1e-6 mm validity floor, which exists to reject degenerate
// geometry and would recognise almost nothing.
inline constexpr double kReconstructionCoincidenceToleranceMm = 1e-3;

// How far from axis-aligned a line may be and still be recognised as
// Horizontal or Vertical. 1e-3 rad is about 0.057 degrees -- roughly 0.06 mm
// across a 60 mm side, which is the scale of error a hand-drawn or
// badly-exported rectangle actually carries.
//
// Distinct from the coincidence tolerance because it is an ANGLE. The two were
// briefly one constant and the result was nonsense in both directions: an
// angular tolerance used as a distance is scale-dependent, and a distance used
// as an angle is not even dimensionally sound.
inline constexpr double kReconstructionAxisAngleToleranceRad = 1e-3;

// The smallest dimension M7 will reconstruct.
//
// Deliberately ABOVE the sketch model's own floor. A line shorter than the
// coincidence tolerance has endpoints that this same analysis would call
// coincident, so reconstructing a Length for it would assert two contradictory
// things about one pair of points: "these are the same point" and "these are
// 0.0005 mm apart". The model's floor (1e-5 mm) cannot express that, because it
// knows nothing about recognition.
inline constexpr double kMinReconstructedDimensionMm =
    10.0 * kReconstructionCoincidenceToleranceMm;

// How far a source-stated value may sit from what the geometry measures before
// M7 stops believing they describe the same thing (spec 18, ADR-M7-009).
//
// Relative, not absolute: 0.05 mm on a 100 mm side is a drawing that wants
// cleaning up, and 0.05 mm on a 0.2 mm slot is a different dimension entirely.
//
// This band answers ONE question -- "do these two numbers describe the same
// feature at all?" -- and it must not be mistaken for a quality threshold. A
// drawing 0.5% out is ordinary and its dimension is still the designer's
// intent; a dimension reading 250 against 100 mm of geometry is pointing at
// something else. 5% separates those cleanly and leaves real drawings alone.
// It was briefly 1%, which put ordinary export rounding inside the refusal
// band and would have started rejecting correct files as self-contradictory.
inline constexpr double kReconstructionValueAgreementFraction = 0.05;

// The relationships, asserted at COMPILE time rather than described in prose.
// Spec 19 requires that the tolerances cannot collapse into contradictory
// values, and M5 already learned that a behavioural test for a tolerance
// relationship passes or fails depending on where the geometry started.
static_assert(kReconstructionCoincidenceToleranceMm > kSketchToleranceMm,
              "recognition must be looser than the model's degeneracy floor, or "
              "reconstruction recognises nothing a real drawing contains");
static_assert(kMinReconstructedDimensionMm > kReconstructionCoincidenceToleranceMm,
              "the smallest reconstructed dimension must exceed the coincidence "
              "tolerance, or a reconstructed Length describes a line whose own "
              "endpoints this analysis calls coincident");
static_assert(kMinReconstructedDimensionMm >= kMinSketchDimensionMm,
              "M7 must never plan a dimension the Sketch model would refuse");
static_assert(kReconstructionValueAgreementFraction > 0.0 &&
                  kReconstructionValueAgreementFraction < 1.0,
              "a disagreement band of 0 rejects every real drawing and a band of "
              "1 accepts a value twice the geometry");

// --- Options ----------------------------------------------------------------
//
// Every recogniser is individually switchable, and that is not a convenience.
// Spec 23 Gate F requires proving each one is load-bearing by turning it off
// and watching a gate fail; without these flags that proof would need the
// source edited, which is exactly the mutation procedure AGENTS.md says has
// repeatedly produced false "guarded" claims in this project.
struct ReconstructionOptions {
    // Explicit source dimensions -> Parameter + dimensional constraint.
    bool reconstructExplicitDimensions{true};

    // Individually, so Gate F can name which recogniser it disabled.
    bool recognizeHorizontal{true};
    bool recognizeVertical{true};
    bool recognizeCoincident{true};
    bool placeFix{true};

    double coincidenceToleranceMm{kReconstructionCoincidenceToleranceMm};
    double axisAngleToleranceRad{kReconstructionAxisAngleToleranceRad};
    double valueAgreementFraction{kReconstructionValueAgreementFraction};
};

// --- Analysis (spec 15) -----------------------------------------------------
//
// Reads the document; changes nothing. The plan it returns is a proposal that
// can be inspected, validated, logged or discarded before any document object
// exists.
//
// `dimensions` is the format-neutral annotation from the import boundary. An
// empty list is not an error: a drawing with no dimensions still reconstructs
// its geometric relations, and that is a useful result.
ReconstructionPlan AnalyzeForReconstruction(const PartDocument& document, ObjectId sketchId,
                                            const std::vector<ImportedDimension2D>& dimensions,
                                            const ReconstructionOptions& options = {});

// --- Application (spec 16) --------------------------------------------------

// --- Provenance (spec 26) ---------------------------------------------------
//
// One record per constraint reconstruction created, answering the five
// questions spec 26 requires the diagnostic layer to be able to answer:
// was it explicit or inferred, which source item produced it, which native
// entities does it target, which Parameter controls it, and -- through
// `skipped` on the report -- was anything dropped.
//
// This exists because after application the answers are UNRECOVERABLE. A
// Length the source stated and a Length M7 chose are the same native object,
// and no amount of inspecting the document afterwards can separate them.
//
// PROVENANCE IS NOT IDENTITY. Every field here is either a native id (which is
// identity, and is authoritative on its own) or a human-facing string. Nothing
// resolves, matches or persists anything by this record, and deleting the whole
// report would leave the document exactly as correct as it was.
struct ProvenanceEntry {
    SketchConstraintId constraintId{kInvalidSketchConstraintId};
    std::string constraintKind;
    ReconstructionOrigin origin{ReconstructionOrigin::ExplicitSource};
    // Which source item produced it -- a DXF handle, or the rule that fired.
    // Diagnostic only (spec 7).
    std::string sourceRef;
    std::vector<SketchEntityId> targets;
    // kInvalidObjectId for the non-dimensional kinds. The name is a snapshot
    // for display; the id is what means anything.
    ObjectId parameterId{kInvalidObjectId};
    std::string parameterName;
};

struct ReconstructionReport {
    ObjectId sketchId{kInvalidObjectId};
    std::vector<ProvenanceEntry> entries;
    std::vector<ReconstructionSkip> skipped;

    const ProvenanceEntry* find(SketchConstraintId id) const noexcept;
    std::size_t countByOrigin(ReconstructionOrigin origin) const noexcept;
    // Everything the source stated outright, as opposed to what M7 chose. The
    // distinction spec 3 says must never be silently mixed.
    std::size_t explicitCount() const noexcept {
        return countByOrigin(ReconstructionOrigin::ExplicitSource);
    }
};

// A human-readable rendering, for the UI's diagnostic panel and for logs.
//
// Skipped items are listed even when nothing was skipped ("none"), because a
// report that simply omits the section when it is empty cannot be told from one
// where the question was never asked.
std::string FormatReconstructionReport(const ReconstructionReport& report);

struct ReconstructionResult {
    bool ok{false};
    ObjectId sketchId{kInvalidObjectId};
    // In creation order, which is NOT identity -- returned so a caller can
    // report, select or undo what was made.
    std::vector<ObjectId> createdParameters;
    std::vector<SketchConstraintId> createdConstraints;
    std::size_t skippedCount{0};
    std::string message;
    ReconstructionReport report;

    explicit operator bool() const noexcept { return ok; }
};

// Applies a plan TRANSACTIONALLY (spec 16).
//
// Either every planned Parameter and constraint exists afterwards, or none
// does and the document is exactly as it was: no half-created Parameter, no
// half-created constraint, no phantom graph edge, no dangling binding. The plan
// is validated first, so the common failures cost nothing; anything that still
// fails mid-apply is unwound in reverse creation order.
//
// Rolling back is the whole reason apply is separate from analyze. A function
// that interpreted source data and mutated as it went could not offer this,
// because by the time it discovered the seventh dimension was unusable it would
// already have created six Parameters.
// Whether a plan still DESCRIBES this document.
//
// `ValidatePlan` checks a plan against itself and needs no document. This is
// the other half: every entity the plan names must exist in the sketch it
// names, and every dimensional value must still agree with the geometry it was
// measured from, within the same band the analysis used.
//
// The document id alone cannot do this job. `ObjectId` is a process-local
// counter, and two documents loaded from ONE saved file carry not just
// overlapping ids but identical ones -- independent review applied document A's
// plan to document B and drove B's 250 mm edge with A's Width=100. What
// separates them is not their identity but their geometry, so that is what is
// checked.
//
// It also covers the narrower cases for free: an entity deleted between analyze
// and apply, and geometry edited in between.
PlanValidation ValidatePlanAgainstDocument(const PartDocument& document,
                                           const ReconstructionPlan& plan);

ReconstructionResult ApplyReconstruction(PartDocument& document, const ReconstructionPlan& plan);

// Analyze, validate and apply in one call, for callers with nothing to inspect.
// Exactly equivalent to the three steps; it exists so the common path does not
// re-implement them slightly differently each time.
ReconstructionResult ReconstructSketch(PartDocument& document, ObjectId sketchId,
                                       const std::vector<ImportedDimension2D>& dimensions,
                                       const ReconstructionOptions& options = {});

// --- Idempotence (spec 25) --------------------------------------------------
//
// True when this sketch already carries reconstructed constraints, judged by
// NATIVE state -- dimensional constraints bound to Parameters -- rather than by
// a provenance flag, so it stays true after save/load and cannot be defeated by
// a document that never recorded provenance.
//
// M7.1's contract: re-running reconstruction on an already-reconstructed sketch
// is refused rather than duplicated, so the Width/Width_2/Width_3 accumulation
// spec 25 forbids cannot happen by accident.
bool SketchAlreadyReconstructed(const PartDocument& document, ObjectId sketchId);

} // namespace paramcad
