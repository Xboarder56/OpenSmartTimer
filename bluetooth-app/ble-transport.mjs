// DOM-free transport rules shared by the browser app and behavioral tests.
export class GattQueue {
  constructor({ timeoutMs = 10000, onTimeout = () => {} } = {}) {
    this.tail = Promise.resolve(); this.epoch = 0;
    this.timeoutMs = timeoutMs; this.onTimeout = onTimeout;
  }
  reset() { this.epoch += 1; }
  run(work) {
    const epoch = this.epoch;
    const check = () => {
      if (epoch !== this.epoch) throw new Error('Bluetooth connection changed; queued operation cancelled');
    };
    const result = this.tail.then(async () => {
      check();
      try {
        const value = await withTimeout(Promise.resolve().then(work), this.timeoutMs);
        check();
        return value;
      } catch (error) {
        // A timed-out browser operation cannot be cancelled independently.
        // Invalidate the queue and close its connection before another runs.
        if (error.message === 'result_timeout' && epoch === this.epoch) {
          this.reset();
          this.onTimeout();
          throw new Error('Bluetooth operation timed out; reconnect before continuing');
        }
        throw error;
      }
    });
    this.tail = result.catch(() => {});
    return result;
  }
}

export class UnknownCommandOutcome extends Error {
  constructor(message, operation) {
    super(message); this.name = 'UnknownCommandOutcome'; this.outcome = 'unknown'; this.operation = operation;
  }
}

export function resultMatches(operation, result) {
  if (!result || result.request_id !== operation.requestId) return false;
  if (result.opcode !== operation.opcode) throw new Error('Result opcode does not match the command');
  if (result.boot_id !== operation.bootId && !['stale_boot', 'system_transition'].includes(result.result))
    throw new Error('Result boot ID does not match the command');
  if (result.result !== 'applied') return true;
  if (operation.opcode === 0x10 ? result.session_sequence === 0 : result.session_sequence !== operation.sessionSequence)
    throw new Error('Result session does not match the command');
  const states = { 0x10: 'running', 0x11: 'open', 0x12: 'paused', 0x13: 'running', 0x15: 'running', 0x16: 'open' };
  if (!['open', 'running', 'paused'].includes(result.state) ||
      (states[operation.opcode] && result.state !== states[operation.opcode]) ||
      !Number.isInteger(result.remaining_ms) || result.remaining_ms < 0 || result.remaining_ms > 86400000 ||
      (result.state === 'open' ? result.remaining_ms !== 0 : result.remaining_ms === 0))
    throw new Error('Resulting timer state is inconsistent with the command');
  return true;
}

export async function withTimeout(promise, milliseconds) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('result_timeout')), milliseconds);
    })]);
  } finally { clearTimeout(timer); }
}

export async function exchangeCommand({ operation, frame, write, wait, read, connected, timeoutMs = 3000 }) {
  let lastError;
  // Retry only on this same authenticated connection. Across reconnects the
  // app reads the cached result but does not blindly resend: an eight-entry
  // deduplication cache is bounded and may have been evicted by another app.
  for (let attempt = 0; attempt < 2 && connected(); attempt += 1) {
    try {
      await write(frame, attempt);
      const result = await wait(timeoutMs);
      if (resultMatches(operation, result)) return result;
    } catch (error) {
      lastError = error;
      // A result notification may precede a failing write acknowledgement.
      try { const received = await wait(0); if (resultMatches(operation, received)) return received; } catch {}
    }
    if (!connected()) break;
    try {
      const cached = await read();
      if (resultMatches(operation, cached)) return cached;
    } catch (error) { lastError = error; }
  }
  throw new UnknownCommandOutcome(
    `The timer may have applied this command. Reconnect to check its result; do not repeat a time adjustment. ${lastError?.message || 'No matching result was received.'}`,
    operation,
  );
}

export function snapshotFromResult(previous, operation, result) {
  // Cached duplicates describe the original operation, not current state.
  if (result.duplicate || result.result !== 'applied') return null;
  const open = result.state === 'open';
  return {
    ...previous,
    state: result.state,
    remaining_ms: result.remaining_ms,
    session_sequence: result.session_sequence,
    session_duration_ms: open ? 0 : operation.opcode === 0x10 ? result.remaining_ms :
      Math.max(previous.session_duration_ms, result.remaining_ms),
    controls_locked: !open && (operation.opcode === 0x15 || previous.controls_locked),
    lock_output_energized: !open,
    last_release_reason: result.release_reason,
    event_revision: (previous.event_revision + (result.changed ? 1 : 0)) >>> 0,
  };
}

export function validPendingOperation(value, allowPredecessor = true) {
  return value && value.schema === 1 && typeof value.deviceId === 'string' &&
    typeof value.op === 'string' &&
    [0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16].includes(value.opcode) &&
    Number.isInteger(value.requestId) && value.requestId > 0 && value.requestId <= 65535 &&
    ['bootId', 'sessionSequence'].every(key => Number.isInteger(value[key]) && value[key] >= 0 && value[key] <= 0xffffffff) &&
    ['valueA', 'valueB'].every(key => Number.isInteger(value[key]) && Math.abs(value[key]) <= 86400000) &&
    (!value.predecessor || (allowPredecessor && value.opcode === 0x11 &&
      validPendingOperation(value.predecessor, false) && value.predecessor.deviceId === value.deviceId));
}
