#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Parameter/Parameter.h"
#include <gtest/gtest.h>
#include <optional>
#include <variant>

namespace {

using namespace paramcad;

TEST(ObjectRegistryTest, M2_REG_001_RegisterAndFind) {
    Parameter parameter("Width", 10.0, UnitType::Millimeter);
    ObjectRegistry registry;
    EXPECT_TRUE(registry.registerObject(parameter.id(), &parameter));
    EXPECT_TRUE(registry.contains(parameter.id()));
    EXPECT_EQ(registry.size(), 1u);

    const ObjectRegistry::ObjectRef* ref = registry.find(parameter.id());
    ASSERT_NE(ref, nullptr);
    ASSERT_TRUE(std::holds_alternative<Parameter*>(*ref));
    EXPECT_EQ(std::get<Parameter*>(*ref), &parameter); // same runtime object

    // Document-level path: addParameter registers automatically.
    PartDocument document("RegDoc");
    Parameter& docParameter = document.addParameter("Height", 20.0, UnitType::Millimeter);
    // Through a CONST document the handle carries const pointees (R2R4-M1):
    // inspection resolves the same runtime object, and cannot mutate it.
    const std::optional<ObjectRegistry::ConstObjectRef> docRef =
        document.objectRegistry().find(docParameter.id());
    ASSERT_TRUE(docRef.has_value());
    EXPECT_EQ(std::get<const Parameter*>(*docRef), &docParameter);
}

TEST(ObjectRegistryTest, M2_REG_002_RejectDuplicateId) {
    Parameter parameter("Width", 10.0, UnitType::Millimeter);
    Parameter other("Height", 20.0, UnitType::Millimeter);
    ObjectRegistry registry;
    EXPECT_TRUE(registry.registerObject(parameter.id(), &parameter));

    // Second registration with the same stable id fails; registry unchanged.
    EXPECT_FALSE(registry.registerObject(parameter.id(), &parameter));
    EXPECT_FALSE(registry.registerObject(parameter.id(), &other));
    EXPECT_EQ(registry.size(), 1u);

    // Invalid id, null handle, and handle/id mismatch are all rejected.
    EXPECT_FALSE(registry.registerObject(kInvalidObjectId, &other));
    EXPECT_FALSE(registry.registerObject(other.id(), static_cast<Parameter*>(nullptr)));
    EXPECT_FALSE(registry.registerObject(other.id(), &parameter)); // ->id() != registered id
    EXPECT_EQ(registry.size(), 1u);
}

TEST(ObjectRegistryTest, M2_REG_003_UnknownLookup) {
    ObjectRegistry registry;
    const ObjectId unknown = ObjectIdGenerator::Next();
    EXPECT_FALSE(registry.contains(unknown));
    EXPECT_EQ(registry.find(unknown), nullptr);            // controlled not-found
    EXPECT_EQ(registry.findRecomputable(unknown), nullptr); // never UB
    EXPECT_FALSE(registry.contains(kInvalidObjectId));
    EXPECT_EQ(registry.find(kInvalidObjectId), nullptr);
}

TEST(ObjectRegistryTest, M2_REG_004_Unregister) {
    Parameter parameter("Width", 10.0, UnitType::Millimeter);
    ObjectRegistry registry;
    EXPECT_TRUE(registry.registerObject(parameter.id(), &parameter));

    EXPECT_TRUE(registry.unregisterObject(parameter.id()));
    EXPECT_FALSE(registry.contains(parameter.id()));
    EXPECT_EQ(registry.find(parameter.id()), nullptr);
    EXPECT_EQ(registry.size(), 0u);

    // Unregistering an unknown id is deterministic: false, no side effects.
    EXPECT_FALSE(registry.unregisterObject(parameter.id()));
}

TEST(ObjectRegistryTest, M2_REG_005_DocumentIsolation) {
    // Two documents with identical runtime scenarios share no registry state.
    PartDocument first("DocA");
    PartDocument second("DocB");
    Parameter& parameterA = first.addParameter("Width", 10.0, UnitType::Millimeter);
    Parameter& parameterB = second.addParameter("Width", 10.0, UnitType::Millimeter);

    EXPECT_TRUE(first.objectRegistry().contains(parameterA.id()));
    EXPECT_FALSE(first.objectRegistry().contains(parameterB.id()));
    EXPECT_TRUE(second.objectRegistry().contains(parameterB.id()));
    EXPECT_FALSE(second.objectRegistry().contains(parameterA.id()));

    // Removing from one document does not affect the other.
    EXPECT_TRUE(first.removeObject(parameterA.id()));
    EXPECT_FALSE(first.objectRegistry().contains(parameterA.id()));
    EXPECT_TRUE(second.objectRegistry().contains(parameterB.id()));
}

} // namespace
