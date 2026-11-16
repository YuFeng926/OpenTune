// SPDX-License-Identifier: AGPL-3.0-or-later
import { timingSafeEqual } from 'node:crypto';
import { createConnection, type Socket } from 'node:net';
import { z } from 'zod';
import { Discovery, type DiscoveryRecord } from './discovery.js';
import { AdapterError, OtcpError } from './errors.js';

const response = z.object({
  jsonrpc: z.literal('2.0'), id: z.string(), result: z.unknown().optional(),
  error: z.object({ code: z.number().int(), message: z.string(), data: z.unknown().optional() }).optional(),
}).refine(v => Object.hasOwn(v, 'result') !== Object.hasOwn(v, 'error'));
const hello = z.object({
  instanceId: z.string(), pid: z.number().int(), protocol: z.string(), flavor: z.string(),
  serverProof: z.string(), capabilities: z.array(z.string()),
  limits: z.object({
    maxLineBytes: z.number().int().positive(), maxInFlightPerSession: z.number().int().min(2),
    jobWaitMaxMs: z.number().int().nonnegative(),
  }),
});
type Pending = { resolve: (value: unknown) => void; reject: (error: Error) => void; cleanup: () => void };

class Connection {
  private socket: Socket;
  private pending = new Map<string, Pending>();
  private sequence = 0n;
  private buffer: Buffer = Buffer.alloc(0);
  private maxLineBytes = 4194304;
  private maxInFlight = 16;
  private jobWaitMaxMs = 30000;
  private capabilities = new Set<string>();
  closed = false;
  readonly ready: Promise<void>;

  constructor(record: DiscoveryRecord) {
    this.socket = createConnection({ host: '127.0.0.1', port: record.port });
    this.socket.setNoDelay(true);
    this.ready = new Promise<void>((resolve, reject) => {
      const onError = () => reject(new AdapterError('CONNECTION_LOST', 'OpenTune connection failed before authentication.'));
      this.socket.once('close', onError);
      this.socket.once('connect', () => {
        this.send('session.hello', {
          token: record.token, protocol: '1.0', client: { name: 'opentune-mcp', version: '0.1.0' },
        }).then(result => {
          const parsed = hello.safeParse(result);
          if (!parsed.success) throw new AdapterError('AUTHENTICATION_FAILED', 'OpenTune returned an invalid handshake.');
          const h = parsed.data;
          const expected = Buffer.from(record.serverProof);
          const proof = Buffer.from(h.serverProof);
          if (proof.length !== expected.length || !timingSafeEqual(proof, expected))
            throw new AdapterError('AUTHENTICATION_FAILED', 'OpenTune server identity verification failed.');
          if (h.instanceId !== record.instanceId || h.pid !== record.pid || h.protocol !== '1.0' || h.flavor !== 'standalone')
            throw new AdapterError('INSTANCE_CHANGED', 'OpenTune handshake does not match the selected instance.');
          this.capabilities = new Set(h.capabilities);
          this.maxLineBytes = h.limits.maxLineBytes;
          this.maxInFlight = h.limits.maxInFlightPerSession;
          this.jobWaitMaxMs = h.limits.jobWaitMaxMs;
          resolve();
        }).catch(error => {
          reject(error instanceof OtcpError && error.error.code === -32001
            ? new AdapterError('AUTHENTICATION_FAILED', 'OpenTune rejected authentication.') : error);
          this.close();
        }).finally(() => this.socket.removeListener('close', onError));
      });
    });
    this.socket.on('data', data => this.receive(data));
    this.socket.on('error', () => this.close());
    this.socket.on('close', () => this.close());
  }

  call(method: string, capability: string, params: Record<string, unknown>, signal?: AbortSignal) {
    if (!this.capabilities.has(capability))
      return Promise.reject(new AdapterError('CAPABILITY_UNAVAILABLE', `OpenTune does not advertise ${capability}.`));
    if (method === 'job.get' && typeof params.waitMs === 'number' && params.waitMs > this.jobWaitMaxMs)
      return Promise.reject(new AdapterError('LIMIT_EXCEEDED', 'waitMs exceeds the connected instance limit.'));
    if (this.pending.size >= this.maxInFlight - (method === 'job.cancel' ? 0 : 1))
      return Promise.reject(new AdapterError('CAPACITY_EXCEEDED', 'OpenTune connection has reached its in-flight request capacity.'));
    return this.send(method, params, signal);
  }

  private send(method: string, params: Record<string, unknown>, signal?: AbortSignal): Promise<unknown> {
    if (signal?.aborted) return Promise.reject(new AdapterError('CANCELLED', 'MCP call cancelled before sending.'));
    if (this.closed) return Promise.reject(new AdapterError('CONNECTION_LOST', 'OpenTune connection is closed.'));
    const id = `otcp-${++this.sequence}`;
    const line = Buffer.from(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    if (line.length - 1 > (method === 'session.hello' ? 8192 : this.maxLineBytes))
      return Promise.reject(new AdapterError('MESSAGE_TOO_LARGE', 'Request exceeds the OpenTune line byte limit.'));
    return new Promise((resolve, reject) => {
      const abort = () => reject(new AdapterError('CANCELLED', 'MCP waiting ended; the sent OpenTune request continues.', 'unknown'));
      const cleanup = () => signal?.removeEventListener('abort', abort);
      // Cancellation keeps the ID and capacity until the actual response arrives.
      this.pending.set(id, { resolve, reject, cleanup });
      signal?.addEventListener('abort', abort, { once: true });
      this.socket.write(line);
    });
  }

  private receive(data: Buffer) {
    this.buffer = Buffer.concat([this.buffer, data]);
    while (!this.closed) {
      const end = this.buffer.indexOf(10);
      if (end < 0) {
        if (this.buffer.length > this.maxLineBytes) this.close('MESSAGE_TOO_LARGE');
        return;
      }
      if (end > this.maxLineBytes) { this.close('MESSAGE_TOO_LARGE'); return; }
      let value: z.infer<typeof response>;
      try {
        value = response.parse(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(this.buffer.subarray(0, end))));
      } catch { this.close('PROTOCOL_ERROR'); return; }
      this.buffer = this.buffer.subarray(end + 1);
      const pending = this.pending.get(value.id);
      if (!pending) { this.close('PROTOCOL_ERROR'); return; }
      this.pending.delete(value.id);
      pending.cleanup();
      if (value.error) pending.reject(new OtcpError(value.error));
      else pending.resolve(value.result);
    }
  }

  close(code = 'CONNECTION_LOST') {
    if (this.closed) return;
    this.closed = true;
    this.socket.destroy();
    this.buffer = Buffer.alloc(0);
    for (const pending of this.pending.values()) {
      pending.cleanup();
      pending.reject(new AdapterError(code, 'OpenTune connection ended before a response was received.', 'unknown'));
    }
    this.pending.clear();
  }
}

function waitForConnection<T>(promise: Promise<T>, signal?: AbortSignal): Promise<T> {
  if (!signal) return promise;
  if (signal.aborted) return Promise.reject(new AdapterError('CANCELLED', 'MCP call cancelled before sending.'));
  return new Promise((resolve, reject) => {
    const abort = () => reject(new AdapterError('CANCELLED', 'MCP call cancelled before sending.'));
    signal.addEventListener('abort', abort, { once: true });
    promise.then(resolve, reject).finally(() => signal.removeEventListener('abort', abort));
  });
}

export class OtcpClient {
  private connections = new Map<string, Connection>();
  private connecting = new Map<string, Promise<Connection>>();
  private closed = false;
  constructor(readonly discovery: Discovery) {}

  async call(instanceId: string, method: string, capability: string, params: Record<string, unknown>, signal?: AbortSignal) {
    if (this.closed) throw new AdapterError('CONNECTION_LOST', 'MCP server is closing.');
    if (signal?.aborted) throw new AdapterError('CANCELLED', 'MCP call cancelled before sending.');
    let connection = this.connections.get(instanceId);
    let connecting = this.connecting.get(instanceId);
    if (connecting || !connection || connection.closed) {
      if (!connecting) {
        connecting = this.connect(instanceId);
        this.connecting.set(instanceId, connecting);
        connecting.then(() => this.connecting.delete(instanceId), () => this.connecting.delete(instanceId));
      }
      connection = await waitForConnection(connecting, signal);
    }
    return connection.call(method, capability, params, signal);
  }

  private async connect(instanceId: string) {
    const record = await this.discovery.find(instanceId);
    if (this.closed) throw new AdapterError('CONNECTION_LOST', 'MCP server is closing.');
    const connection = new Connection(record);
    this.connections.set(instanceId, connection);
    try { await connection.ready; }
    catch (error) {
      if (error instanceof AdapterError)
        throw new AdapterError(error.code, error.message, 'not_sent');
      throw error;
    }
    return connection;
  }

  close() {
    this.closed = true;
    for (const connection of this.connections.values()) connection.close();
    this.connections.clear();
  }
}
