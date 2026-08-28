#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace paramcad {

class PartDocument;

// M56 -- ONE ANSWER TO "IS THIS A FILE, OR SOMETHING WE MAKE?"
//
// M45 gave the program its first generated part: an instance names
// `std:ISO 4762 M8x30` and a screw is built from the catalogue rather than
// opened from disk. M56 adds a second, `frm:SHS 40x40x3 L=250`, and that is
// the moment the question stops being a special case and starts being a KIND.
//
// The alternative was to copy M45's block in the resolver and add `frm:` to it.
// That is the defect this project keeps paying for -- and it had already begun:
// the exploded parts list opens every instance's source path as a FILE to see
// what is inside it, and a catalogue path is not a file, so an assembly holding
// a single M8 screw could not be counted at all. One place knows about schemes,
// so a third scheme cannot reach only two of the three callers.
bool IsLibraryPath(std::string_view path) noexcept;

// THE PART A LIBRARY PATH NAMES, built from its numbers.
//
// `part` is null exactly when `why` is set, and `why` names WHICH part of the
// path could not be understood -- the section, the length or the cut -- because
// "not a part this library holds" sends a reader to check all three.
struct LibraryBuild {
    std::unique_ptr<PartDocument> part;
    std::string why;

    explicit operator bool() const noexcept { return part != nullptr; }
};
//
// `kernel` is needed only by the springs (M60), whose geometry is a helix and
// therefore cannot come from a sketch the way every other library part's does.
// Passed to all of them rather than to one, because a caller should not have
// to know which kinds need it.
LibraryBuild BuildLibraryPart(std::string_view path, class IGeometryKernel& kernel);

} // namespace paramcad
