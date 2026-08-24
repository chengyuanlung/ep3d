#include "Core/Assembly/Mate.h"

#include "Core/Geometry/Transform.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace paramcad {

std::string_view toString(MateType type) noexcept {
    switch (type) {
        case MateType::Fastened: return "Fastened";
        case MateType::Revolute: return "Revolute";
        case MateType::Slider: return "Slider";
        case MateType::Cylindrical: return "Cylindrical";
        case MateType::Ball: return "Ball";
        case MateType::Planar: return "Planar";
        case MateType::Parallel: return "Parallel";
    }
    return "Fastened";
}

MateFreedom FreedomOf(MateType type) noexcept {
    // ROADMAP §20.1, AS CODE. Reading down the `free` initialisers is reading
    // the "保留的自由度" column: tx, ty, tz, rx, ry, rz.
    MateFreedom out{};
    switch (type) {
        case MateType::Fastened:
            return out; // nothing
        case MateType::Revolute:
            out.free[5] = true; // about z
            return out;
        case MateType::Slider:
            out.free[2] = true; // along z
            return out;
        case MateType::Cylindrical:
            out.free[2] = true; // along z
            out.free[5] = true; // and about it
            return out;
        case MateType::Ball:
            out.free[3] = out.free[4] = out.free[5] = true; // all three turns
            return out;
        case MateType::Planar:
            out.free[0] = out.free[1] = true; // in the shared XY
            out.free[5] = true;               // and spin in that plane
            return out;
        case MateType::Parallel:
            // AN ALIGNMENT MATE. It says the two axes point the same way and
            // NOTHING about where the parts are, so everything except the two
            // turns that would tilt the axis apart stays free. That is why
            // §20.1 leaves its "保留的自由度" cell blank rather than giving a
            // number: it is not a joint.
            out.free[0] = out.free[1] = out.free[2] = true;
            out.free[5] = true;
            return out;
    }
    return out;
}

Mate::Mate(std::string name, MateType type, ObjectId leadingInstanceId,
           std::string leadingConnector, ObjectId followingInstanceId,
           std::string followingConnector, MateValues values)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), type_(type),
      leadingInstanceId_(leadingInstanceId), leadingConnector_(std::move(leadingConnector)),
      followingInstanceId_(followingInstanceId),
      followingConnector_(std::move(followingConnector)), values_(values) {}

Mate::Mate(ObjectId id, std::string name, MateType type, ObjectId leadingInstanceId,
           std::string leadingConnector, ObjectId followingInstanceId,
           std::string followingConnector, MateValues values)
    : id_(RestoreObjectId(id)), name_(std::move(name)), type_(type),
      leadingInstanceId_(leadingInstanceId), leadingConnector_(std::move(leadingConnector)),
      followingInstanceId_(followingInstanceId),
      followingConnector_(std::move(followingConnector)), values_(values) {}

int Mate::primaryComponent() const noexcept {
    const MateFreedom freedom = FreedomOf(type_);
    for (int i = 0; i < static_cast<int>(kMateComponentCount); ++i)
        if (freedom.free[static_cast<std::size_t>(i)]) return i;
    return static_cast<int>(kMateComponentCount);
}

double Mate::value() const noexcept {
    const int component = primaryComponent();
    return component == static_cast<int>(kMateComponentCount)
               ? 0.0
               : values_[static_cast<std::size_t>(component)];
}

double Mate::clampToLimit(int component, double wanted) const noexcept {
    if (component < 0 || component >= static_cast<int>(kMateComponentCount)) return wanted;
    const Limit& limit = limits_[static_cast<std::size_t>(component)];
    if (!limit.enabled) return wanted;
    // CLAMPED, not refused: roadmap §22 is explicit that a drag past a limit
    // stops at the limit rather than erroring. It is reported to the caller so
    // it is never silent -- a stop nobody is told about is a control that
    // appears to be broken.
    return std::min(std::max(wanted, limit.min), limit.max);
}

Transform3D MateTransform(MateType type, const MateValues& values) noexcept {
    // THE FREE COMPONENTS, DRIVEN TO THEIR VALUES. A pinned component cannot
    // carry a number at all (the document refuses one), so masking here is
    // belt as well as braces -- and it is what makes this function say the
    // same thing as the freedom table rather than something adjacent to it.
    const MateFreedom freedom = FreedomOf(type);
    std::array<double, kMateComponentCount> driven{};
    for (std::size_t i = 0; i < kMateComponentCount; ++i)
        if (freedom.free[i]) driven[i] = values[i];
    return TransformOfComponents(driven);
}

int MateResiduals(MateType type, const MateValues& values, const Transform3D& relative,
                  double* out, bool alsoPinFreedoms) noexcept {
    // The error between where the follower's connector IS and where this mate
    // wants it. Zero in every pinned component exactly when the mate holds.
    const Transform3D error = Compose(Inverse(MateTransform(type, values)), relative);
    const std::array<double, kMateComponentCount> components = ComponentsOf(error);
    const MateFreedom freedom = FreedomOf(type);
    int written = 0;
    for (std::size_t i = 0; i < kMateComponentCount; ++i)
        if (alsoPinFreedoms || !freedom.free[i]) out[written++] = components[i];
    return written;
}

} // namespace paramcad
