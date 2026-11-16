// SPDX-License-Identifier: AGPL-3.0-or-later
import { McpServer } from '@modelcontextprotocol/server';
import { z } from 'zod';
import { Discovery } from './discovery.js';
import { AdapterError, OtcpError } from './errors.js';
import { OtcpClient } from './otcp-client.js';
import { readTools } from './tools/read.js';
import { contentTools } from './tools/content.js';
import { arrangementTools } from './tools/arrangement.js';
import { applicationTools } from './tools/application.js';
import { jobTools } from './tools/jobs.js';

export const tools = [...readTools, ...contentTools, ...arrangementTools, ...applicationTools, ...jobTools];
const object = z.record(z.string(), z.unknown());
const errorSchema = z.union([
  z.object({ source: z.literal('otcp'), code: z.number(), message: z.string(), data: z.unknown().optional() }),
  z.object({ source: z.literal('adapter'), code: z.string(), message: z.string(), executionState: z.enum(['not_sent', 'unknown']) }),
]);
const productResult = z.looseObject({
  asOf: object.describe('Revisions observed by this read.').optional(),
  tokens: object.describe('Current opaque revision tokens after the operation.').optional(),
  revisionDiff: object.optional(),
  applied: z.boolean().optional(), noOp: z.boolean().optional(), dryRun: z.boolean().optional(),
  dirty: z.object({ before: z.boolean(), after: z.boolean() }).optional(),
  undo: object.nullable().describe('Owner-reported undoability, reason and revert recipe; null for preview or handed-off work.').optional(),
  warnings: z.array(object).optional(), sideEffects: object.optional(),
  job: z.looseObject({
    jobId: z.string(), state: z.enum(['queued', 'running', 'succeeded', 'failed', 'cancelled']),
  }).nullable().optional().describe('Task reference or snapshot. Inspect state and the terminal result/error.'),
});
export function toolResult(structuredContent: Record<string, unknown>, isError = false) {
  return { content: [{ type: 'text' as const, text: JSON.stringify(structuredContent) }], structuredContent, isError };
}
export function errorResult(error: unknown, context: Record<string, unknown> = {}) {
  if (error instanceof OtcpError) return toolResult({ ...context, error: { source: 'otcp', ...error.error } }, true);
  if (error instanceof AdapterError) return toolResult({ ...context, error: {
    source: 'adapter', code: error.code, message: error.message, executionState: error.executionState,
  } }, true);
  throw error;
}
export function createServer(discovery: Discovery, client = new OtcpClient(discovery)) {
  const server = new McpServer({ name: 'opentune-mcp', version: '0.1.0' }, { instructions:
    'Discover with instances_list and pass the selected instanceId on every call. Read session_info and project_get before editing. '
    + 'Preserve handles, cursors, revisions and absolute time values from the API. Read current revisions, preview where supported, then commit with the same revision. '
    + 'Results preserve OpenTune dirty, undo, warnings and sideEffects. Jobs are complete only when job.state is succeeded, failed or cancelled; '
    + 'job_get waits once for up to 30000 ms. Cancelling an MCP wait leaves the product task running; use job_cancel to request task cancellation. '
    + 'Read back state after executionState unknown before deciding the next operation. A new instance or project requires freshly read handles.',
  });
  server.server.onclose = () => client.close();
  server.registerTool('instances_list', {
    description: 'List local OpenTune discovery records and invalid-file statuses. Records mean discovered; business calls authenticate the selected instance. Credentials stay internal.',
    inputSchema: z.strictObject({}),
    outputSchema: z.object({ instances: z.array(z.object({ instanceId: z.string(), pid: z.number(), appVersion: z.string(),
      build: object, flavor: z.literal('standalone'), startedAtMs: z.number(), connectionState: z.literal('discovered'),
    })).optional(), issues: z.array(z.object({ file: z.string(), status: z.string() })).optional(), error: errorSchema.optional() }),
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: false },
  }, async () => {
    try { return toolResult(await discovery.list()); }
    catch (error) { return errorResult(error); }
  });
  for (const tool of tools) {
    server.registerTool(tool.name, { description: tool.description, inputSchema: tool.schema,
      outputSchema: z.object({ instanceId: z.string(), method: z.literal(tool.method),
        result: productResult.describe(`Original ${tool.method} result. Method-specific fields are preserved alongside the documented common fields.`).optional(),
        error: errorSchema.optional(),
      }).refine(v => (v.result !== undefined) !== (v.error !== undefined), 'Exactly one result or error is present.'), annotations: tool.annotations,
    }, async (args, ctx) => {
      const { instanceId, ...params } = args as { instanceId: string; [key: string]: unknown };
      const context = { instanceId, method: tool.method };
      try { return toolResult({ ...context, result: await client.call(instanceId, tool.method, tool.capability, params, ctx.mcpReq.signal) }); }
      catch (error) { return errorResult(error, context); }
    });
  }
  return server;
}
