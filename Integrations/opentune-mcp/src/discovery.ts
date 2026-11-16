// SPDX-License-Identifier: AGPL-3.0-or-later
import { constants } from 'node:fs';
import { lstat, open, readdir, realpath } from 'node:fs/promises';
import { homedir } from 'node:os';
import path from 'node:path';
import { z } from 'zod';
import { AdapterError } from './errors.js';

const secret = z.string().regex(/^(?:[0-9a-f]{2}){16,64}$/);
const recordSchema = z.object({
  protocolMajors: z.array(z.number().int()).refine(v => v.includes(1)),
  instanceId: z.string().min(1).max(128), pid: z.number().int().positive(),
  processStartTimeMs: z.number().int().nonnegative(),
  port: z.number().int().min(1).max(65535), token: secret, serverProof: secret,
  launchNonce: z.string().nullable(), appVersion: z.string().min(1).max(64),
  build: z.object({ commit: z.string().regex(/^[0-9a-f]{40}$/), tag: z.string().nullable() }),
  buildTag: z.string().nullable(), sourceUrl: z.string(),
  startedAtMs: z.number().int().nonnegative(), flavor: z.literal('standalone'),
});
export type DiscoveryRecord = z.infer<typeof recordSchema>;

export function discoveryDirectory(override?: string, env = process.env, platform = process.platform): string {
  const paths = platform === 'win32' ? path.win32 : path.posix;
  let directory = override;
  if (directory === undefined && env.OPENTUNE_DATA_DIR)
    directory = paths.join(env.OPENTUNE_DATA_DIR, 'control', 'instances');
  if (directory === undefined) {
    if (platform === 'win32') {
      if (!env.LOCALAPPDATA) throw new AdapterError('DISCOVERY_UNAVAILABLE', 'LOCALAPPDATA must be an absolute directory.');
      directory = paths.join(env.LOCALAPPDATA, 'OpenTune', 'control', 'instances');
    } else if (platform === 'darwin') {
      directory = paths.join(env.HOME ?? homedir(), 'Library/Application Support/OpenTune/control/instances');
    } else {
      directory = paths.join(env.XDG_STATE_HOME ?? paths.join(env.HOME ?? homedir(), '.local/state'), 'OpenTune/control/instances');
    }
  }
  if (!paths.isAbsolute(directory)) throw new AdapterError('DISCOVERY_UNAVAILABLE', 'Discovery directory must be absolute.');
  return directory;
}

function owned(info: { uid: number; mode: number }, mask: number): boolean {
  return process.platform === 'win32' || (info.uid === process.geteuid!() && (info.mode & mask) === 0);
}

export class Discovery {
  constructor(readonly directory: string) {}
  private async scan() {
    const records: DiscoveryRecord[] = [];
    const issues: { file: string; status: string }[] = [];
    let directory: string;
    let names: string[];
    try {
      const info = await lstat(this.directory);
      if (!info.isDirectory() || !owned(info, 0o022))
        throw new AdapterError('DISCOVERY_UNAVAILABLE', 'Discovery directory must be owned by this user and protected from other writers.');
      directory = await realpath(this.directory);
      names = await readdir(directory);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === 'ENOENT') return { records, issues };
      if (error instanceof AdapterError) throw error;
      throw new AdapterError('DISCOVERY_UNAVAILABLE', 'Discovery directory could not be read.');
    }
    for (const name of names.filter(n => n.endsWith('.json')).sort()) {
      try {
        if (!/^[1-9][0-9]*[.]json$/.test(name)) throw new Error();
        const file = path.join(directory, name);
        const info = await lstat(file);
        if (!info.isFile() || !owned(info, 0o077) || info.size > 8192 || path.dirname(await realpath(file)) !== directory)
          throw new Error();
        const handle = await open(file, constants.O_RDONLY | (process.platform === 'win32' ? 0 : constants.O_NOFOLLOW));
        let text: string;
        try {
          const opened = await handle.stat();
          if (!opened.isFile() || opened.dev !== info.dev || opened.ino !== info.ino || !owned(opened, 0o077)) throw new Error();
          const buffer = Buffer.alloc(8193);
          const { bytesRead } = await handle.read(buffer, 0, buffer.length, 0);
          if (bytesRead > 8192) throw new Error();
          text = new TextDecoder('utf-8', { fatal: true }).decode(buffer.subarray(0, bytesRead));
        } finally { await handle.close(); }
        const record = recordSchema.parse(JSON.parse(text));
        if (name !== `${record.pid}.json`) throw new Error();
        records.push(record);
      } catch { issues.push({ file: name, status: 'invalid_or_unreadable' }); }
    }
    return { records, issues };
  }
  async list() {
    const { records, issues } = await this.scan();
    return {
      instances: records.map(({ instanceId, pid, appVersion, build, flavor, startedAtMs }) =>
        ({ instanceId, pid, appVersion, build, flavor, startedAtMs, connectionState: 'discovered' as const })),
      issues,
    };
  }
  async find(instanceId: string): Promise<DiscoveryRecord> {
    const { records } = await this.scan();
    const matches = records.filter(r => r.instanceId === instanceId);
    if (matches.length === 0) throw new AdapterError('INSTANCE_NOT_FOUND', 'Discover running OpenTune instances and select an instanceId.');
    if (matches.length !== 1) throw new AdapterError('INSTANCE_CHANGED', 'Multiple discovery records claim this instance.');
    return matches[0]!;
  }
}
