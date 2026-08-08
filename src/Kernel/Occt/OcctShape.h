#pragma once

#include "Core/Kernel/KernelShape.h"
#include <TopoDS_Shape.hxx>
#include <utility>

namespace paramcad {

// Concrete IShapeHandle wrapping an OCCT TopoDS_Shape (ADR-M3-001). This is
// the only OCCT-visible shape type in the codebase; src/Core never sees it
// and only ever holds the opaque KernelShape/IShapeHandle base. Only
// translation units under src/Kernel/Occt (which link OCCT) ever
// dynamic_cast a KernelShape's handle down to this concrete type.
class OcctShape final : public IShapeHandle {
public:
    explicit OcctShape(TopoDS_Shape shape) noexcept : shape_(std::move(shape)) {}

    const TopoDS_Shape& shape() const noexcept { return shape_; }

private:
    TopoDS_Shape shape_;
};

} // namespace paramcad
