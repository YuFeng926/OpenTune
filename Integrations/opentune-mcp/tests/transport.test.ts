// SPDX-License-Identifier: AGPL-3.0-or-later
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { chmod, symlink, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fixture, frame, type Request } from './helpers.ts';
import { discoveryDirectory } from '../dist/discovery.js';
import { OtcpError } from '../dist/errors.js';

test('private discovery metadata, corrupt files, symlinks, permissions and directory precedence', async t => {
  const f = await fixture(t);
  await writeFile(path.join(f.directory, '23456.json'), '{', { mode: 0o600 });
  await symlink(f.recordFile, path.join(f.directory, '34567.json'));
  const listed = await f.discovery.list();
  assert.equal(listed.instances.length, 1);
  assert.equal(listed.issues.length, 2);
  const json = JSON.stringify(listed);
  assert.ok(!json.includes(f.record.token) && !json.includes(f.record.serverProof));
  assert.equal(f.connections, 0);
  if (process.platform !== 'win32') {
    await chmod(f.recordFile, 0o644);
    assert.equal((await f.discovery.list()).instances.length, 0);
    await chmod(f.recordFile, 0o600);
    await chmod(f.directory, 0o777);
    await assert.rejects(f.discovery.list(), { code: 'DISCOVERY_UNAVAILABLE' });
    await chmod(f.directory, 0o700);
  }
  assert.equal(discoveryDirectory('/custom', { OPENTUNE_DATA_DIR: '/data' }, 'darwin'), '/custom');
  assert.equal(discoveryDirectory(undefined, { OPENTUNE_DATA_DIR: '/data' }, 'darwin'), '/data/control/instances');
  assert.equal(discoveryDirectory(undefined, { HOME: '/home/user' }, 'darwin'), '/home/user/Library/Application Support/OpenTune/control/instances');
  assert.equal(discoveryDirectory(undefined, { LOCALAPPDATA: 'C:\\Users\\test\\AppData\\Local' }, 'win32'), 'C:\\Users\\test\\AppData\\Local\\OpenTune\\control\\instances');
  assert.throws(() => discoveryDirectory('relative'), { code: 'DISCOVERY_UNAVAILABLE' });
});

test('concurrent first access authenticates once; split UTF-8, batched lines and reversed replies retain IDs', async t => {
  const batch: Request[] = [];
  const f = await fixture(t, { onRequest(r, s) {
    batch.push(r);
    if (batch.length === 3) {
      const bytes = Buffer.from(batch.toReversed().map(r => frame(r.id, { ...r.params, text: '音高' })).join(''));
      const cut = bytes.indexOf(Buffer.from('音')) + 1;
      s.write(bytes.subarray(0, cut));
      setImmediate(() => s.write(bytes.subarray(cut)));
    }
  } });
  const results = await Promise.all([1, 2, 3].map(n => f.call({ n })));
  assert.deepEqual(results, [1, 2, 3].map(n => ({ n, text: '音高' })));
  assert.equal(f.connections, 1);
});

test('identity mismatch and missing capabilities never send business calls', async t => {
  for (const [hello, code] of [
    [{ serverProof: 'cc'.repeat(32) }, 'AUTHENTICATION_FAILED'],
    [{ instanceId: 'changed' }, 'INSTANCE_CHANGED'],
    [{ capabilities: [] }, 'CAPABILITY_UNAVAILABLE'],
  ] as const) {
    const f = await fixture(t, { hello });
    await assert.rejects(f.call(), { code, executionState: 'not_sent' });
    assert.equal(f.requests.length, 0);
  }
});

test('upstream BUSY and executed/retryable errors retain their original envelope', async t => {
  const error = { code: -32000, message: 'BUSY', data: { kind: 'BUSY', reason: 'session_limit', executed: false, retryable: true } };
  const f = await fixture(t, { onHello(r, s) { s.end(JSON.stringify({ jsonrpc: '2.0', id: r.id, error }) + '\n'); } });
  await assert.rejects(f.call(), e => e instanceof OtcpError && JSON.stringify(e.error) === JSON.stringify(error));
});

test('send-side byte limits use UTF-8 bytes and do not dispatch oversize requests', async t => {
  const f = await fixture(t, { hello: { limits: { maxLineBytes: 150, maxInFlightPerSession: 16, jobWaitMaxMs: 30000 } } });
  await assert.rejects(f.call({ text: '音'.repeat(40) }), { code: 'MESSAGE_TOO_LARGE', executionState: 'not_sent' });
  assert.equal(f.requests.length, 0);
});

test('oversize, malformed UTF-8, unknown and duplicate response IDs close the connection', async t => {
  for (const kind of ['oversize', 'utf8', 'unknown', 'duplicate']) {
    const f = await fixture(t, {
      hello: { limits: { maxLineBytes: 128, maxInFlightPerSession: 16, jobWaitMaxMs: 30000 } },
      onRequest(r, s) {
        if (kind === 'oversize') s.write('x'.repeat(129));
        if (kind === 'utf8') s.write(Buffer.from([0xff, 10]));
        if (kind === 'unknown') s.write(frame('unknown', {}));
        if (kind === 'duplicate') s.write(frame(r.id, {}) + frame(r.id, {}));
      },
    });
    if (kind === 'duplicate') {
      await f.call();
      await f.call();
      assert.equal(f.connections, 2);
    } else {
      await assert.rejects(f.call(), { code: kind === 'oversize' ? 'MESSAGE_TOO_LARGE' : 'PROTOCOL_ERROR', executionState: 'unknown' });
    }
  }
});

test('disconnect after send reports unknown; later call reconnects without replaying writes', async t => {
  const f = await fixture(t, { onRequest(r, s) { if (r.params.n === 1) s.destroy(); else s.write(frame(r.id, r.params)); } });
  await assert.rejects(f.call({ n: 1 }), { code: 'CONNECTION_LOST', executionState: 'unknown' });
  assert.deepEqual(await f.call({ n: 2 }), { n: 2 });
  assert.equal(f.connections, 2);
  assert.equal(f.requests.length, 2);
  await assert.rejects(f.client.call('gone', 'project.get', 'read.v1', {}), { code: 'INSTANCE_NOT_FOUND', executionState: 'not_sent' });
});

test('cancelled waits retain in-flight capacity until reply and reserve a slot for job.cancel', async t => {
  const f = await fixture(t, {
    hello: { limits: { maxLineBytes: 4194304, maxInFlightPerSession: 3, jobWaitMaxMs: 30000 } },
    onRequest(r, s) { if (r.method === 'job.cancel') s.write(frame(r.id, { cancelRequested: true })); },
  });
  const abort = new AbortController();
  const seen = once(f.events, 'request');
  const waiting = f.call({ n: 1 }, abort.signal);
  const rejection = assert.rejects(waiting, { code: 'CANCELLED', executionState: 'unknown' });
  await seen;
  abort.abort();
  await rejection;
  const secondSeen = once(f.events, 'request');
  const second = f.call({ n: 2 });
  await secondSeen;
  await assert.rejects(f.call(), { code: 'CAPACITY_EXCEEDED', executionState: 'not_sent' });
  assert.deepEqual(await f.client.call(f.record.instanceId, 'job.cancel', 'jobs.v1', { jobId: 'j:01234567:1' }), { cancelRequested: true });
  const socket = [...f.sockets][0]!;
  socket.write(f.requests.slice(0, 2).map(r => frame(r.id, r.params)).join(''));
  assert.deepEqual(await second, { n: 2 });
  const cancelled = AbortSignal.abort();
  await assert.rejects(f.call({}, cancelled), { code: 'CANCELLED', executionState: 'not_sent' });
  assert.deepEqual(f.requests.map(r => r.method), ['project.get', 'project.get', 'job.cancel']);
});
