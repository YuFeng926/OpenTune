#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
import { parseArgs } from 'node:util';
import { serveStdio } from '@modelcontextprotocol/server/stdio';
import { Discovery, discoveryDirectory } from './discovery.js';
import { OtcpClient } from './otcp-client.js';
import { createServer } from './server.js';

try {
  const { values } = parseArgs({ options: { 'discovery-dir': { type: 'string' }, help: { type: 'boolean' } } });
  if (values.help) {
    process.stderr.write('Usage: opentune-mcp [--discovery-dir <absolute-directory>]\nNode 24+, MCP stdio, local OpenTune OTCP 1.0.\n');
  } else {
    const discovery = new Discovery(discoveryDirectory(values['discovery-dir']));
    const client = new OtcpClient(discovery);
    const handle = serveStdio(() => createServer(discovery, client), {
      onerror: () => process.stderr.write('opentune-mcp: MCP transport error\n'),
    });
    const close = () => { client.close(); void handle.close(); };
    process.stdin.once('end', close);
    process.once('SIGINT', close);
    process.once('SIGTERM', close);
  }
} catch {
  process.stderr.write('opentune-mcp: startup failed; use --help and an absolute discovery directory.\n');
  process.exitCode = 1;
}
