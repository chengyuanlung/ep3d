#include "Core/Kernel/EdgeQuery.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace paramcad {

namespace {

std::string DirectionWords(Vec3 direction) {
    constexpr double kAxis = 0.999;
    if (direction.z > kAxis) return "top";
    if (direction.z < -kAxis) return "bottom";
    if (direction.x > kAxis) return "right";
    if (direction.x < -kAxis) return "left";
    if (direction.y > kAxis) return "back";
    if (direction.y < -kAxis) return "front";
    return {};
}

std::string Number(double value) {
    // Two decimals is enough to recognise a direction and short enough to read
    // inside a sentence.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

std::string Vector(Vec3 v) {
    return "(" + Number(v.x) + ", " + Number(v.y) + ", " + Number(v.z) + ")";
}

std::string DescribeOne(const EdgeQuery& query) {
    if (std::holds_alternative<AllEdges>(query)) return "every edge";
    if (const auto* face = std::get_if<EdgesOfExtremeFace>(&query)) {
        const std::string words = DirectionWords(face->direction);
        return words.empty() ? "the outermost face towards " + Vector(face->direction) +
                                   " and its edges"
                             : "the " + words + " face's edges";
    }
    if (const auto* face = std::get_if<EdgesOfFace>(&query)) {
        // The face query's own words, so the two vocabularies read as one
        // sentence rather than two.
        return "the edges of " + DescribeFaceQuery(face->face);
    }
    if (const auto* made = std::get_if<EdgesCreatedBy>(&query)) {
        // By ID, because that is what the query holds and what a user can check
        // against the tree. Naming the feature would mean reaching into the
        // document from a function that deliberately does not know about one.
        return "the edges of everything feature " + std::to_string(made->featureId) + " created";
    }
    const auto& parallel = std::get<EdgesParallelTo>(query);
    const std::string words = DirectionWords(parallel.direction);
    if (words == "top" || words == "bottom") return "every vertical edge";
    return "every edge parallel to " + Vector(parallel.direction);
}

} // namespace

std::string DescribeEdgeSelection(const EdgeSelection& selection) {
    // EMPTY IS NOT "everything". A selection that matched nothing and a
    // selection that asked for everything produce opposite solids, and a
    // description that read the same for both would be the one thing a user
    // checks before pressing OK.
    if (selection.empty()) return "nothing selected";
    std::string text;
    for (const EdgeQuery& query : selection) {
        if (!text.empty()) text += ", plus ";
        text += DescribeOne(query);
    }
    return text;
}

EdgeSelectionPick SelectionForPickedFace(const FacePlane& picked,
                                         const std::vector<FacePlane>& faces) {
    EdgeSelectionPick pick;
    if (!picked.isFace) {
        pick.message = "Click a flat face of the solid to choose its edges";
        return pick;
    }
    if (!picked.planar) {
        pick.message = "That face is curved; edges are chosen by picking a flat face";
        return pick;
    }

    const auto dot = [](Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const double length = std::sqrt(dot(picked.normal, picked.normal));
    if (!std::isfinite(length) || length < 1e-12) {
        pick.message = "That face has no usable direction";
        return pick;
    }
    const Vec3 n{picked.normal.x / length, picked.normal.y / length, picked.normal.z / length};
    const double pickedOffset = dot(picked.point, n);

    // Is this face the OUTERMOST one facing that way? Only then does
    // "the outermost face towards n" name the face the user actually clicked.
    constexpr double kFacing = 0.9;   // the same cone the kernel's query uses
    constexpr double kSameMm = 1e-6;  // the project's length tolerance
    double furthest = pickedOffset;
    for (const FacePlane& face : faces) {
        if (!face.planar) continue;
        if (dot(face.normal, n) < kFacing) continue;
        furthest = std::max(furthest, dot(face.point, n));
    }
    if (furthest > pickedOffset + kSameMm) {
        // An INNER face -- a pocket floor, a counterbore. "The outermost face
        // towards +Z" is the top face, not this one, so that sentence cannot
        // be used. PROVENANCE can: "what the feature that made this face
        // created" names it exactly, and keeps naming it when the geometry
        // moves (M17.13).
        //
        // It selects everything that feature made, not this face alone --
        // a pocket's floor AND its walls -- which is what a user filleting the
        // inside of a pocket means, and the message says so.
        if (picked.createdBy != 0) {
            pick.ok = true;
            pick.selection = EdgeSelection{EdgesCreatedBy{static_cast<ObjectId>(picked.createdBy)}};
            pick.message = DescribeEdgeSelection(pick.selection);
            return pick;
        }
        // No provenance to fall back on: the face was read without its owning
        // shape, or built before any feature claimed it.
        pick.message = "That face is not the outermost one in its direction, and nothing "
                       "recorded which feature made it -- so there is no way to name it that "
                       "would still mean this face after a rebuild.";
        return pick;
    }

    pick.ok = true;
    pick.selection = EdgeSelection{EdgesOfExtremeFace{n}};
    pick.message = DescribeEdgeSelection(pick.selection);
    return pick;
}

} // namespace paramcad
