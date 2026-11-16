// SPDX-License-Identifier: AGPL-3.0-or-later
import { z, obj, opaque, track, sec, bool, filePath, tool } from './common.js';
const jobId = opaque.regex(/^j:[0-9a-f]{8}:[1-9][0-9]*$/);
export const jobTools = [
  tool('job.get', 'jobs.v1', obj({ jobId, waitMs: z.number().int().min(0).max(30000).optional() }),
    'Read one task snapshot, optionally waiting once up to 30000 ms (default 0). Check job.state: succeeded, failed or cancelled is terminal. MCP cancellation ends only this wait.', { read: true }),
  tool('job.cancel', 'jobs.v1', obj({ jobId }), 'Request cancellation of a cancellable task. cancelRequested is acknowledgement; read job_get for its final state. Saves are non-cancellable.', { idempotent: true }),
  tool('import.start', 'import.v1', obj({ items: z.array(obj({ path: filePath })).min(1).max(64),
    layout: z.enum(['single', 'sequential', 'separateTracks']), trackId: track.max(2147483647).optional(), newTrack: bool,
    startTimelineSec: sec.max(1e7).optional(), generateNotes: bool, focus: bool,
    idempotencyKey: z.string().min(1).max(128).regex(/^[!-~]+$/).optional(),
  }).refine(v => v.layout !== 'single' || v.items.length === 1, 'single imports exactly one item.')
    .refine(v => v.trackId === undefined || v.newTrack === undefined, 'trackId and newTrack are mutually exclusive.'),
  'Start a local audio import job at absolute timeline seconds. Read session_info limits first. Defaults: newTrack false, generateNotes false, focus true. Optional idempotencyKey follows API retention rules.', { destructive: false, open: true }),
  tool('export.start', 'export.v1', obj({ kind: z.enum(['clip', 'track', 'master']),
    target: z.union([obj({ placementId: opaque }), obj({ trackId: track.max(2147483647) })]).optional(),
    path: filePath, ifExists: z.enum(['error', 'rename', 'overwrite']).optional(), allowDry: bool,
  }).refine(v => v.kind === 'master' ? v.target === undefined
    : v.target !== undefined && (v.kind === 'clip' ? 'placementId' in v.target : 'trackId' in v.target),
  'master has no target; clip requires {placementId}; track requires {trackId}.'),
  'Export WAV to an absolute local path as a job. ifExists defaults to error; allowDry defaults to false. Inspect the final job state and result before using the file.', { open: true }),
];
