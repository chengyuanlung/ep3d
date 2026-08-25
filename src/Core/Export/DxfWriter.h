#pragma once

#include "Core/Drawing/DrawingDocument.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace paramcad {

// WRITING DXF (M35.5).
//
// IN CORE, WITH NO LIBRARY BEHIND IT.
//
// Reading DXF needs a parser, and that parser is GPL and lives behind a neutral
// interface in exactly one translation unit (ADR-M6-001/003). WRITING it needs
// none of that: a DXF is a list of integer group codes and values, one per
// line, and emitting one is less work than pulling in something that can also
// read them. So this has no dependency, no licence reach, and can be tested by
// the Core suite.
//
// R12 (AC1009), deliberately.
//
// It is the version every CAD program made in the last thirty years reads
// without argument, and the drawing tables M33 took from EasyCad -- ACI colour,
// lineweight in hundredths, layer flags, signed dash patterns, bulges -- are
// all R12's own model, so almost nothing has to be translated. What R12 costs
// is the ELLIPSE entity, which does not exist in it; an ellipse is written as
// a polyline and the result SAYS SO rather than pretending the file is
// lossless.

// WHAT COULD NOT BE SAID EXACTLY, in words a user can act on.
//
// A writer that silently approximated would produce a file that opens cleanly
// and is subtly not the drawing -- which is worse than one that refuses,
// because nobody checks a file that opened.
struct DxfWriteLoss {
    std::string what;   // "ELLIPSE"
    std::string detail; // why, and what was written instead
};

struct DxfWriteResult {
    bool ok = false;
    std::string why;
    std::size_t entities = 0;
    std::vector<DxfWriteLoss> losses;

    explicit operator bool() const noexcept { return ok; }
};

// Writes `document`'s sheet contents to `out` as R12 DXF.
//
// WHAT GOES IN: the authored geometry, the layer and linetype tables, the
// dimensions, and every projected view's curves FLATTENED INTO PLACE.
//
// A DXF has no concept of a view: there is no way to say "this group of curves
// is a front view of that part at 1:2, and it updates". So a view's curves are
// written at their position on the paper with the scale already applied, which
// is what they LOOK like -- and the associativity is left behind, on purpose,
// because the alternative is inventing a private extension nothing else reads.
// The header comment in the file says so, so whoever opens it knows.
DxfWriteResult WriteDxf(const DrawingDocument& document, std::ostream& out);
DxfWriteResult WriteDxfFile(const DrawingDocument& document, const std::string& path);

} // namespace paramcad
