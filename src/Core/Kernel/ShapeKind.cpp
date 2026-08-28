#include "Core/Kernel/ShapeKind.h"

namespace paramcad {

namespace {

struct Row {
    ShapeKind kind;
    std::string_view token;
    std::string_view name;
};

// ONE ROW PER KIND, so a kind cannot be in the parser and missing from a
// message. The same shape M57's formats and M56's sections have.
constexpr Row kKinds[] = {
    {ShapeKind::Empty, "Empty", "nothing"},
    {ShapeKind::Vertex, "Vertex", "a point"},
    {ShapeKind::Wire, "Wire", "a wire"},
    {ShapeKind::Face, "Face", "a single surface"},
    {ShapeKind::Shell, "Shell", "a shell"},
    {ShapeKind::Solid, "Solid", "a solid"},
    {ShapeKind::Compound, "Compound", "several shapes together"},
};

const Row& RowOf(ShapeKind kind) noexcept {
    for (const Row& row : kKinds)
        if (row.kind == kind) return row;
    return kKinds[0];
}

} // namespace

std::string_view toString(ShapeKind kind) noexcept { return RowOf(kind).token; }
std::string_view NameOf(ShapeKind kind) noexcept { return RowOf(kind).name; }

bool ParseShapeKind(std::string_view text, ShapeKind& into) noexcept {
    for (const Row& row : kKinds) {
        if (row.token != text) continue;
        into = row.kind;
        return true;
    }
    return false;
}

std::string WhyNotASolid(ShapeKind kind) {
    switch (kind) {
    case ShapeKind::Solid:
        return {};
    case ShapeKind::Empty:
        return "there is nothing here";
    case ShapeKind::Vertex:
    case ShapeKind::Wire:
        return "this is " + std::string(NameOf(kind)) +
               ", which has no surface and encloses nothing";
    case ShapeKind::Face:
        return "this is a single surface. It has area and no thickness, so there is no "
               "material in it to measure -- thicken it, or sew it to the rest of the "
               "surfaces it belongs with";
    case ShapeKind::Shell:
        // THE ONE WORTH SPELLING OUT. Somebody with a closed shell has done
        // nearly all the work and would otherwise be handed a zero.
        return "this is a shell: surfaces joined into a skin. Even a shell that closes "
               "perfectly is not a solid until the kernel is told it bounds material, so "
               "there is nothing here to weigh yet -- thicken it, or make a solid of it";
    case ShapeKind::Compound:
        return "this is several shapes at once, and which of them was meant is not "
               "something this can decide";
    }
    return "this is not a solid";
}

} // namespace paramcad
