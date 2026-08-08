#pragma once

#include "Core/Document/ObjectId.h"
#include <string>

namespace paramcad {

class Sketch {
public:
    explicit Sketch(std::string name);
    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }

private:
    ObjectId id_;
    std::string name_;
};

} // namespace paramcad
