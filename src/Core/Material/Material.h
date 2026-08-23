#pragma once

#include "Core/Document/ObjectId.h"
#include <string>
#include <utility>

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


    double elasticModulusPa{0.0};
    double poissonRatio{0.0};
    double yieldStrengthPa{0.0};
    ContactProperties contact{};

private:
    // PRIVATE with PartDocument as the only caller, completing round 3's
    // `material()`-returns-const fix and round 4's registry projection
    // (R2R4-M1). Those two close the CONST doors; this closes the last
    // non-const one -- `addMaterial` hands back a `Material&`, so
    // `doc.addMaterial(...).setDensity(x)` changed the density with no graph
    // dirtying at all, and the cached mass went on reading `valid`. The facade
    // (`PartDocument::setMaterialDensity`) is the only way in, and it dirties.
    friend class PartDocument;

    void setDensity(double densityKgPerM3) noexcept { densityKgPerM3_ = densityKgPerM3; }

    ObjectId id_;
    // PRIVATE with PartDocument as the only caller (M17.16, ADR-M17-039).
    //
    // A rename is ONE undo step and must refuse a duplicate; both decisions
    // live in PartDocument::renameObject, and a public setter here would be a
    // way around both. Every other name-writing rule in this file is enforced
    // the same way rather than described in a comment.
    friend class PartDocument;
    void setName(std::string name) { name_ = std::move(name); }

    std::string name_;
    double densityKgPerM3_;
};

} // namespace paramcad
