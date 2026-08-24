#include "Core/Assembly/Mate.h"

#include <cmath>
#include <utility>

namespace paramcad {

std::string_view toString(MateType type) noexcept {
    switch (type) {
        case MateType::Fastened: return "Fastened";
        case MateType::Revolute: return "Revolute";
        case MateType::Slider: return "Slider";
    }
    return "Fastened";
}

Mate::Mate(std::string name, MateType type, ObjectId leadingInstanceId,
           std::string leadingConnector, ObjectId followingInstanceId,
           std::string followingConnector, double value)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), type_(type),
      leadingInstanceId_(leadingInstanceId), leadingConnector_(std::move(leadingConnector)),
      followingInstanceId_(followingInstanceId),
      followingConnector_(std::move(followingConnector)), value_(value) {}

Mate::Mate(ObjectId id, std::string name, MateType type, ObjectId leadingInstanceId,
           std::string leadingConnector, ObjectId followingInstanceId,
           std::string followingConnector, double value)
    : id_(RestoreObjectId(id)), name_(std::move(name)), type_(type),
      leadingInstanceId_(leadingInstanceId), leadingConnector_(std::move(leadingConnector)),
      followingInstanceId_(followingInstanceId),
      followingConnector_(std::move(followingConnector)), value_(value) {}

Transform3D MateTransform(MateType type, double value) noexcept {
    Transform3D t;
    switch (type) {
        case MateType::Fastened:
            // Nothing. The two connectors ARE the same place, which is what
            // "fastened" says. A Fastened mate cannot carry a value at all --
            // AssemblyDocument refuses one -- so there is no case here where a
            // number is quietly dropped.
            return t;
        case MateType::Revolute:
            // About the connectors' shared +Z, which is why a connector's Z
            // axis is the thing a user points along a hinge pin.
            t.rotation = Quaternion{std::cos(value / 2.0), 0.0, 0.0, std::sin(value / 2.0)};
            return t;
        case MateType::Slider:
            // Along the same axis. One axis for both, so a part built with its
            // connector pointing down a slot slides down the slot.
            t.translation = Vec3{0.0, 0.0, value};
            return t;
    }
    return t;
}

} // namespace paramcad
