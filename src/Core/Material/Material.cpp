#include "Core/Material/Material.h"
#include <utility>

namespace paramcad {

Material::Material(std::string name, double densityKgPerM3)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), densityKgPerM3_(densityKgPerM3) {}

Material::Material(ObjectId id, std::string name, double densityKgPerM3, double elasticModulusPa_,
                   double poissonRatio_, double yieldStrengthPa_, ContactProperties contact_)
    : id_(RestoreObjectId(id)), name_(std::move(name)), densityKgPerM3_(densityKgPerM3) {
    elasticModulusPa = elasticModulusPa_;
    poissonRatio = poissonRatio_;
    yieldStrengthPa = yieldStrengthPa_;
    contact = contact_;
}

} // namespace paramcad
