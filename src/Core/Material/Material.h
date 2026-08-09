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
    // Restore constructor (deserialization): keeps the persisted id and full
    // record (ADR-M3-005). Advances the id generator past the id so future
    // ids cannot collide.
    Material(ObjectId id, std::string name, double densityKgPerM3, double elasticModulusPa,
             double poissonRatio, double yieldStrengthPa, ContactProperties contact);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    double density() const noexcept { return densityKgPerM3_; }
    // ADR-M3-005: mirrors Parameter::setValue's role as a dirty-source bridge
    // (via PartDocument::setMaterialDensity, which also marks the graph node
    // dirty). Density-only changes must not alter geometry (spec 14).
    void setDensity(double densityKgPerM3) noexcept { densityKgPerM3_ = densityKgPerM3; }

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
