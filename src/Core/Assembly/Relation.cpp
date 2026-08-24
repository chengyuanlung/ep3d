#include "Core/Assembly/Relation.h"

#include <cmath>

namespace paramcad {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
} // namespace

std::string_view toString(RelationType type) noexcept {
    switch (type) {
        case RelationType::Gear: return "Gear";
        case RelationType::RackAndPinion: return "RackAndPinion";
        case RelationType::Screw: return "Screw";
        case RelationType::Linear: return "Linear";
    }
    return "Gear";
}

Relation::Relation(ObjectId id, std::string name, RelationType type, CoupledFreedom driver,
                   CoupledFreedom driven, double ratio, bool reversed)
    : id_(id),
      name_(std::move(name)),
      type_(type),
      driver_(driver),
      driven_(driven),
      ratio_(ratio),
      reversed_(reversed) {}

double Relation::valueFor(double driverValue) const noexcept {
    const double sign = reversed_ ? -1.0 : 1.0;
    switch (type_) {
        case RelationType::Gear:
        case RelationType::Linear:
            // A PLAIN RATIO. Both freedoms are the same kind of quantity --
            // two angles or two distances -- so no conversion exists to get
            // wrong.
            return sign * ratio_ * driverValue;
        case RelationType::RackAndPinion:
        case RelationType::Screw:
            // MILLIMETRES PER TURN, converted HERE and nowhere else. A lead
            // screw is quoted as "4 mm per revolution" and a rack as the
            // travel of one pinion turn; the mate stores radians. Dividing by
            // 2*pi at each call site is how half of them end up dividing by
            // pi instead.
            return sign * ratio_ * (driverValue / kTwoPi);
    }
    return 0.0;
}

bool Relation::couplesAFreedomToItself() const noexcept {
    return driver_.mateId == driven_.mateId && driver_.component == driven_.component;
}

std::size_t FirstFreeComponentOfKind(const MateFreedom& freedom, bool rotation) noexcept {
    for (std::size_t c = 0; c < kMateComponentCount; ++c)
        if (freedom.free[c] && IsRotation(static_cast<MateComponent>(c)) == rotation) return c;
    return kMateComponentCount;
}

std::string WhyRelationIsRefused(RelationType type, const CoupledFreedom& driver,
                                 const CoupledFreedom& driven) {
    if (driver.mateId == kInvalidObjectId || driven.mateId == kInvalidObjectId)
        return "a relation needs two freedoms to couple";
    if (driver.mateId == driven.mateId && driver.component == driven.component)
        return "a relation cannot couple a freedom to itself";

    const bool driverTurns = IsRotation(driver.component);
    const bool drivenTurns = IsRotation(driven.component);

    switch (type) {
        case RelationType::Gear:
            if (!driverTurns || !drivenTurns)
                return "a gear couples two ROTATIONS; one of these is a translation";
            if (driver.mateId == driven.mateId)
                return "a gear couples two different mates; a mate cannot gear to itself";
            return {};
        case RelationType::Linear:
            if (driverTurns || drivenTurns)
                return "a linear relation couples two TRANSLATIONS; one of these is a rotation";
            if (driver.mateId == driven.mateId)
                return "a linear relation couples two different mates";
            return {};
        case RelationType::RackAndPinion:
            if (!driverTurns)
                return "a rack and pinion is driven by the PINION's rotation";
            if (drivenTurns)
                return "a rack and pinion drives the RACK's translation";
            if (driver.mateId == driven.mateId)
                return "a rack and pinion couples two mates; for one mate, use a screw";
            return {};
        case RelationType::Screw:
            // THE ONE-MATE CASE, and the reason this whole model couples
            // freedoms rather than mates (§20.5). A screw turns and advances
            // at once, and both of those belong to the SAME cylindrical mate.
            if (driver.mateId != driven.mateId)
                return "a screw couples one mate's own rotation to its own travel; "
                       "for two mates, use a rack and pinion";
            if (!driverTurns) return "a screw is driven by its rotation";
            if (drivenTurns) return "a screw drives its travel, which is a translation";
            return {};
    }
    return "that is not a relation type";
}

} // namespace paramcad
