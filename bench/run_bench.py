"""Reproducible benchmark runner for ratelimit-cpp: one command per environment.

  python bench/run_bench.py --env windows-native      (native Windows, MSVC, Visual Studio generator)
  python bench/run_bench.py --env ubuntu-wsl2         (Ubuntu under WSL2, GCC, Ninja; run from a WSL-native clone)

Each run configures and builds Release with tests and benchmarks (JNI off), runs ctest, runs rl_bench
(Google Benchmark: 10 repetitions, random interleaving, 0.2 s minimum warm-up, every repetition kept)
and runs rl_latency 7 times (`rl_latency 0.5 8`). It writes

  bench/results/<YYYY-MM-DD>-<sha7>[-<tag>]/<env>/
      raw/           harness output: tests.txt, rl_bench.json, rl_bench.txt, latency-run-NN.txt
      meta.json      bench-meta/1: hardware, OS, power, load, toolchain, flags, workload, timer, validity, raw sha256
      summary.json   bench-summary/1: n, median, p25, p75, min, max per counter, computed only from raw/

and prints the environment directory as its last line. It refuses a dirty tree unless --allow-dirty
(recorded in meta.json source.dirty) and never overwrites an existing environment directory.

  python bench/run_bench.py --resummarize bench/results/<session>/<env>
      rebuild summary.json from raw/ and meta.json alone (byte-identical for identical raw files)

Labels (BENCH-05/06): gbench real_time per iteration is measure amortized_per_op, never per-op latency;
only rl_latency percentiles are per_op_latency. BM_MemoryPerKey counters exist only where glibc heap
accounting is compiled in (ubuntu-wsl2) and carry build instrumented:glibc-heap-accounting.
Whole runs are marked invalid (kept in raw/, left out of summary values) when the machine is on battery,
when CPU busy at the start of a measured step stays above --max-busy-pct after waiting, or when a harness
step fails. No individual sample is ever dropped. Standard library only (Python 3.11).
"""

import argparse
import datetime
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True  # no bench/__pycache__ in the measured tree
sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmeta  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
PROJECT = "ratelimit-cpp"
REPO_SLUG = "BrianKYildirim/ratelimit-cpp"
ENVIRONMENTS = ("windows-native", "ubuntu-wsl2")
TAG_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
CMAKE_OPTIONS = ["-DRL_BUILD_JNI=OFF", "-DRL_BUILD_TESTS=ON", "-DRL_BUILD_BENCH=ON"]
LINEAR_TICKS = 8192  # rl_latency Histogram: 1-tick buckets below this, power-of-two buckets above
HEAP_BUILD = "instrumented:glibc-heap-accounting"
REDACTION_NOTE = "raw text: absolute repo and home paths replaced by <repo> and <home>; line endings normalized to LF"
GLIBC_SKIP = "heap accounting requires glibc"

# gbench counter -> (summary key, measure, unit). real_time is added separately (amortized_per_op).
COUNTERS = (("wall_ops/s", "wall_ops_s", "throughput", "ops/s"),
            ("inspect_ops/s", "inspect_ops_s", "throughput", "ops/s"),
            ("bytes/key", "bytes_per_key", "heap_bytes", "bytes"),
            ("empty_KiB", "empty_KiB", "heap_bytes", "KiB"))
TIME_UNIT_NS = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}
LATENCY_HEADER = re.compile(r"FastClock uses the TSC: (yes|no); ([0-9.]+) ticks/ns; timing overhead ([0-9.]+) ns \(subtracted\)")
LATENCY_PERCENTILES = (("p50_ns", 50), ("p90_ns", 90), ("p99_ns", 99), ("p999_ns", 99.9))
WARMUP_POLICY = ("gbench --benchmark_min_warmup_time=0.2; rl_latency: fixture insert and throughput phase precede "
                 "the latency phase, no separate warm-up")
OUTLIER_POLICY = "no samples dropped; whole runs marked invalid on precondition failure"


def log(msg):
    print(f"[run_bench] {msg}", file=sys.stderr, flush=True)


def die(msg, code=1):
    log(f"error: {msg}")
    sys.exit(code)


def rel(path, base=REPO):
    """A path shown relative to base with forward slashes (never absolute)."""
    try:
        return Path(os.path.relpath(Path(path).resolve(), Path(base).resolve())).as_posix()
    except ValueError:  # another drive on Windows
        return f"<outside-repo>/{Path(path).name}"


# ---------- Raw output: path redaction and LF ----------

def redactor(repo, home):
    """Replaces the absolute repo root and home directory in captured text (both slash styles and the
    JSON-escaped double-backslash form), longest first."""
    subs = []
    for path, token in ((repo, "<repo>"), (home, "<home>")):
        p = str(Path(path).resolve())
        forms = {p, p.replace("\\", "/"), p.replace("/", "\\"), p.replace("/", "\\").replace("\\", "\\\\")}
        for form in sorted(forms, key=len, reverse=True):
            if len(form) > 3:
                subs.append((re.compile(re.escape(form), re.IGNORECASE if os.name == "nt" else 0), token))

    def apply(data: bytes) -> bytes:
        text = data.replace(b"\r\n", b"\n").decode("utf-8", errors="surrogateescape")
        for rx, token in subs:
            text = rx.sub(token, text)
        return text.encode("utf-8", errors="surrogateescape")
    return apply


# ---------- Build ----------

def cmake_cache(build_dir):
    out = {}
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):[A-Z]+=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def compiler_facts(build_dir):
    """CMAKE_CXX_COMPILER_ID and _VERSION from CMakeFiles/<cmake version>/CMakeCXXCompiler.cmake."""
    for f in sorted((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")):
        text = f.read_text(encoding="utf-8", errors="replace")
        get = lambda k: (re.search(rf'set\({k} "([^"]*)"\)', text) or [None, None])[1]  # noqa: E731
        return get("CMAKE_CXX_COMPILER_ID"), get("CMAKE_CXX_COMPILER_VERSION")
    return None, None


def fetched_versions():
    """Google Benchmark and googletest versions from the FetchContent URL tags in CMakeLists.txt. (gbench's own
    context library_version is `git describe` of the enclosing ratelimit-cpp checkout, because the archive
    has no .git, so it is not used.)"""
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    out = {}
    for name, key in (("google/benchmark", "google_benchmark"), ("google/googletest", "googletest")):
        m = re.search(rf"github\.com/{name}/archive/refs/tags/([^/\s]+)\.zip", text)
        out[key] = f"{m.group(1)} (FetchContent URL tag)" if m else "unknown"
    return out


def build(env, build_dir):
    gen = ["-G", "Visual Studio 17 2022", "-A", "x64"] if env == "windows-native" else ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    configure = ["cmake", "-S", str(REPO), "-B", str(build_dir), *gen, *CMAKE_OPTIONS]
    compile_ = ["cmake", "--build", str(build_dir), "--config", "Release", "--parallel", str(os.cpu_count() or 4)]
    build_dir.mkdir(parents=True, exist_ok=True)
    logfile = build_dir / "run_bench-build.log"  # inside the (ignored) build directory
    with open(logfile, "wb") as f:
        for step in (configure, compile_):
            log(" ".join(step))
            p = subprocess.run(step, stdout=f, stderr=subprocess.STDOUT, check=False)
            if p.returncode != 0:
                die(f"{step[0]} {step[1]} failed with exit status {p.returncode}; see {logfile}")
    cache = cmake_cache(build_dir)
    cid, cver = compiler_facts(build_dir)
    cmake_version = (subprocess.run(["cmake", "--version"], capture_output=True, text=True, check=True).stdout.splitlines() or ["?"])[0]
    toolchain = {
        "compiler_id": cid or "unknown", "compiler_version": cver or "unknown",
        "generator": cache.get("CMAKE_GENERATOR", "unknown"), "build_type": "Release",
        "cxx_flags_release": cache.get("CMAKE_CXX_FLAGS_RELEASE", "").strip() or "(empty)",
        "cxx_flags": cache.get("CMAKE_CXX_FLAGS", "").strip(),
        "cmake_options": [*gen, *CMAKE_OPTIONS],
        "cmake_version": cmake_version.replace("cmake version ", ""),
        "cmake_system": f"{cache.get('CMAKE_HOST_SYSTEM_NAME', '')}".strip() or None,
    }
    sub = ["Release"] if env == "windows-native" else []
    ext = ".exe" if os.name == "nt" else ""
    exes = {name: build_dir.joinpath(*sub, name + ext) for name in ("rl_bench", "rl_latency")}
    for name, exe in exes.items():
        if not exe.is_file():
            die(f"{name} was not built at {exe}")
    return toolchain, exes


# ---------- Validity gate ----------

def gate(step, args, env):
    """Precondition sample before a measured step: AC power and CPU busy. Waits up to --quiet-timeout for
    quiet; returns the record kept in meta.json validity.runs."""
    waited, start = 0.0, time.monotonic()
    while True:
        busy = benchmeta.cpu_busy_pct(1.0)
        power = benchmeta.power_status()
        reasons = []
        if power["ac_line"] is False:
            reasons.append("on battery (AC line offline)")
        if busy is not None and busy > args.max_busy_pct:
            reasons.append(f"CPU busy {busy}% > {args.max_busy_pct}% at start")
        waited = round(time.monotonic() - start, 1)
        if not reasons or waited >= args.quiet_timeout:
            break
        log(f"{step}: waiting for quiet ({'; '.join(reasons)}; waited {waited} s)")
        time.sleep(4)
    if reasons:
        reasons[-1] += f" after waiting {waited} s"
    return {"step": step, "cpu_busy_pct": busy, "ac_line": power["ac_line"], "waited_s": waited,
            "busy_source": "WSL VM /proc/stat (not the Windows host)" if env == "ubuntu-wsl2" else "GetSystemTimes",
            "valid": not reasons, "reasons": reasons}


def run_step(argv, cwd=None):
    """(exit status, combined stdout+stderr bytes). check=True; a failure is returned, not raised."""
    try:
        p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd, check=True)
        return 0, p.stdout
    except subprocess.CalledProcessError as e:
        return e.returncode, e.stdout or b""


# ---------- Parsing ----------

def parse_ctest(text):
    m = re.search(r"(\d+)% tests passed, (\d+) tests? failed out of (\d+)", text)
    if not m:
        return None
    failed, total = int(m.group(2)), int(m.group(3))
    skipped = len(re.findall(r"\(Skipped\)", text))
    return {"passed": total - failed - skipped, "failed": failed, "skipped": skipped, "total": total}


def bench_key(run_name):
    """gbench run_name -> summary key: drop the real_time segment, '/' -> '.'."""
    return ".".join(s for s in run_name.split("/") if s != "real_time")


def gbench_runs(data):
    """({key: [iteration entries by repetition_index]}, {key: skip message}) from gbench JSON."""
    groups, skipped = {}, {}
    for b in data.get("benchmarks", []):
        if b.get("run_type", "iteration") != "iteration":
            continue
        key = bench_key(b.get("run_name") or b["name"])
        msg = b.get("error_message") or b.get("skip_message")
        if b.get("error_occurred") or msg or b.get("skipped"):
            skipped[key] = msg or "skipped"
            continue
        groups.setdefault(key, []).append(b)
    for entries in groups.values():
        entries.sort(key=lambda b: b.get("repetition_index", 0))
    return groups, skipped


def parse_latency(text):
    """(header dict or None, [(scenario, threads, mops, cpu_pct, p50, p90, p99, p999, max)])."""
    m = LATENCY_HEADER.search(text)
    header = {"fastclock_uses_tsc": m.group(1) == "yes", "ticks_per_ns": float(m.group(2)),
              "overhead_ns": float(m.group(3))} if m else None
    rows = []
    for line in text.splitlines():
        f = line.split()
        if len(f) == 9 and f[1].isdigit():
            try:
                rows.append((f[0], int(f[1]), *map(float, f[2:])))
            except ValueError:
                continue
    return header, rows


# ---------- Statistics ----------

def stats(values, unit, measure, build, **extra):
    """Stats object: run-level values in run order; median and inclusive quartiles as the checker recomputes them."""
    vals = list(values)
    if len(vals) == 1:
        med = p25 = p75 = vals[0]
    else:
        med = statistics.median(vals)
        q = statistics.quantiles(vals, n=4, method="inclusive")
        p25, p75 = q[0], q[2]
    return {"n": len(vals), "values": vals, "median": med, "min": min(vals), "max": max(vals), "p25": p25, "p75": p75,
            "unit": unit, "measure": measure, "build": build, **extra}


def ratio_stats(num, den, definition, unit="ratio", combine=lambda a, b: a / b):
    """Per-repetition ratio of two {repetition_index: value} maps, paired by repetition index."""
    idx = sorted(set(num) & set(den))
    vals = [combine(num[i], den[i]) for i in idx if den[i]]
    return stats(vals, unit, "derived_ratio", "uninstrumented", definition=definition) if vals else None


def summarize_rl_bench(data):
    groups, skipped = gbench_runs(data)
    out, by_rep = {}, {}
    for key, entries in sorted(groups.items()):
        entry, reps = {}, {}
        heap = key.startswith("BM_MemoryPerKey")
        if not heap:  # BM_MemoryPerKey's time is one whole fill, not a per-call figure
            rt = [b["real_time"] * TIME_UNIT_NS[b.get("time_unit", "ns")] for b in entries]
            entry["real_time_ns"] = stats(rt, "ns", "amortized_per_op", "uninstrumented",
                                          description="gbench real_time per iteration (loop time / iterations), not a per-call latency")
            reps["real_time_ns"] = {b.get("repetition_index", i): v for i, (b, v) in enumerate(zip(entries, rt))}
        for counter, name, measure, unit in COUNTERS:
            if all(counter in b for b in entries):
                vals = [float(b[counter]) for b in entries]
                entry[name] = stats(vals, unit, measure, HEAP_BUILD if measure == "heap_bytes" else "uninstrumented")
                reps[name] = {b.get("repetition_index", i): v for i, (b, v) in enumerate(zip(entries, vals))}
        if entry:
            out[key], by_rep[key] = entry, reps
    return out, by_rep, skipped


def derived_ratios(by_rep):
    def series(key, counter):
        return by_rep.get(key, {}).get(counter)
    specs = {
        "hot_denied_scaling_t8_over_t1": ("BM_Hot_Denied.threads:8", "BM_Hot_Denied.threads:1", "wall_ops_s",
                                          "wall_ops_s at 8 threads / at 1 thread, BM_Hot_Denied, per repetition"),
        "hot_allowed_scaling_t8_over_t1": ("BM_Hot_Allowed.threads:8", "BM_Hot_Allowed.threads:1", "wall_ops_s",
                                           "wall_ops_s at 8 threads / at 1 thread, BM_Hot_Allowed, per repetition"),
        "inspect_same_key_cost": ("BM_Hot_Allowed_Inspected.inspectors:1.pace_ns:0.same_key:1.threads:1",
                                  "BM_Hot_Allowed_Inspected.inspectors:1.pace_ns:0.same_key:0.threads:1", "real_time_ns",
                                  "real_time_ns with 1 back-to-back inspector on the hot key / on an untracked key "
                                  "(same_key 1 vs 0, inspectors 1, pace 0, 1 thread), per repetition"),
    }
    out = {}
    for name, (a, b, counter, definition) in specs.items():
        num, den = series(a, counter), series(b, counter)
        if num and den:
            s = ratio_stats(num, den, definition)
            if s:
                out[name] = s
    full = series("BM_TryAcquire_Disjoint_Allowed.0.threads:1", "real_time_ns")
    batched = series("BM_TryAcquireAt_Disjoint_Allowed.0.threads:1", "real_time_ns")
    if full and batched:
        s = ratio_stats(full, batched, "(TryAcquire_Disjoint - TryAcquireAt_Disjoint) / TryAcquire_Disjoint real_time_ns "
                        "at 1 thread, per repetition: the share of try_acquire time spent reading the clock "
                        "(TryAcquireAt reads it once per 64 calls)", unit="fraction", combine=lambda a, b: (a - b) / a)
        if s:
            out["clock_share"] = s
    return out


def summarize_latency(runs):
    """runs: [(header, rows)] of valid rl_latency runs in run order -> {scenario: {t<threads>: {...}}}."""
    cells = {}
    for header, rows in runs:
        for row in rows:
            cells.setdefault((row[0], row[1]), []).append((header, row))
    out = {}
    for (scenario, threads), items in cells.items():
        group = {"mops_s": stats([r[2] for _, r in items], "Mops/s", "throughput", "uninstrumented"),
                 "cpu_pct": {"unit": "%", "values": [r[3] for _, r in items],
                             "note": "process CPU time / (wall time * threads) per run; context for mops_s, not a claimable measure"}}
        for col, (name, pct) in enumerate(LATENCY_PERCENTILES, start=4):
            vals = [r[col] for _, r in items]
            exact = all(v * h["ticks_per_ns"] < LINEAR_TICKS - 0.5 for (h, _), v in zip(items, vals))
            group[name] = stats(vals, "ns", "per_op_latency", "uninstrumented", percentile=pct,
                                resolution="exact-1-tick" if exact else "log2-bucket-lower-bound")
        group["max_ns"] = {"unit": "ns", "values": [r[8] for _, r in items],
                           "note": "largest timed call per run (exact ticks); not a percentile, so not a per_op_latency stats object"}
        out.setdefault(scenario, {})[f"t{threads}"] = group
    return out


def build_summary(env_dir, meta):
    """summary.json from raw/ and meta.json only (also what --resummarize runs)."""
    env_dir = Path(env_dir)
    read = lambda p: (env_dir / p).read_text(encoding="utf-8", errors="surrogateescape")  # noqa: E731
    runs = meta["validity"]["runs"]
    used = []
    summary = {"schema": "bench-summary/1", "project": meta["project"], "environment_id": meta["environment_id"],
               "source_sha": meta["source"]["sha"]}
    wl = meta["workload"]
    summary["policy"] = {
        "quantile_method": "inclusive", "runs": wl["rl_bench"]["repetitions"],
        "runs_by_harness": {"rl_bench": wl["rl_bench"]["repetitions"], "rl_latency": wl["rl_latency"]["runs"]},
        "warmup": WARMUP_POLICY, "outliers": OUTLIER_POLICY,
        "values": "run-level: one value per gbench repetition (rl_bench) or per rl_latency process (rl_latency), in run order",
        "invalid_runs_excluded": [r["file"] for r in runs if not r["valid"]],
        "latency": ("rl_latency percentiles are over all threads' calls in one run with the median empty-region timing "
                    f"overhead subtracted; values at or above {LINEAR_TICKS} ticks are power-of-two bucket lower bounds "
                    "(resolution log2-bucket-lower-bound)"),
    }
    if (env_dir / "raw/tests.txt").is_file():
        tests = parse_ctest(read("raw/tests.txt"))
        if tests:
            summary["tests"] = tests
            used.append("raw/tests.txt")
    bench = next((r for r in runs if r["step"] == "rl_bench"), None)
    if bench and bench["valid"]:
        data = json.loads(read(bench["file"]))
        rl_bench, by_rep, _ = summarize_rl_bench(data)
        if rl_bench:
            summary["rl_bench"] = rl_bench
            used.append(bench["file"])
            derived = derived_ratios(by_rep)
            if derived:
                summary["derived"] = derived
    lat = []
    for r in runs:
        if r["step"] == "rl_latency" and r["valid"]:
            header, rows = parse_latency(read(r["file"]))
            if header and rows:
                lat.append((header, rows))
                used.append(r["file"])
    if lat:
        summary["rl_latency"] = summarize_latency(lat)
    summary["generated_from"] = sorted(used)
    return summary


# ---------- Run ----------

def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--env", choices=ENVIRONMENTS, help="environment id (required unless --resummarize)")
    p.add_argument("--out", help="results root (default bench/results in this repo; relative paths are from the cwd)")
    p.add_argument("--session-date", default=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
                   help="YYYY-MM-DD in the session directory name (default: today, UTC)")
    p.add_argument("--tag", help="optional session directory suffix: <date>-<sha7>-<tag>")
    p.add_argument("--allow-dirty", action="store_true", help="run on a tree with uncommitted tracked changes (recorded)")
    p.add_argument("--smoke", action="store_true", help="quick check: 2 repetitions, min_time 0.05s, 1 latency run of 0.1 s phases")
    p.add_argument("--filter", help="gbench --benchmark_filter regex")
    p.add_argument("--build-dir", help="CMake build directory (default build-bench-<env> in this repo)")
    p.add_argument("--repetitions", type=int, help="rl_bench repetitions (default 10; smoke 2)")
    p.add_argument("--latency-runs", type=int, help="rl_latency invocations (default 7; smoke 1)")
    p.add_argument("--latency-seconds", type=float, help="rl_latency seconds per phase (default 0.5; smoke 0.1)")
    p.add_argument("--latency-max-threads", type=int, default=8, help="rl_latency max threads (default 8)")
    p.add_argument("--max-busy-pct", type=float, default=10.0, help="a run is invalid when CPU busy at its start stays above this (default 10)")
    p.add_argument("--quiet-timeout", type=float, default=120.0, help="seconds to wait for quiet before marking a run invalid (default 120)")
    p.add_argument("--resummarize", metavar="ENVDIR", help="rebuild ENVDIR/summary.json from raw/ and meta.json, then exit")
    args = p.parse_args(argv)
    if not args.resummarize and not args.env:
        p.error("--env is required")
    if args.tag is not None and not TAG_RE.match(args.tag):
        p.error(f"--tag must match {TAG_RE.pattern}")
    try:
        datetime.date.fromisoformat(args.session_date)
    except ValueError:
        p.error("--session-date must be YYYY-MM-DD")
    args.repetitions = args.repetitions or (2 if args.smoke else 10)
    args.latency_runs = args.latency_runs or (1 if args.smoke else 7)
    args.latency_seconds = args.latency_seconds or (0.1 if args.smoke else 0.5)
    return args


def shown_command(argv):
    """The documented command with every path argument made repo-relative."""
    out, path_flags, nxt = ["python", "bench/run_bench.py"], ("--out", "--build-dir", "--resummarize"), False
    for a in argv:
        if nxt:
            out.append(rel(a, REPO) if os.path.isabs(a) or os.sep in a or "/" in a else a)
            nxt = False
        elif a in path_flags:
            out.append(a)
            nxt = True
        elif any(a.startswith(f + "=") for f in path_flags):
            k, v = a.split("=", 1)
            out.append(f"{k}={rel(v, REPO)}")
        else:
            out.append(a)
    return out


def check_platform(env):
    if env == "windows-native" and os.name != "nt":
        die("--env windows-native must run on native Windows")
    if env == "ubuntu-wsl2" and not benchmeta.is_wsl():
        die("--env ubuntu-wsl2 must run inside WSL2")


def resummarize(env_dir):
    env_dir = Path(env_dir)
    meta = json.loads((env_dir / "meta.json").read_text(encoding="utf-8"))
    benchmeta.write_json(env_dir / "summary.json", build_summary(env_dir, meta))
    print(env_dir.as_posix())


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    args = parse_args(argv)
    if args.resummarize:
        resummarize(args.resummarize)
        return 0
    env = args.env
    check_platform(env)
    source = benchmeta.git_source(REPO, REPO_SLUG)
    if source["dirty"] and not args.allow_dirty:
        die(f"tracked files have uncommitted changes ({', '.join(source['modified'])}); commit them, or pass "
            "--allow-dirty to run anyway (the run is then recorded as dirty)")
    session = f"{args.session_date}-{source['sha'][:7]}" + (f"-{args.tag}" if args.tag else "")
    out_root = Path(args.out).resolve() if args.out else REPO / "bench" / "results"
    env_dir = out_root / session / env
    if env_dir.exists():
        die(f"refusing to overwrite existing results directory {env_dir.as_posix()}; use another --tag or --session-date", 3)
    build_dir = Path(args.build_dir).resolve() if args.build_dir else REPO / f"build-bench-{env}"
    toolchain, exes = build(env, build_dir)

    (env_dir / "raw").mkdir(parents=True, exist_ok=False)
    raw = env_dir / "raw"
    redact = redactor(REPO, Path.home())
    linux = env == "ubuntu-wsl2"
    bench_rel, lat_rel = rel(exes["rl_bench"]), rel(exes["rl_latency"])
    gbench_flags = [f"--benchmark_repetitions={args.repetitions}", "--benchmark_enable_random_interleaving=true",
                    "--benchmark_min_warmup_time=0.2", "--benchmark_report_aggregates_only=false",
                    "--benchmark_out_format=json", f"--benchmark_context=environment_id={env}"]
    if args.smoke:
        gbench_flags.append("--benchmark_min_time=0.05s")
    if args.filter:
        gbench_flags.append(f"--benchmark_filter={args.filter}")
    lat_args = [f"{args.latency_seconds:g}", str(args.latency_max_threads)]
    ctest = ["ctest", "--test-dir", str(build_dir), "--output-on-failure"] + (["-C", "Release"] if not linux else [])
    workload = {
        "smoke": args.smoke,
        "steps": [["ctest", "--test-dir", rel(build_dir), "--output-on-failure"] + (["-C", "Release"] if not linux else []),
                  [bench_rel, *gbench_flags, "--benchmark_out=raw/rl_bench.json"],
                  [lat_rel, *lat_args]],
        "rl_bench": {"repetitions": args.repetitions, "random_interleaving": True, "min_warmup_time_s": 0.2,
                     "min_time": "0.05s" if args.smoke else "gbench default (0.5 s)", "filter": args.filter,
                     "report_aggregates_only": False},
        "rl_latency": {"runs": args.latency_runs, "seconds_per_phase": args.latency_seconds,
                       "max_threads": args.latency_max_threads},
        "validity": {"max_busy_pct": args.max_busy_pct, "quiet_timeout_s": args.quiet_timeout},
        "runner": {"python": sys.version.split()[0], "implementation": sys.implementation.name},
        "build_dir": rel(build_dir),
    }
    started = benchmeta.collect(PROJECT, env, shown_command(argv), workload)
    log(f"collected environment ({started['hardware']['cpu_model']}); measuring into {env_dir.as_posix()}")

    runs, notes, reasons = [], [REDACTION_NOTE, "raw/rl_bench.json context.library_version is git describe of this "
                                "checkout (the gbench archive has no .git); library.google_benchmark is the real version"], []
    code, text = run_step(ctest)
    (raw / "tests.txt").write_bytes(redact(text))
    tests = parse_ctest(redact(text).decode("utf-8", "replace"))
    if code != 0 or not tests or tests["failed"]:
        reasons.append(f"ctest failed (exit status {code}; {tests})")
    log(f"ctest: {tests}")

    g = gate("rl_bench", args, env)
    g["file"] = "raw/rl_bench.json"
    log("rl_bench " + " ".join(gbench_flags))
    code, text = run_step([str(exes["rl_bench"]), *gbench_flags, f"--benchmark_out={raw / 'rl_bench.json'}"])
    (raw / "rl_bench.txt").write_bytes(redact(text))
    jpath = raw / "rl_bench.json"
    if jpath.is_file():
        jpath.write_bytes(redact(jpath.read_bytes()))
    if code != 0 or not jpath.is_file():
        g["valid"], g["reasons"] = False, g["reasons"] + [f"rl_bench failed (exit status {code})"]
        if not jpath.is_file():
            jpath.write_bytes(b"")
    runs.append(g)

    headers = []
    for i in range(1, args.latency_runs + 1):
        g = gate("rl_latency", args, env)
        g["run"], g["file"] = i, f"raw/latency-run-{i:02d}.txt"
        log(f"rl_latency {' '.join(lat_args)} (run {i} of {args.latency_runs})")
        code, text = run_step([str(exes["rl_latency"]), *lat_args])
        data = redact(text)
        (env_dir / g["file"]).write_bytes(data)
        header, rows = parse_latency(data.decode("utf-8", "replace"))
        if code != 0 or not header or not rows:
            g["valid"], g["reasons"] = False, g["reasons"] + [f"rl_latency failed (exit status {code})"]
        if header:
            headers.append(header)
        runs.append(g)

    ended = benchmeta.collect(PROJECT, env, shown_command(argv), workload)
    for r in runs:
        reasons += [f"{r['file']}: {x}" for x in r["reasons"]]

    try:
        gb = json.loads((raw / "rl_bench.json").read_text(encoding="utf-8"))
    except ValueError:
        gb = {}
    _, skipped = gbench_runs(gb)
    for key, msg in sorted(skipped.items()):
        notes.append(f"rl_bench {key}: skipped by the benchmark ({msg})")
    if any(msg == GLIBC_SKIP for msg in skipped.values()):
        notes.append("BM_MemoryPerKey (bytes_per_key, empty_KiB) needs the glibc heap-accounting operator new "
                     "replacement, so it is absent from this environment's summary (D-12)")
    if linux:
        notes.append("rl_bench on glibc links a global operator new/delete replacement for heap accounting; it counts "
                     "only inside BM_MemoryPerKey, but every rl_bench benchmark runs through it (RESEARCH Pitfall 7)")
        notes.append("cpu_busy_pct and loadavg describe the WSL VM only, not Windows host activity (RESEARCH Pitfall 4)")
    if source["dirty"]:
        notes.append(f"dirty tree (--allow-dirty): modified {source['modified']}, untracked {source['untracked']}")
    if args.smoke:
        notes.append("smoke run: reduced repetitions, min_time and latency phases; not for claims")
    if started["power"]["ac_line"] != ended["power"]["ac_line"]:
        notes.append(f"AC line changed during the run: {started['power']['ac_line']} -> {ended['power']['ac_line']}")
    if len({h["fastclock_uses_tsc"] for h in headers}) > 1:
        notes.append("rl_latency runs disagree on whether FastClock uses the TSC")

    tpn = statistics.median(h["ticks_per_ns"] for h in headers) if headers else None
    overhead = statistics.median(h["overhead_ns"] for h in headers) if headers else None
    meta = started
    meta["load"]["loadavg_1m_end"] = ended["load"]["loadavg_1m_start"]
    meta["load"]["cpu_busy_pct_end"] = ended["load"]["cpu_busy_pct_start"]
    meta.update({
        "session_date": args.session_date, "tag": args.tag, "source": source, "toolchain": toolchain,
        "library": {"fastclock_uses_tsc": headers[0]["fastclock_uses_tsc"] if headers else None,
                    # bench_core.cpp compiles its operator new replacement under __GLIBC__
                    "heap_accounting_wrapper": platform.libc_ver()[0] == "glibc",
                    **fetched_versions(),
                    "gbench_library_build_type": gb.get("context", {}).get("library_build_type")},
        "instrumentation": ("glibc-heap-accounting (operator new replacement in rl_bench; counters only in BM_MemoryPerKey)"
                            if linux else "none"),
        "timer": {"name": "TSC: rdtsc/rdtscp fenced with lfence (rl_latency); std::chrono steady clock (gbench real_time)",
                  "ticks_per_ns": tpn, "overhead_ns": overhead,
                  "resolution_ns": (1.0 / tpn) if tpn else 1.0,
                  "overhead_note": "median cost of an empty timed region, already subtracted from every rl_latency sample",
                  "source": "rl_latency first line, median over runs" if headers else "no rl_latency output; resolution not measured"},
        "validity": {"valid": not reasons, "reasons": reasons, "runs": runs, "tests": tests},
        "power_end": ended["power"], "date_utc_end": ended["date_utc"],
        "notes": notes,
    })
    meta["raw_files"] = benchmeta.raw_file_entries(env_dir)
    benchmeta.write_json(env_dir / "meta.json", meta)
    benchmeta.write_json(env_dir / "summary.json", build_summary(env_dir, meta))
    log("valid" if not reasons else f"INVALID: {'; '.join(reasons)}")
    print(env_dir.as_posix())
    return 0


if __name__ == "__main__":
    sys.exit(main())
