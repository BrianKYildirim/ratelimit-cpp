// The Node leg of the parity test (tests/parity/README.md): replays a canonical trace through the WebAssembly build
// of the C API on one thread and compares its outcome lines with the golden file byte for byte. Compare mode only,
// by design: golden files come from rl_parity_native --write, never from the WebAssembly build itself.
// Usage: node run_wasm.mjs <ratelimit.mjs> <trace.jsonl> <golden.out>
// Exit: 0 equal, 1 golden mismatch, 2 usage or trace error.
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const MAX_KEY_BYTES = 256, U64_MAX = 2n ** 64n - 1n, U32_MAX = 2n ** 32n - 1n;
// Struct layouts on wasm32 (include/ratelimit/ratelimit_c.h): rl_limit_t 16 bytes, rl_decision_t 24
// (allowed u8 @0, remaining @8, reset_ms @16), rl_bucket_state_t 56 (tracked @0, would_allow @1, six u64 from @8).
const LIMIT_BYTES = 16, STATE_BYTES = 56;

class TraceError extends Error {}
const fail = (n, reason) => { throw new TraceError(`trace line ${n}: ${reason}`); };

// ---------- Trace parsing (canonical lines only, the same rules as rl_parity_native.c) ----------

const DEC = '(0|[1-9][0-9]{0,19})';
const NEW_LINE = new RegExp(`^\\{"op":"new","shards":${DEC},"hint":${DEC}\\}$`);
const OP_LINE = new RegExp(`^\\{"op":"([a-z]+)","key":"([0-9a-f]*)","cap":${DEC},"rate":${DEC},"t":${DEC}\\}$`);

function decimal(s, max, n, field) {
  const v = BigInt(s);
  if (v > max) fail(n, `${field} ${s} is out of range`);
  return v;
}

function parseTrace(text) {
  const cr = text.indexOf('\r');
  if (cr !== -1) fail(text.slice(0, cr).split('\n').length, 'carriage return (CR) is not allowed');
  if (text === '') fail(1, 'the trace is empty; the first line must be the "new" op');
  const lines = text.split('\n');
  if (lines.at(-1) !== '') fail(lines.length, 'missing final newline');
  lines.pop();
  const ops = [];
  let shards = 0n, hint = 0n;
  lines.forEach((line, i) => {
    const n = i + 1;
    let again;
    if (i === 0) {
      const m = NEW_LINE.exec(line);
      if (!m) fail(n, 'the first line must be the canonical "new" op');
      shards = decimal(m[1], U32_MAX, n, 'shards');
      hint = decimal(m[2], U32_MAX, n, 'hint');
      again = `{"op":"new","shards":${shards},"hint":${hint}}`;
    } else {
      const m = OP_LINE.exec(line);
      if (!m) {
        if (line.startsWith('{"op":"new"')) fail(n, '"new" may appear only once, on the first line');
        fail(n, 'not a canonical allow or inspect op');
      }
      if (m[1] === 'new') fail(n, '"new" may appear only once, on the first line');
      if (m[1] !== 'allow' && m[1] !== 'inspect') fail(n, `unknown op "${m[1]}"`);
      if (m[2].length % 2 !== 0) fail(n, 'the key must be an even number of hex digits');
      if (m[2].length > 2 * MAX_KEY_BYTES) fail(n, `the key is longer than ${MAX_KEY_BYTES} bytes`);
      const op = { op: m[1], hex: m[2], cap: decimal(m[3], U64_MAX, n, 'cap'), rate: decimal(m[4], U64_MAX, n, 'rate'),
        t: decimal(m[5], U64_MAX, n, 't') };
      again = `{"op":"${op.op}","key":"${op.hex}","cap":${op.cap},"rate":${op.rate},"t":${op.t}}`;
      ops.push(op);
    }
    if (again !== line) fail(n, 'does not re-serialize to itself');
  });
  return { shards: Number(shards), hint: Number(hint), ops };
}

// ---------- Replay (own minimal marshaller over the module's exports) ----------

function run(module, { shards, hint, ops }) {
  const h = module._rl_new_inproc(shards, hint);
  if (!h) throw new Error('rl_new_inproc failed');
  const scratch = module._malloc(LIMIT_BYTES + STATE_BYTES); // limit @0, result @LIMIT_BYTES
  if (!scratch) throw new Error('malloc failed');
  // A fresh view after every call: memory growth replaces HEAPU8.buffer.
  const view = () => new DataView(module.HEAPU8.buffer);
  const cstr = (p) => { const heap = module.HEAPU8; return Buffer.from(heap.subarray(p, heap.indexOf(0, p))).toString('latin1'); };
  try {
    const out = [`N ${cstr(module._rl_version() >>> 0)}`];
    for (const o of ops) {
      const bytes = Buffer.from(o.hex, 'hex');
      const key = module._malloc(bytes.length + 1); // bytes and a NUL: the C API reads the key up to its first NUL
      if (!key) throw new Error('malloc failed');
      try {
        module.HEAPU8.set(bytes, key);
        module.HEAPU8[key + bytes.length] = 0;
        let dv = view();
        dv.setBigUint64(scratch, o.cap, true);
        dv.setBigUint64(scratch + 8, o.rate, true);
        const r = scratch + LIMIT_BYTES;
        if (o.op === 'allow') {
          module._rl_allow_at(r, h, key, scratch, o.t);
          dv = view();
          out.push(`A ${dv.getUint8(r)} ${dv.getBigUint64(r + 8, true)} ${dv.getBigUint64(r + 16, true)}`);
        } else {
          module._rl_inspect_at(r, h, key, scratch, o.t);
          dv = view();
          const u64s = [8, 16, 24, 32, 40, 48].map((off) => dv.getBigUint64(r + off, true));
          out.push(`I ${dv.getUint8(r)} ${dv.getUint8(r + 1)} ${u64s.join(' ')}`);
        }
      } finally { module._free(key); }
    }
    return out.map((l) => `${l}\n`).join('');
  } finally {
    module._free(scratch);
    module._rl_free(h);
  }
}

// ---------- Compare ----------

// Returns null if equal, else the message for the first differing line.
function compare(golden, produced) {
  if (golden === produced) return null;
  const g = golden.split('\n'), p = produced.split('\n');
  for (let i = 0; ; i++) {
    const gl = i < g.length - 1 ? g[i] : (g[i] || null), pl = i < p.length - 1 ? p[i] : (p[i] || null);
    if (gl !== pl || (i === g.length - 1) !== (i === p.length - 1)) {
      const show = (l) => (l === null ? '<end of file>' : l);
      const note = gl === pl ? ' (only one of them ends in a newline)' : '';
      return `line ${i + 1}: expected ${show(gl)} got ${show(pl)}${note}`;
    }
  }
}

async function main(argv) {
  if (argv.length !== 3) {
    console.error('usage: node run_wasm.mjs <ratelimit.mjs> <trace.jsonl> <golden.out>');
    return 2;
  }
  const [glue, tracePath, goldenPath] = argv;
  let text, golden;
  try {
    text = readFileSync(tracePath).toString('latin1'); // one char per byte; non-ASCII bytes fail the patterns
    golden = readFileSync(goldenPath).toString('latin1');
  } catch (err) {
    console.error(`cannot read: ${err.message}`);
    return 2;
  }
  let trace;
  try { trace = parseTrace(text); } catch (err) {
    if (!(err instanceof TraceError)) throw err;
    console.error(err.message);
    return 2;
  }
  const factory = (await import(pathToFileURL(glue).href)).default;
  const produced = run(await factory(), trace);
  const diff = compare(golden, produced);
  if (diff) { console.error(diff); return 1; }
  console.log(`parity ok: ${tracePath}`);
  return 0;
}

process.exitCode = await main(process.argv.slice(2)).catch((err) => {
  console.error(`unexpected error: ${err.message}`);
  return 2;
});
