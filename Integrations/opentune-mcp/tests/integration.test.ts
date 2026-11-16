// SPDX-License-Identifier: AGPL-3.0-or-later
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { watch, createWriteStream } from 'node:fs';
import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { mcp } from './helpers.ts';

const executable = process.env.OPENTUNE_TEST_APP;
test('real Standalone: import, preview, edit, undo, export and save/reopen', { skip: !executable, timeout: 180000 }, async t => {
  const directory = await mkdtemp(path.join(tmpdir(), 'opentune-mcp-live-'));
  t.diagnostic(`Integration artifacts: ${directory}`);
  await writeFile(path.join(directory, 'app-preferences.settings'), '<PROPERTIES><VALUE name="shared.onboardingShown" val="1"/><VALUE name="shared.demoTourConsumed" val="1"/></PROPERTIES>');
  const readyFile = path.join(directory, 'ready.json');
  let notifyReady: () => void;
  const ready = new Promise<void>(resolve => { notifyReady = resolve; });
  const watcher = watch(directory, (_event, file) => { if (file === 'ready.json') notifyReady(); });
  t.after(() => watcher.close());
  const app = spawn(executable!, ['--control-api', `--control-api-ready-file=${readyFile}`], {
    env: { ...process.env, OPENTUNE_DATA_DIR: directory }, stdio: ['ignore', 'pipe', 'pipe'],
  });
  const output = createWriteStream(path.join(directory, 'app.log'));
  app.stdout.pipe(output, { end: false });
  app.stderr.pipe(output, { end: false });
  const exited = once(app, 'exit');
  t.after(async () => {
    if (app.exitCode === null && app.signalCode === null) app.kill('SIGKILL');
    await exited;
    output.end();
  });
  await Promise.race([ready, exited.then(([code]) => { throw new Error(`Standalone exited before ready: ${code}`); })]);
  watcher.close();
  const { client } = await mcp(t, path.join(directory, 'control', 'instances'));
  const discovery = await client.callTool({ name: 'instances_list', arguments: {} });
  const instances = discovery.structuredContent!.instances as { instanceId: string; pid: number }[];
  assert.equal(instances.length, 1);
  assert.equal(instances[0]!.pid, app.pid);
  const instanceId = instances[0]!.instanceId;
  const seen = new Set<string>(['instances_list']);
  const call = async (name: string, args = {}) => {
    seen.add(name);
    const response = await client.callTool({ name, arguments: { instanceId, ...args } });
    assert.equal(response.isError, false, `${name}: ${JSON.stringify(response.structuredContent ?? response.content)}`);
    return response.structuredContent!.result as any;
  };
  const finish = async (started: any) => {
    if (!started.job) return started;
    const result = await call('job_get', { jobId: started.job.jobId, waitMs: 30000 });
    assert.equal(result.job.state, 'succeeded', JSON.stringify(result));
    return result.job.result;
  };
  assert.equal((await call('session_info')).protocol, '1.0');
  await call('ui_dismiss', { target: 'onboarding' });
  assert.equal((await call('project_get')).placements.length, 0);
  const audioPath = process.env.OPENTUNE_TEST_AUDIO ?? path.resolve('../../assets/《赐我》-苡柠-非商用演示.wav');
  const imported = await finish(await call('import_start', {
    items: [{ path: audioPath }], layout: 'single', newTrack: true,
    startTimelineSec: 0, generateNotes: true, idempotencyKey: 'integration-import',
  }));
  assert.equal(imported.counts.imported, 1);
  assert.equal(imported.items[0].f0State, 'ready');
  assert.equal(imported.items[0].notesGenerated, true);
  const project = await call('project_get');
  const { contentId, placementId, trackId } = project.placements[0];
  const notePage = await call('content_get_notes', { contentId, limit: 500 });
  assert.ok(notePage.notes.length > 0);
  const note = notePage.notes.find((n: any) => n.voiced);
  const editedPitch = note.targetMidi === 60 ? 62 : 60;
  const edit = {
    contentId, expectedRevision: notePage.asOf.contents[0].revision,
    ops: [{ op: 'setTargetPitch', select: { indices: [note.index] }, targetMidi: editedPitch }],
  };
  const preview = await call('content_edit_notes', { ...edit, dryRun: true });
  assert.equal(preview.dryRun, true);
  assert.equal(preview.applied, false);
  assert.deepEqual((await call('content_get_notes', { contentId, limit: 500 })).notes, notePage.notes);
  const committed = await call('content_edit_notes', edit);
  assert.equal(committed.applied, true);
  assert.equal(committed.undo.undoable, true);
  assert.equal(committed.dirty.after, true);
  assert.equal((await call('content_get_notes', { contentId, limit: 500 })).notes[note.index].targetMidi, editedPitch);
  const conflict = await client.callTool({ name: 'content_edit_notes', arguments: { instanceId, ...edit } });
  assert.equal(conflict.isError, true);
  const conflictError = conflict.structuredContent!.error as any;
  assert.equal(conflictError.source, 'otcp');
  assert.equal(conflictError.data.kind, 'REVISION_CONFLICT');
  assert.equal(conflictError.data.executed, false);
  assert.equal(typeof conflictError.data.retryable, 'boolean');
  let history = (await call('edit_get_history')).history;
  await call('edit_undo', { expectedUndoId: history.undoTop.undoId });
  assert.equal((await call('content_get_notes', { contentId, limit: 500 })).notes[note.index].targetMidi, note.targetMidi);
  history = (await call('edit_get_history')).history;
  await call('edit_redo', { expectedUndoId: history.redoTop.undoId });
  assert.equal((await call('content_get_notes', { contentId, limit: 500 })).notes[note.index].targetMidi, editedPitch);
  const details = await call('content_get_details', { contentId });
  assert.equal(details.analysis.f0State, 'ready');
  assert.ok((await call('content_get_pitch_curve', { contentId, limit: 10 })).pointCount > 0);
  await call('content_segment_notes', { contentId, noteSplitCents: 50 });
  await call('content_set_key', { contentId, key: { root: 0, scale: 'major' }, expectedKeyRevision: details.asOf.contents[0].keyRevision });
  const tunedDetails = await call('content_get_details', { contentId });
  await call('content_auto_tune', { contentId, mode: 'snapNotes', key: 'clipKey', expectedKeyRevision: tunedDetails.asOf.contents[0].keyRevision,
    expectedRevision: tunedDetails.asOf.contents[0].revision, dryRun: true });
  await call('content_set_pitch_shift', { contentId, pitchShift: { semitones: 0, cents: 0 } });
  await finish(await call('content_edit_time_grid', { contentId, op: 'seed' }));
  const grid = await call('content_get_details', { contentId });
  assert.ok(grid.timeGrid.handles.length > 0);
  const handle = grid.timeGrid.handles[0];
  await call('content_edit_time_grid', { contentId, op: 'moveHandles', expectedGridRevision: grid.asOf.contents[0].gridRevision,
    moves: [{ handleId: handle.handleId, outputSec: handle.outputSec }], dryRun: true });
  await call('content_shift_timing', { contentId, expectedGridRevision: grid.asOf.contents[0].gridRevision,
    range: { startContentSec: note.startContentSec, endContentSec: note.endContentSec }, deltaSec: 0, dryRun: true });
  const positioned = await call('project_get');
  const move = { expectedArrangementRevision: positioned.asOf.arrangementRevision,
    ops: [{ op: 'move', placementId, startTimelineSec: 0.123456 }] };
  await call('arrangement_edit', { ...move, dryRun: true });
  await call('arrangement_edit', move);
  assert.equal((await call('project_get')).placements[0].startTimelineSec, 0.123456);
  await call('track_set', { tracks: [{ trackId, volumeDb: -3, colour: '#112233' }] });
  await call('track_add', { count: 1 });
  await call('track_duplicate', { trackId: 1, mode: 'share' });
  const withTracks = await call('project_get');
  await call('arrangement_copy', { sources: [{ placementId }], mode: 'window', toTrackId: 1,
    startTimelineSec: project.placements[0].endTimelineSec, expectedArrangementRevision: withTracks.asOf.arrangementRevision });
  const withCopy = await call('project_get');
  await call('track_delete', { trackId: 1, expectedArrangementRevision: withCopy.asOf.arrangementRevision, confirm: true });
  await call('transport_control', { action: 'seek', positionTimelineSec: 0.123456 });
  await call('transport_set_tempo', { bpm: 123, timeSignature: { numerator: 7, denominator: 8 } });
  await call('transport_set_loop', { enabled: true });
  assert.equal((await call('transport_get')).transport.bpm, 123);
  await call('ui_set_selection', { placementId, openEditor: true });
  await call('ui_set_workspace', { workspace: 'pianoRoll' });
  assert.equal((await call('ui_get_state')).ui.workspace, 'pianoRoll');
  await call('prefs_get');
  await call('prefs_set', { tuningHz: 440 });
  const audio = await call('audio_get');
  assert.equal(audio.device.running, true);
  await call('audio_set', { bufferSizeFrame: audio.device.bufferSizeFrame });
  await call('app_get_log', { maxLines: 1 });
  const exportPath = path.join(directory, 'mix.wav');
  const exported = await finish(await call('export_start', { kind: 'master', path: exportPath, ifExists: 'error', allowDry: false }));
  assert.equal(exported.path, exportPath);
  assert.deepEqual(exported.dryContents, []);
  const wav = await readFile(exportPath);
  assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
  assert.equal(wav.toString('ascii', 8, 12), 'WAVE');
  assert.ok(wav.length > 1000);
  const projectPath = path.join(directory, 'saved.otproj');
  await finish(await call('project_save_as', { path: projectPath }));
  assert.equal((await call('project_get')).project.dirty, false);
  await finish(await call('project_save'));
  await finish(await call('project_save_copy', { path: path.join(directory, 'copy.otproj') }));
  assert.equal((await call('project_get')).project.path, projectPath);
  const savedNotes = await call('content_get_notes', { contentId, limit: 500 });
  await finish(await call('project_open', { path: projectPath }));
  const reopened = await call('project_get');
  assert.equal(reopened.project.dirty, false);
  assert.notEqual(reopened.placements[0].contentId, contentId);
  assert.equal(reopened.placements[0].startTimelineSec, 0.123456);
  // Export completion also waits for the reopened project's rendered pitch state.
  const reopenedPath = path.join(directory, 'reopened.wav');
  const reexported = await finish(await call('export_start', { kind: 'master', path: reopenedPath, ifExists: 'error', allowDry: false }));
  assert.deepEqual(reexported.dryContents, []);
  assert.equal((await readFile(reopenedPath)).equals(wav), true, 'saved/reopened project exports the same WAV bytes');
  assert.deepEqual((await call('content_get_notes', { contentId: reopened.placements[0].contentId, limit: 500 })).notes, savedNotes.notes);
  const stale = await client.callTool({ name: 'content_get_details', arguments: { instanceId, contentId } });
  assert.equal(stale.isError, true);
  await call('project_clear_recent');
  const cancellable = await call('import_start', { items: [{ path: audioPath }], layout: 'single', newTrack: true, generateNotes: true });
  const waiting = call('job_get', { jobId: cancellable.job.jobId, waitMs: 30000 });
  await call('project_get');
  const cancellation = await call('job_cancel', { jobId: cancellable.job.jobId });
  assert.equal(cancellation.cancelRequested, true);
  assert.equal((await waiting).job.state, 'cancelled');
  const peers = await Promise.all([1, 2, 3].map(() => mcp(t, path.join(directory, 'control', 'instances'))));
  for (const peer of peers)
    assert.equal((await peer.client.callTool({ name: 'session_info', arguments: { instanceId } })).isError, false);
  const fifth = await mcp(t, path.join(directory, 'control', 'instances'));
  const full = await fifth.client.callTool({ name: 'session_info', arguments: { instanceId } });
  assert.equal((full.structuredContent!.error as any).data.reason, 'session_limit');
  for (const peer of [...peers, fifth]) await peer.client.close();
  await call('app_quit', { discardUnsaved: true });
  await exited;
  const gone = await client.callTool({ name: 'instances_list', arguments: {} });
  assert.deepEqual(gone.structuredContent!.instances, []);
  assert.equal(seen.size, 45);
  t.diagnostic(`Real tools exercised: ${seen.size}; exported WAV: ${wav.length} bytes`);
});
