#pragma once

#include "Core/Assembly/Relation.h"
#include "Core/Document/ObjectId.h"

#include <string>

namespace paramcad {

class AssemblyDocument;

// M58.2 -- WHERE THE GENERATOR MEETS THE MECHANISM.
//
// EP3D has coupled two rotations by a typed ratio since M31, and since M58 it
// can build the gears themselves. Those are the two halves of one fact, and
// this is where they are joined so they cannot come apart.
//
// THE FAILURE THIS RULES OUT is not exotic. A pair is set up as 20 and 40 teeth
// with a ratio of 0.5 typed in. Somebody changes the pinion to 24 teeth,
// because that is what the centre distance now needs. The model shows a
// 24-tooth pinion, the mechanism still turns at 2:1, and every number on the
// screen is a number somebody chose. Nothing is dangling and nothing is
// invalid.

struct GearMeshResult {
    bool ok = false;
    std::string why;

    ObjectId relationId = kInvalidObjectId;
    // ALL DERIVED FROM THE TWO PATHS, reported so the caller can put them on
    // the drawing without working any of them out a second time.
    double ratio = 0.0;            // signed: negative because they turn opposite ways
    double centreDistanceMm = 0.0; // where the second shaft has to go
    double contactRatio = 0.0;     // whether it will actually run

    explicit operator bool() const noexcept { return ok; }
};

// COUPLE TWO GEAR INSTANCES, deriving the ratio from what they ARE.
//
// Both instances have to name gears (`gear:m2 z20 b10`), because that is what
// makes the derivation possible: a relation between two arbitrary parts is
// still a typed ratio and always will be. An instance that is not a gear is
// refused BY NAME, so "this does not work on those two" says which two.
GearMeshResult MeshGears(AssemblyDocument& assembly, std::string name,
                         ObjectId driverInstanceId, ObjectId drivenInstanceId,
                         CoupledFreedom driver, CoupledFreedom driven);

// WHETHER A RELATION STILL AGREES WITH THE GEARS IT COUPLES, or empty when it
// does.
//
// Relation stores its ratio -- it is a saved, serialized number, and that is
// the right shape for the general case where there are no gears to read it
// off. So this is M46's answer applied again: the fact CAN drift, therefore
// there is a named question that says whether it has, and both numbers appear
// in the sentence.
//
// Empty for a relation that couples anything other than two gear instances:
// there is nothing to disagree with, and calling that agreement would be a
// green light nobody earned.
std::string WhyMeshDisagrees(const AssemblyDocument& assembly, ObjectId relationId);

} // namespace paramcad
