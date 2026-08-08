#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include <iostream>
#include <memory>

// PARAMCAD_HAVE_OCCT_KERNEL is defined by CMake only when the OCCT-backed
// ParametricCADKernelOcct target was built (OCCT found at configure time,
// spec 16). This is the ONLY place in the application that ever names a
// concrete IGeometryKernel implementation -- Core and BoxFeature never do
// (ADR-M3-003).
#ifdef PARAMCAD_HAVE_OCCT_KERNEL
#include "Kernel/Occt/OcctGeometryKernel.h"
#endif

int main() {
    using namespace paramcad;

    PartDocument part("DemoPart");
    Parameter& width = part.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = part.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& depth = part.addParameter("Depth", 20.0, UnitType::Millimeter);
    part.addMaterial("Aluminum 6061", 2700.0);
    Body& body = part.addBody("Body001");
    part.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());

    std::cout << "Created PartDocument: " << part.name() << '\n';
    std::cout << "Parameters: " << part.parameters().items().size() << '\n';

#ifdef PARAMCAD_HAVE_OCCT_KERNEL
    OcctGeometryKernel kernel;
    part.setGeometryKernel(&kernel);
    const DocumentRecomputeReport report = part.recompute();
    if (report.success) {
        const MassProperties& mp = part.massProperties();
        std::cout << "Recompute succeeded. Volume=" << mp.volumeMm3 << " mm^3, Mass="
                  << mp.massKg << " kg, COM=(" << mp.centerOfMassMm.x << ", "
                  << mp.centerOfMassMm.y << ", " << mp.centerOfMassMm.z << ") mm\n";
    } else {
        std::cout << "Recompute reported failures (see items for diagnostics).\n";
    }
#else
    std::cout << "OCCT kernel not built (PARAMCAD_BUILD_KERNEL_OCCT off or OCCT not found); "
                 "geometry recompute skipped.\n";
#endif
    return 0;
}
