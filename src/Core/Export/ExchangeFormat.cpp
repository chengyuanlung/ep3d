#include "Core/Export/ExchangeFormat.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

namespace paramcad {

namespace {

// ONE ROW PER FORMAT, so a format cannot be in the parser and missing from the
// dialog filter. The same shape M39's threads and M56's sections have, and for
// the same reason.
struct Row {
    ExchangeFormat format;
    std::string_view token; // what a saved file or a script calls it
    std::string_view name;  // what a message calls it
    bool canExport;
    bool canImport;
};

constexpr std::array<Row, 3> kFormats{{
    {ExchangeFormat::Step, "Step", "STEP", true, true},
    {ExchangeFormat::Iges, "Iges", "IGES", true, true},
    // STL WRITES AND DOES NOT READ, and that is a decision rather than a gap --
    // see the header.
    {ExchangeFormat::Stl, "Stl", "STL", true, false},
}};

const Row& RowOf(ExchangeFormat format) noexcept {
    for (const Row& row : kFormats)
        if (row.format == format) return row;
    return kFormats.front();
}

std::string LowerSuffixOf(std::string_view path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    // A DOT IN A DIRECTORY NAME NEEDS NO GUARD, and there was one here until
    // the mutation gate showed it could be deleted without changing any
    // answer. It cannot: if the last dot comes before the last separator then
    // what follows it CONTAINS that separator, and no extension does -- so
    // "D:/v1.2/housing" yields ".2/housing", which matches nothing, exactly as
    // the guard intended. A check that can only ever agree with the code after
    // it is one more thing to keep right.
    std::string suffix(path.substr(dot));
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix;
}

} // namespace

std::string_view toString(ExchangeFormat format) noexcept { return RowOf(format).token; }

bool ParseExchangeFormat(std::string_view text, ExchangeFormat& into) noexcept {
    for (const Row& row : kFormats) {
        if (row.token != text) continue;
        into = row.format;
        return true;
    }
    return false;
}

std::string_view NameOf(ExchangeFormat format) noexcept { return RowOf(format).name; }

bool CanExport(ExchangeFormat format) noexcept { return RowOf(format).canExport; }
bool CanImport(ExchangeFormat format) noexcept { return RowOf(format).canImport; }

const std::vector<std::string_view>& ExtensionsOf(ExchangeFormat format) {
    static const std::vector<std::string_view> step{".step", ".stp"};
    static const std::vector<std::string_view> iges{".iges", ".igs"};
    static const std::vector<std::string_view> stl{".stl"};
    static const std::vector<std::string_view> none{};
    switch (format) {
    case ExchangeFormat::Step: return step;
    case ExchangeFormat::Iges: return iges;
    case ExchangeFormat::Stl: return stl;
    }
    return none;
}

std::optional<ExchangeFormat> FormatOfName(std::string_view path) noexcept {
    const std::string suffix = LowerSuffixOf(path);
    if (suffix.empty()) return std::nullopt;
    for (const Row& row : kFormats)
        for (const std::string_view extension : ExtensionsOf(row.format))
            if (extension == suffix) return row.format;
    return std::nullopt;
}

std::string WhyNameRefused(std::string_view path, bool forWriting) {
    std::string offered;
    for (const Row& row : kFormats) {
        if (forWriting ? !row.canExport : !row.canImport) continue;
        for (const std::string_view extension : ExtensionsOf(row.format))
            offered += (offered.empty() ? "" : ", ") + std::string(extension);
    }
    // AND IT SAYS WHY THE ONE THEY ASKED FOR IS NOT THERE, when there is a
    // reason. "Use .step, .iges or .stl" is unhelpful to somebody who just
    // typed .stl at an import prompt: they need to know STL is triangles, not
    // that they mistyped.
    const std::optional<ExchangeFormat> known = FormatOfName(path);
    if (known && !forWriting && !CanImport(*known))
        return "'" + std::string(path) + "' is " + std::string(NameOf(*known)) +
               ", which this program writes but does not read: it is triangles, so what "
               "came back would be a faceted copy of the part rather than the part";
    return "'" + std::string(path) + "' has no extension this can " +
           (forWriting ? "write" : "read") + "; use " + offered;
}

std::string FileDialogFilter(bool forWriting) {
    std::string filter;
    for (const Row& row : kFormats) {
        if (forWriting ? !row.canExport : !row.canImport) continue;
        std::string patterns;
        for (const std::string_view extension : ExtensionsOf(row.format))
            patterns += (patterns.empty() ? "" : " ") + ("*" + std::string(extension));
        filter += (filter.empty() ? "" : ";;") + std::string(row.name) + " (" + patterns + ")";
    }
    return filter;
}

std::optional<ExchangeFormat> FormatOfContents(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    // The first few lines are enough for all three, and reading more of a file
    // that turns out to be neither is wasted work on the path that already
    // failed.
    std::string head(4096, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    if (head.empty()) return std::nullopt;

    // STEP announces itself, by the standard's own rule.
    if (head.find("ISO-10303-21") != std::string::npos) return ExchangeFormat::Step;

    // IGES IS A CARD FORMAT. Every line is 80 columns and column 73 carries the
    // section letter -- S for start, G for global, and F on the flag line of a
    // compressed or binary file. There is no magic number to look for, so the
    // shape of the record IS the identification.
    std::size_t at = 0;
    for (int line = 0; line < 8 && at < head.size(); ++line) {
        std::size_t end = head.find('\n', at);
        if (end == std::string::npos) end = head.size();
        std::size_t length = end - at;
        while (length > 0 && (head[at + length - 1] == '\r')) --length;
        // A BOUNDS GUARD, AND ONLY THAT. The mutation gate showed that both
        // this length check and the carriage-return trim above can be broken
        // without changing what any file is identified as, and that reading is
        // right: on a line shorter than 73 columns the byte at index 72 is the
        // line separator, and a separator is not a section letter. What the
        // check earns is the buffer -- without it the last line of a file can
        // be indexed past its end. Kept for that, and said so, rather than
        // left looking like a discriminator that no test can move.
        if (length >= 73) {
            const char section = head[at + 72];
            if (section == 'S' || section == 'G' || section == 'F') return ExchangeFormat::Iges;
        }
        at = end + 1;
    }

    // STL, both flavours -- worth naming even though nothing reads one, so
    // that a user who tries gets told what the file IS rather than that it is
    // unrecognised.
    if (head.rfind("solid", 0) == 0) return ExchangeFormat::Stl;
    if (head.size() >= 84) {
        std::uint32_t triangles = 0;
        for (int i = 0; i < 4; ++i)
            triangles |= static_cast<std::uint32_t>(static_cast<unsigned char>(head[80 + i]))
                         << (8 * i);
        in.clear();
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size == static_cast<std::streamoff>(84) +
                        static_cast<std::streamoff>(triangles) * 50)
            return ExchangeFormat::Stl;
    }
    return std::nullopt;
}

} // namespace paramcad
