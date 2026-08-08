#include "Core/Sketch/Sketch.h"
#include <utility>

namespace paramcad {

Sketch::Sketch(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

} // namespace paramcad
