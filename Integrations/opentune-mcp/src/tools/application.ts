// SPDX-License-Identifier: AGPL-3.0-or-later
import { z, obj, opaque, track, bool, filePath, text, oneOf, tool } from './common.js';
export const applicationTools = [
  tool('ui.dismiss', 'ui.bridge', obj({ target: z.enum(['dialogs', 'onboarding']), confirm: bool })
    .refine(v => v.target === 'dialogs' || v.confirm === undefined, 'Only dialogs accepts confirm.'),
  'Dismiss dialogs or onboarding through the editor. Dialog confirmation follows the API and may have product side effects.'),
  tool('ui.setSelection', 'ui.bridge', obj({ placementId: opaque.optional(), trackId: track.optional(), openEditor: bool, dryRun: bool })
    .refine(v => oneOf(v, 'placementId', 'trackId'), 'Supply exactly one placementId or trackId.')
    .refine(v => v.trackId === undefined || v.openEditor === undefined, 'openEditor is accepted only with placementId.'),
  'Select exactly one placement or track. openEditor is available for a placement and can generate notes/change workspace or tempo. dryRun previews those effects.'),
  tool('ui.setWorkspace', 'ui.workspace', obj({ workspace: z.enum(['arrangement', 'pianoRoll']) }),
    'Switch the editor workspace; product side effects including tempo changes are returned.'),
  tool('project.save', 'project.io', obj({}), 'Start saving the current project as a non-cancellable job. A new project needs project_save_as.', { open: true }),
  tool('project.saveAs', 'project.io', obj({ path: filePath, confirm: bool }), 'Save as an absolute .otproj path. confirm controls replacing an existing file. Returns a non-cancellable job.', { open: true }),
  tool('project.open', 'project.io', obj({ path: filePath, discardUnsaved: bool }), 'Open a local .otproj project as a job. discardUnsaved explicitly allows replacing dirty state. Handles change with the project epoch.', { open: true }),
  tool('project.clearRecent', 'project.io', obj({}), 'Clear the application’s recent-project list.', { idempotent: true, open: true }),
  tool('project.saveCopy', 'project.copy', obj({ path: filePath }), 'Write a project copy to an absolute .otproj path while retaining current project identity. Returns a job.', { open: true }),
  tool('prefs.set', 'prefs.write', obj({ tuningHz: z.number().min(400).max(500).optional(),
    editingScheme: z.enum(['openTune', 'openDyne']).optional(), hybridMode: bool, experimentalFeatures: bool,
    vocoderWeight: text(64).optional(), renderingPriority: z.enum(['gpuFirst', 'cpuFirst']).optional(),
    f0Model: text(64).optional(), accessibilityModeEnabled: bool,
  }).refine(v => Object.keys(v).some(k => k !== 'instanceId'), 'Supply at least one preference.'),
  'Set application preferences. Read prefs_get processing options before selecting a model or vocoder. Changes can affect rendering.', { open: true }),
  tool('audio.set', 'audio.device', obj({ deviceType: text(64).optional(), outputDevice: text(256).optional(),
    sampleRateHz: z.number().min(2.2250738585072014e-308).max(1e9).optional(), bufferSizeFrame: z.number().int().min(1).max(2147483647).optional(),
    outputChannelPair: z.number().int().min(0).max(2147483647).optional(),
  }).refine(v => Object.keys(v).some(k => k !== 'instanceId'), 'Supply at least one audio setting.'),
  'Set audio device configuration. Read audio_get for supported values first; changes can restart the audio device.', { open: true }),
  tool('app.quit', 'app.quit', obj({ discardUnsaved: bool }), 'Explicitly quit the selected OpenTune instance. discardUnsaved permits discarding dirty state. OpenTune replies before exiting.', { open: true }),
];
