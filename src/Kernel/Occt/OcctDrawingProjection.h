#pragma once

#include "Core/Kernel/DrawingProjection.h"
#include "Core/Kernel/KernelShape.h"

namespace paramcad {

// Hidden-line removal, in its own translation unit because it is the only
// place in this kernel that pulls in TKHLR.
DrawingProjectionResult ProjectShapeForDrawing(const KernelShape& shape,
                                               const DrawingProjectionRequest& request);

} // namespace paramcad
