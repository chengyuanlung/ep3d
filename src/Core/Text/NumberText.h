#pragma once

#include <string>

namespace paramcad {

// M47 -- ONE WAY TO WRITE A NUMBER DOWN.
//
// This function existed three times, byte for byte identical, in
// Annotation.cpp, HoleStandards.cpp and StandardParts.cpp. They agreed. They
// agreed because somebody kept them in step by hand, and each was covered by
// its own tests, which is precisely the shape this project keeps closing:
// two things that must match, copied, each tested alone.
//
// AND THE MATCH IS LOAD-BEARING, not cosmetic. HoleStandards writes the thread
// designation M8x1.25 that a hole callout and a hole table both carry.
// StandardParts writes std:ISO 4762 M8x30, and that string is not a label --
// it is the path a part instance resolves through (M45). Let one copy round
// differently from another and a catalogue part stops being found, or a hole
// table lists a thread the callout beside it spells another way. Neither
// failure is visible in the copy that changed.
//
// So there is one now, and the fourth caller (M47's weld sizes) could not have
// added a fourth copy even by accident.

// A measurement as a drawing writes it: 3.2, not 3.200000. Four decimals is
// where it rounds, which is finer than any drawing states and coarse enough
// that binary noise never reaches the paper.
std::string ShortNumber(double value);

} // namespace paramcad
