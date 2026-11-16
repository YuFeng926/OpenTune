// SPDX-License-Identifier: AGPL-3.0-or-later
export type ExecutionState = 'not_sent' | 'unknown';
export class AdapterError extends Error {
  constructor(readonly code: string, message: string, readonly executionState: ExecutionState = 'not_sent') {
    super(message);
  }
}
export interface RpcError { code: number; message: string; data?: unknown }
export class OtcpError extends Error {
  constructor(readonly error: RpcError) { super(error.message); }
}
