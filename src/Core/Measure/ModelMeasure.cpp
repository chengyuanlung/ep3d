#include "Core/Measure/ModelMeasure.h"

#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/ShapeKind.h"

namespace paramcad {

MeasureResult MeasureSolid(IGeometryKernel& kernel, const KernelShape& shape,
                           double densityKgPerM3) {
    MeasureResult out;
    if (!shape.isValid()) {
        out.message = "there is no solid here to measure";
        return out;
    }

    // AND IT HAS TO BE A SOLID (M59).
    //
    // Until surfaces existed this could not be wrong, so it was not asked.
    // A shell is a perfectly valid KernelShape: it builds, it draws, it has a
    // bounding box -- and OCCT hands back mass properties for it, with a
    // volume of ZERO. Zero is a number. It would have gone into the items list
    // as a volume, into a mass as nothing, and onto a cut list as 0 kg, and
    // the only sign would be a part that weighs nothing.
    const ShapeKind kind = kernel.kindOfShape(shape);
    const std::string notSolid = WhyNotASolid(kind);
    if (!notSolid.empty()) {
        out.message = notSolid;
        return out;
    }

    const KernelMassPropertiesResult mass = kernel.calculateMassProperties(shape);
    if (!mass) {
        out.message = mass.message.empty() ? std::string("this solid could not be measured")
                                           : mass.message;
        return out;
    }

    out.items.push_back(MeasureItem{"Volume", mass.properties.volumeMm3,
                                    MeasureUnit::CubicMillimetre, false});

    // MASS ONLY WHEN THERE IS A MATERIAL. Zero grams is a number somebody
    // would put in a lifting calculation, and "this part has no material yet"
    // is a different sentence from "this part weighs nothing".
    if (densityKgPerM3 > 0.0) {
        constexpr double kMm3PerM3 = 1.0e9;
        const double kilograms = mass.properties.volumeMm3 / kMm3PerM3 * densityKgPerM3;
        out.items.push_back(MeasureItem{"Mass", kilograms, MeasureUnit::Kilogram, false});
    }

    out.items.push_back(MeasureItem{"Centre of mass X", mass.properties.centerOfMassMm.x,
                                    MeasureUnit::Millimetre, false});
    out.items.push_back(MeasureItem{"Centre of mass Y", mass.properties.centerOfMassMm.y,
                                    MeasureUnit::Millimetre, false});
    out.items.push_back(MeasureItem{"Centre of mass Z", mass.properties.centerOfMassMm.z,
                                    MeasureUnit::Millimetre, false});

    const KernelBoundsResult bounds = kernel.boundsOfShape(shape);
    if (bounds.ok) {
        // HOW MUCH ROOM IT TAKES, which is the question behind "will it fit in
        // the machine" and is not the same as any dimension on the drawing.
        out.items.push_back(MeasureItem{"Extent X", bounds.max.x - bounds.min.x,
                                        MeasureUnit::Millimetre, false});
        out.items.push_back(MeasureItem{"Extent Y", bounds.max.y - bounds.min.y,
                                        MeasureUnit::Millimetre, false});
        out.items.push_back(MeasureItem{"Extent Z", bounds.max.z - bounds.min.z,
                                        MeasureUnit::Millimetre, false});
    }

    out.ok = true;
    return out;
}

MeasureResult MeasureBetweenSolids(IGeometryKernel& kernel, const KernelShape& a,
                                   const KernelShape& b) {
    MeasureResult out;
    if (!a.isValid() || !b.isValid()) {
        out.message = "two solids are needed, and one of these is not there";
        return out;
    }
    // TWO SOLIDS, and a shell is not one (M59). An interference between a
    // solid and a skin is zero however deep the skin is buried in it, because
    // a skin has no inside -- and zero here reads as "these do not touch".
    for (const KernelShape* one : {&a, &b}) {
        const std::string notSolid = WhyNotASolid(kernel.kindOfShape(*one));
        if (notSolid.empty()) continue;
        out.message = "two solids are needed, and " + notSolid;
        return out;
    }

    const KernelInterferenceResult overlap = kernel.measureInterference(a, b);
    if (!overlap) {
        out.message = overlap.message.empty()
                          ? std::string("these two could not be compared")
                          : overlap.message;
        return out;
    }

    // WHAT CAN BE SAID: whether they share space, and how much.
    out.items.push_back(MeasureItem{"Overlapping", overlap.volumeMm3 > 0.0 ? 1.0 : 0.0,
                                    MeasureUnit::Count, false});
    out.items.push_back(MeasureItem{"Overlap volume", overlap.volumeMm3,
                                    MeasureUnit::CubicMillimetre, false});

    // AND WHAT CANNOT, SAID OUT LOUD.
    //
    // "How far apart are they" is the question a user actually asks of two
    // bodies, and this program has no way to answer it: the kernel exposes no
    // signed-distance query, and the interference above is a VOLUME -- zero
    // for every pair that does not already overlap, and therefore silent about
    // the gap. The same missing primitive put contact solving out of reach
    // (M46).
    //
    // Reporting 0.0 mm would give the same number for a hair's breadth and a
    // metre, so the refusal is part of the answer rather than an omission from
    // it.
    if (overlap.volumeMm3 <= 0.0)
        out.message = "they do not overlap. HOW FAR APART they are is not something this "
                      "program can measure: the kernel answers with the volume two solids "
                      "share, which is zero for everything that does not already touch";

    out.ok = true;
    return out;
}

} // namespace paramcad
