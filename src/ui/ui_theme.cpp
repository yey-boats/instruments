#include "ui_theme.h"

namespace ui {

// Default = night (dark) theme. Backgrounds dark navy, text near-white,
// accents tuned for legibility against sunlight + cabin lighting.
Palette theme = {
    /*bg*/        0x05101c,
    /*panel*/     0x0a2540,
    /*panel_edge*/0x223a55,
    /*fg*/        0xeaf2ff,
    /*fg_dim*/    0x6c8bb1,
    /*accent*/    0x9ec5fe,
    /*warn*/      0xffb84d,
    /*alarm*/     0xff1f3a,
    /*good*/      0x33d17a,
    /*port*/      0xff4d6d,
    /*starboard*/ 0x33d17a,
    /*grid*/      0x3b6294,
};

void use_night() {
    theme = {
        0x05101c, 0x0a2540, 0x223a55, 0xeaf2ff, 0x6c8bb1, 0x9ec5fe,
        0xffb84d, 0xff1f3a, 0x33d17a, 0xff4d6d, 0x33d17a, 0x3b6294,
    };
}

void use_day() {
    // Sun-readable: white background, dark text, saturated accents.
    theme = {
        0xf2f6fb, 0xffffff, 0xc8d4e3, 0x0a1a2b, 0x4d6b8a, 0x0a59c4,
        0xc8801a, 0xb00020, 0x1a8a4f, 0xb00020, 0x1a8a4f, 0x7a96b4,
    };
}

}  // namespace ui
