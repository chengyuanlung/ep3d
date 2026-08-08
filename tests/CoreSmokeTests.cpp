#include "Core/Document/PartDocument.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace paramcad;

    PartDocument part("TestPart");
    auto& width = part.parameters().add("Width", 100.0, UnitType::Millimeter);
    assert(width.id() != kInvalidObjectId);
    assert(part.parameters().findByName("Width") == &width);

    width.setValue(120.0);
    assert(width.value() == 120.0);
    assert(width.state() == ParameterState::Dirty);

    auto& body = part.addBody("Body001");
    assert(body.id() != kInvalidObjectId);

    std::cout << "Core smoke tests passed.\n";
    return 0;
}
