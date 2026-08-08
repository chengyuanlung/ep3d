#include "Core/Document/PartDocument.h"
#include <gtest/gtest.h>

namespace {

using namespace paramcad;

TEST(CoreSmokeTests, ParameterAndBodyCreation) {
    PartDocument part("TestPart");
    auto& width = part.parameters().add("Width", 100.0, UnitType::Millimeter);
    EXPECT_NE(width.id(), kInvalidObjectId);
    EXPECT_EQ(part.parameters().findByName("Width"), &width);

    width.setValue(120.0);
    EXPECT_EQ(width.value(), 120.0);
    EXPECT_EQ(width.state(), ParameterState::Dirty);

    auto& body = part.addBody("Body001");
    EXPECT_NE(body.id(), kInvalidObjectId);
}

} // namespace
