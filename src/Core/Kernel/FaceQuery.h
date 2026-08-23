#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Kernel/FaceGeometry.h"

#include <optional>
#include <string>
#include <vector>

namespace paramcad {

// WHICH FACE, as a sentence re-answered every rebuild (M17.14, ADR-M17-036).
//
// The same architecture EdgeQuery uses, for the same reason: a stored face is
// an index, and an index points at whatever is now in that position. What
// differs is the shape of the sentence -- an edge selection is a UNION of
// queries ("these edges, plus those"), while naming ONE face is a matter of
// narrowing until only one is left.
//
// So this is a CONJUNCTION: every condition that is set must hold. That is
// what lets "the top face of what the pocket cut" be said at all --
// `createdBy` alone names the floor AND the walls, `extremeTowards` alone
// names the part's own top, and only together do they name the floor.
//
// A query with NOTHING set matches every face. That is never what a caller
// means, so it is refused where it is resolved rather than quietly returning
// the first face the explorer happens to visit.
struct FaceQuery {
    // Made by this feature. The only condition that describes PROVENANCE, and
    // therefore the only one that still means the same thing after the
    // geometry moves (ADR-M17-035).
    std::optional<ObjectId> createdBy;

    // The face lying furthest along this direction among those facing it.
    // "The top face", "the bottom face" -- and, combined with createdBy, "the
    // top face of what this feature made".
    std::optional<Vec3> extremeTowards;

    // The face whose outward normal points this way. Narrows a set that
    // `createdBy` left ambiguous -- a pocket's four walls face four different
    // ways -- without claiming anything about position.
    std::optional<Vec3> facing;

    bool empty() const noexcept {
        return !createdBy.has_value() && !extremeTowards.has_value() && !facing.has_value();
    }
};

// The ONE face a query names, or a refusal saying why not (M17.14).
//
// Never "the first match": a query that narrows to several faces, or to none,
// has no answer -- and inventing one puts a sketch or a fillet somewhere the
// user did not choose and cannot predict. The message carries the count, so
// the fix (add a condition) is obvious from the failure.
struct FaceQueryResult {
    bool ok{false};
    std::string message; // always set, on success and refusal alike
    FacePlane face{};
};

// SEVERAL faces, as several sentences (M20).
//
// A union of queries, exactly as EdgeSelection is -- and for the same reason a
// FaceQuery on its own is a CONJUNCTION: narrowing names one face, and naming
// several means saying it several times. A shell opens "the top face, and the
// face the pocket made"; a draft tapers "these four walls".
//
// Deliberately NOT a FaceQuery with the narrowing relaxed. A query that matched
// several faces would make `resolveFace` ambiguous everywhere it is already
// used, and the ambiguity would surface as "the first face the explorer
// happened to visit" -- which is what naming one face exists to avoid.
//
// An EMPTY selection is a real answer for a draft (nothing to taper is a
// caller mistake) and for a shell (a shell with no opening is a hollow shape
// with no way in). Both refuse it rather than guessing.
using FaceSelection = std::vector<FaceQuery>;

// What the query says, in words a user can check against the part. The one
// place a face query is turned into text, so every surface that shows one says
// the same thing.
std::string DescribeFaceQuery(const FaceQuery& query);

// ...and the same for a selection of them, joined. One place, so a shell's
// diagnostic and a draft's read alike.
std::string DescribeFaceSelection(const FaceSelection& selection);

} // namespace paramcad
