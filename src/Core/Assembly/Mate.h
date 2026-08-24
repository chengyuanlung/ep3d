#pragma once

#include "Core/Assembly/MateFreedom.h"
#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"

#include <string>
#include <string_view>

namespace paramcad {

// WHAT A MATE TAKES AWAY (M24, roadmap §20.1).
//
// A mate is stated by the degrees of freedom it LEAVES, not by the ones it
// removes, because that is what a user asks for: a hinge is "one rotation
// left", not "five removed".
enum class MateType {
    Fastened,    // 0 left  -- the two connectors are the same place
    Revolute,    // 1 rotation, about the connectors' shared +Z
    Slider,      // 1 translation, along the connectors' shared +Z
    Cylindrical, // 1 rotation + 1 translation, both about/along +Z (M25)
    Ball,        // 3 rotations, about a shared point (M25)
    Planar,      // 2 translations + 1 rotation, in the connectors' XY (M25)
    Parallel,    // an ALIGNMENT mate: it pins the two axes parallel and
                 // nothing else, so it leaves 3 translations and the spin
                 // about the shared axis (M25)
};

std::string_view toString(MateType type) noexcept;

// WHICH COMPONENTS THIS KIND OF MATE LEAVES FREE (M25, ADR-M25-001).
//
// This function IS the mate-type table of roadmap §20.1. Everything else about
// a mate type -- its middle transform, its residuals, the freedoms it leaves --
// is computed from what this returns, so a new type is one entry here and not a
// new idea of what "connected" means anywhere else.
MateFreedom FreedomOf(MateType type) noexcept;

// ONE MATE, BETWEEN TWO MATE CONNECTORS (M24, ADR-M24-002).
//
// Each side is an INSTANCE and a connector NAME on the part that instance
// brings in. Not a connector ObjectId: the connector is defined in the PART
// file and is reused by every instance of it (roadmap §21), so there is no id
// in this document to point at -- and there must not be, because two
// instances of one part would then need two copies of the same connector.
//
// A name is a reference like every other in this project: it is re-resolved
// on every rebuild against whatever that part currently has, so a connector
// that was renamed or deleted in the part FAILS LOUDLY here rather than
// resolving to something else (ADR-M4-004's rule about indices, applied to
// the one place across a document boundary).
//
// WHICH SIDE MOVES is decided by the solve, not stored here: whichever end is
// already placed leads, and the other follows. A mate is a statement about a
// relationship, and storing a direction in it would be a second answer to a
// question the ground already settles.
//
// `value` is the mate's one remaining freedom, in the unit that freedom has:
// RADIANS for a revolute, MILLIMETRES for a slider. A Fastened mate has no
// freedom, so a non-zero value on one is REFUSED at the door rather than
// ignored -- ignoring it would let a user believe they had offset something.
class Mate {
public:
    Mate(std::string name, MateType type, ObjectId leadingInstanceId,
         std::string leadingConnector, ObjectId followingInstanceId,
         std::string followingConnector, MateValues values);
    // Restore constructor (deserialization): keeps the persisted id.
    Mate(ObjectId id, std::string name, MateType type, ObjectId leadingInstanceId,
         std::string leadingConnector, ObjectId followingInstanceId,
         std::string followingConnector, MateValues values);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    MateType type() const noexcept { return type_; }

    ObjectId leadingInstanceId() const noexcept { return leadingInstanceId_; }
    const std::string& leadingConnector() const noexcept { return leadingConnector_; }
    ObjectId followingInstanceId() const noexcept { return followingInstanceId_; }
    const std::string& followingConnector() const noexcept { return followingConnector_; }

    // ONE NUMBER PER COMPONENT. Only the free ones may be non-zero, and the
    // rest is refused rather than ignored (AssemblyDocument::requireMatable).
    const MateValues& values() const noexcept { return values_; }
    MateFreedom freedom() const noexcept { return FreedomOf(type_); }

    // The FIRST free component's value -- what a single-freedom mate means by
    // "the angle" or "the offset". Zero for a fastened mate, which has none.
    //
    // Kept because most mates have exactly one freedom and reading
    // `values()[5]` at every call site would put the component index in a
    // hundred places, which is where an off-by-one lives.
    double value() const noexcept;
    // Which component that is, or Count-many when there is none.
    int primaryComponent() const noexcept;

    // --- Motion limits (M25, roadmap §22) ------------------------------------
    //
    // Per component, because a cylindrical mate can be limited in rotation and
    // in travel independently. A drag or a drive outside them is CLAMPED
    // rather than refused (§22 is explicit about this), and the clamp is
    // reported so it is never silent.
    struct Limit {
        bool enabled = false;
        double min = 0.0;
        double max = 0.0;
    };
    const std::array<Limit, kMateComponentCount>& limits() const noexcept { return limits_; }
    // The value this component would be allowed to take. Equal to `wanted`
    // when there is no limit or it is already inside one.
    double clampToLimit(int component, double wanted) const noexcept;

    // --- Driving (M25) --------------------------------------------------------
    //
    // A DRIVEN mate holds its values; an undriven one lets a closed-loop solve
    // choose them. In an assembly with no loops this makes no difference --
    // every mate's values are used as given -- so it costs nothing to leave
    // alone until there is a mechanism.
    bool isDriven() const noexcept { return driven_; }

    // The other end. Used by the solve, which walks the mate graph from a
    // grounded instance outwards and does not care which side was typed first.
    ObjectId otherEnd(ObjectId instanceId) const noexcept {
        if (instanceId == leadingInstanceId_) return followingInstanceId_;
        if (instanceId == followingInstanceId_) return leadingInstanceId_;
        return kInvalidObjectId;
    }
    const std::string& connectorOn(ObjectId instanceId) const noexcept {
        static const std::string kNone;
        if (instanceId == leadingInstanceId_) return leadingConnector_;
        if (instanceId == followingInstanceId_) return followingConnector_;
        return kNone;
    }

private:
    friend class AssemblyDocument;

    void setName(std::string name) { name_ = std::move(name); }
    void setValues(const MateValues& values) noexcept { values_ = values; }
    void setLimit(int component, Limit limit) noexcept { limits_[component] = limit; }
    void setDriven(bool driven) noexcept { driven_ = driven; }

    ObjectId id_;
    std::string name_;
    MateType type_;
    ObjectId leadingInstanceId_;
    std::string leadingConnector_;
    ObjectId followingInstanceId_;
    std::string followingConnector_;
    MateValues values_{};
    std::array<Limit, kMateComponentCount> limits_{};
    bool driven_ = false;
};

// THE ONE FORMULA (M24, ADR-M24-003; generalised in M25).
//
// What a mate contributes, expressed in the shared connector frame: its free
// components, driven to their values. Identity for a fastened mate, a turn
// about +Z for a revolute, a slide along +Z for a slider, both for a
// cylindrical, and so on -- but written ONCE, from the freedom table, rather
// than once per type.
Transform3D MateTransform(MateType type, const MateValues& values) noexcept;

// HOW FAR THIS MATE IS FROM BEING SATISFIED (M25, ADR-M25-002).
//
// `relative` is where the follower's connector actually sits in the leader
// connector's frame. What the mate wants is MateTransform(type, values), so
// the error is the one composed out of the other -- and the components of that
// error which the mate PINS are the equations a closed-loop solve drives to
// zero. The free ones are left out, because they are exactly what the mate
// does not care about.
//
// `alsoPinFreedoms` makes the FREE components equations as well, which is what
// a DRIVEN mate means: not "you may turn about z" but "you are at this angle".
// Without it a driven mate that happens to be the one closing a loop would have
// its angle quietly absorbed as slack, and the number the user typed would be
// overwritten by the solve.
//
// Returns how many it wrote into `out` (which must hold at least six).
int MateResiduals(MateType type, const MateValues& values, const Transform3D& relative,
                  double* out, bool alsoPinFreedoms = false) noexcept;

} // namespace paramcad
