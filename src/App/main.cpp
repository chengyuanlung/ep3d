#include "Core/Document/PartDocument.h"
#include <iostream>
#include <memory>

int main() {
    using namespace paramcad;

    PartDocument part("DemoPart");
    part.addParameter("Width", 100.0, UnitType::Millimeter);
    part.addParameter("Height", 50.0, UnitType::Millimeter);
    part.addParameter("Length", 20.0, UnitType::Millimeter);
    part.setMaterial(std::make_shared<Material>("Aluminum 6061", 2700.0));
    part.addBody("Body001");

    std::cout << "Created PartDocument: " << part.name() << '\n';
    std::cout << "Parameters: " << part.parameters().items().size() << '\n';
    return 0;
}
