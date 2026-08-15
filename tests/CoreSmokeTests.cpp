#include "Core/Document/PartDocument.h"
#include <gtest/gtest.h>

namespace {

using namespace paramcad;

TEST(CoreSmokeTests, ParameterAndBodyCreation) {
    PartDocument part("TestPart");
    auto& width = part.addParameter("Width", 100.0, UnitType::Millimeter);
    EXPECT_NE(width.id(), kInvalidObjectId);
    EXPECT_EQ(part.parameters().findByName("Width"), &width);

    ASSERT_TRUE(part.setParameterValue(width.id(), 120.0)); // facade (mutators private since round 3)
    EXPECT_EQ(width.value(), 120.0);
    EXPECT_EQ(width.state(), ParameterState::Dirty);

    auto& body = part.addBody("Body001");
    EXPECT_NE(body.id(), kInvalidObjectId);
}

} // namespace
