#pragma once

#include <memory>

namespace paramcad {

// Pure opaque marker (ADR-M3-001). Only a Kernel/Occt translation unit can
// meaningfully dynamic_cast a KernelShape's handle to a concrete type (e.g.
// OcctShape); Core never inspects it.
class IShapeHandle {
public:
    virtual ~IShapeHandle() = default;
};

// Kernel-neutral runtime shape handle. Copyable value type wrapping a
// shared_ptr (CodingRule 6 -- no owning raw pointer). Default-constructed
// state is invalid/empty. Carries no ObjectId and no comparable identity:
// BoxFeature::id() (from Feature) remains the only persistent identity for a
// feature's shape (shape identity rule, spec 7). Never serialize a
// KernelShape or IShapeHandle -- runtime-only, exactly like ObjectRegistry
// handles.
class KernelShape {
public:
    KernelShape() noexcept = default;
    explicit KernelShape(std::shared_ptr<IShapeHandle> handle) noexcept;

    bool isValid() const noexcept;
    const IShapeHandle* handle() const noexcept;

private:
    std::shared_ptr<IShapeHandle> handle_;
};

} // namespace paramcad
