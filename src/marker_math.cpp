#include "marker_math.h"

#include <string.h>

namespace ui {

// Canonical token table, indexed by GlyphId. Order MUST match the enum and the
// manifest "glyphs" list in src/capabilities.cpp.
static const char *const kGlyphTokens[(uint8_t)GlyphId::COUNT] = {
    "triangle",   "diamond",     "circle",       "bar",           "cross",
    "chevron_in", "chevron_out", "chevron_left", "chevron_right", "chevron_double",
};

GlyphId glyph_from_token(const char *token) {
    if (!token) return GlyphId::COUNT;
    for (uint8_t i = 0; i < (uint8_t)GlyphId::COUNT; ++i) {
        if (strcmp(token, kGlyphTokens[i]) == 0) return (GlyphId)i;
    }
    return GlyphId::COUNT;
}

const char *glyph_to_token(GlyphId g) {
    if ((uint8_t)g >= (uint8_t)GlyphId::COUNT) return "";
    return kGlyphTokens[(uint8_t)g];
}

}  // namespace ui
