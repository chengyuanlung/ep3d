#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// M57 -- WHAT A FILE NAME MEANS, decided in one place.
//
// This file exists because of what M57 found rather than what M57 added.
// Adding IGES was meant to be half a milestone: OCCT reads and writes it, and
// the STEP path was right there to copy. What was there to copy was the
// PROBLEM.
//
// "Which format is this?" was answered twice -- once in the script's `export`
// command and once in the viewer's Save As -- as two if/else chains over the
// same extensions, each with its own wording for the refusal. They agreed
// because somebody kept them in step by hand. Adding a third format to two
// chains is how a program comes to write IGES from the command line and not
// from the menu, and nothing about either copy looks wrong on its own.
//
// So there is one now, and the failure mode this closes is specific: a format
// added to one caller and not the other.

enum class ExchangeFormat {
    Step, // ISO 10303, AP214
    Iges, // IGES 5.3
    Stl,  // triangles
};

std::string_view toString(ExchangeFormat format) noexcept;
bool ParseExchangeFormat(std::string_view text, ExchangeFormat& into) noexcept;
// What a message calls it: "STEP", "IGES", "STL".
std::string_view NameOf(ExchangeFormat format) noexcept;
// Including the dot, in the order a file dialog should offer them.
const std::vector<std::string_view>& ExtensionsOf(ExchangeFormat format);

// WHAT CAN BE READ IS NOT WHAT CAN BE WRITTEN, and the asymmetry is real
// rather than an omission: STL is triangles, so reading one back would give a
// faceted approximation of a part in place of the part, and every downstream
// operation -- a fillet, a section, a dimension -- would work on the facets.
// A program that imported STL would look like it round-tripped.
bool CanExport(ExchangeFormat format) noexcept;
bool CanImport(ExchangeFormat format) noexcept;

// --- Deciding by name -------------------------------------------------------

// THE FORMAT A NAME ASKS FOR. Used when WRITING, because the extension is what
// the file will be read as at the far end -- asking for the format separately
// would let a .step be written as STL, and every reader would then refuse it
// for a reason that names neither this program nor the choice.
std::optional<ExchangeFormat> FormatOfName(std::string_view path) noexcept;

// The one sentence every caller prints when a name names nothing, listing what
// it could have said. Two callers with two wordings is how a user learns that
// the menu and the script disagree about what this program can do.
std::string WhyNameRefused(std::string_view path, bool forWriting);

// "STEP (*.step *.stp);;IGES (*.iges *.igs);;STL (*.stl)" -- the Qt file
// dialog's filter, built from the same list the parser reads, so a format
// cannot be offered in the dialog and refused by the writer.
std::string FileDialogFilter(bool forWriting);

// --- Deciding by content ----------------------------------------------------

// WHAT THE FILE ACTUALLY IS, read from its first lines.
//
// Asked of the content and not the extension, for the reason
// IsAssemblySourceFile gives: the extension is a convention and the header is
// the format. Exchange files get renamed more than any other kind in a
// mechanical shop -- a supplier sends `housing.stp` that is IGES inside, and a
// reader that trusted the name would report a STEP syntax error about a
// perfectly good IGES file.
//
// Nothing, when the file cannot be opened or is none of these.
std::optional<ExchangeFormat> FormatOfContents(const std::string& path);

} // namespace paramcad
