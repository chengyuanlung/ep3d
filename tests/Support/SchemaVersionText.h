#pragma once

#include "Core/Serialization/PartDocumentSerializer.h"

#include <cstddef>
#include <string>

namespace paramcad::testing {

// Editing a SAVED FILE'S schema version, without spelling the current one out.
//
// Back-compatibility tests work by taking a file this build just wrote and
// relabelling it as an older version, to prove the loader still reads what an
// older EP3D would have produced. They used to do that by searching for the
// literal `"schemaVersion": 20` -- so every schema bump turned four unrelated
// suites red for a reason that had nothing to do with what they test. The
// M17.19 bump did exactly that, and so did the one before it.
//
// ONE test pins the literal on purpose, because a bump that nobody noticed is
// worth an alarm. Everything else asks.

inline std::string SchemaVersionField(int version) {
    return "\"schemaVersion\": " + std::to_string(version);
}

inline std::string CurrentSchemaVersionField() {
    return SchemaVersionField(CurrentSchemaVersion());
}

// `saved` relabelled as `version`. Returns it unchanged if the stamp is not
// there -- the caller's own assertion on the result is what reports that, and a
// throw here would blame the helper for the document's problem.
inline std::string WithSchemaVersion(std::string saved, int version) {
    const std::string current = CurrentSchemaVersionField();
    const std::size_t at = saved.find(current);
    if (at == std::string::npos) return saved;
    return saved.replace(at, current.size(), SchemaVersionField(version));
}

} // namespace paramcad::testing
