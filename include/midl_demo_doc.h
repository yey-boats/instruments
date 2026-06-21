#pragma once
// Baked MIDL demo document — 2x2 grid: windrose + 3 value/bar tiles.
// Used by the `midl-render` console command to exercise the render path
// without requiring a live SignalK delivery (Slice 2 / Task 4).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

namespace midl {
namespace demo {
// clang-format off
inline constexpr const char *SQUARE_480_JSON = R"midl({
  "midl":"1.0.0","class":"square-480",
  "screens":[{"id":"midl","title":"MIDL Demo",
    "elements":{
      "wind":{"type":"windrose","name":"WIND","bindings":{"value":{"kind":"signalk","path":"environment.wind.speedApparent"},"dir":{"kind":"signalk","path":"environment.wind.angleApparent"}}},
      "sog":{"type":"single-value","name":"SOG","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}},
      "depth":{"type":"single-value","name":"DEPTH","format":{"unit":"m"},"bindings":{"value":{"kind":"signalk","path":"environment.depth.belowTransducer"}}},
      "batt":{"type":"bar","name":"BATT","format":{"range":[0,1]},"bindings":{"value":{"kind":"signalk","path":"electrical.batteries.house.stateOfCharge"}}}
    },
    "layout":{"rows":2,"cols":2,"cells":[{"element":"wind"},{"element":"sog"},{"element":"depth"},{"element":"batt"}]}
  }]
})midl";
// clang-format on
}  // namespace demo
}  // namespace midl
