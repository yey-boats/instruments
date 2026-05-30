#include "font_resolver.h"

namespace font_resolver {

uint16_t resolve(uint16_t requested, const uint16_t *sizes, size_t count) {
    if (!sizes || count == 0) return requested;
    // Smallest / largest are bounds.
    if (requested <= sizes[0]) return sizes[0];
    if (requested >= sizes[count - 1]) return sizes[count - 1];
    // Walk to find the largest entry that is <= requested.
    uint16_t best = sizes[0];
    for (size_t i = 0; i < count; ++i) {
        if (sizes[i] == requested) return sizes[i];  // exact
        if (sizes[i] <= requested)
            best = sizes[i];
        else
            break;  // sizes sorted ascending; further entries can't be lower
    }
    return best;
}

}  // namespace font_resolver
