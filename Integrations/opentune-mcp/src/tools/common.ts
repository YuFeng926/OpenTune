// SPDX-License-Identifier: AGPL-3.0-or-later
import { z } from 'zod';
export { z };
export const obj = z.strictObject;
export const opaque = z.string().min(1).describe('Opaque value from OpenTune; pass unchanged.');
export const revision = opaque.max(128).regex(/^[A-Za-z0-9._:-]+$/);
export const sec = z.number().nonnegative().describe('Absolute seconds in the field’s named clock; preserve the API value.');
export const track = z.number().int().min(0).max(2147483647);
export const bool = z.boolean().optional();
export const text = (max: number) => z.string().min(1).refine(v => [...v].length <= max, `At most ${max} Unicode code points.`);
export const filePath = text(4096).describe('Absolute local path on the OpenTune machine.');
export const contentRange = obj({ startContentSec: sec, endContentSec: sec });
export const timelineRange = obj({ startTimelineSec: sec, endTimelineSec: sec });
export const outputRange = obj({ startOutputSec: sec, endOutputSec: sec });
export const key = obj({ root: z.number().int().min(0).max(11), scale: z.enum([
  'major', 'minor', 'chromatic', 'harmonicMinor', 'dorian', 'mixolydian', 'pentatonicMajor', 'pentatonicMinor',
]) });
export const tuneKey = z.union([key, z.literal('clipKey')]);
export const tuningParams = {
  retuneSpeed: z.number().min(0).max(1).optional(), vibratoDepth: z.number().min(0).max(100).optional(),
  vibratoRateHz: z.number().min(3).max(12).optional(),
};
export const unique = <T>(values: T[]) => new Set(values).size === values.length;
export const oneOf = (v: Record<string, unknown>, a: string, b: string) => (v[a] !== undefined) !== (v[b] !== undefined);
export const anyOf = (v: Record<string, unknown>, keys: string[]) => keys.some(k => v[k] !== undefined);

export interface ToolDefinition {
  name: string; method: string; capability: string; description: string;
  schema: z.ZodObject;
  annotations: { readOnlyHint: boolean; destructiveHint: boolean; idempotentHint: boolean; openWorldHint: boolean };
}
export function tool(method: string, capability: string, schema: z.ZodObject, description: string,
  hints: { read?: boolean; destructive?: boolean; idempotent?: boolean; open?: boolean } = {}): ToolDefinition {
  return {
    name: method.replaceAll('.', '_').replace(/[A-Z]/g, c => `_${c.toLowerCase()}`), method, capability,
    description: `${description} OTCP: ${method}.`,
    schema: schema.safeExtend({ instanceId: opaque.describe('Target instanceId from instances_list.') }),
    annotations: {
      readOnlyHint: hints.read ?? false, destructiveHint: hints.destructive ?? !hints.read,
      idempotentHint: hints.idempotent ?? hints.read ?? false, openWorldHint: hints.open ?? false,
    },
  };
}
