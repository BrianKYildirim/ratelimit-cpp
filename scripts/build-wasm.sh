#!/usr/bin/env bash
# Builds the C API (c_api/ratelimit_c.cpp + ratelimit_core) as a single-threaded WebAssembly ES module
# with a pinned Emscripten, then loads it under Node and makes one real rl_allow_at decision.
#
#   source ~/emsdk/emsdk_env.sh
#   bash scripts/build-wasm.sh [--allow-dirty] <out-dir>          (run from the repo root)
#
# Writes <out-dir>/ratelimit.mjs (ES-module glue) and <out-dir>/ratelimit.wasm, keeping the CMake tree
# in <out-dir>/build, and prints the smoke line, the sha256 of both files and the emcc version line.
# The Emscripten version and the build inputs (RL_BUILD_TS, RL_GIT_DESCRIBE) are constants below, so the
# same sources rebuild to the same bytes. A tree with tracked changes, or one that is not a git checkout,
# is refused unless --allow-dirty is given.
set -euo pipefail

EMSDK_VERSION=7.0.0
RL_BUILD_TS=0
RL_GIT_DESCRIBE=wasm

USAGE="usage: bash scripts/build-wasm.sh [--allow-dirty] <out-dir>"
die() { echo "build-wasm: $*" >&2; exit 1; }

allow_dirty=0
out=""
for arg in "$@"; do
  case "$arg" in
    --allow-dirty) allow_dirty=1 ;;
    -*) die "unknown option $arg; $USAGE" ;;
    *) [ -z "$out" ] || die "$USAGE"; out="$arg" ;;
  esac
done
[ -n "$out" ] || die "$USAGE"

[ -f CMakeLists.txt ] && [ -f c_api/ratelimit_c.cpp ] || die "run this from the ratelimit-cpp repo root"

# Pinned toolchain: the first line of `emcc --version` must name EMSDK_VERSION.
command -v emcc >/dev/null 2>&1 || die "activate emsdk $EMSDK_VERSION: source ~/emsdk/emsdk_env.sh"
emcc_line=$(emcc --version 2>/dev/null | sed -n 1p)
case "$emcc_line" in
  *" $EMSDK_VERSION "*) ;;
  *) die "activate emsdk $EMSDK_VERSION: source ~/emsdk/emsdk_env.sh (found: $emcc_line)" ;;
esac

# Only a clean checkout is built by default, so the output matches a commit.
if [ "$allow_dirty" -eq 0 ]; then
  [ -e .git ] || die "not a git checkout; pass --allow-dirty to build these sources anyway"
  status=$(git status --porcelain --untracked-files=no) || die "git status failed"
  [ -z "$status" ] || die "the tree has tracked changes; commit them or pass --allow-dirty"
fi

mkdir -p "$out"
out=$(cd "$out" && pwd)

emcmake cmake --fresh -S . -B "$out/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DRL_BUILD_WASM=ON \
  -DRL_BUILD_SHARED=OFF \
  -DRL_BUILD_TESTS=OFF \
  -DRL_BUILD_BENCH=OFF \
  -DRL_BUILD_JNI=OFF \
  -DRL_BUILD_TS="$RL_BUILD_TS" \
  -DRL_GIT_DESCRIBE="$RL_GIT_DESCRIBE"
cmake --build "$out/build" --target ratelimit_wasm

cp "$out/build/ratelimit.mjs" "$out/build/ratelimit.wasm" "$out/"

# Smoke: load the module, read rl_version, and make one decision at t = 0 for a
# new key with capacity 2 and no refill. The expected line is exact.
smoke=$(node --input-type=module - "$out/ratelimit.mjs" <<'EOF'
import { pathToFileURL } from 'node:url';

const mjs = process.argv[process.argv.length - 1];
const { default: factory } = await import(pathToFileURL(mjs).href);
const m = await factory();

const cstr = (p) => {
  let end = p;
  while (m.HEAPU8[end] !== 0) end++;
  return new TextDecoder().decode(m.HEAPU8.subarray(p, end));
};

const version = cstr(m._rl_version());
const h = m._rl_new_inproc(0, 0);
if (!h) throw new Error('rl_new_inproc returned NULL');

// rl_limit_t {capacity u64 @0, refill_per_sec u64 @8}; rl_decision_t {allowed u8 @0, remaining u64 @8, reset_ms u64 @16}
const limit = m._malloc(16);
const sret = m._malloc(24);
const keyBytes = new TextEncoder().encode('smoke\0');
const key = m._malloc(keyBytes.length);
m.HEAPU8.set(keyBytes, key);
let dv = new DataView(m.HEAPU8.buffer);
dv.setBigUint64(limit, 2n, true);
dv.setBigUint64(limit + 8, 0n, true);

m._rl_allow_at(sret, h, key, limit, 0n);

dv = new DataView(m.HEAPU8.buffer); // the heap may have grown
const line = `smoke: rl_version ${version} allow ${dv.getUint8(sret)} ${dv.getBigUint64(sret + 8, true)} ${dv.getBigUint64(sret + 16, true)}`;

m._free(key);
m._free(sret);
m._free(limit);
m._rl_free(h);

console.log(line);
EOF
)
echo "$smoke"
[ "$smoke" = "smoke: rl_version 0.1.0 allow 1 1 0" ] || die "smoke test failed"

(cd "$out" && sha256sum ratelimit.mjs ratelimit.wasm)
echo "$emcc_line"
