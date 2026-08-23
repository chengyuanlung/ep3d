#pragma once

#include "Core/Kernel/FaceQuery.h"
#include "Core/Kernel/KernelShape.h"

#include <TopoDS_Face.hxx>

#include <string>

namespace paramcad {

// The face a query names, as TOPOLOGY (M20).
//
// Shares ResolveFaceQuery's narrowing -- one place decides which face a
// sentence means, and the two entry points differ only in whether the caller
// wants the geometry or the face itself. A shell has to hand OCCT the face to
// open and a draft the face to taper; neither can be done with a plane.
//
// THIS HEADER NAMES AN OCCT TYPE, so only translation units under
// src/Kernel/Occt may include it. The neutral half is in OcctFaceQuery.h,
// which the viewer includes and which is built without OCCT's include paths.
//
// Returns a null face and fills `why` on refusal.
TopoDS_Face FaceForQuery(const KernelShape& shape, const FaceQuery& query, std::string& why);

} // namespace paramcad
