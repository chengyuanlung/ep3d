#pragma once

#include "Core/Document/ObjectId.h"

#include <map>
#include <string>
#include <vector>

namespace paramcad {

// M54 -- VARIANTS: one part, several sizes.
//
// A bracket in three lengths, a shaft in five diameters, a panel in the sizes
// the shop actually stocks. Inventor calls it an iPart, and the roadmap put it
// after the drawing work for a reason that has held: it MULTIPLIES. Built
// while parts, assemblies and drawings were still moving, everything would
// have been done three times.
//
// THE FAILURE THIS IS FOR: a drawing of variant B carrying variant A's
// dimensions. Both are real sizes of a real part, every number is one the part
// has had, and nothing on the sheet is dangling or red. The wrong bracket gets
// made, in the right quantity, to a drawing that checks out.
//
// SO NOTHING REMEMBERS WHICH VARIANT IS ACTIVE.
//
// A stored "current variant" is a second copy of a fact the parameters already
// hold, and it goes stale the first time somebody nudges a dimension: the
// document goes on saying B while the model is B-with-a-change. Instead the
// question is answered by COMPARING -- the active variant is the one whose
// values the parameters currently have, and when they have nobody's, the
// answer is that there is no active variant. There is nowhere to keep a stale
// answer, so there is no stale answer to keep.
//
// It is the same move as M42's balloon (no stored number), M38's sections (no
// stored letter) and M46's interference (an answer that knows when it stopped
// being true).

// ONE ROW OF THE TABLE.
struct PartVariant {
    std::string name;
    // WHICH PARAMETERS, AND WHAT THEY ARE IN THIS ROW.
    //
    // Ordered by id so two rows of the same table compare and serialise the
    // same way whatever order they were typed in.
    std::map<ObjectId, double> values;
};

// A VARIANT TABLE HAS COLUMNS, and every row fills every one of them.
//
// The first variant added fixes the set of parameters this part varies by;
// every later row has to name exactly those. A row that named fewer would be a
// size with a dimension nobody stated -- and "whichever value happened to be
// in the parameter when you switched" is not a specification, it is a
// leftover.
//
// It also makes "which variant is this" a clean comparison rather than a
// search for the best partial match, which is the kind of question that has a
// defensible answer and no correct one.
std::string WhyVariantRefused(const PartVariant& variant,
                              const std::vector<PartVariant>& existing);

// WHICH VARIANT THESE VALUES ARE, or empty when they are nobody's.
//
// `current` is what the parameters hold right now. Compared exactly: a variant
// is a stated size, and "close enough to B" is how a part ends up being made
// to a number nobody chose.
std::string WhichVariantMatches(const std::vector<PartVariant>& variants,
                                const std::map<ObjectId, double>& current);

// The columns this table varies by, taken from its first row -- or empty for a
// part with no variants.
std::vector<ObjectId> VariantColumns(const std::vector<PartVariant>& variants);

// Finds a row by name, or nullptr.
const PartVariant* FindVariant(const std::vector<PartVariant>& variants,
                               const std::string& name);

} // namespace paramcad
