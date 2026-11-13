# Parity: native C API vs WebAssembly

These tests check that the WebAssembly build of the C API (`ratelimit_wasm`, see
"Build for WebAssembly" in the top-level README) makes exactly the same decisions
and reports exactly the same inspection values as a native build. Every field of
`rl_decision_t` and `rl_bucket_state_t` is compared, byte for byte.

```
tests/parity/
  traces/<name>.jsonl     canonical traces, shared by every leg
  golden/<name>.out       expected outcome lines, written by the native runner only
  rl_parity_native.c      native runner over the C API: compare, or --write a golden
  run_wasm.mjs            Node runner over the WebAssembly module: compare only
```

## How it works

Each runner reads a trace, makes the calls it lists through the C API
(`rl_new_inproc`, `rl_allow_at`, `rl_inspect_at`, `rl_free`) in order on one
thread, writes one outcome line per call, and compares the result with
`golden/<name>.out`.

- **Golden files come only from the native runner** (`rl_parity_native --write`).
  `run_wasm.mjs` has no write mode, so the WebAssembly build can never produce its
  own expected output.
- The native runner compiles `c_api/ratelimit_c.cpp` and links `ratelimit_core`:
  the same translation units as the WebAssembly module.
- Times are always passed in (`rl_allow_at`, `rl_inspect_at`). The runners never
  call `rl_now_ns`, `rl_inspect`, `rl_allow` or `rl_evict_idle`, which the
  WebAssembly module does not export.

### Single-threaded by design

Parity is single-threaded by design. The native runner makes every call on one
thread in trace order, which is the same sequence the single-threaded
WebAssembly build sees. These tests make no claim about multithreaded behavior;
the concurrency tests in `tests/test_core.cpp` cover that for native builds.

## Trace format

JSON Lines, LF only, with a final newline and no CR. Each line has exactly one of
these shapes, with the keys in this order and no spaces:

```
{"op":"new","shards":S,"hint":H}
{"op":"allow","key":"<hex>","cap":C,"rate":R,"t":T}
{"op":"inspect","key":"<hex>","cap":C,"rate":R,"t":T}
```

- `new` appears exactly once, as the first line: `rl_new_inproc(S, H)`.
- `allow` is `rl_allow_at(h, key, {C, R}, T)`; `inspect` is `rl_inspect_at(h, key, {C, R}, T)`.
- `key` is the key's bytes in lowercase hex, an even number of digits, at most
  256 bytes. It may be empty. The C API reads a key up to its first NUL, so a key
  with an embedded `00` byte is the same key as its prefix.
- Numbers are unsigned decimals without leading zeros. `S` and `H` fit in 32 bits;
  `C`, `R` and `T` fit in 64 bits.
- A line that does not re-serialize to itself byte for byte is rejected. The
  runners are deliberately strict: they are not general JSON parsers.

## Outcome format

One line per trace line, LF only, every value an unsigned decimal:

```
N <rl_version()>
A <allowed> <remaining> <reset_ms>
I <tracked> <would_allow> <stored_nanotokens> <stored_at_ns> <nanotokens> <tokens> <until_token_ns> <until_full_ns>
```

`N` answers the `new` line, `A` an `allow` and `I` an `inspect`. `RL_NEVER`
appears as `18446744073709551615`.

## Exit codes

Both runners use the same codes:

- `0`: the outcome equals the golden file.
- `1`: golden mismatch; prints `line N: expected <golden line> got <produced line>`.
- `2`: usage error, unreadable file, or a trace error; prints `trace line N: <reason>`.

## Running each leg

Neither leg is part of a default build: with default options, `ctest` runs the
same tests as before.

**Linux (GCC)**, from the repository root:

```bash
cmake -S . -B build-parity -G Ninja -DCMAKE_BUILD_TYPE=Release -DRL_BUILD_PARITY=ON \
  -DRL_BUILD_TESTS=OFF -DRL_BUILD_BENCH=OFF -DRL_BUILD_JNI=OFF
cmake --build build-parity
ctest --test-dir build-parity --output-on-failure      # parity_native.<name>
```

**Windows (MSVC)**, from a Developer environment:

```bat
cmake -S . -B build-parity -DRL_BUILD_PARITY=ON -DRL_BUILD_TESTS=OFF -DRL_BUILD_BENCH=OFF -DRL_BUILD_JNI=OFF
cmake --build build-parity --config Release
ctest --test-dir build-parity -C Release -R parity_native --output-on-failure
```

The native runner reads and writes files in binary mode, so MSVC builds compare
and write the same LF bytes as GCC.

**WebAssembly**, with the Emscripten SDK active (`source <emsdk>/emsdk_env.sh`):

```bash
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DRL_BUILD_WASM=ON \
  -DRL_BUILD_SHARED=OFF -DRL_BUILD_TESTS=OFF -DRL_BUILD_BENCH=OFF -DRL_BUILD_JNI=OFF
cmake --build build-wasm
ctest --test-dir build-wasm --output-on-failure        # parity_wasm.<name>
```

The WebAssembly tests run `run_wasm.mjs` with the Node that emsdk configures
(`CMAKE_CROSSCOMPILING_EMULATOR`), else `$EMSDK_NODE`, else `node` on `PATH`.

Every `*.jsonl` in `traces/` is registered as a test; the list is re-checked at
build time.

### Writing a golden file

Only after a deliberate change of library behavior, and only with the native
runner:

```bash
build-parity/rl_parity_native --write tests/parity/traces/<name>.jsonl tests/parity/golden/<name>.out
```

Never write a golden file from the WebAssembly build or from JavaScript.

## Edge traces

Hand-written. Each one replays the behavior of the tests it names, through
`rl_allow_at` and `rl_inspect_at` only.

| Trace | Behavior | From |
|-------|----------|------|
| `edge-initial-full` | An untracked key reads as a full bucket; allows drain it; the next one is denied | `Core.InitialFull` |
| `edge-c-api-inspection` | The `check_inspection()` sequence, rebased to sixty seconds, including the embedded-NUL keys | `tests/test_c_api.c` |
| `edge-quiet-denial` | A denial just under a millisecond after the stored time leaves `stored_at_ns` unchanged; one at exactly a millisecond updates it; a one-third-token remainder is kept exact | `Core.QuietDenialsDoNotChangeLaterVerdicts`, `Inspect.QuietDenialsKeepValuesExact` |
| `edge-rate-zero` | No refill: denied forever, waits are `RL_NEVER`, `reset_ms` is zero | `Core.NoRefillNeverRecovers`, `Inspect.EdgeLimits` |
| `edge-capacity-zero` | Capacity zero: always denied, `until_token_ns` is `RL_NEVER`, `until_full_ns` is zero | `Core.CapacityZero`, `Core.CapacityZeroWithRefillNeverAllows` |
| `edge-capacity-clamp` | The largest capacity, the next one and `UINT64_MAX` give the same outcomes | `Core.CapacityAboveMaxIsClamped`, `Inspect.EdgeLimits` |
| `edge-lowered-capacity` | A lowered capacity clamps the stored balance; raising it again mints nothing | `Core.LoweringCapacityClampsBalance`, `Inspect.EdgeLimits` |
| `edge-clock-backwards` | A time before the stored time adds no tokens and does not move the stored time back | `Core.ClockGoingBackwardsAddsNoTokens`, `Inspect.TimeBeforeStoredTimeAddsNothing` |
| `edge-overflow` | A refill rate of `UINT64_MAX` and times near `UINT64_MAX` saturate at capacity | `Core.HugeRateAndIdleDoNotOverflow`, `Inspect.EdgeLimits` |
| `edge-first-nanosecond` | A wait ends at its first nanosecond; `reset_ms` rounds up | `Inspect.WaitsAreTheFirstNanosecond`, `Core.ResetMsIsExactAndRoundsUp` |
| `edge-keys` | The empty key, keys around the inline length, long keys, a non-ASCII key, and embedded-NUL keys equal to their prefix | `Core.KeysWithEmbeddedNulAndEmptyKey`, `Core.KeyLengthsAroundInlineLimit`, `Inspect.LongAndEmbeddedNulKeys` |

## Random traces

Seeded traces exported from the traffic generator of the portfolio's rate
limiter demo, so the demo's own traffic shapes are part of parity. Each preset
runs ten seconds of virtual time, applies its scripted clicks, bursts and
setting changes, and inspects every demo key every quarter second.

They were generated with `node tools/ratelimit-trace.mjs --preset <name> --out <file>`
in [github.com/BrianKYildirim/portfolio](https://github.com/BrianKYildirim/portfolio).
The generator script is `tools/ratelimit-trace.mjs`; the traffic engine is
`scripts/demos/ratelimit-traffic.js`. The traces are data: they are never
regenerated as part of a comparison, because the random clients' arrival times
use the JavaScript engine's `Math.log`.

| Trace | Preset | Seed | Generator sha256 | Engine sha256 | Lines |
|-------|--------|------|------------------|---------------|-------|
| `random-r1` | r1 | one | `9eab1aa46582bcf2212a9e3bf47109ac0cd9ec545c9547444ece5a709c407684` | `7393a6c0e70b5999a2dcef42971eb27931a905111896d458b8fcaa5577fdfd3b` | 332 |
| `random-r2` | r2 | two | `9eab1aa46582bcf2212a9e3bf47109ac0cd9ec545c9547444ece5a709c407684` | `7393a6c0e70b5999a2dcef42971eb27931a905111896d458b8fcaa5577fdfd3b` | 441 |
| `random-r3` | r3 | three | `9eab1aa46582bcf2212a9e3bf47109ac0cd9ec545c9547444ece5a709c407684` | `7393a6c0e70b5999a2dcef42971eb27931a905111896d458b8fcaa5577fdfd3b` | 1371 |
| `random-r4` | r4 | four | `9eab1aa46582bcf2212a9e3bf47109ac0cd9ec545c9547444ece5a709c407684` | `7393a6c0e70b5999a2dcef42971eb27931a905111896d458b8fcaa5577fdfd3b` | 498 |

Line counts include the `new` line.
