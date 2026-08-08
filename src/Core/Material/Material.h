#pragma once

#include "Core/Document/ObjectId.h"
#include <string>

namespace paramcad {

struct ContactProperties {
    double staticFriction{0.0};
    double dynamicFriction{0.0};
    double restitution{0.0};
};

class Material {
public:
    Material(std::string name, double densityKgPerM3);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    double density() const noexcept { return densityKgPerM3_; }

    double elasticModulusPa{0.0};
    double poissonRatio{0.0};
    double yieldStrengthPa{0.0};
    ContactProperties contact{};

private:
    ObjectId id_;
    std::string name_;
    double densityKgPerM3_;
};

} // namespace paramcad
