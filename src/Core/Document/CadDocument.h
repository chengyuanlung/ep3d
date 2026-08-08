#pragma once

#include "Core/Document/ObjectId.h"
#include <string>

namespace paramcad {

enum class DocumentType { Part, Assembly };

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
