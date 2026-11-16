// SPDX-License-Identifier: AGPL-3.0-or-later
import { z, obj, opaque, revision, sec, bool, key, tuneKey, tuningParams, contentRange, outputRange, unique, anyOf, tool } from './common.js';
const select = z.union([
  obj({ indices: z.array(z.number().int().min(0).max(9e15)).min(1).max(1000).refine(unique, 'Indices must be unique.') }),
  obj({ range: contentRange }), obj({ all: z.literal(true) }),
]);
const noteOp = z.discriminatedUnion('op', [
  obj({ op: z.literal('setTargetPitch'), select, targetMidi: z.number().min(0).max(127) }),
  obj({ op: z.literal('shiftCents'), select, deltaCents: z.number().min(-1200).max(1200) }),
  obj({ op: z.literal('snapToScale'), select, key: tuneKey }),
  obj({ op: z.literal('setParams'), select, ...tuningParams, pitchDriftScale: z.number().min(-2).max(3).optional() })
    .refine(v => anyOf(v, ['retuneSpeed', 'vibratoDepth', 'vibratoRateHz', 'pitchDriftScale']), 'setParams needs at least one parameter.'),
]);
export const contentTools = [
  tool('content.setKey', 'edit.v1', obj({ contentId: opaque, key, expectedKeyRevision: revision.optional() }),
    'Set content key/scale after reading current content. Returns the owner’s dirty, history and revert semantics.'),
  tool('content.autoTune', 'edit.v1', obj({
    contentId: opaque, mode: z.enum(['regenerate', 'snapNotes', 'notesOnly']), expectedRevision: revision,
    ranges: z.array(contentRange.refine(v => v.endContentSec >= v.startContentSec, 'End must be at or after start.')).min(1).max(16).optional(),
    key: tuneKey.optional(), expectedKeyRevision: revision.optional(),
    params: obj({ ...tuningParams, noteSplitCents: z.number().min(0).max(200).optional() }).optional(), dryRun: bool,
  }).refine(v => v.mode === 'notesOnly' ? v.key === undefined && v.ranges === undefined : v.key !== undefined,
    'regenerate/snapNotes require key; notesOnly accepts neither key nor ranges.')
    .refine(v => v.mode !== 'snapNotes' || v.params === undefined, 'snapNotes accepts no params.')
    .refine(v => v.key !== 'clipKey' || v.expectedKeyRevision !== undefined, 'clipKey requires expectedKeyRevision.'),
  'Auto-tune existing analyzed content using a current expectedRevision. Ranges use content seconds. clipKey requires expectedKeyRevision. dryRun previews the same owner operation.'),
  tool('content.editNotes', 'edit.v1', obj({ contentId: opaque, expectedRevision: revision,
    expectedKeyRevision: revision.optional(), ops: z.array(noteOp).min(1).max(32), dryRun: bool,
  }).refine(v => v.expectedKeyRevision !== undefined || !v.ops.some(op => op.op === 'snapToScale' && op.key === 'clipKey'), 'clipKey requires expectedKeyRevision.'),
  'Edit target pitch, cents, scale snap or tuning parameters of existing notes. Read notes/revision first; indices refer to that revision. Ranges use content seconds. dryRun previews; commit uses the same revision.'),
  tool('content.setPitchShift', 'edit.v1', obj({ contentId: opaque,
    pitchShift: obj({ semitones: z.number().int().min(-24).max(24), cents: z.number().int().min(-99).max(99) }),
  }), 'Set whole-content pitch shift. This API takes no expectedRevision; returns actual changes and undo state.'),
  tool('content.shiftTiming', 'timing.v1', obj({ contentId: opaque, expectedGridRevision: revision,
    range: z.union([contentRange, outputRange]), deltaSec: z.number().min(-10).max(10), dryRun: bool,
  }), 'Shift an existing seeded time grid by deltaSec within a content- or output-clock range. Read grid revision first. dryRun previews; result reports clamping.'),
  tool('content.editTimeGrid', 'timing.v1', obj({ contentId: opaque, op: z.enum(['seed', 'moveHandles']),
    expectedGridRevision: revision.optional(), moves: z.array(obj({ handleId: opaque.refine(v => Buffer.byteLength(v) <= 64), outputSec: sec })).min(1).max(64).optional(), dryRun: bool,
  }).refine(v => v.op === 'seed'
    ? v.expectedGridRevision === undefined && v.moves === undefined && v.dryRun === undefined
    : v.expectedGridRevision !== undefined && v.moves !== undefined,
  'seed takes only contentId/op; moveHandles requires expectedGridRevision and moves.'),
  'Seed a time grid as a job, or move existing handles to absolute output seconds. Read handles and grid revision first. Only moveHandles accepts dryRun.'),
  ...['undo', 'redo'].map(action => tool(`edit.${action}`, 'history.v1', obj({ expectedUndoId: opaque.optional(), force: bool })
    .refine(v => v.force === true || v.expectedUndoId !== undefined, 'expectedUndoId is required unless force is true.'),
  `${action === 'undo' ? 'Undo' : 'Redo'} through the product owner. Read edit_get_history first and pass expectedUndoId. force explicitly bypasses the identity check.`)),
];
