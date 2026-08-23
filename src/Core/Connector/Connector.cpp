#include "Core/Connector/Connector.h"
#include <utility>

namespace paramcad {

Connector::Connector(std::string name, ConnectorRole role, ObjectId frameId,
                     ConnectorOwner owner)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), role_(role), frameId_(frameId),
      owner_(owner) {}

Connector::Connector(ObjectId id, std::string name, ConnectorRole role, ObjectId frameId,
                     ConnectorOwner owner)
    : id_(RestoreObjectId(id)), name_(std::move(name)), role_(role), frameId_(frameId),
      owner_(owner) {}

} // namespace paramcad
