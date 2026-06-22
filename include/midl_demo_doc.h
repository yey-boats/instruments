#pragma once
// Baked MIDL demo document — 3 screens (Dashboard / Navigation / Speed), each a
// 2x2 grid. Exercises the multi-screen apply_all() render + navigation path
// without requiring a live SignalK delivery. Used by the `midl-render` console
// command, the ConfigApplyMidl pump case, and the YEYBOATS_MIDL_ONLY boot.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

namespace midl {
namespace demo {
// clang-format off
inline constexpr const char *SQUARE_480_JSON = R"midl({
  "midl":"1.0.0","class":"square-480",
  "settings":{"defaultScreen":"dash"},
  "screens":[
    {"id":"dash","title":"Dashboard","elements":{
      "wind":{"type":"windrose","name":"WIND","bindings":{"value":{"kind":"signalk","path":"environment.wind.speedApparent"},"dir":{"kind":"signalk","path":"environment.wind.angleApparent"}}},
      "sog":{"type":"single-value","name":"SOG","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}},
      "depth":{"type":"single-value","name":"DEPTH","format":{"unit":"m"},"bindings":{"value":{"kind":"signalk","path":"environment.depth.belowKeel"}}},
      "batt":{"type":"bar","name":"BATT","format":{"range":[0,1]},"bindings":{"value":{"kind":"signalk","path":"electrical.batteries.house.stateOfCharge"}}}},
      "layout":{"rows":2,"cols":2,"cells":[{"element":"wind"},{"element":"sog"},{"element":"depth"},{"element":"batt"}]}},
    {"id":"nav","title":"Navigation","elements":{
      "hdg":{"type":"compass","name":"HDG","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"},"dir":{"kind":"signalk","path":"navigation.courseRhumbline.bearingTrackTrue"}}},
      "sog":{"type":"single-value","name":"SOG","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}},
      "cog":{"type":"single-value","name":"COG","format":{"unit":"deg"},"bindings":{"value":{"kind":"signalk","path":"navigation.courseOverGroundTrue"}}},
      "pos":{"type":"text","name":"POS","bindings":{"value":{"kind":"signalk","path":"navigation.position"}}}},
      "layout":{"rows":2,"cols":2,"cells":[{"element":"hdg"},{"element":"sog"},{"element":"cog"},{"element":"pos"}]}},
    {"id":"speed","title":"Speed","elements":{
      "sog":{"type":"single-value","name":"SOG","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}},
      "stw":{"type":"single-value","name":"STW","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"navigation.speedThroughWater"}}},
      "depth":{"type":"single-value","name":"DEPTH","format":{"unit":"m"},"bindings":{"value":{"kind":"signalk","path":"environment.depth.belowKeel"}}},
      "aws":{"type":"single-value","name":"AWS","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"environment.wind.speedApparent"}}}},
      "layout":{"rows":2,"cols":2,"cells":[{"element":"sog"},{"element":"stw"},{"element":"depth"},{"element":"aws"}]}}
  ]
})midl";
// clang-format on
}  // namespace demo
}  // namespace midl
