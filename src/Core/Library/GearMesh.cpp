#include "Core/Library/GearMesh.h"

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Assembly/Mate.h"
#include "Core/Library/SpurGear.h"
#include "Core/Text/NumberText.h"

#include <cmath>
#include <optional>
#include <utility>

namespace paramcad {

namespace {

GearMeshResult refuse(std::string why) {
    GearMeshResult out;
    out.why = std::move(why);
    return out;
}

// The gear an instance names, or nothing when it does not name one.
std::optional<SpurGear> GearOf(const AssemblyDocument& assembly, ObjectId instanceId) {
    const Instance* one = assembly.findInstance(instanceId);
    if (one == nullptr) return std::nullopt;
    return SpurGearOfPath(one->sourcePath());
}

// THE GEAR ON ONE END OF A COUPLING, found through the mate the freedom names.
//
// A mate has two instances and the freedom does not say which of them turns.
// Rather than guess -- a revolute mate is usually part-to-housing, but nothing
// enforces that -- this takes the side that IS a gear, and gives up when
// neither is or both are. Giving up means "there is nothing here to compare",
// which is the honest answer: inventing one would let a wrong ratio pass as
// checked.
std::optional<SpurGear> GearBehind(const AssemblyDocument& assembly,
                                   const CoupledFreedom& freedom) {
    const Mate* mate = assembly.findMate(freedom.mateId);
    if (mate == nullptr) return std::nullopt;
    const std::optional<SpurGear> leading = GearOf(assembly, mate->leadingInstanceId());
    const std::optional<SpurGear> following = GearOf(assembly, mate->followingInstanceId());
    if (leading.has_value() == following.has_value()) return std::nullopt;
    return leading ? leading : following;
}

} // namespace

GearMeshResult MeshGears(AssemblyDocument& assembly, std::string name,
                         ObjectId driverInstanceId, ObjectId drivenInstanceId,
                         CoupledFreedom driver, CoupledFreedom driven) {
    const Instance* driverInstance = assembly.findInstance(driverInstanceId);
    const Instance* drivenInstance = assembly.findInstance(drivenInstanceId);
    if (driverInstance == nullptr || drivenInstance == nullptr)
        return refuse("one of these is not an instance of this assembly");
    if (driverInstanceId == drivenInstanceId)
        return refuse("'" + driverInstance->name() + "' cannot mesh with itself");

    // BY NAME, so "this does not work on those two" says which two.
    const std::optional<SpurGear> first = SpurGearOfPath(driverInstance->sourcePath());
    if (!first)
        return refuse("'" + driverInstance->name() +
                      "' is not a gear, so there is no tooth count to take a ratio from");
    const std::optional<SpurGear> second = SpurGearOfPath(drivenInstance->sourcePath());
    if (!second)
        return refuse("'" + drivenInstance->name() +
                      "' is not a gear, so there is no tooth count to take a ratio from");

    const std::string why = WhyPairRefused(*first, *second);
    if (!why.empty())
        return refuse("'" + driverInstance->name() + "' and '" + drivenInstance->name() +
                      "' will not mesh: " + why);

    GearMeshResult out;
    out.ratio = GearRatio(*first, *second);
    out.centreDistanceMm = CentreDistanceMm(*first, *second);
    out.contactRatio = ContactRatio(*first, *second);

    // THE SIGN BECOMES `reversed`, IN ONE PLACE. Relation keeps a magnitude
    // and a flag, GearRatio hands back a signed number, and the conversion
    // between the two conventions is here -- because two conversions is how a
    // pair comes to turn the same way in the solve and opposite ways on the
    // drawing.
    Relation& made = assembly.addRelation(std::move(name), RelationType::Gear, driver, driven,
                                          std::fabs(out.ratio), out.ratio < 0.0);
    out.relationId = made.id();
    out.ok = true;
    return out;
}

std::string WhyMeshDisagrees(const AssemblyDocument& assembly, ObjectId relationId) {
    const Relation* relation = assembly.findRelation(relationId);
    if (relation == nullptr || relation->type() != RelationType::Gear) return {};

    const std::optional<SpurGear> driver = GearBehind(assembly, relation->driver());
    const std::optional<SpurGear> driven = GearBehind(assembly, relation->driven());
    // NOTHING TO COMPARE IS NOT AGREEMENT. A relation between two parts that
    // are not gears is a typed ratio and always will be, and reporting it as
    // checked would be a green light nobody earned.
    if (!driver || !driven) return {};

    const std::string why = WhyPairRefused(*driver, *driven);
    if (!why.empty())
        return "the two gears this couples do not mesh at all: " + why;

    const double wanted = GearRatio(*driver, *driven);
    const double held = (relation->reversed() ? -1.0 : 1.0) * relation->ratio();
    if (std::fabs(wanted - held) < 1e-9) return {};

    // BOTH NUMBERS IN THE SENTENCE. "The ratio is wrong" sends a reader to
    // look for what it should be; the tooth counts and the two ratios say it.
    return "this couples " + std::to_string(driver->teeth) + " teeth to " +
           std::to_string(driven->teeth) + ", which is a ratio of " + ShortNumber(wanted) +
           ", and it is set to " + ShortNumber(held) +
           " -- one of the two gears has been changed since it was made";
}

} // namespace paramcad
