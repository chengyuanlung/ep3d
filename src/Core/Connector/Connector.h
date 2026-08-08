#pragma once

#include "Core/Document/ObjectId.h"
#include <string>

namespace paramcad {

enum class ConnectorRole {
    Generic,
    Mount,
    Shaft,
    LinearGuide,
    ToolFlange,
    Electrical,
    Pneumatic
};

class Connector {
public:
    Connector(std::string name, ConnectorRole role, ObjectId frameId);

    ObjectId id() const noexcept { return id_; }
    ObjectId frameId() const noexcept { return frameId_; }
    ConnectorRole role() const noexcept { return role_; }

private:
    ObjectId id_;
    std::string name_;
    ConnectorRole role_;
    ObjectId frameId_;
};

} // namespace paramcad
