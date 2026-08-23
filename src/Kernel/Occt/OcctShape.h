#pragma once

#include "Core/Kernel/KernelShape.h"
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>
#include <cstdint>
#include <map>
#include <utility>

namespace paramcad {

// WHO MADE WHICH FACE (M17.13, ADR-M17-035).
//
// The provenance a `CreatedBy` query is answered from: "the faces Pocket001
// created", carried on the shape itself and passed forward as later features
// consume it.
//
// The key is an opaque TAG, not an ObjectId, and that is deliberate. The
// kernel layer knows nothing about documents, features or ids (ADR-M4-003);
// it is handed a number by whoever built the shape and hands the same number
// back. Core puts a feature's id in it. Nothing here has to agree with the
// document about what the number means.
//
// Faces rather than edges, because a boolean's history speaks in faces and an
// edge belongs to two of them -- "the edges Pocket001 created" is derived by
// taking the edges of its faces, which is both what a user means and the only
// answer that stays stable when a downstream fillet replaces the edges.
using ShapeProvenance = std::map<std::uint64_t, TopTools_IndexedMapOfShape>;

// Concrete IShapeHandle wrapping an OCCT TopoDS_Shape (ADR-M3-001). This is
// the only OCCT-visible shape type in the codebase; src/Core never sees it
// and only ever holds the opaque KernelShape/IShapeHandle base. Only
// translation units under src/Kernel/Occt (which link OCCT) ever
// dynamic_cast a KernelShape's handle down to this concrete type.
class OcctShape final : public IShapeHandle {
public:
    explicit OcctShape(TopoDS_Shape shape) noexcept : shape_(std::move(shape)) {}
    OcctShape(TopoDS_Shape shape, ShapeProvenance provenance) noexcept
        : shape_(std::move(shape)), provenance_(std::move(provenance)) {}

    const TopoDS_Shape& shape() const noexcept { return shape_; }

    // Empty for a shape nobody tagged -- every shape built before provenance
    // existed, and every one built by a path that has no feature to name. An
    // empty table means a CreatedBy query matches nothing, which is refused
    // loudly rather than treated as "everything".
    const ShapeProvenance& provenance() const noexcept { return provenance_; }

private:
    TopoDS_Shape shape_;
    ShapeProvenance provenance_;
};

} // namespace paramcad
