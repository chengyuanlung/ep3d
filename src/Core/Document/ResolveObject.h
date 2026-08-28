#pragma once

#include "Core/Document/ObjectId.h"

namespace paramcad {

class ObjectRegistry;
class ISolidFeature;
class Parameter;
class Sketch;

// M59 -- LOOKING A DOCUMENT OBJECT UP BY ID, ONCE.
//
// This was twenty-six copies of three functions.
//
// Every feature that reads a Parameter carried its own file-local
// `resolveParameter`: BoxFeature, PadFeature, PocketFeature, RevolveFeature,
// HoleFeature, ShellFeature, DraftFeature, EdgeDressFeatures,
// SheetContourFeature, TransformFeatures. Every feature that reads a Sketch
// carried its own `resolveSketch` -- one of them under a different NAME,
// `resolveSketchFor`, which is how a copy hides from the search that would
// have found it. Eight more resolved a solid feature. M59 needed to look a
// parameter up for the eleventh time, which is what put the question.
//
// AND THEY HAD ALREADY DRIFTED. The ten fell into three variants -- two
// differing only in a comment, and a third that begins
//
//     if (id == kInvalidObjectId) return nullptr;
//
// which the other nine do not have. Somebody added that guard where they
// needed it and it stayed where they put it. That is the whole failure mode
// this project keeps closing, caught in the act: nine copies that never
// learned the tenth's lesson, each covered by its own tests, all passing.
//
// The guard is kept here, because it is the strictest of the three and asking
// a registry about the invalid id is not a question with a useful answer.

// The Parameter with this id, or nullptr when there is none -- or when the id
// names something that is not a Parameter, which is the case that matters: a
// feature holding an id that now belongs to a sketch must not read it as a
// number.
const Parameter* ResolveParameter(const ObjectRegistry& registry, ObjectId id);

// The Sketch with this id, on the same terms.
const Sketch* ResolveSketch(const ObjectRegistry& registry, ObjectId id);

// The solid-producing feature with this id, on the same terms.
//
// Eight more copies, in three variants -- BooleanFeature, PocketFeature,
// HoleFeature, ShellFeature, DraftFeature, EdgeDressFeatures,
// TransformFeatures and MassPropertiesNode. This one has a second step the
// others do not: the registry holds an IRecomputable and the caller wants the
// solid interface, so there is a cast. That made it the most worth writing
// once, and it was the one written eight times.
const ISolidFeature* ResolveSolidFeature(const ObjectRegistry& registry, ObjectId id);

} // namespace paramcad
