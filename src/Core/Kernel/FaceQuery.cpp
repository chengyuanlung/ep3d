#include "Core/Kernel/FaceQuery.h"

#include <cstdio>

namespace paramcad {

namespace {

// Shared with DescribeEdgeSelection's wording on purpose: a user reading "the
// top face" in one place and "the +Z face" in another would reasonably wonder
// whether they are the same thing.
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
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

std::string Vector(Vec3 v) {
    return "(" + Number(v.x) + ", " + Number(v.y) + ", " + Number(v.z) + ")";
}

} // namespace

std::string DescribeFaceQuery(const FaceQuery& query) {
    // EMPTY IS NOT "any face". A query that names nothing and a query that
    // names everything are different failures, and only one of them is worth
    // telling a user they can fix.
    if (query.empty()) return "no face named";

    std::string text = "the ";
    if (query.extremeTowards.has_value()) {
        const std::string words = DirectionWords(*query.extremeTowards);
        text += words.empty() ? "outermost face towards " + Vector(*query.extremeTowards)
                              : words + " face";
    } else if (query.facing.has_value()) {
        const std::string words = DirectionWords(*query.facing);
        text += words.empty() ? "face pointing " + Vector(*query.facing) : words + "-facing face";
    } else {
        text += "face";
    }

    // Provenance last, because it reads as the qualifier it is: "the top face
    // OF WHAT feature 12 made".
    if (query.createdBy.has_value())
        text += " of what feature " + std::to_string(*query.createdBy) + " made";
    // Both direction conditions set is unusual but legal -- say both rather
    // than silently showing one.
    if (query.extremeTowards.has_value() && query.facing.has_value())
        text += ", pointing " + Vector(*query.facing);
    return text;
}

std::string DescribeFaceSelection(const FaceSelection& selection) {
    if (selection.empty()) return "no faces";
    std::string text;
    for (const FaceQuery& query : selection) {
        if (!text.empty()) text += ", and ";
        text += DescribeFaceQuery(query);
    }
    return text;
}

} // namespace paramcad
