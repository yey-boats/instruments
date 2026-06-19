// Host consistency check: the catalog's element WidgetKind names must
// exactly match the firmware's 9 painters (include/ui_layouts.h
// WidgetKind). Update BOTH this list and the catalog when adding a
// painter. Exits non-zero on mismatch.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include "yb_midl_catalog.h"

int main() {
    // Mirror of include/ui_layouts.h WidgetKind (the 9 real painters).
    const std::set<std::string> kinds = {
        "Numeric", "Compass", "Gauge", "Bar", "WindRose", "Autopilot", "Text", "Button", "Trend",
    };
    std::set<std::string> catalog;
    for (size_t i = 0; i < yb_midl_catalog::ELEMENT_COUNT; ++i)
        catalog.insert(yb_midl_catalog::ELEMENTS[i].widget_kind);

    if (catalog != kinds) {
        std::fprintf(stderr, "catalog WidgetKind set != firmware painters\n");
        for (const auto &k : kinds)
            if (!catalog.count(k)) std::fprintf(stderr, "  missing from catalog: %s\n", k.c_str());
        for (const auto &k : catalog)
            if (!kinds.count(k)) std::fprintf(stderr, "  extra in catalog: %s\n", k.c_str());
        return 1;
    }
    std::printf("catalog covers all %zu painters\n", kinds.size());
    return 0;
}
