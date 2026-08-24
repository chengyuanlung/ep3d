#pragma once

#include "Core/Assembly/MateFreedom.h"
#include "Core/Document/ObjectId.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace paramcad {

// The four relations of roadmap §20.5 (M31).
//
// A relation COUPLES TWO FREEDOMS so that one follows the other. It is not a
// mate: a mate says where two parts are relative to each other, and a relation
// says that two numbers a mate solve was free to choose must move together.
enum class RelationType {
    Gear,          // two rotations, in a fixed ratio
    RackAndPinion, // a rotation and a translation
    Screw,         // a rotation and a translation OF THE SAME MATE
    Linear,        // two translations, in a fixed ratio
};

std::string_view toString(RelationType type) noexcept;

// ONE FREEDOM: which component of which mate.
//
// A MATE ID, never an instance pair. §20.5's first reading of the reference
// model says so and gives the reason: a relation's INPUT is a mate, so it must
// hold a MateId -- a stable identity (A03) -- and holding the two instances
// instead would make it ambiguous the moment two mates join the same pair.
struct CoupledFreedom {
    ObjectId mateId{kInvalidObjectId};
    MateComponent component{MateComponent::RZ};
};

// A relation between two freedoms.
//
// ONE SHAPE FOR BOTH ARITIES, and this is the whole design decision.
//
// §20.5's second reading is that Screw takes a SINGLE mate and couples that
// mate's own rotation to its own translation -- so "a relation is a
// relationship between two THINGS" is the wrong primitive, and a model built
// on it would need Screw as a special case forever.
//
// Couple two FREEDOMS instead and both arities are the same object: a gear
// names a rotation on each of two mates, a screw names the rotation and the
// translation of one. Nothing here asks how many mates are involved, because
// that was never the question.
class Relation {
public:
    Relation(ObjectId id, std::string name, RelationType type, CoupledFreedom driver,
             CoupledFreedom driven, double ratio, bool reversed);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    RelationType type() const noexcept { return type_; }
    const CoupledFreedom& driver() const noexcept { return driver_; }
    const CoupledFreedom& driven() const noexcept { return driven_; }

    // WHAT THE NUMBER MEANS depends on the type, and the type is the only
    // thing that says so:
    //
    //   Gear, Linear          a plain ratio. driven = ratio * driver.
    //   RackAndPinion, Screw  MILLIMETRES PER TURN, which is how a lead screw
    //                         and a rack are both specified and quoted. The
    //                         conversion to radians happens in ONE place
    //                         (valueFor below) so no call site can get 2*pi
    //                         wrong on its own.
    double ratio() const noexcept { return ratio_; }
    bool reversed() const noexcept { return reversed_; }
    void setRatio(double ratio) noexcept { ratio_ = ratio; }
    void setReversed(bool reversed) noexcept { reversed_ = reversed; }

    // What the DRIVEN freedom must be, given what the driver currently is.
    //
    // THE ONE PLACE the coupling is computed. The solve calls it, the DOF
    // report calls it and the UI calls it, so there is no second formula to
    // disagree with this one about which way "reversed" goes.
    double valueFor(double driverValue) const noexcept;

    // Whether this relation's two freedoms are the same one. A relation of a
    // freedom to itself is not a coupling, it is an equation that says a
    // number equals a multiple of itself -- true only at zero, or for a ratio
    // of one, and meaningless either way.
    bool couplesAFreedomToItself() const noexcept;

private:
    ObjectId id_;
    std::string name_;
    RelationType type_;
    CoupledFreedom driver_;
    CoupledFreedom driven_;
    double ratio_;
    bool reversed_;
};

// Whether `component` is a rotation. Relations care, because millimetres per
// turn is only meaningful from a rotation to a translation.
constexpr bool IsRotation(MateComponent component) noexcept {
    return component == MateComponent::RX || component == MateComponent::RY ||
           component == MateComponent::RZ;
}

// What a relation of this type REQUIRES of its two freedoms, as a sentence, or
// empty when the pair is acceptable.
//
// One function, because "is this relation valid" is one question and the four
// answers differ only in which components they will take. A per-type check
// spread over the document, the serializer and the UI is the shape this
// project keeps removing.
std::string WhyRelationIsRefused(RelationType type, const CoupledFreedom& driver,
                                 const CoupledFreedom& driven);

// Which KIND of freedom each end of this type is: true for a rotation.
//
//   Gear           rotation -> rotation
//   Linear         translation -> translation
//   RackAndPinion  rotation -> translation   (the pinion drives the rack)
//   Screw          rotation -> translation   (of the same mate)
//
// The same table WhyRelationIsRefused enforces, readable forwards -- so a
// caller that has to CHOOSE a pair does not have to guess and then be
// refused.
constexpr bool RelationDriverIsRotation(RelationType type) noexcept {
    return type != RelationType::Linear;
}
constexpr bool RelationDrivenIsRotation(RelationType type) noexcept {
    return type == RelationType::Gear;
}

// The freedom a user means when they pick a mate for one end of a relation:
// that mate's FIRST FREE component of the kind this end needs.
//
// ONE RULE, HERE. The menu needs it, a script would need it, and a second copy
// is how the toolbar and the loader end up disagreeing about which axis a gear
// turns on. `kMateComponentCount` is returned when the mate has no freedom of
// that kind -- which is a refusal the caller must report, not work around.
std::size_t FirstFreeComponentOfKind(const MateFreedom& freedom, bool rotation) noexcept;

} // namespace paramcad
