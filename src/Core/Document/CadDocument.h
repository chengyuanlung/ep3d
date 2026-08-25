#pragma once

#include "Core/Document/ObjectId.h"
#include <string>

namespace paramcad {

// M32 adds the third. Kept an enum rather than a string because every reader
// of it is a switch, and a switch over a closed set is a compiler error when a
// case is forgotten -- which is how the drawing type got added to every one of
// them in a single pass.
enum class DocumentType { Part, Assembly, Drawing };

class CadDocument {
public:
    explicit CadDocument(std::string name);
    virtual ~CadDocument() = default;

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    virtual DocumentType type() const noexcept = 0;

protected:
    // Restore constructor (deserialization): keeps the persisted id and
    // advances the id generator past it so future ids cannot collide.
    CadDocument(ObjectId id, std::string name);

private:
    ObjectId id_;
    std::string name_;
};

} // namespace paramcad
