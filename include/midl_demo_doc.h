#pragma once
// Baked MIDL demo document — 6 screens (Wind / Course / Engine / Power / Race /
// Anchor), synced 1:1 with the MIDL web demo's canonical "standard layout
// library" (midl/library/*.midl.yaml — wind-steering, navigation,
// engine-systems, electrical, racing-vmg, anchor-watch; see
// midl/ts/test/library.test.ts, which asserts these 6 docs validate against
// square-480 and collectively exercise all 9 catalog element types). Each
// screen below is that library doc's `square-480` variant layout verbatim
// (screen id, element types, bindings, format, style — including gauge/bar
// `style.zones` and `format.decimals`, which the renderer is gaining support
// for). Exercises the multi-screen apply_all() render + navigation path
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
    {"id":"dash","title":"Wind","elements":{
      "wind":{"type":"windrose","name":"WIND","format":{"unit":"kn","decimals":1},"bindings":{"value":{"kind":"signalk","path":"environment.wind.speedApparent"},"dir":{"kind":"signalk","path":"environment.wind.angleApparent"}}},
      "sog":{"type":"single-value","name":"SOG","format":{"unit":"kn","decimals":1},"bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}},
      "hdg":{"type":"compass","name":"HDG","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"},"dir":{"kind":"signalk","path":"navigation.headingTrue"}}}},
      "layout":{"flow":"row","children":[{"element":"wind"},{"flow":"col","children":[{"element":"sog"},{"element":"hdg"}]}]}},
    {"id":"nav","title":"Course","elements":{
      "dtw":{"type":"single-value","name":"DTW","format":{"unit":"nm","decimals":1},"bindings":{"value":{"kind":"signalk","path":"navigation.courseGreatCircle.nextPoint.distance"}}},
      "btw":{"type":"single-value","name":"BTW","format":{"unit":"deg","decimals":0},"bindings":{"value":{"kind":"signalk","path":"navigation.courseGreatCircle.nextPoint.bearingTrue"}}},
      "cog":{"type":"compass","name":"COG","bindings":{"value":{"kind":"signalk","path":"navigation.courseOverGroundTrue"},"dir":{"kind":"signalk","path":"navigation.courseOverGroundTrue"}}},
      "xte":{"type":"bar","name":"XTE","format":{"unit":"nm","decimals":2},"style":{"range":[-0.2,0.2],"center":0},"bindings":{"value":{"kind":"signalk","path":"navigation.courseGreatCircle.crossTrackError"}}}},
      "layout":{"rows":2,"cols":2,"cells":[{"element":"dtw"},{"element":"btw"},{"element":"cog"},{"element":"xte"}]}},
    {"id":"engine","title":"Engine","elements":{
      "rpm":{"type":"gauge","name":"RPM","format":{"unit":"rpm","decimals":0},"style":{"range":[0,3600],"zones":[{"lt":600,"color":"warn"},{"lt":3000,"color":"good"},{"lt":3601,"color":"bad"}]},"bindings":{"value":{"kind":"signalk","path":"propulsion.main.revolutions"}}},
      "temp":{"type":"gauge","name":"COOLANT","format":{"unit":"degC","decimals":0},"style":{"range":[0,130],"zones":[{"lt":60,"color":"warn"},{"lt":100,"color":"good"},{"lt":130,"color":"bad"}]},"bindings":{"value":{"kind":"signalk","path":"propulsion.main.temperature"}}},
      "fuel":{"type":"bar","name":"FUEL","format":{"unit":"%","decimals":0},"style":{"range":[0,100],"zones":[{"lt":20,"color":"bad"},{"lt":40,"color":"warn"},{"lt":101,"color":"good"}]},"bindings":{"value":{"kind":"signalk","path":"tanks.fuel.0.currentLevel"}}},
      "oil":{"type":"single-value","name":"OIL","format":{"unit":"bar","decimals":1},"bindings":{"value":{"kind":"signalk","path":"propulsion.main.oilPressure"}}}},
      "layout":{"rows":2,"cols":2,"cells":[{"element":"rpm"},{"element":"temp"},{"element":"fuel"},{"element":"oil"}]}},
    {"id":"power","title":"Power","elements":{
      "soc":{"type":"bar","name":"SOC","format":{"unit":"%","decimals":0},"style":{"range":[0,100],"zones":[{"lt":20,"color":"bad"},{"lt":50,"color":"warn"},{"lt":101,"color":"good"}]},"bindings":{"value":{"kind":"signalk","path":"electrical.batteries.house.capacity.stateOfCharge"}}},
      "volts":{"type":"gauge","name":"VOLTS","format":{"unit":"V","decimals":1},"style":{"range":[10,15],"zones":[{"lt":11.5,"color":"bad"},{"lt":12.2,"color":"warn"},{"lt":15,"color":"good"}]},"bindings":{"value":{"kind":"signalk","path":"electrical.batteries.house.voltage"}}},
      "solar":{"type":"trend","name":"SOLAR","format":{"unit":"W"},"bindings":{"value":{"kind":"signalk","path":"electrical.solar.0.panelPower"}}},
      "amps":{"type":"single-value","name":"CURRENT","format":{"unit":"A"},"bindings":{"value":{"kind":"signalk","path":"electrical.batteries.house.current"}}}},
      "layout":{"rows":2,"cols":2,"cells":[{"element":"soc"},{"element":"volts"},{"element":"solar"},{"element":"amps"}]}},
    {"id":"race","title":"Race","elements":{
      "wind":{"type":"windrose","name":"WIND","bindings":{"value":{"kind":"signalk","path":"environment.wind.speedApparent"},"dir":{"kind":"signalk","path":"environment.wind.angleApparent"}}},
      "vmg":{"type":"trend","name":"VMG","format":{"unit":"kn"},"bindings":{"value":{"kind":"signalk","path":"performance.velocityMadeGood"}}},
      "ap":{"type":"autopilot","name":"PILOT","bindings":{"value":{"kind":"signalk","path":"steering.autopilot.state"}}},
      "tack":{"type":"button","name":"TACK","action":{"kind":"command","target":"steering.autopilot.tack"}}},
      "layout":{"rows":2,"cols":2,"cells":[{"element":"wind"},{"element":"vmg"},{"element":"ap"},{"element":"tack"}]}},
    {"id":"anchor","title":"Anchor","elements":{
      "depth":{"type":"single-value","name":"DEPTH","format":{"unit":"m"},"bindings":{"value":{"kind":"signalk","path":"environment.depth.belowTransducer"}}},
      "heading":{"type":"compass","name":"HEADING","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"},"dir":{"kind":"signalk","path":"navigation.headingTrue"}}},
      "status":{"type":"text","name":"STATE","bindings":{"value":{"kind":"signalk","path":"navigation.state"}}}},
      "layout":{"flow":"row","children":[{"element":"depth"},{"flow":"col","children":[{"element":"heading"},{"element":"status"}]}]}}
  ]
})midl";
// clang-format on
}  // namespace demo
}  // namespace midl
