#pragma once

#include "Core/Import/ImportedGeometry.h"
#include <string>

namespace paramcad {

// Reads a DXF file into the format-neutral representation (ADR-M6-003).
//
// This header names NO libdxfrw type, so the importer, the document and every
// Core test can use the result without the DXF library being present -- the
// same arrangement IGeometryKernel has for OCCT. `libdxfrw` appears in exactly
// one translation unit, DxfReader.cpp, and in exactly one CMake target.
//
// LICENCE (ADR-M6-001): libdxfrw is GPL-2.0-only, adopted by explicit owner
// decision. That copyleft reaches whatever links this target. Keeping the
// dependency to one translation unit behind a neutral interface is what makes
// replacing it later an exercise in rewriting one file.

enum class DxfReadError {
    None,
    FileNotFound,
    NotReadable,   // present but the parser could not open or decode it
    MalformedFile, // parsed far enough to know it is wrong
};

const char* DxfReadErrorName(DxfReadError error) noexcept;

struct DxfReadResult {
    ImportedSketchGeometry geometry;
    DxfReadError error{DxfReadError::None};
    std::string message;

    explicit operator bool() const noexcept { return error == DxfReadError::None; }
};

// Reads `path`. Never throws; a failure is a result whose status says why
// (spec 11 requires the cause, not a bare "import failed").
//
// Entities of kinds M6 does not import, and supported kinds carrying values the
// model would reject, are recorded in `geometry.skipped` rather than failing the
// read -- the file is legitimate, it simply contains more than M6 handles
// (ADR-M6-005 policy, spec 4: never silently reinterpreted as another kind).
DxfReadResult ReadDxfFile(const std::string& path);

} // namespace paramcad
