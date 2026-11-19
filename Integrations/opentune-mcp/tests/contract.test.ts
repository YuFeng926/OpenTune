// SPDX-License-Identifier: AGPL-3.0-or-later
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFile } from 'node:fs/promises';
import { z } from 'zod';
import { tools } from '../dist/server.js';
import { fixture, mcp, main, frame } from './helpers.ts';

const contentId = 'c:01234567:1:1';
const placementId = 'p:01234567:1:1';
const revision = 'r0123456789abcdef0';
const range = { startContentSec: 1.234567890123, endContentSec: 2.345678901234 };
const samples: Record<string, Record<string, unknown>> = {
  'session.info': {}, 'project.get': {}, 'transport.get': {}, 'prefs.get': {}, 'ui.getState': {}, 'edit.getHistory': {}, 'audio.get': {},
  'content.getNotes': { contentId, range, limit: 500 }, 'content.getPitchCurve': { contentId, range, limit: 10000, stepFrame: 1000 },
  'content.segmentNotes': { contentId, range, noteSplitCents: 200 }, 'content.getDetails': { placementId }, 'app.getLog': { maxLines: 1000 },
  'content.setKey': { contentId, key: { root: 11, scale: 'harmonicMinor' }, expectedKeyRevision: revision },
  'content.autoTune': { contentId, mode: 'regenerate', expectedRevision: revision, key: 'clipKey', expectedKeyRevision: revision, ranges: [range], dryRun: true },
  'content.editNotes': { contentId, expectedRevision: revision, ops: [{ op: 'setTargetPitch', select: { indices: [0, 2] }, targetMidi: 60 }], dryRun: true },
  'content.setPitchShift': { contentId, pitchShift: { semitones: -24, cents: 99 } },
  'content.shiftTiming': { contentId, expectedGridRevision: revision, range, deltaSec: -10, dryRun: true },
  'content.editTimeGrid': { contentId, op: 'moveHandles', expectedGridRevision: revision, moves: [{ handleId: 'h:1', outputSec: 1.234567890123 }], dryRun: true },
  'edit.undo': { expectedUndoId: 'u:01234567:1' }, 'edit.redo': { force: true },
  'transport.control': { action: 'seek', positionTimelineSec: 1.234567890123 },
  'transport.setTempo': { bpm: 120.123456789, timeSignature: { numerator: 7, denominator: 8 } }, 'transport.setLoop': { enabled: true },
  'arrangement.edit': { expectedArrangementRevision: revision, ops: [{ op: 'move', placementId, startTimelineSec: 5.123456789012 }], dryRun: true },
  'arrangement.copy': { sources: [{ placementId }], mode: 'window', toTrackId: 2, startTimelineSec: 5.123456789012, expectedArrangementRevision: revision },
  'track.set': { tracks: [{ trackId: 0, muted: false, volumeDb: -3.25, colour: '#ab12Cd' }] },
  'track.add': { count: 64 }, 'track.duplicate': { trackId: 0, mode: 'share' },
  'track.delete': { trackId: 0, expectedArrangementRevision: revision, confirm: true },
  'ui.dismiss': { target: 'dialogs', confirm: true }, 'ui.setSelection': { placementId, openEditor: true, dryRun: true },
  'ui.setWorkspace': { workspace: 'pianoRoll' }, 'project.save': {}, 'project.saveAs': { path: '/tmp/example.otproj', confirm: true },
  'project.open': { path: '/tmp/example.otproj', discardUnsaved: true }, 'project.clearRecent': {},
  'project.saveCopy': { path: '/tmp/copy.otproj' }, 'prefs.set': { tuningHz: 440.123, editingScheme: 'openTune', renderingPriority: 'cpuFirst' },
  'audio.set': { sampleRateHz: 44100, bufferSizeFrame: 512 }, 'app.quit': { discardUnsaved: false },
  'job.get': { jobId: 'j:01234567:1', waitMs: 30000 }, 'job.cancel': { jobId: 'j:01234567:1' },
  'import.start': { items: [{ path: '/tmp/voice.wav' }], layout: 'single', newTrack: true, startTimelineSec: 0, generateNotes: true, idempotencyKey: 'import-1' },
  'export.start': { kind: 'master', path: '/tmp/mix.wav', ifExists: 'error', allowDry: false },
};

test('44 explicit OTCP mappings match the actual method/capability registry', async () => {
  const source = await readFile('../../Source/Control/ControlProtocol.cpp', 'utf8');
  const actual = [...source.matchAll(/[{]"([a-z]+[.][a-zA-Z]+)", "([a-z.0-9]+)"[}]/g)]
    .filter(m => m[1] !== 'session.hello').map(m => [m[1], m[2]]).sort();
  assert.deepEqual(tools.map(t => [t.method, t.capability]).sort(), actual);
  assert.equal(tools.length, 44);
  assert.equal(new Set(tools.map(t => t.name)).size, 44);
  for (const tool of tools) {
    assert.ok(samples[tool.method], tool.method);
    const input = { ...samples[tool.method], instanceId: '0123456789abcdef' };
    assert.deepEqual(tool.schema.parse(input), input, tool.method);
    assert.equal(tool.schema.safeParse({ ...input, unexpected: true }).success, false, tool.method);
    assert.equal(tool.schema.safeParse(samples[tool.method]).success, false, tool.method);
    const schema = z.toJSONSchema(tool.schema);
    assert.equal(schema.type, 'object');
    assert.equal(schema.additionalProperties, false);
    assert.ok(schema.required?.includes('instanceId'));
    assert.equal(Object.keys(tool.annotations).length, 4);
  }
});

test('mutual exclusions, required revisions and protocol boundaries reject invalid input', () => {
  const invalid: [string, Record<string, unknown>][] = [
    ['content.getNotes', { contentId, placementId }],
    ['content.getNotes', { contentId, range: { startTimelineSec: 0, endTimelineSec: 1 } }],
    ['content.getNotes', { contentId, limit: 501 }],
    ['content.getPitchCurve', { contentId, stepFrame: 1001 }],
    ['content.autoTune', { contentId, mode: 'notesOnly', expectedRevision: revision, key: 'clipKey' }],
    ['content.autoTune', { contentId, mode: 'snapNotes', expectedRevision: revision, key: { root: 0, scale: 'major' }, params: {} }],
    ['content.editNotes', { contentId, expectedRevision: revision, ops: [{ op: 'shiftCents', select: { indices: [0, 0] }, deltaCents: 1 }] }],
    ['content.editNotes', { contentId, expectedRevision: revision, ops: [{ op: 'snapToScale', select: { all: true }, key: 'clipKey' }] }],
    ['content.setPitchShift', { contentId, pitchShift: { semitones: 25, cents: 0 } }],
    ['content.editTimeGrid', { contentId, op: 'seed', dryRun: true }],
    ['content.editTimeGrid', { contentId, op: 'moveHandles', moves: [] }],
    ['edit.undo', {}], ['transport.control', { action: 'play', positionTimelineSec: 1 }],
    ['transport.setTempo', { timeSignature: { numerator: 4, denominator: 3 } }],
    ['arrangement.edit', { expectedArrangementRevision: revision, ops: [{ op: 'move', placementId }], confirm: true }],
    ['track.set', { tracks: [{ trackId: 1, volumeDb: 0, volumeLinear: 1 }] }],
    ['track.set', { tracks: [{ trackId: 1 }] }], ['track.add', { count: 65 }], ['track.duplicate', { trackId: 2147483648 }],
    ['ui.setSelection', { trackId: 1, openEditor: false }], ['ui.dismiss', { target: 'onboarding', confirm: false }],
    ['prefs.set', {}], ['prefs.set', { vocoderWeight: 'hifigan.onnx' }], ['audio.set', {}], ['job.get', { jobId: 'j:01234567:1', waitMs: 30001 }],
    ['import.start', { items: [{ path: '/tmp/voice.wav' }], layout: 'single', trackId: 0, newTrack: false }],
    ['export.start', { kind: 'master', path: '/tmp/voice.wav', target: { trackId: 0 } }],
  ];
  for (const [method, params] of invalid)
    assert.equal(tools.find(t => t.method === method)!.schema.safeParse({ ...params, instanceId: 'i' }).success, false, method);
});

for (const mode of ['legacy', 'auto'] as const) {
  test(`${mode} MCP stdio lists 45 tools and round-trips every mapping without changing values`, async t => {
    const f = await fixture(t);
    const peer = await mcp(t, f.directory, mode);
    const list = await peer.client.listTools();
    assert.equal(list.tools.length, 45);
    assert.equal(f.connections, 0);
    const discovered = await peer.client.callTool({ name: 'instances_list', arguments: {} });
    assert.equal((discovered.structuredContent!.instances as unknown[]).length, 1);
    for (const tool of tools) {
      const result = await peer.client.callTool({ name: tool.name, arguments: { instanceId: f.record.instanceId, ...samples[tool.method] } });
      assert.equal(result.isError, false, tool.name);
      assert.deepEqual(result.structuredContent, { instanceId: f.record.instanceId, method: tool.method, result: samples[tool.method] });
      assert.deepEqual(JSON.parse((result.content[0] as { text: string }).text), result.structuredContent);
    }
    assert.equal(f.requests.length, 44);
    assert.equal(f.connections, 1);
    assert.equal(peer.stderr, '');
    const wire = JSON.stringify(discovered) + JSON.stringify(list);
    assert.ok(!wire.includes(f.record.token) && !wire.includes(f.record.serverProof));
  });
}

test('MCP keeps business errors, failed jobs and unknown execution state distinct', async t => {
  const upstream = { code: -32000, message: 'REVISION_CONFLICT', data: { kind: 'REVISION_CONFLICT', reason: 'content_changed', executed: false, retryable: false } };
  const f = await fixture(t, { onRequest(r, s) {
    if (r.method === 'project.get') s.write(JSON.stringify({ jsonrpc: '2.0', id: r.id, error: upstream }) + '\n');
    else if (r.method === 'job.get') s.write(frame(r.id, { job: { jobId: r.params.jobId, state: 'failed', error: upstream } }));
    else s.destroy();
  } });
  const { client } = await mcp(t, f.directory);
  const call = (name: string, params = {}) => client.callTool({ name, arguments: { instanceId: f.record.instanceId, ...params } });
  const error = await call('project_get');
  assert.equal(error.isError, true);
  assert.deepEqual(error.structuredContent!.error, { source: 'otcp', ...upstream });
  const job = await call('job_get', { jobId: 'j:01234567:1' });
  assert.equal(job.isError, false);
  assert.equal((job.structuredContent!.result as { job: { state: string } }).job.state, 'failed');
  const unknown = await call('track_add');
  assert.equal((unknown.structuredContent!.error as { executionState: string }).executionState, 'unknown');
});

test('MCP initializes and lists tools with no running OpenTune', async t => {
  const f = await fixture(t);
  const { client } = await mcp(t, `${f.directory}/missing`);
  assert.equal((await client.listTools()).tools.length, 45);
  const listed = await client.callTool({ name: 'instances_list', arguments: {} });
  assert.deepEqual(listed.structuredContent, { instances: [], issues: [] });
  const missing = await client.callTool({ name: 'project_get', arguments: { instanceId: 'absent' } });
  assert.equal((missing.structuredContent!.error as { code: string }).code, 'INSTANCE_NOT_FOUND');
});

test('MCP cancellation leaves the sent OTCP call running and the connection usable', async t => {
  let pendingId = '';
  const f = await fixture(t, { onRequest(r, s) {
    if (!pendingId) pendingId = r.id;
    else s.write(frame(pendingId, {}) + frame(r.id, { completed: true }));
  } });
  const { client } = await mcp(t, f.directory);
  const abort = new AbortController();
  const sent = once(f.events, 'request');
  const waiting = client.callTool({ name: 'project_get', arguments: { instanceId: f.record.instanceId } }, { signal: abort.signal });
  const cancelled = assert.rejects(waiting);
  await sent;
  abort.abort();
  await cancelled;
  const next = await client.callTool({ name: 'project_get', arguments: { instanceId: f.record.instanceId } });
  assert.deepEqual(next.structuredContent!.result, { completed: true });
  assert.deepEqual(f.requests.map(r => r.method), ['project.get', 'project.get']);
  assert.equal(f.connections, 1);
});

test('stdin EOF exits and closes authenticated sockets', async t => {
  const f = await fixture(t);
  const child = spawn(process.execPath, [main, '--discovery-dir', f.directory], { stdio: 'pipe' });
  t.after(() => child.kill());
  const exited = once(child, 'exit');
  let stdout = '';
  let resolveCall: () => void;
  const called = new Promise<void>(resolve => { resolveCall = resolve; });
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', chunk => {
    stdout += chunk;
    if (stdout.includes('"id":2')) resolveCall();
  });
  child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize', params: { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'eof-test', version: '1' } } }) + '\n');
  child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
  child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'tools/call', params: { name: 'project_get', arguments: { instanceId: f.record.instanceId } } }) + '\n');
  await called;
  const socketClosed = once([...f.sockets][0]!, 'close');
  child.stdin.end();
  assert.equal((await exited)[0], 0);
  await socketClosed;
  for (const line of stdout.trim().split('\n')) assert.equal(JSON.parse(line).jsonrpc, '2.0');
});
