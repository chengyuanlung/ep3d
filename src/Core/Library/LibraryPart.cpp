#include "Core/Library/LibraryPart.h"

#include "Core/Document/PartDocument.h"
#include "Core/Frame/FrameProfile.h"
#include "Core/Library/CompressionSpring.h"
#include "Core/Library/SpurGear.h"
#include "Core/Library/StandardParts.h"

#include <optional>

namespace paramcad {

bool IsLibraryPath(std::string_view path) noexcept {
    return IsStandardPartPath(path) || IsFrameMemberPath(path) || IsSpurGearPath(path) ||
           IsCompressionSpringPath(path);
}

LibraryBuild BuildLibraryPart(std::string_view path, IGeometryKernel& kernel) {
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
    if (IsSpurGearPath(path)) {
        const std::optional<SpurGear> gear = SpurGearOfPath(path);
        if (!gear) {
            // THE REASON, WHEN THERE IS ONE. A gear path fails for two
            // unrelated reasons -- it is not written the way a gear path is
            // written, or it names a gear nobody can cut -- and undercut is
            // the second kind. Saying only "not a gear" would send somebody to
            // check their typing about a tooth count that is the real problem.
            const std::optional<SpurGear> shape =
                ParseGearDesignation(path.substr(kSpurGearScheme.size()));
            out.why = shape ? std::string(path) + ": " + WhyGearRefused(*shape)
                            : std::string(path) +
                                  " is not written the way a gear is -- it wants a module, a "
                                  "tooth count and a width, as in gear:m2 z20 b10";
            return out;
        }
        out.part = BuildSpurGear(*gear);
        if (!out.part) out.why = std::string(path) + " could not be built";
        return out;
    }
    if (IsCompressionSpringPath(path)) {
        const std::optional<CompressionSpring> spring = CompressionSpringOfPath(path);
        if (!spring) {
            // THE REASON, WHEN THERE IS ONE -- M58's split, for M58's reason.
            // "Not a spring" would send somebody to check their typing about a
            // spring index that is the real problem.
            const std::optional<CompressionSpring> read =
                ParseSpringDesignation(path.substr(kCompressionSpringScheme.size()));
            out.why = read ? std::string(path) + ": " + WhySpringRefused(*read)
                           : std::string(path) +
                                 " is not written the way a spring is -- it wants a wire, a "
                                 "mean diameter, a coil count and a free length, as in "
                                 "spr:d2 D16 n8 L50";
            return out;
        }
        out.part = BuildCompressionSpring(*spring, kernel);
        if (!out.part) out.why = std::string(path) + " could not be built";
        return out;
    }
    out.why = std::string(path) + " does not name anything this library makes";
    return out;
}

} // namespace paramcad
