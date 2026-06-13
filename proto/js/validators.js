import Ajv2020 from "ajv/dist/2020.js";
import schema from "../schema/espdisp-control-1.schema.json" with { type: "json" };
const ajv = new Ajv2020({ allowUnionTypes: true, strict: false });
ajv.addSchema(schema, "espdisp-control-1");
export function validator(defName) {
  return ajv.compile({ $ref: `espdisp-control-1#/$defs/${defName}` });
}
export const validate = {
  Attach: validator("Attach"), AttachAck: validator("AttachAck"),
  Switch: validator("Switch"), SwitchAck: validator("SwitchAck"),
  DeviceRecord: validator("DeviceRecord"), ControlState: validator("ControlState"),
};
