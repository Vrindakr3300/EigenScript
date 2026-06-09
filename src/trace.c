/* ================================================================
 * EigenScript Trace — implementation (Phase 1 + 2).
 * ================================================================
 * Tape format (text, one record per line):
 *   L <line>                    source-line event
 *   A <name>=<value>            name-keyed assignment delta
 *   N <fn>=<value>              nondeterministic builtin return
 *
 * Value encoding:
 *   <num>          numeric (immediate, tracked, or heap VAL_NUM)
 *   null           VAL_NULL / immediate null slot
 *   true|false
 *   "<str…>"       heap string, content truncated at TRACE_STR_MAX
 *   <list:N>       heap list of length N (content in Phase 2.5)
 *   <dict:N>       heap dict of size N
 *   <fn>           heap function/builtin
 *   <buffer:N>     heap buffer of length N
 *   <heap>         other heap types (fallback)
 *
 * Phase 2 also dedupes adjacent identical L events that have no A/N
 * between them — the compiler emits per-statement LINEs and bare
 * repeats are pure noise. An A or N event resets the dirty bit so the
 * next L always fires after progress was made.
 */

#include "eigenscript.h"
#include "trace.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACE_STR_MAX     60       /* truncate strings in `A` events */
#define TRACE_NONDET_MAX  65536    /* per-record cap for `N` events;
                                    * over-cap payloads emit a marker so
                                    * tape stays sized for visual debug */

int g_trace_enabled = 0;
int g_replay_enabled = 0;

/* ----- Phase 3.0a: prev-value table.
 *
 * Open-addressing hash table keyed by interned name pointer. Each entry
 * holds the slot most recently assigned to that name (`current`) and
 * the one immediately before it (`prev`). `prev of x` reads `prev`.
 *
 * Refcount discipline: stored heap/tracked slots are incref'd on store
 * and decref'd on displacement or shutdown. Immediate slots (number,
 * null, bool) are no-ops for slot_incref/decref, so the same code path
 * is correct for every Value shape.
 *
 * The table runs unconditionally — `prev of x` must work whether or
 * not EIGS_TRACE is set, because it's a language-level interrogative,
 * not a debug-tape feature. Per-assign cost is ~one cache line read +
 * a pointer-equality compare. */

typedef struct {
    int      line;
    EigsSlot value;
} HistoryEntry;

typedef struct {
    const char    *name;
    EigsSlot       prev;
    EigsSlot       current;
    uint8_t        has_current;
    uint8_t        has_prev;
    HistoryEntry  *history;     /* append-only, indexed by assign order */
    int            hist_count;
    int            hist_cap;
} PrevEntry;

static PrevEntry *g_prev_tab = NULL;
static int        g_prev_cap = 0;   /* power of two */
static int        g_prev_count = 0;

/* Line currently being executed by the VM. Updated by trace_line; used
 * by trace_assign to stamp each history entry. Starts at 0 — assignments
 * before the first OP_LINE (rare; usually pre-main setup) land at 0. */
static int        g_current_line = 0;

#define PREV_INIT_CAP 16
#define PREV_LOAD_NUM 3            /* grow when count*4 >= cap*3  (~75%) */
#define PREV_LOAD_DEN 4

static uint32_t prev_hash_ptr(const char *p) {
    /* Fibonacci hash of the pointer bits; interned strings already
     * provide identity, so this just spreads bits across the table. */
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static PrevEntry *prev_lookup_slot(PrevEntry *tab, int cap, const char *name) {
    uint32_t mask = (uint32_t)(cap - 1);
    uint32_t i = prev_hash_ptr(name) & mask;
    while (tab[i].name && tab[i].name != name) {
        i = (i + 1) & mask;
    }
    return &tab[i];
}

static void prev_grow(void) {
    int new_cap = g_prev_cap ? g_prev_cap * 2 : PREV_INIT_CAP;
    PrevEntry *nt = calloc((size_t)new_cap, sizeof(PrevEntry));
    if (!nt) return;
    for (int i = 0; i < g_prev_cap; i++) {
        PrevEntry *e = &g_prev_tab[i];
        if (!e->name) continue;
        PrevEntry *dst = prev_lookup_slot(nt, new_cap, e->name);
        *dst = *e;
    }
    free(g_prev_tab);
    g_prev_tab = nt;
    g_prev_cap = new_cap;
}

static void prev_record_assign(const char *name, EigsSlot value) {
    if (!name) return;
    if (g_prev_count * PREV_LOAD_DEN >= g_prev_cap * PREV_LOAD_NUM) {
        prev_grow();
        if (!g_prev_tab) return;
    }
    PrevEntry *e = prev_lookup_slot(g_prev_tab, g_prev_cap, name);
    if (!e->name) {
        e->name = name;
        g_prev_count++;
    }
    if (e->has_current) {
        /* Shift current -> prev; drop the old prev. */
        if (e->has_prev) slot_decref(e->prev);
        e->prev = e->current;
        e->has_prev = 1;
    }
    slot_incref(value);
    e->current = value;
    e->has_current = 1;

    /* Append to history for `at <line>` queries. Stamp with the
     * current VM line as cached by trace_line. */
    if (e->hist_count >= e->hist_cap) {
        int new_cap = e->hist_cap ? e->hist_cap * 2 : 8;
        HistoryEntry *nh = realloc(e->history, (size_t)new_cap * sizeof(HistoryEntry));
        if (!nh) return;
        e->history = nh;
        e->hist_cap = new_cap;
    }
    slot_incref(value);
    e->history[e->hist_count].line  = g_current_line;
    e->history[e->hist_count].value = value;
    e->hist_count++;
}

int trace_query_prev(const char *interned_name, EigsSlot *out) {
    if (!interned_name || !out || !g_prev_tab) return 0;
    PrevEntry *e = prev_lookup_slot(g_prev_tab, g_prev_cap, interned_name);
    if (!e->name || !e->has_prev) return 0;
    *out = e->prev;
    slot_incref(*out);
    return 1;
}

/* Walk history backward — entries are appended in execution order, so
 * the array is monotone-ish but not strictly sorted (a backward jump
 * could in principle re-execute earlier lines; the latest such write
 * is the answer, which is exactly what backward scan from the end
 * gives us). */
static int find_hist_idx_at_or_before(PrevEntry *e, int line) {
    for (int i = e->hist_count - 1; i >= 0; i--) {
        if (e->history[i].line <= line) return i;
    }
    return -1;
}

int trace_query_at(int kind, const char *interned_name, int line, EigsSlot *out) {
    if (!interned_name || !out || !g_prev_tab) return 0;
    PrevEntry *e = prev_lookup_slot(g_prev_tab, g_prev_cap, interned_name);
    if (!e->name) return 0;

    if (kind == 1) {
        /* `who is x at L` — binding name is timeless. */
        Value *s = make_str(interned_name);
        *out = slot_from_value(s);
        return 1;
    }

    if (kind == 2) {
        /* `when is x at L` — count of assignments with line ≤ L. */
        int count = 0;
        for (int i = 0; i < e->hist_count; i++) {
            if (e->history[i].line <= line) count++;
        }
        *out = slot_from_num((double)count);
        return 1;
    }

    int idx = find_hist_idx_at_or_before(e, line);
    if (idx < 0) return 0;

    if (kind == 0) {
        /* `what is x at L` — value at most recent assign ≤ L. */
        *out = e->history[idx].value;
        slot_incref(*out);
        return 1;
    }

    if (kind == 6) {
        /* `prev of x at L` — value at the assign immediately preceding
         * the one that produced `x`'s state at L. */
        if (idx < 1) return 0;
        *out = e->history[idx - 1].value;
        slot_incref(*out);
        return 1;
    }

    /* where/why/how require observer history we don't capture yet. */
    return 0;
}

Value *trace_state_at(int line) {
    Value *out = make_dict(g_prev_count > 0 ? g_prev_count : 8);
    if (!out || !g_prev_tab) return out;
    for (int i = 0; i < g_prev_cap; i++) {
        PrevEntry *e = &g_prev_tab[i];
        if (!e->name || e->hist_count == 0) continue;
        int idx = find_hist_idx_at_or_before(e, line);
        if (idx < 0) continue;
        Value *v = slot_to_value(e->history[idx].value);
        dict_set(out, e->name, v);
        val_decref(v);
    }
    return out;
}

static FILE *g_trace_fp = NULL;
static int   g_trace_initialized = 0;

/* Line dedupe state. g_last_line < 0 means "no line written yet, always
 * emit". g_line_dirty == 1 means an A or N event landed since the last
 * L, so the next L is meaningful even if numerically identical. */
static int g_last_line  = -1;
static int g_line_dirty = 0;

/* Forward decl — implementation below; called from trace_init. */
static void trace_replay_init(void);

void trace_init(void) {
    if (g_trace_initialized) return;
    g_trace_initialized = 1;

    /* g_trace_enabled gates the VM hook sites for both the file-tape
     * AND the prev-value map. Prev tracking is a language feature
     * (`prev of x` must work without EIGS_TRACE), so the gate is on
     * unconditionally. The file output is then separately gated on
     * g_trace_fp inside the writers. */
    g_trace_enabled = 1;

    /* Phase 3 — if EIGS_REPLAY is set, open that tape for streaming reads.
     * Done first so trace_init can succeed even if EIGS_TRACE is unset. */
    trace_replay_init();

    const char *path = getenv("EIGS_TRACE");
    if (!path || !*path) return;

    g_trace_fp = fopen(path, "w");
    if (!g_trace_fp) {
        fprintf(stderr, "trace: cannot open EIGS_TRACE=%s: %s\n",
                path, strerror(errno));
        return;
    }
    setvbuf(g_trace_fp, NULL, _IOFBF, 64 * 1024);
}

/* ----- Phase 3: replay reader.
 *
 * Streams an existing tape (written by a prior EIGS_TRACE run) and serves
 * the N records to nondet builtins in order via trace_replay_take. L and
 * A records are skipped — the contract is that nondet outcomes appear in
 * the same order both runs, not that line numbers line up exactly. */

static FILE *g_replay_fp = NULL;
static char *g_replay_line = NULL;     /* growable read buffer */
static size_t g_replay_cap = 0;

static void trace_replay_init(void) {
    const char *path = getenv("EIGS_REPLAY");
    if (!path || !*path) return;
    g_replay_fp = fopen(path, "r");
    if (!g_replay_fp) {
        fprintf(stderr, "trace: cannot open EIGS_REPLAY=%s: %s\n",
                path, strerror(errno));
        return;
    }
    g_replay_enabled = 1;
}

static void replay_shutdown(void) {
    if (g_replay_fp) { fclose(g_replay_fp); g_replay_fp = NULL; }
    free(g_replay_line); g_replay_line = NULL; g_replay_cap = 0;
    g_replay_enabled = 0;
}

/* getline that does not need _GNU_SOURCE — keeps a growable buffer in the
 * file-static slot, reads up to and including the next newline (or EOF).
 * Returns the length (newline NOT included, NUL-terminated) or -1 at EOF. */
static int read_tape_line(void) {
    if (!g_replay_fp) return -1;
    if (g_replay_cap < 256) {
        g_replay_cap = 256;
        g_replay_line = realloc(g_replay_line, g_replay_cap);
        if (!g_replay_line) { g_replay_cap = 0; return -1; }
    }
    size_t len = 0;
    int c;
    while ((c = fgetc(g_replay_fp)) != EOF) {
        if (len + 2 > g_replay_cap) {
            size_t nc = g_replay_cap * 2;
            char *nb = realloc(g_replay_line, nc);
            if (!nb) return -1;
            g_replay_line = nb; g_replay_cap = nc;
        }
        if (c == '\n') break;
        g_replay_line[len++] = (char)c;
    }
    if (c == EOF && len == 0) return -1;
    g_replay_line[len] = '\0';
    return (int)len;
}

/* Un-escape a tape-format quoted string in place. Reads the byte stream
 * starting at `*p` (which must point one past the opening quote), writes
 * the decoded bytes to `out` (max `out_cap-1` plus NUL), advances `*p`
 * past the closing quote. Returns the decoded length on success, -1 on
 * malformed input. Handles \", \\, \n, \r, \xNN; everything else literal. */
static int unescape_string(const char **p, char *out, int out_cap) {
    int n = 0;
    const char *s = *p;
    while (*s && *s != '"') {
        if (n + 1 >= out_cap) return -1;
        if (*s == '\\') {
            s++;
            switch (*s) {
                case '"':  out[n++] = '"';  s++; break;
                case '\\': out[n++] = '\\'; s++; break;
                case 'n':  out[n++] = '\n'; s++; break;
                case 'r':  out[n++] = '\r'; s++; break;
                case 'x': {
                    s++;
                    int hi = -1, lo = -1;
                    if (s[0]) hi = (s[0] >= '0' && s[0] <= '9') ? s[0] - '0'
                                  : (s[0] >= 'a' && s[0] <= 'f') ? s[0] - 'a' + 10
                                  : (s[0] >= 'A' && s[0] <= 'F') ? s[0] - 'A' + 10 : -1;
                    if (hi >= 0 && s[1]) lo = (s[1] >= '0' && s[1] <= '9') ? s[1] - '0'
                                  : (s[1] >= 'a' && s[1] <= 'f') ? s[1] - 'a' + 10
                                  : (s[1] >= 'A' && s[1] <= 'F') ? s[1] - 'A' + 10 : -1;
                    if (hi < 0 || lo < 0) return -1;
                    out[n++] = (char)((hi << 4) | lo);
                    s += 2;
                    break;
                }
                default: return -1;
            }
        } else {
            out[n++] = *s++;
        }
    }
    if (*s != '"') return -1;
    *p = s + 1;
    out[n] = '\0';
    return n;
}

/* Parse one value from the tape encoding. Supports the primitive shapes
 * (num, null, true/false, "string"). Lists/dicts/buffers/heap markers are
 * not yet replayable — return NULL so the caller falls back to the real
 * source. */
static Value *parse_value(const char *s) {
    while (*s == ' ') s++;
    if (!*s) return NULL;
    if (strcmp(s, "null") == 0) return make_null();
    if (strcmp(s, "true") == 0) return make_num(1.0);
    if (strcmp(s, "false") == 0) return make_num(0.0);
    if (*s == '"') {
        const char *p = s + 1;
        /* Worst case the decoded string is the same length as the
         * encoded body (escapes only shrink). */
        size_t cap = strlen(p) + 1;
        char *buf = malloc(cap);
        if (!buf) return NULL;
        int n = unescape_string(&p, buf, (int)cap);
        if (n < 0) { free(buf); return NULL; }
        Value *v = make_str(buf);
        free(buf);
        return v;
    }
    /* Numeric: strtod must consume the whole token. */
    char *end = NULL;
    double d = strtod(s, &end);
    if (end == s) return NULL;
    while (*end == ' ') end++;
    if (*end != '\0') return NULL;
    return make_num(d);
}

int trace_replay_take(const char *fn, Value **out) {
    if (!g_replay_fp || !out) return 0;
    for (;;) {
        int len = read_tape_line();
        if (len < 0) {
            /* Tape exhausted — turn off replay so future calls skip the
             * read overhead, and let the builtin run normally. */
            replay_shutdown();
            return 0;
        }
        if (len < 2 || g_replay_line[0] != 'N' || g_replay_line[1] != ' ')
            continue;  /* skip L, A, blanks, anything else */

        char *p = g_replay_line + 2;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *rec_name = p;
        const char *rec_val  = eq + 1;

        if (fn && strcmp(rec_name, fn) != 0) {
            fprintf(stderr, "trace: replay name mismatch — expected '%s', got '%s' (using anyway)\n",
                    fn, rec_name);
        }
        Value *v = parse_value(rec_val);
        if (!v) return 0;   /* unparseable — fall through to live source */
        *out = v;
        return 1;
    }
}

void trace_shutdown(void) {
    if (g_trace_fp) {
        fflush(g_trace_fp);
        fclose(g_trace_fp);
        g_trace_fp = NULL;
    }
    g_trace_enabled = 0;

    if (g_prev_tab) {
        for (int i = 0; i < g_prev_cap; i++) {
            PrevEntry *e = &g_prev_tab[i];
            if (!e->name) continue;
            if (e->has_prev)    slot_decref(e->prev);
            if (e->has_current) slot_decref(e->current);
            for (int j = 0; j < e->hist_count; j++)
                slot_decref(e->history[j].value);
            free(e->history);
        }
        free(g_prev_tab);
        g_prev_tab = NULL;
        g_prev_cap = 0;
        g_prev_count = 0;
    }

    replay_shutdown();
}

void trace_line(int line) {
    /* Cache outside the fp gate — `at <line>` history stamping needs
     * the current line regardless of whether the tape is open. */
    g_current_line = line;

    if (!g_trace_fp) return;
    if (line == g_last_line && !g_line_dirty) return;
    fprintf(g_trace_fp, "L %d\n", line);
    g_last_line  = line;
    g_line_dirty = 0;
}

/* Strings get quoted + truncated. \, ", \n, \r escaped so the tape is
 * one event per text line. Truncation marker is a trailing '…' (UTF-8
 * ellipsis) inside the closing quote. */
static void write_string(const char *s) {
    if (!s) { fputs("\"\"", g_trace_fp); return; }
    fputc('"', g_trace_fp);
    int written = 0;
    for (const char *p = s; *p && written < TRACE_STR_MAX; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', g_trace_fp); fputc(c, g_trace_fp); written += 2; }
        else if (c == '\n')        { fputs("\\n", g_trace_fp); written += 2; }
        else if (c == '\r')        { fputs("\\r", g_trace_fp); written += 2; }
        else                       { fputc(c, g_trace_fp);    written += 1; }
    }
    if ((int)strlen(s) > TRACE_STR_MAX) fputs("…", g_trace_fp);
    fputc('"', g_trace_fp);
}

/* Format a heap or tracked Value*. Numeric heap values unwrap to their
 * number; collections show their size for at-a-glance scanning. */
static void write_value_ptr(Value *v) {
    if (!v) { fputs("null", g_trace_fp); return; }
    switch (v->type) {
        case VAL_NUM:          fprintf(g_trace_fp, "%.17g", v->data.num); break;
        case VAL_NULL:         fputs("null", g_trace_fp); break;
        case VAL_STR:          write_string(v->data.str); break;
        case VAL_LIST:         fprintf(g_trace_fp, "<list:%d>", v->data.list.count); break;
        case VAL_DICT:         fprintf(g_trace_fp, "<dict:%d>", v->data.dict.count); break;
        case VAL_FN:           fputs("<fn>", g_trace_fp); break;
        case VAL_BUILTIN:      fputs("<builtin>", g_trace_fp); break;
        case VAL_BUFFER:       fprintf(g_trace_fp, "<buffer:%d>", v->data.buffer.count); break;
        case VAL_JSON_RAW:     fputs("<json>", g_trace_fp); break;
        case VAL_TEXT_BUILDER: fputs("<text>", g_trace_fp); break;
        default:               fputs("<heap>", g_trace_fp); break;
    }
}

static void write_slot(EigsSlot s) {
    if (slot_is_num(s))  { fprintf(g_trace_fp, "%.17g", s.d); return; }
    if (slot_is_null(s)) { fputs("null", g_trace_fp); return; }
    if (slot_is_bool(s)) { fputs(slot_as_bool(s) ? "true" : "false", g_trace_fp); return; }
    if (slot_is_tracked(s) || slot_is_heap(s)) { write_value_ptr(slot_as_ptr(s)); return; }
    fputs("<unknown>", g_trace_fp);
}

void trace_assign(const char *name, EigsSlot value) {
    /* Prev-map update runs regardless of EIGS_TRACE — `prev of x` is a
     * language feature, not a tape feature. The file write below is
     * still gated on the tape being open. */
    prev_record_assign(name, value);

    if (!g_trace_fp) return;
    if (!name) name = "?";
    fputs("A ", g_trace_fp);
    fputs(name, g_trace_fp);
    fputc('=', g_trace_fp);
    write_slot(value);
    fputc('\n', g_trace_fp);
    g_line_dirty = 1;
}

/* ----- Full-fidelity writer for nondet records.
 *
 * Recursive emission of lists/dicts; full string content; cap the whole
 * record at TRACE_NONDET_MAX bytes. The `budget` is decremented on every
 * byte written. When budget runs out, the rest of the record collapses
 * to a single "…<truncated:RESIDUAL>" marker so Phase 3 replay knows the
 * record cannot be used for full determinism. */

static void wf_putc(int c, int *budget) {
    if (*budget <= 0) return;
    fputc(c, g_trace_fp); (*budget)--;
}
static void wf_puts(const char *s, int *budget) {
    while (*s && *budget > 0) { fputc(*s++, g_trace_fp); (*budget)--; }
}
static void wf_printf(int *budget, const char *fmt, ...) {
    if (*budget <= 0) return;
    char buf[64];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > *budget) n = *budget;
    fwrite(buf, 1, n, g_trace_fp);
    *budget -= n;
}

static void write_string_full(const char *s, int *budget) {
    wf_putc('"', budget);
    if (s) {
        for (const char *p = s; *p && *budget > 0; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { wf_putc('\\', budget); wf_putc(c, budget); }
            else if (c == '\n')        { wf_puts("\\n", budget); }
            else if (c == '\r')        { wf_puts("\\r", budget); }
            else if (c < 0x20)         { wf_printf(budget, "\\x%02x", c); }
            else                       { wf_putc(c, budget); }
        }
    }
    wf_putc('"', budget);
}

static void write_value_ptr_full(Value *v, int *budget) {
    if (*budget <= 0) return;
    if (!v) { wf_puts("null", budget); return; }
    switch (v->type) {
        case VAL_NUM:  wf_printf(budget, "%.17g", v->data.num); break;
        case VAL_NULL: wf_puts("null", budget); break;
        case VAL_STR:  write_string_full(v->data.str, budget); break;
        case VAL_LIST: {
            wf_putc('[', budget);
            int n = v->data.list.count;
            for (int i = 0; i < n && *budget > 0; i++) {
                if (i) wf_puts(", ", budget);
                write_value_ptr_full(v->data.list.items[i], budget);
            }
            wf_putc(']', budget);
            break;
        }
        case VAL_DICT: {
            wf_putc('{', budget);
            int n = v->data.dict.count;
            for (int i = 0; i < n && *budget > 0; i++) {
                if (i) wf_puts(", ", budget);
                write_string_full(v->data.dict.keys[i], budget);
                wf_puts(": ", budget);
                write_value_ptr_full(v->data.dict.vals[i], budget);
            }
            wf_putc('}', budget);
            break;
        }
        case VAL_BUFFER: {
            wf_putc('[', budget);
            int n = v->data.buffer.count;
            for (int i = 0; i < n && *budget > 0; i++) {
                if (i) wf_puts(", ", budget);
                wf_printf(budget, "%.17g", v->data.buffer.data[i]);
            }
            wf_putc(']', budget);
            break;
        }
        case VAL_FN:       wf_puts("<fn>", budget); break;
        case VAL_BUILTIN:  wf_puts("<builtin>", budget); break;
        default:           wf_puts("<heap>", budget); break;
    }
}

void trace_nondet_value(const char *fn, Value *v) {
    if (!g_trace_fp) return;
    if (!fn) fn = "?";
    fputs("N ", g_trace_fp);
    fputs(fn, g_trace_fp);
    fputc('=', g_trace_fp);
    int budget = TRACE_NONDET_MAX;
    write_value_ptr_full(v, &budget);
    if (budget <= 0) fprintf(g_trace_fp, "…<truncated>");
    fputc('\n', g_trace_fp);
    g_line_dirty = 1;
}
