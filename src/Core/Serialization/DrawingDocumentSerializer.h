#pragma once

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/SerializationError.h"

#include <iosfwd>
#include <memory>
#include <string>

namespace paramcad {

struct DrawingLoadResult {
    std::unique_ptr<DrawingDocument> document;
    SerializationError error = SerializationError::None;
    std::string message;

    explicit operator bool() const noexcept { return document != nullptr; }
};

// THE SAME SHAPE THE OTHER TWO SERIALIZERS HAVE, deliberately (M32).
//
// A drawing is a third document type, not a third file format: same header,
// same version ceiling, same "validate everything the loader checks BEFORE a
// byte is written" rule (ADR-M3-008). What differs is the arrays.
SaveResult saveDrawingDocument(const DrawingDocument& document, std::ostream& out);
SaveResult saveDrawingDocumentToFile(const DrawingDocument& document, const std::string& path);

DrawingLoadResult loadDrawingDocument(std::istream& in);
DrawingLoadResult loadDrawingDocumentFromFile(const std::string& path);

} // namespace paramcad
