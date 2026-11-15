#include "layout.h"

#include <string.h>

namespace layout {

ZoomAction zoom_action(bool zoomable, const char *zoom) {
    if (!zoomable) return ZOOM_NONE;
    if (!zoom || !zoom[0] || strcmp(zoom, "auto") == 0) return ZOOM_AUTO_SCALE;
    return ZOOM_SHOW_SCREEN;
}

}  // namespace layout
