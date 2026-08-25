#pragma once

#include "Core/Kernel/KernelShape.h"

#include <functional>
#include <string>

namespace paramcad {

class DocumentBase;
struct RecomputeContext;

// "THAT BODY, IN THAT FILE" -- resolved to a solid, ONCE (M32.2).
//
// This was Instance::recompute's first hundred lines, and it stayed there
// while it was the only thing that needed it. A DRAWING VIEW needs the same
// sentence answered the same way -- open the file, decide whether it is a part
// or an assembly, rebuild it from its own features, find the body, hand back
// the tip -- and a second copy is how the two would begin to disagree about
// what an empty body name means, or which of a multi-body part is "the" one.
//
// WHAT IT DOES NOT DO is place the shape. An instance puts it where its frame
// says; a view projects it from where it stands. Placement is the caller's,
// and this hands back the shape in the SOURCE DOCUMENT'S OWN COORDINATES.
struct SourceShapeResult {
    bool ok = false;
    std::string message; // set on refusal, and only then
    KernelShape shape;
    // Which kind of file it turned out to be. Asked of the file's own header
    // rather than of its extension, so a file renamed by hand is still read as
    // what it is.
    bool wasAssembly = false;

    explicit operator bool() const noexcept { return ok; }
};

// `bodyName` EMPTY means "the only one" for a part, and a part holding several
// bodies with no name given is refused WITH THE NAMES -- taking the first
// would make the caller silently mean a different body the day somebody added
// one, which is position-as-identity (ADR-M4-004). For an assembly the name is
// ignored: an assembly is resolved whole.
//
// `sawDocument`, when given, is called with the loaded document just before it
// goes out of scope. An instance uses it to harvest the part's mate
// connectors, which are rebuilt from the file every pass for the same reason
// the solid is. A view has no use for it.
SourceShapeResult ResolveSourceShape(
    const std::string& sourcePath, const std::string& bodyName,
    const RecomputeContext& context,
    const std::function<void(const DocumentBase&)>& sawDocument = {});

// Is this file an assembly? Asked of the header, not the extension.
bool IsAssemblySourceFile(const std::string& path);

} // namespace paramcad
