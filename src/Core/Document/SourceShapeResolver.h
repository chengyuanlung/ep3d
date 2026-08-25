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

// WHAT IS IN THAT FILE, as one number, or 0 when it cannot be read.
//
// A drawing view holds this so the shell can say WHICH views are behind their
// models rather than offering "Update" against everything. It is a fact about
// the disk, not about the document, so it is never serialized: a stamp written
// to a file would describe a previous session and could only mislead.
//
// THE CONTENT, NOT THE MODIFICATION TIME, and this is not belt-and-braces.
//
// The first version hashed `last_write_time` and it was FLAKY -- it passed
// alone and failed in a full run. The cause is not the test: two saves that
// land inside one filesystem timestamp tick are indistinguishable by mtime,
// so a user who edits and saves quickly gets a drawing that says it is up to
// date and shows the old part. That is precisely the failure this whole block
// exists to prevent, and a check that can produce it is worse than no check,
// because it is believed.
//
// The cost is a file read per view per ASK -- not per recompute. The resolver
// already re-reads and rebuilds the whole model on every pass (ADR-M22-003),
// so this is small beside what it guards.
//
// ZERO IS "UNKNOWN", not "empty". A missing file compares unequal to a stamp
// taken from a real one, which is the honest answer -- a view whose model has
// been deleted is out of step in a way no comparison of contents captures.
long long SourceFileStamp(const std::string& path);

} // namespace paramcad
