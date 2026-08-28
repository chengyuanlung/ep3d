#pragma once

#include "Core/Measure/SketchMeasure.h"

#include <string>

namespace paramcad {

class IGeometryKernel;
class KernelShape;

// M55 -- MEASURING A SOLID.
//
// The same vocabulary a sketch measurement uses (MeasureItem, MeasureUnit,
// MeasureResult) rather than a second one. A program with two kinds of
// "measured number" has two places to print a unit wrong, and M18 already paid
// for that once: an area reported in millimetres because UnitType had no
// square millimetre in it.
//
// AN INSTANTANEOUS QUERY. No ObjectId, nothing stored, nothing undoable -- ask
// again after a rebuild and the answer is the new one.

// What one solid is: how much of it there is, where its middle is, and how
// much room it takes.
//
// `densityKgPerM3` of zero means the part has no material, and then no mass is
// reported at all -- rather than zero grams, which is a number somebody would
// put in a lifting calculation.
MeasureResult MeasureSolid(IGeometryKernel& kernel, const KernelShape& shape,
                           double densityKgPerM3);

// What TWO solids are to each other.
//
// AND THIS IS WHERE THE HONEST ANSWER IS A REFUSAL. The question a user asks
// of two bodies is "how far apart are they", and this program cannot answer
// it: the kernel has no signed-distance query, and measureInterference returns
// a VOLUME -- which is zero for every pair that does not already overlap, and
// so says nothing at all about the gap (M46 settled this, and it is the same
// missing primitive that put contact solving out of reach).
//
// So the gap is refused, by name, with the reason. What IS reported is the
// overlap: whether they share any space and how much. A tool that answered
// "0.0 mm" for the distance between two bodies that do not touch would be
// giving the same number for a hair's breadth and a metre.
MeasureResult MeasureBetweenSolids(IGeometryKernel& kernel, const KernelShape& a,
                                   const KernelShape& b);

} // namespace paramcad
