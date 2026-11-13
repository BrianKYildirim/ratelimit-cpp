/* Replays a canonical parity trace (tests/parity/README.md) through the C API
   on one thread, in trace order, and compares the outcome lines with a golden
   file byte for byte, or writes that golden file with --write. This runner is
   the only writer of golden files; run_wasm.mjs only compares.
   Usage: rl_parity_native [--write] <trace.jsonl> <golden.out>
   Exit: 0 equal (or written), 1 golden mismatch, 2 usage or trace error. */
#include <ratelimit/ratelimit_c.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEY_BYTES 256
#define MAX_LINE 1024 /* the longest canonical line is 617 bytes */

/* ---------- Files and buffers ---------- */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} buf_t;

static int buf_printf(buf_t* b, const char* fmt, ...) {
    char tmp[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) return 0;
    if (b->len + (size_t)n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        char* p;
        while (cap < b->len + (size_t)n) cap *= 2;
        p = (char*)realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, tmp, (size_t)n);
    b->len += (size_t)n;
    return 1;
}

/* Reads a whole file in binary mode; NULL on any error. */
static char* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    buf_t b = {NULL, 0, 0};
    char chunk[4096];
    size_t n;
    if (!f) return NULL;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (b.len + n > b.cap) {
            size_t cap = b.cap ? b.cap * 2 : 65536;
            char* p;
            while (cap < b.len + n) cap *= 2;
            p = (char*)realloc(b.data, cap);
            if (!p) {
                free(b.data);
                fclose(f);
                return NULL;
            }
            b.data = p;
            b.cap = cap;
        }
        memcpy(b.data + b.len, chunk, n);
        b.len += n;
    }
    if (ferror(f)) {
        free(b.data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (!b.data) b.data = (char*)malloc(1); /* an empty file is still a successful read */
    *len = b.len;
    return b.data;
}

static int trace_error(size_t line, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "trace line %lu: ", (unsigned long)line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 2;
}

/* ---------- Trace parsing (canonical lines only) ---------- */

static int hex_value(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
}

/* Parses the first line: {"op":"new","shards":S,"hint":H}. */
static int parse_new(const char* line, size_t len, size_t n, unsigned* shards, unsigned* hint) {
    uint64_t s = 0, h = 0;
    int end = -1;
    char again[MAX_LINE + 1];
    if (sscanf(line, "{\"op\":\"new\",\"shards\":%" SCNu64 ",\"hint\":%" SCNu64 "}%n", &s, &h, &end) != 2 ||
        end < 0 || (size_t)end != len)
        return trace_error(n, "the first line must be the canonical \"new\" op");
    if (s > 0xFFFFFFFFu || h > 0xFFFFFFFFu) return trace_error(n, "shards and hint must fit in 32 bits");
    snprintf(again, sizeof again, "{\"op\":\"new\",\"shards\":%" PRIu64 ",\"hint\":%" PRIu64 "}", s, h);
    if (strcmp(again, line) != 0) return trace_error(n, "does not re-serialize to itself");
    *shards = (unsigned)s;
    *hint = (unsigned)h;
    return 0;
}

typedef struct {
    int inspect; /* 0 = allow, 1 = inspect */
    char key[MAX_KEY_BYTES + 1];
    rl_limit_t limit;
    uint64_t t;
} op_t;

/* Parses {"op":"allow"|"inspect","key":"<hex>","cap":C,"rate":R,"t":T}. */
static int parse_op(const char* line, size_t len, size_t n, op_t* op) {
    char name[8], hex[2 * MAX_KEY_BYTES + 2], again[MAX_LINE + 1];
    uint64_t cap = 0, rate = 0, t = 0;
    int end = -1;
    size_t i, hex_len;
    hex[0] = '\0';
    if (sscanf(line, "{\"op\":\"%7[a-z]\",\"key\":\"%513[0-9a-f]\",\"cap\":%" SCNu64 ",\"rate\":%" SCNu64
                     ",\"t\":%" SCNu64 "}%n", name, hex, &cap, &rate, &t, &end) != 5 ||
        end < 0 || (size_t)end != len) {
        end = -1; /* %[ needs at least one character, so the empty key has its own pattern */
        hex[0] = '\0';
        if (sscanf(line, "{\"op\":\"%7[a-z]\",\"key\":\"\",\"cap\":%" SCNu64 ",\"rate\":%" SCNu64
                         ",\"t\":%" SCNu64 "}%n", name, &cap, &rate, &t, &end) != 4 ||
            end < 0 || (size_t)end != len) {
            if (strncmp(line, "{\"op\":\"new\"", 11) == 0)
                return trace_error(n, "\"new\" may appear only once, on the first line");
            return trace_error(n, "not a canonical allow or inspect op");
        }
    }
    if (strcmp(name, "allow") == 0) op->inspect = 0;
    else if (strcmp(name, "inspect") == 0) op->inspect = 1;
    else if (strcmp(name, "new") == 0) return trace_error(n, "\"new\" may appear only once, on the first line");
    else return trace_error(n, "unknown op \"%s\"", name);
    hex_len = strlen(hex);
    if (hex_len % 2 != 0) return trace_error(n, "the key must be an even number of hex digits");
    if (hex_len > 2 * MAX_KEY_BYTES) return trace_error(n, "the key is longer than %d bytes", MAX_KEY_BYTES);
    snprintf(again, sizeof again, "{\"op\":\"%s\",\"key\":\"%s\",\"cap\":%" PRIu64 ",\"rate\":%" PRIu64 ",\"t\":%" PRIu64 "}",
             name, hex, cap, rate, t);
    if (strcmp(again, line) != 0) return trace_error(n, "does not re-serialize to itself");
    for (i = 0; i < hex_len / 2; ++i) op->key[i] = (char)(hex_value(hex[2 * i]) * 16 + hex_value(hex[2 * i + 1]));
    op->key[hex_len / 2] = '\0'; /* the C API reads the key up to its first NUL */
    op->limit.capacity = cap;
    op->limit.refill_per_sec = rate;
    op->t = t;
    return 0;
}

/* ---------- Replay ---------- */

/* Runs the trace into out; returns 0, or 2 after printing a trace error. */
static int run_trace(const char* text, size_t len, buf_t* out) {
    void* h = NULL;
    size_t pos = 0, n = 0;
    const char* cr = (const char*)memchr(text, '\r', len);
    int rc = 0;
    if (cr) {
        size_t line = 1, i;
        for (i = 0; i < (size_t)(cr - text); ++i) line += text[i] == '\n';
        return trace_error(line, "carriage return (CR) is not allowed");
    }
    if (len == 0) return trace_error(1, "the trace is empty; the first line must be the \"new\" op");
    if (text[len - 1] != '\n') {
        size_t line = 1, i;
        for (i = 0; i < len; ++i) line += text[i] == '\n';
        return trace_error(line, "missing final newline");
    }
    while (pos < len) {
        const char* nl = (const char*)memchr(text + pos, '\n', len - pos);
        size_t line_len = (size_t)(nl - (text + pos));
        char line[MAX_LINE + 1];
        ++n;
        if (line_len > MAX_LINE) {
            rc = trace_error(n, "longer than %d bytes", MAX_LINE);
            break;
        }
        memcpy(line, text + pos, line_len);
        line[line_len] = '\0';
        pos += line_len + 1;
        if (memchr(line, '\0', line_len)) {
            rc = trace_error(n, "NUL byte in the line");
            break;
        }
        if (n == 1) {
            unsigned shards = 0, hint = 0;
            if ((rc = parse_new(line, line_len, n, &shards, &hint)) != 0) break;
            h = rl_new_inproc(shards, hint);
            if (!h) {
                fprintf(stderr, "rl_new_inproc failed\n");
                return 2;
            }
            if (!buf_printf(out, "N %s\n", rl_version())) {
                rc = trace_error(n, "out of memory");
                break;
            }
        } else {
            op_t op;
            int ok;
            if ((rc = parse_op(line, line_len, n, &op)) != 0) break;
            if (!op.inspect) {
                const rl_decision_t d = rl_allow_at(h, op.key, op.limit, op.t);
                ok = buf_printf(out, "A %u %" PRIu64 " %" PRIu64 "\n", (unsigned)d.allowed, d.remaining, d.reset_ms);
            } else {
                const rl_bucket_state_t s = rl_inspect_at(h, op.key, op.limit, op.t);
                ok = buf_printf(out, "I %u %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                                (unsigned)s.tracked, (unsigned)s.would_allow, s.stored_nanotokens, s.stored_at_ns,
                                s.nanotokens, s.tokens, s.until_token_ns, s.until_full_ns);
            }
            if (!ok) {
                rc = trace_error(n, "out of memory");
                break;
            }
        }
    }
    if (h) rl_free(h);
    return rc;
}

/* ---------- Compare ---------- */

/* Length of the line starting at p, without its LF; *next is where the next line starts. */
static size_t line_at(const char* p, const char* end, const char** next) {
    const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
    if (!nl) {
        *next = end;
        return (size_t)(end - p);
    }
    *next = nl + 1;
    return (size_t)(nl - p);
}

/* Prints the first line where golden and produced output differ; returns 1, or 0 if they are equal. */
static int compare(const char* golden, size_t glen, const buf_t* out) {
    const char *g = golden, *gend = golden + glen, *p = out->data, *pend = out->data + out->len;
    size_t n;
    if (glen == out->len && (glen == 0 || memcmp(golden, out->data, glen) == 0)) return 0;
    for (n = 1;; ++n) {
        const char *gnext = gend, *pnext = pend;
        const int gdone = g >= gend, pdone = p >= pend;
        const size_t gl = gdone ? 0 : line_at(g, gend, &gnext), pl = pdone ? 0 : line_at(p, pend, &pnext);
        if (gdone || pdone || gl != pl || memcmp(g, p, gl) != 0) {
            fprintf(stderr, "line %lu: expected %.*s got %.*s\n", (unsigned long)n, (int)(gdone ? 13 : gl),
                    gdone ? "<end of file>" : g, (int)(pdone ? 13 : pl), pdone ? "<end of file>" : p);
            return 1;
        }
        if ((gnext[-1] == '\n') != (pnext[-1] == '\n')) {
            fprintf(stderr, "line %lu: expected %.*s got %.*s (only one of them ends in a newline)\n", (unsigned long)n,
                    (int)gl, g, (int)pl, p);
            return 1;
        }
        g = gnext;
        p = pnext;
    }
}

int main(int argc, char** argv) {
    const int write_mode = argc == 4 && strcmp(argv[1], "--write") == 0;
    const char *trace_path, *golden_path;
    char *text, *golden;
    size_t len = 0, glen = 0;
    buf_t out = {NULL, 0, 0};
    int rc;
    if (!((argc == 3 && strcmp(argv[1], "--write") != 0) || write_mode)) {
        fprintf(stderr, "usage: rl_parity_native [--write] <trace.jsonl> <golden.out>\n");
        return 2;
    }
    trace_path = argv[write_mode ? 2 : 1];
    golden_path = argv[write_mode ? 3 : 2];
    if (!(text = read_file(trace_path, &len))) {
        fprintf(stderr, "cannot read %s\n", trace_path);
        return 2;
    }
    rc = run_trace(text, len, &out);
    free(text);
    if (rc != 0) {
        free(out.data);
        return rc;
    }
    if (write_mode) {
        /* "wb": no CRLF translation on Windows, so every toolchain writes the same bytes. */
        FILE* f = fopen(golden_path, "wb");
        int ok = f != NULL;
        if (ok) ok = fwrite(out.data, 1, out.len, f) == out.len;
        if (f && fclose(f) != 0) ok = 0;
        if (!ok) {
            fprintf(stderr, "cannot write %s\n", golden_path);
            free(out.data);
            return 2;
        }
        printf("wrote %s\n", golden_path);
        free(out.data);
        return 0;
    }
    if (!(golden = read_file(golden_path, &glen))) {
        fprintf(stderr, "cannot read %s\n", golden_path);
        free(out.data);
        return 2;
    }
    rc = compare(golden, glen, &out);
    if (rc == 0) printf("parity ok: %s\n", trace_path);
    free(golden);
    free(out.data);
    return rc;
}
