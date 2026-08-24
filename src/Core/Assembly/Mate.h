#pragma once

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
    Fastened, // 0 left  -- the two connectors are the same place
    Revolute, // 1 rotation, about the connectors' shared +Z
    Slider,   // 1 translation, along the connectors' shared +Z
};

std::string_view toString(MateType type) noexcept;

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
         std::string followingConnector, double value);
    // Restore constructor (deserialization): keeps the persisted id.
    Mate(ObjectId id, std::string name, MateType type, ObjectId leadingInstanceId,
         std::string leadingConnector, ObjectId followingInstanceId,
         std::string followingConnector, double value);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    MateType type() const noexcept { return type_; }

    ObjectId leadingInstanceId() const noexcept { return leadingInstanceId_; }
    const std::string& leadingConnector() const noexcept { return leadingConnector_; }
    ObjectId followingInstanceId() const noexcept { return followingInstanceId_; }
    const std::string& followingConnector() const noexcept { return followingConnector_; }

    double value() const noexcept { return value_; }

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
    void setValue(double value) noexcept { value_ = value; }

    ObjectId id_;
    std::string name_;
    MateType type_;
    ObjectId leadingInstanceId_;
    std::string leadingConnector_;
    ObjectId followingInstanceId_;
    std::string followingConnector_;
    double value_ = 0.0;
};

// THE ONE FORMULA (M24, ADR-M24-003).
//
// What a mate contributes, expressed in the shared connector frame: identity
// for a fastened mate, a turn about +Z for a revolute, a slide along +Z for a
// slider. Every mate type is this one function plus a placement rule written
// once, so a new mate type cannot come with its own idea of what "connected"
// means.
Transform3D MateTransform(MateType type, double value) noexcept;

} // namespace paramcad
