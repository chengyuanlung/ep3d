#include "Core/Library/LibraryPart.h"

#include "Core/Document/PartDocument.h"
#include "Core/Frame/FrameProfile.h"
#include "Core/Library/StandardParts.h"

#include <optional>

namespace paramcad {

bool IsLibraryPath(std::string_view path) noexcept {
    return IsStandardPartPath(path) || IsFrameMemberPath(path);
}

LibraryBuild BuildLibraryPart(std::string_view path) {
    LibraryBuild out;
    if (IsStandardPartPath(path)) {
        const std::optional<FastenerSpec> spec = FastenerOfPath(path);
        if (!spec) {
            out.why = std::string(path) +
                      " is not a part this library holds -- check the standard, the size "
                      "and, for a screw, that the length is one that is made";
            return out;
        }
        out.part = BuildStandardPart(*spec);
        if (!out.part) out.why = std::string(path) + " could not be built";
        return out;
    }
    if (IsFrameMemberPath(path)) {
        const std::optional<FrameMemberSpec> spec = FrameMemberOfPath(path);
        if (!spec) {
            // WHICH HALF OF THE PATH IS WRONG. A frame path carries a section
            // and a cut, and they fail for unrelated reasons: a section that
            // is not stocked, or a mitre that eats the whole member. Telling
            // the two apart is the difference between changing a number and
            // changing the design.
            out.why = std::string(path) +
                      " is not a member this library can make -- the section has to be one "
                      "the catalogue stocks, and the length and end cuts have to leave a "
                      "member behind";
            return out;
        }
        out.part = BuildFrameMember(*spec);
        if (!out.part) out.why = std::string(path) + " could not be built";
        return out;
    }
    out.why = std::string(path) + " does not name anything this library makes";
    return out;
}

} // namespace paramcad
