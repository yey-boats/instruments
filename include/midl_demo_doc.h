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
      "layout":{"rows":2,"cols":2,"cells":[{"element":"sog"},{"element":"stw"},{"element":"depth"},{"element":"aws"}]}},
    {"id":"steering","title":"Steering","elements":{
      "hdg":{"type":"compass","name":"HDG","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"},"dir":{"kind":"signalk","path":"navigation.courseRhumbline.bearingTrackTrue"}}},
      "rud":{"type":"gauge","name":"RUDDER","format":{"range":[-35,35],"precision":0,"unit":"deg"},"bindings":{"value":{"kind":"signalk","path":"steering.rudderAngle"}}},
      "xte":{"type":"single-value","name":"XTE","format":{"unit":"nm"},"bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.crossTrackError"}}},
      "vmg":{"type":"single-value","name":"VMG","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.velocityMadeGood"}}},
      "n10":{"type":"button","name":"-10","action":{"kind":"command","target":"autopilot heading -10"}},
      "n1":{"type":"button","name":"-1","action":{"kind":"command","target":"autopilot heading -1"}},
      "p1":{"type":"button","name":"+1","action":{"kind":"command","target":"autopilot heading 1"}},
      "p10":{"type":"button","name":"+10","action":{"kind":"command","target":"autopilot heading 10"}}},
      "layout":{"flow":"col","weights":[3,1],"children":[
        {"rows":2,"cols":2,"cells":[{"element":"hdg"},{"element":"rud"},{"element":"xte"},{"element":"vmg"}]},
        {"flow":"row","children":[{"element":"n10"},{"element":"n1"},{"element":"p1"},{"element":"p10"}]}]}},
    {"id":"route","title":"Route","elements":{
      "dtw":{"type":"single-value","name":"DTW","format":{"unit":"nm"},"bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.nextPoint.distance"}}},
      "btw":{"type":"single-value","name":"BTW","format":{"unit":"deg"},"bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.nextPoint.bearingTrue"}}},
      "xte":{"type":"single-value","name":"XTE","format":{"unit":"nm"},"bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.crossTrackError"}}},
      "vmg":{"type":"single-value","name":"VMG","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.velocityMadeGood"}}}},
      "layout":{"rows":2,"cols":2,"cells":[{"element":"dtw"},{"element":"btw"},{"element":"xte"},{"element":"vmg"}]}}
  ]
})midl";
// clang-format on
}  // namespace demo
}  // namespace midl
