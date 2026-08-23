#pragma once

#include "Core/Document/ObjectId.h"
#include <string>

namespace paramcad {

class PartDocument;

enum class ConnectorRole {
    Generic,
    Mount,
    Shaft,
    LinearGuide,
    ToolFlange,
    Electrical,
    Pneumatic
};

// Where a connector belongs (roadmap §18.3).
//
// A connector owned by a PART definition is reusable by every assembly instance
// of that part (§21) -- that reuse is the whole reason connector-based mating is
// a better foundation than referencing transient topology. A connector owned by
// an ASSEMBLY belongs to that assembly alone.
//
// EP3D has no Assembly yet (M11), so every connector today is PartDefinition.
// The field exists now because adding an owner later would mean migrating every
// file that already has connectors, and because the distinction is what §21's
// reuse rule is stated in terms of.
enum class ConnectorOwner { PartDefinition, Assembly };

// A named, role-carrying coordinate system on a part (M10.3, ADR-M10-004).
//
// A connector IS a frame plus meaning: it holds no geometry of its own, it
// references a ReferenceFrame by ObjectId, and the frame answers where it is.
// Roadmap §18 lists the content as "Origin / X-Y-Z axes / Owner ObjectId /
// Attachment semantic reference"; the origin and axes are the frame, and the
// owner and attachment are here.
//
// FIRST-CLASS WHICHEVER ROUTE CREATED IT (§18.1). The reference model has an
// explicit route (its own item in the feature list) and an implicit one (created
// inside a mate dialog, listed under that mate). They differ in WHEN they are
// made and WHERE they are listed -- never in whether they have an ObjectId,
// because a mate built on something that cannot be re-resolved violates A03
// outright. EP3D therefore has one kind of connector and no "implicit" variant
// with weaker identity; a UI that creates one mid-dialog creates this.
//
// NOT here, deliberately: visibility. It is per-context presentation state
// (§18.3) -- the same connector can be hidden in the part and shown in the
// assembly -- and A02 keeps presentation out of Core.
class Connector {
public:
    Connector(std::string name, ConnectorRole role, ObjectId frameId,
              ConnectorOwner owner = ConnectorOwner::PartDefinition);
    Connector(ObjectId id, std::string name, ConnectorRole role, ObjectId frameId,
              ConnectorOwner owner);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    ObjectId frameId() const noexcept { return frameId_; }
    ConnectorRole role() const noexcept { return role_; }
    ConnectorOwner owner() const noexcept { return owner_; }

private:
    friend class PartDocument;

    void setRole(ConnectorRole role) noexcept { role_ = role; }
    void setFrameId(ObjectId frameId) noexcept { frameId_ = frameId; }

    ObjectId id_;
    std::string name_;
    ConnectorRole role_;
    ObjectId frameId_;
    ConnectorOwner owner_ = ConnectorOwner::PartDefinition;
};

} // namespace paramcad
