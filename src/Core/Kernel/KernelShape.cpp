#include "Core/Kernel/KernelShape.h"
#include <utility>

namespace paramcad {

KernelShape::KernelShape(std::shared_ptr<IShapeHandle> handle) noexcept
    : handle_(std::move(handle)) {}

bool KernelShape::isValid() const noexcept {
    return handle_ != nullptr;
}

const IShapeHandle* KernelShape::handle() const noexcept {
    return handle_.get();
}

} // namespace paramcad
