#pragma once

#include "Core/Body/Body.h"
#include "Core/Connector/Connector.h"
#include "Core/Document/CadDocument.h"
#include "Core/Material/Material.h"
#include "Core/Parameter/ParameterManager.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Reference/ReferenceFrame.h"
#include <memory>
#include <vector>

namespace paramcad {

class PartDocument final : public CadDocument {
public:
    explicit PartDocument(std::string name);
    // Restore constructor (deserialization): keeps the persisted document id.
    // Frames are not serialized in schema v1, so the Origin frame is
    // auto-created with a fresh id, exactly as in the fresh constructor.
    PartDocument(ObjectId id, std::string name);

    DocumentType type() const noexcept override { return DocumentType::Part; }

    ParameterManager& parameters() noexcept { return parameters_; }
    const ParameterManager& parameters() const noexcept { return parameters_; }

    const std::vector<std::unique_ptr<Body>>& bodies() const noexcept { return bodies_; }
    const std::vector<std::unique_ptr<ReferenceFrame>>& frames() const noexcept { return frames_; }

    Body& addBody(std::string name);
    // Restore path (deserialization): adds a body that keeps its persisted id.
    Body& restoreBody(ObjectId id, std::string name);
    ReferenceFrame& addFrame(std::string name, ObjectId parentFrameId = kInvalidObjectId);
    Connector& addConnector(std::string name, ConnectorRole role, ObjectId frameId);

    void setMaterial(std::shared_ptr<Material> material) { material_ = std::move(material); }
    const std::shared_ptr<Material>& material() const noexcept { return material_; }

    MassProperties& massProperties() noexcept { return massProperties_; }
    const MassProperties& massProperties() const noexcept { return massProperties_; }

private:
    ParameterManager parameters_;
    std::vector<std::unique_ptr<Body>> bodies_;
    std::vector<std::unique_ptr<ReferenceFrame>> frames_;
    std::vector<std::unique_ptr<Connector>> connectors_;
    std::shared_ptr<Material> material_;
    MassProperties massProperties_;
};

} // namespace paramcad
