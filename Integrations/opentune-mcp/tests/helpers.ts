// SPDX-License-Identifier: AGPL-3.0-or-later
import { createServer, type Socket } from 'node:net';
import { once, EventEmitter } from 'node:events';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import type { TestContext } from 'node:test';
import { Client } from '@modelcontextprotocol/client';
import { StdioClientTransport } from '@modelcontextprotocol/client/stdio';
import { Discovery } from '../dist/discovery.js';
import { OtcpClient } from '../dist/otcp-client.js';
import { tools } from '../dist/server.js';

export const main = path.resolve('dist/main.js');
export type Request = { id: string; method: string; params: Record<string, unknown> };
export const frame = (id: string, result: unknown) => JSON.stringify({ jsonrpc: '2.0', id, result }) + '\n';
export async function fixture(t: TestContext, options: {
  hello?: Record<string, unknown>; onRequest?: (r: Request, s: Socket) => void;
  onHello?: (r: Request, s: Socket, result: Record<string, unknown>) => void;
} = {}) {
  const directory = await mkdtemp(path.join(tmpdir(), 'opentune-mcp-'));
  const sockets = new Set<Socket>();
  const events = new EventEmitter();
  const requests: Request[] = [];
  let connections = 0;
  const record = {
    protocolMajors: [1], instanceId: '0123456789abcdef', pid: 12345, processStartTimeMs: 1, port: 0,
    token: 'aa'.repeat(32), serverProof: 'bb'.repeat(32), launchNonce: null, appVersion: '1.8.2',
    build: { commit: 'c'.repeat(40), tag: null }, buildTag: null, sourceUrl: 'https://example.com/source', startedAtMs: 1, flavor: 'standalone',
  };
  const server = createServer(socket => {
    ++connections;
    sockets.add(socket);
    socket.on('error', () => {});
    socket.on('close', () => sockets.delete(socket));
    let buffer = '';
    socket.setEncoding('utf8');
    socket.on('data', data => {
      buffer += data;
      let end: number;
      while ((end = buffer.indexOf('\n')) >= 0) {
        const request: Request = JSON.parse(buffer.slice(0, end));
        buffer = buffer.slice(end + 1);
        if (request.method === 'session.hello') {
          const result = {
            instanceId: record.instanceId, pid: record.pid, protocol: '1.0', flavor: 'standalone', serverProof: record.serverProof,
            capabilities: [...new Set(tools.map(t => t.capability))],
            limits: { maxLineBytes: 4194304, maxInFlightPerSession: 16, jobWaitMaxMs: 30000 }, ...options.hello,
          };
          if (options.onHello) options.onHello(request, socket, result);
          else socket.write(frame(request.id, result));
        } else {
          requests.push(request);
          events.emit('request', request);
          if (options.onRequest) options.onRequest(request, socket);
          else socket.write(frame(request.id, request.params));
        }
      }
    });
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  record.port = (server.address() as { port: number }).port;
  const recordFile = path.join(directory, `${record.pid}.json`);
  await writeFile(recordFile, JSON.stringify(record), { mode: 0o600 });
  const discovery = new Discovery(directory);
  const client = new OtcpClient(discovery);
  t.after(async () => {
    client.close();
    for (const socket of sockets) socket.destroy();
    await new Promise<void>(resolve => server.close(() => resolve()));
    await rm(directory, { recursive: true });
  });
  return { directory, recordFile, record, discovery, client, requests, events, sockets,
    get connections() { return connections; },
    call: (params = {}, signal?: AbortSignal) => client.call(record.instanceId, 'project.get', 'read.v1', params, signal),
  };
}

export async function mcp(t: TestContext, directory: string, mode: 'legacy' | 'auto' = 'legacy') {
  const transport = new StdioClientTransport({ command: process.execPath, args: [main, '--discovery-dir', directory], stderr: 'pipe' });
  let stderr = '';
  transport.stderr?.on('data', data => { stderr += data; });
  const client = new Client({ name: 'opentune-mcp-test', version: '1.0.0' }, { capabilities: {}, mode });
  t.after(() => client.close());
  await client.connect(transport);
  return { client, transport, get stderr() { return stderr; } };
}
