#include "Core/Material/Material.h"
#include <utility>

namespace paramcad {

Material::Material(std::string name, double densityKgPerM3)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), densityKgPerM3_(densityKgPerM3) {}

} // namespace paramcad
