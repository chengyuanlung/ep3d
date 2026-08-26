#include "Core/Text/NumberText.h"

#include <cstdio>

namespace paramcad {

std::string ShortNumber(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", value);
    std::string out(text);
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

} // namespace paramcad
