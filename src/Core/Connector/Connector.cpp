#include "Core/Connector/Connector.h"
#include <utility>

namespace paramcad {

Connector::Connector(std::string name, ConnectorRole role, ObjectId frameId)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), role_(role), frameId_(frameId) {}

} // namespace paramcad
