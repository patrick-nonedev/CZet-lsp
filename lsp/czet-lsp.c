/* SPDX-License-Identifier: GPL-3.0-or-later */
/* czet-lsp — mini Language Server para CZet (C11, solo libc, single-file).
 *
 * Compilar:
 *   cc -O2 -std=c11 -Wall -o czet-lsp czet-lsp.c
 *   # o con el propio driver:
 *   czet -static czet-lsp.c -o czet-lsp
 *
 * Uso (stdio, como cualquier LSP):
 *   czet-lsp [--log=/tmp/czet-lsp.log] [--no-czet] [--help]
 *
 *   --log=PATH  vuelca tráfico y errores ahí (por defecto sin log).
 *   --no-czet   no invoca al driver `czet` para diagnósticos reales;
 *               solo usa los chequeos internos (balance + reglas CZet).
 *   --help      esta ayuda.
 *
 * Lo que hace (mínimo usable):
 *   initialize / initialized / shutdown / exit
 *   textDocument/didOpen / didChange / didClose  (sync completa)
 *   textDocument/completion  (keywords CZet + C + <utils/...> + APIs)
 *   textDocument/hover       (docs cortas de cada keyword CZet)
 *   textDocument/definition  (búsqueda naive en el mismo fichero)
 *   textDocument/documentSymbol (funcs, structs, unions, enums, _Macro, _Operator)
 *   textDocument/publishDiagnostics (balance + reglas + `czet -fsyntax-only` si existe)
 *
 * Protocolo: JSON-RPC 2.0 sobre stdio con cabeceras `Content-Length`.
 * El parseo JSON es intencionalmente mínimo (búsqueda de claves + unescape),
 * suficiente para lo que envían vim/neovim/VSCode.
 *
 * Extensiones: .ct (preferida), .czet (aceptada), .c plano con -std=czet.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>

/* ============================ config ============================ */

#define CZET_LSP_VERSION "0.1.0"
#define MAX_DOCS 64
#define MAX_DOC_BYTES (1u << 20) /* 1 MiB por fichero abierto */
#define MAX_DIAG 100
#define MAX_SYMBOLS 512

static const char *g_log_path = NULL;
static FILE *g_log = NULL;
static int g_use_czet = 1;

static void lsp_log(const char *fmt, ...)
{
    if (!g_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

/* ============================ docs ============================ */

typedef struct {
    int open;
    int version;
    char uri[1024];
    char *text;      /* NUL-terminated */
    size_t len;
} Doc;

static Doc g_docs[MAX_DOCS];

static Doc *doc_find(const char *uri)
{
    for (int i = 0; i < MAX_DOCS; i++)
        if (g_docs[i].open && strcmp(g_docs[i].uri, uri) == 0)
            return &g_docs[i];
    return NULL;
}

static Doc *doc_get_or_create(const char *uri)
{
    Doc *d = doc_find(uri);
    if (d)
        return d;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (!g_docs[i].open) {
            g_docs[i].open = 1;
            g_docs[i].version = 0;
            snprintf(g_docs[i].uri, sizeof g_docs[i].uri, "%s", uri);
            g_docs[i].text = NULL;
            g_docs[i].len = 0;
            return &g_docs[i];
        }
    }
    return NULL;
}

static void doc_set_text(Doc *d, const char *s, size_t n)
{
    if (n > MAX_DOC_BYTES)
        n = MAX_DOC_BYTES;
    free(d->text);
    d->text = (char *)malloc(n + 1);
    if (!d->text) {
        d->len = 0;
        return;
    }
    memcpy(d->text, s, n);
    d->text[n] = '\0';
    d->len = n;
}

/* ============================ JSON utils ============================ */

/* Devuelve malloc'd con el string escapado para JSON. */
static char *json_escape(const char *s)
{
    size_t n = strlen(s);
    char *o = (char *)malloc(n * 6 + 1);
    if (!o)
        return NULL;
    char *p = o;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  *p++ = '\\'; *p++ = '"'; break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        case '\t': *p++ = '\\'; *p++ = 't'; break;
        default:
            if (c < 0x20) {
                sprintf(p, "\\u%04x", c);
                p += 6;
            } else {
                *p++ = (char)c;
            }
            break;
        }
    }
    *p = '\0';
    return o;
}

/* Unescape JSON string src (sin las comillas) en dst (cap dstsz). */
static void json_unescape_to(const char *src, char *dst, size_t dstsz)
{
    size_t di = 0;
    for (const char *s = src; *s && di + 1 < dstsz; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': dst[di++] = '\n'; break;
            case 'r': dst[di++] = '\r'; break;
            case 't': dst[di++] = '\t'; break;
            case 'b': dst[di++] = '\b'; break;
            case 'f': dst[di++] = '\f'; break;
            case '"': dst[di++] = '"'; break;
            case '\\': dst[di++] = '\\'; break;
            case '/': dst[di++] = '/'; break;
            case 'u':
                /* \uXXXX mínimo: solo ASCII, resto -> '?' */
                if (isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2]) &&
                    isxdigit((unsigned char)s[3]) && isxdigit((unsigned char)s[4])) {
                    char h[5] = { s[1], s[2], s[3], s[4], 0 };
                    long v = strtol(h, NULL, 16);
                    if (v > 0 && v < 0x80)
                        dst[di++] = (char)v;
                    else
                        dst[di++] = '?';
                    s += 4;
                } else {
                    dst[di++] = '?';
                }
                break;
            default: dst[di++] = *s; break;
            }
        } else {
            dst[di++] = *s;
        }
    }
    dst[di] = '\0';
}

/* Localiza el valor de "key" (con comillas) y devuelve puntero al valor
 * (tras los ':' y espacios). NULL si no existe. */
static const char *js_val(const char *json, const char *key)
{
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

/* Extrae string JSON de una clave a out. 0 ok, -1 falta. */
static int js_str(const char *json, const char *key, char *out, size_t outsz)
{
    const char *p = js_val(json, key);
    if (!p || *p != '"')
        return -1;
    p++;
    /* buscar cierre no escapado */
    const char *e = p;
    while (*e) {
        if (*e == '\\') {
            e += 2;
            continue;
        }
        if (*e == '"')
            break;
        e++;
    }
    size_t raw = (size_t)(e - p);
    char *tmp = (char *)malloc(raw + 1);
    if (!tmp)
        return -1;
    memcpy(tmp, p, raw);
    tmp[raw] = '\0';
    json_unescape_to(tmp, out, outsz);
    free(tmp);
    return 0;
}

/* Extrae entero de una clave. 0 ok, -1 falta. */
static int js_int(const char *json, const char *key, long *out)
{
    const char *p = js_val(json, key);
    if (!p)
        return -1;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p)
        return -1;
    *out = v;
    return 0;
}

/* Extrae el id en crudo (número, "string" o null) para reenviarlo tal cual.
 * Devuelve malloc'd (hay que liberarlo). Si no hay id -> NULL. */
static char *js_id_raw(const char *json)
{
    const char *p = js_val(json, "id");
    if (!p)
        return NULL;
    if (*p == '"') {
        const char *e = p + 1;
        while (*e) {
            if (*e == '\\') {
                e += 2;
                continue;
            }
            if (*e == '"') {
                e++;
                break;
            }
            e++;
        }
        size_t n = (size_t)(e - p);
        char *o = (char *)malloc(n + 1);
        if (!o)
            return NULL;
        memcpy(o, p, n);
        o[n] = '\0';
        return o;
    }
    /* número / true / false / null: hasta , } o espacio */
    const char *e = p;
    while (*e && *e != ',' && *e != '}' && !isspace((unsigned char)*e))
        e++;
    size_t n = (size_t)(e - p);
    if (n == 0 || n > 64)
        return NULL;
    char *o = (char *)malloc(n + 1);
    if (!o)
        return NULL;
    memcpy(o, p, n);
    o[n] = '\0';
    return o;
}

/* Extrae uri de params.textDocument.uri (el LSP anida así).
 * Busca la primera "uri" del mensaje: en la práctica es la del documento. */
static int js_uri(char *out, size_t outsz, const char *json)
{
    return js_str(json, "uri", out, outsz);
}

/* Posición params.position.{line,character}. */
static void js_position(const char *json, long *line, long *ch)
{
    *line = 0;
    *ch = 0;
    /* La primera "line"/"character" del body suele ser la del cursor;
     * para diagnostics internos no importa la ambigüedad. */
    js_int(json, "line", line);
    js_int(json, "character", ch);
    if (*line < 0)
        *line = 0;
    if (*ch < 0)
        *ch = 0;
}

/* ============================ RPC framing ============================ */

static int rpc_read(char **body_out)
{
    char line[1024];
    long content_len = -1;
    /* cabeceras */
    for (;;) {
        if (!fgets(line, sizeof line, stdin))
            return -1; /* EOF */
        /* fin de cabeceras: línea vacía */
        if (line[0] == '\r' || line[0] == '\n')
            break;
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            content_len = atol(line + 15);
    }
    if (content_len < 0 || content_len > (long)(8 << 20))
        return -2;
    char *body = (char *)malloc((size_t)content_len + 1);
    if (!body)
        return -2;
    size_t got = 0;
    while (got < (size_t)content_len) {
        size_t r = fread(body + got, 1, (size_t)content_len - got, stdin);
        if (r == 0)
            break;
        got += r;
    }
    if (got != (size_t)content_len) {
        free(body);
        return -1;
    }
    body[content_len] = '\0';
    *body_out = body;
    return 0;
}

static void rpc_send(const char *json)
{
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
    lsp_log("SEND: %s", json);
}

/* ============================ posiciones ============================ */

static size_t offset_of(const char *text, long line, long ch)
{
    size_t off = 0;
    long l = 0;
    size_t len = strlen(text);
    while (off < len && l < line) {
        if (text[off] == '\n')
            l++;
        off++;
    }
    /* misma línea: avanzar ch caracteres (bytes; suficiente para ASCII) */
    long c = 0;
    while (off < len && text[off] != '\n' && c < ch) {
        off++;
        c++;
    }
    return off;
}

static int is_wordc(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '$';
}

/* Palabra bajo el cursor (incluye '_' inicial y '$' de macros). */
static void word_at(const char *text, size_t off, char *out, size_t outsz,
                    size_t *start_off)
{
    size_t len = strlen(text);
    if (off > len)
        off = len;
    size_t s = off, e = off;
    while (s > 0 && is_wordc(text[s - 1]))
        s--;
    while (e < len && is_wordc(text[e]))
        e++;
    size_t n = e - s;
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, text + s, n);
    out[n] = '\0';
    if (start_off)
        *start_off = s;
}

/* Prefijo de la línea hasta character (para completion). */
static void line_prefix(const char *text, long line, long ch,
                        char *out, size_t outsz)
{
    size_t off = offset_of(text, line, ch);
    size_t s = off;
    while (s > 0 && text[s - 1] != '\n')
        s--;
    size_t n = off - s;
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, text + s, n);
    out[n] = '\0';
}

/* Nº de línea (0-based) donde empieza el offset. */
static long line_of(const char *text, size_t off)
{
    long l = 0;
    for (size_t i = 0; i < off && text[i]; i++)
        if (text[i] == '\n')
            l++;
    return l;
}

/* ============================ conocimiento CZet ============================ */

typedef struct {
    const char *label;
    const char *kind;   /* texto humano; el LSP manda kind numérico aparte */
    int lsp_kind;       /* 1 Text, 3 Function, 5 Field, 9 Module, 14 Keyword... */
    const char *detail;
    const char *doc;
} Compl;

/* Keywords CZet + C + fragmentos + stdlib. Suficiente para ser útil. */
static const Compl g_compl[] = {
    /* --- extensiones CZet --- */
    { "_Macro", "keyword", 14, "CZet macro", "Sintaxis nueva estilo macro_rules!:\n`_Macro NOMBRE<$p: frag> { (forma) { expansion } };`" },
    { "_Reflect", "keyword", 14, "CZet reflection", "`reflect_t r = _Reflect(T);` kinds 0..10, campos kind/size/align/name/count/elem/fields." },
    { "_Operator", "keyword", 14, "CZet operator overload", "`T _Operator(Tipo, \"+\") { ... }` con `a`/`b` como punteros." },
    { "defer", "keyword", 14, "CZet defer", "`defer { ... };` al salir del bloque; `defer;` ejecuta ya." },
    { "constexpr", "keyword", 14, "CZet constexpr", "`constexpr T f(...) { ... }` se evalúa en compilación si args const." },
    { "reflect_t", "type", 8, "primitive", "Metadata de tipo: kind/size/align/name/count/elem/fields." },
    { "reflect_field_t", "type", 8, "primitive", "{name, offset, type} por miembro/parámetro." },
    /* --- fragmentos de macro --- */
    { "ident", "fragment", 1, "_Macro fragment", "Un único nombre." },
    { "expr", "fragment", 1, "_Macro fragment", "Expresión balanceada (backtracking longest-first)." },
    { "type", "fragment", 1, "_Macro fragment", "Como expr, pensado para tipos." },
    { "stmt", "fragment", 1, "_Macro fragment", "Run balanceado; puede tragar comas." },
    { "block", "fragment", 1, "_Macro fragment", "Bloque `{ ... }` completo." },
    { "tokens", "fragment", 1, "_Macro fragment", "Run crudo balanceado." },
    { "number", "fragment", 1, "_Macro fragment", "Un literal numérico." },
    { "string", "fragment", 1, "_Macro fragment", "Un literal de cadena." },
    { "literal", "fragment", 1, "_Macro fragment", "Cualquier literal simple." },
    /* --- campos reflect --- */
    { "kind", "field", 5, "reflect_t", "0 other,1 void,2 int,3 float,4 ptr,5 array,6 struct,7 union,8 enum,9 func,10 typedef." },
    { "size", "field", 5, "reflect_t", "sizeof en bytes (0 en void/func)." },
    { "align", "field", 5, "reflect_t", "_Alignof en bytes." },
    { "name", "field", 5, "reflect_t", "Nombre de tipo/tag o \"\"." },
    { "count", "field", 5, "reflect_t", "nº elems/miembros/enums/parámetros." },
    { "elem", "field", 5, "reflect_t", "Tipo elemento/apuntado/subyacente/retorno." },
    { "fields", "field", 5, "reflect_t", "Array de reflect_field_t o NULL." },
    { "offset", "field", 5, "reflect_field_t", "Offset en bytes (o índice de parámetro)." },
    /* --- stdlib headers --- */
    { "utils/arena.h", "header", 9, "stdlib", "Bump allocator: arena_init/alloc/calloc/reset/free." },
    { "utils/str.h", "header", 9, "stdlib", "String builder: str_init/push_cstr/push_fmt/cstr/clear/free." },
    { "utils/slice.h", "header", 9, "stdlib", "Vista struct slice<T>: slice_make/len/at/sub." },
    { "utils/hashmap.h", "header", 9, "stdlib", "Mapa string->void*: hmap_init/put/get/del/count/free." },
    { "utils/btree.h", "header", 9, "stdlib", "Btree ordenado tidwall." },
    { "utils/critbit.h", "header", 9, "stdlib", "Crit-bit trie agl/djb." },
    { "utils/save_memory.h", "header", 9, "stdlib", "Persistencia binaria: sm_save/sm_load (+_r)." },
    { "utils/double_buffering.h", "header", 9, "stdlib", "dbuf_init/write/read SPSC." },
    { "utils/triple_buffering.h", "header", 9, "stdlib", "tbuf_init/write/read SPSC." },
    { "utils/thread_pool.h", "header", 9, "stdlib", "pool_create/submit/try_submit/wait/destroy (necesita -pthread)." },
    { "utils/aco.h", "header", 9, "stdlib", "Corrutinas: co_sched_init/spawn/run/free, co_yield." },
    { "utils/server.h", "header", 9, "stdlib", "TCP mínimo: tcp_listen/connect/accept/send_all/recv_all/port/close." },
    { "utils/ipc.h", "header", 9, "stdlib", "AF_UNIX IPC: ipc_server_open/close, ipc_client_open/close, *_send/recv_all." },
    /* --- APIs más usadas (para completion tras prefijo) --- */
    { "arena_init", "func", 3, "utils/arena.h", "arena_init(&a, cap);" },
    { "arena_alloc", "func", 3, "utils/arena.h", "arena_alloc(&a, bytes, align);" },
    { "str_push_cstr", "func", 3, "utils/str.h", "str_push_cstr(&s, \"...\");" },
    { "slice_make", "func", 3, "utils/slice.h", "slice_make<T>(&s, buf, n);" },
    { "hmap_put", "func", 3, "utils/hashmap.h", "hmap_put(&m, key, val);" },
    { "hmap_get", "func", 3, "utils/hashmap.h", "hmap_get(&m, key);" },
    { "sm_save", "func", 3, "utils/save_memory.h", "sm_save<T>(f, &v);" },
    { "sm_load", "func", 3, "utils/save_memory.h", "sm_load<T>(f, &v);" },
    { "dbuf_write", "func", 3, "utils/double_buffering.h", "dbuf_write<T>(&b, &v);" },
    { "tbuf_write", "func", 3, "utils/triple_buffering.h", "tbuf_write<T>(&b, &v);" },
    { "pool_create", "func", 3, "utils/thread_pool.h", "pool_create(4, 64);" },
    { "pool_submit", "func", 3, "utils/thread_pool.h", "pool_submit(p, fn, arg);" },
    { "co_spawn", "func", 3, "utils/aco.h", "co_spawn(&s, fn, arg, 0);" },
    { "co_yield", "func", 3, "utils/aco.h", "Cede al scheduler round-robin." },
    { "tcp_listen", "func", 3, "utils/server.h", "tcp_listen(host, port, backlog);" },
    { "ipc_server_open", "func", 3, "utils/ipc.h", "ipc_server_open(path, &srv, &cli);" },
    { NULL, NULL, 0, NULL, NULL }
};

typedef struct {
    const char *word;
    const char *md;
} HoverDoc;

static const HoverDoc g_hover[] = {
    { "_Macro", "**`_Macro` — sintaxis nueva** (docs/GUIA_MACROS.md)\n\n```c\n_Macro NOMBRE<$p: fragmento, ...> {\n  (forma A) { expansión A }\n  (forma B) { expansión B }\n};\n```\nFragmentos: `ident expr type stmt block tokens number string literal`. Repetición: `$(...)*|+|?`. Higiene: lo declarado se renombra a `nombre__czet<N>`." },
    { "_Reflect", "**`_Reflect(T)` → `reflect_t`** (docs/GUIA_REFLECT.md)\n\nCampos: `kind size align name count elem fields`. Kinds: 0 other, 1 void, 2 int, 3 float, 4 ptr, 5 array, 6 struct, 7 union, 8 enum, 9 func, 10 typedef. No se evalúa (como `sizeof`). Solo con `-std=czet`." },
    { "_Operator", "**`_Operator` — sobrecarga** (tests/operators.czet)\n\n```c\nstruct Vec2 _Operator(struct Vec2, \"+\") { ...; }\n```\nOperandos `a`/`b` como punteros. Ops: `+ - * / % == != < > <= >= & | ^ << >>`, compuestos `+= ... >>=`, unarios `! ~`, `++ --`, `=`." },
    { "defer", "**`defer`** (tests/defer.czet)\n\n`defer { ... };` ejecuta al salir del bloque (`return/goto/break/continue` incluidos). `defer;` ejecuta ya." },
    { "constexpr", "**`constexpr`** (tests/constexpr.czet)\n\n```c\nconstexpr size_t factorial(int n) { ... }\nconstexpr size_t x = factorial(5);\n```\nCon funciones (más que C23)." },
    { "reflect_t", "**`reflect_t` (primitivo)**\n\n`{int kind; unsigned long size, align; const char *name; unsigned long count; reflect_t *elem; reflect_field_t *fields;}`. Siempre disponible, sin headers." },
    { "reflect_field_t", "**`reflect_field_t` (primitivo)**\n\n`{const char *name; unsigned long offset; reflect_t *type;}`." },
    { "struct", "CZet: `struct box<T,U> { T a; U b; };` genérico monomorfizado. Uso: `struct box<int,double> b;`." },
    { "union", "CZet: `union cell<T> { T v; long long raw; };` genérico." },
    { "arena_init", "`<utils/arena.h>`: `arena_init(&a, cap)` bump allocator con `reset`." },
    { "str_push_cstr", "`<utils/str.h>`: string builder que crece." },
    { "slice_make", "`<utils/slice.h>`: `struct slice<T> {T*data; unsigned long len;}`." },
    { "hmap_put", "`<utils/hashmap.h>`: mapa abierto string→`void*` (claves por puntero)." },
    { "sm_save", "`<utils/save_memory.h>`: binario little-endian empaquetado vía `_Reflect`." },
    { "dbuf_write", "`<utils/double_buffering.h>`: SPSC lock-free, último completo gana." },
    { "tbuf_write", "`<utils/triple_buffering.h>`: como dbuf + slot spare." },
    { "pool_create", "`<utils/thread_pool.h>`: `pool_create(4,64)` — necesita `-pthread` en glibc." },
    { "co_spawn", "`<utils/aco.h>`: corrutinas libaco round-robin." },
    { "tcp_listen", "`<utils/server.h>`: TCP mínimo IPv4/IPv6, `\"0\"` = puerto efímero." },
    { "ipc_server_open", "`<utils/ipc.h>`: AF_UNIX `SOCK_STREAM`, 0 ok / -1 errno." },
    { NULL, NULL }
};

/* ============================ diagnósticos ============================ */

typedef struct {
    long line, ch, eline, ech;
    int sev; /* 1 error, 2 warning */
    char msg[256];
} Diag;

static Diag g_diag[MAX_DIAG];
static int g_ndiag = 0;

static void diag_add(long l, long c, long el, long ec, int sev, const char *msg)
{
    if (g_ndiag >= MAX_DIAG)
        return;
    Diag *d = &g_diag[g_ndiag++];
    d->line = l;
    d->ch = c;
    d->eline = el;
    d->ech = ec;
    d->sev = sev;
    snprintf(d->msg, sizeof d->msg, "%s", msg);
}

static int is_idc(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* Chequeo 1: balance (){}[] ignorando comentarios/strings/chars. */
static void check_balance(const char *t)
{
    char stack[1024];
    long sl[1024], sc[1024];
    int sp = 0;
    long line = 0, col = 0;
    enum { N, LCOM, BCOM, STR, CHR } st = N;
    for (size_t i = 0; t[i]; i++) {
        char c = t[i], n = t[i + 1];
        if (c == '\n') {
            line++;
            col = 0;
            if (st == LCOM)
                st = N;
            continue;
        }
        col++;
        if (st == LCOM) {
            continue;
        } else if (st == BCOM) {
            if (c == '*' && n == '/') {
                st = N;
                i++;
                col++;
            }
            continue;
        } else if (st == STR) {
            if (c == '\\') {
                i++;
                col++;
            } else if (c == '"') {
                st = N;
            }
            continue;
        } else if (st == CHR) {
            if (c == '\\') {
                i++;
                col++;
            } else if (c == '\'') {
                st = N;
            }
            continue;
        }
        /* estado normal */
        if (c == '/' && n == '/') {
            st = LCOM;
            i++;
            col++;
        } else if (c == '/' && n == '*') {
            st = BCOM;
            i++;
            col++;
        } else if (c == '"') {
            st = STR;
        } else if (c == '\'') {
            st = CHR;
        } else if (c == '(' || c == '{' || c == '[') {
            if (sp < 1024) {
                stack[sp] = c;
                sl[sp] = line;
                sc[sp] = col - 1;
                sp++;
            }
        } else if (c == ')' || c == '}' || c == ']') {
            char want = c == ')' ? '(' : c == '}' ? '{' : '[';
            if (sp == 0) {
                char m[64];
                snprintf(m, sizeof m, "unmatched '%c'", c);
                diag_add(line, col - 1, line, col, 1, m);
            } else if (stack[sp - 1] != want) {
                char m[96];
                snprintf(m, sizeof m, "mismatched '%c', opened '%c' at %ld:%ld",
                         c, stack[sp - 1], sl[sp - 1] + 1, sc[sp - 1] + 1);
                diag_add(line, col - 1, line, col, 1, m);
                sp--;
            } else {
                sp--;
            }
        }
    }
    for (int k = 0; k < sp && g_ndiag < MAX_DIAG; k++) {
        char m[96];
        snprintf(m, sizeof m, "unclosed '%c' opened here", stack[k]);
        diag_add(sl[k], sc[k], sl[k], sc[k] + 1, 1, m);
    }
}

/* Chequeo 2: reglas CZet (tokens fuera de comentarios/strings). */
static void check_czet_rules(const char *t)
{
    /* Copia enmascarada: comentarios/strings -> espacios (conservando \n). */
    size_t n = strlen(t);
    char *m = (char *)malloc(n + 1);
    if (!m)
        return;
    enum { N, LCOM, BCOM, STR, CHR } st = N;
    for (size_t i = 0; i < n; i++) {
        char c = t[i], nx = i + 1 < n ? t[i + 1] : 0;
        if (st == N) {
            if (c == '/' && nx == '/') {
                st = LCOM;
                m[i] = ' ';
            } else if (c == '/' && nx == '*') {
                st = BCOM;
                m[i] = ' ';
            } else if (c == '"') {
                st = STR;
                m[i] = ' ';
            } else if (c == '\'') {
                st = CHR;
                m[i] = ' ';
            } else {
                m[i] = c;
            }
        } else if (st == LCOM) {
            m[i] = (c == '\n') ? '\n' : ' ';
            if (c == '\n')
                st = N;
        } else if (st == BCOM) {
            if (c == '*' && nx == '/') {
                m[i] = ' ';
                m[i + 1] = ' ';
                i++;
                st = N;
            } else {
                m[i] = (c == '\n') ? '\n' : ' ';
            }
        } else if (st == STR) {
            if (c == '\\' && i + 1 < n) {
                m[i] = ' ';
                m[i + 1] = ' ';
                i++;
            } else if (c == '"') {
                m[i] = ' ';
                st = N;
            } else {
                m[i] = (c == '\n') ? '\n' : ' ';
            }
        } else { /* CHR */
            if (c == '\\' && i + 1 < n) {
                m[i] = ' ';
                m[i + 1] = ' ';
                i++;
            } else if (c == '\'') {
                m[i] = ' ';
                st = N;
            } else {
                m[i] = (c == '\n') ? '\n' : ' ';
            }
        }
    }
    m[n] = '\0';

    /* _Reflect debe ir seguido de '(' */
    for (char *p = m; (p = strstr(p, "_Reflect"));) {
        int ok = (p == m || !is_idc(p[-1])) && !is_idc(p[8]);
        if (ok) {
            const char *q = p + 8;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                q++;
            if (*q != '(') {
                long l = line_of(t, (size_t)(p - m));
                long c0 = (long)(p - m);
                /* columna: distancia al \n anterior */
                long col = 0;
                for (const char *r = p; r > t && *(r - 1) != '\n'; r--)
                    col++;
                (void)c0;
                diag_add(l, col, l, col + 8, 2,
                         "_Reflect sin '(': usa _Reflect(T) como sizeof(T)");
            }
        }
        p += 8;
    }
    /* _Operator debe ir seguido de '(' */
    for (char *p = m; (p = strstr(p, "_Operator"));) {
        int ok = (p == m || !is_idc(p[-1])) && !is_idc(p[9]);
        if (ok) {
            const char *q = p + 9;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                q++;
            if (*q != '(') {
                long l = line_of(t, (size_t)(p - m));
                long col = 0;
                for (const char *r = p; r > t && *(r - 1) != '\n'; r--)
                    col++;
                diag_add(l, col, l, col + 9, 2,
                         "_Operator sin '(': T _Operator(Tipo, \"op\") { ... }");
            }
        }
        p += 9;
    }
    /* defer debe ir seguido de '{' o ';' */
    for (char *p = m; (p = strstr(p, "defer"));) {
        int before = (p == m || !is_idc(p[-1]));
        int after = !is_idc(p[5]);
        if (before && after) {
            const char *q = p + 5;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                q++;
            if (*q != '{' && *q != ';') {
                long l = line_of(t, (size_t)(p - m));
                long col = 0;
                for (const char *r = p; r > t && *(r - 1) != '\n'; r--)
                    col++;
                diag_add(l, col, l, col + 5, 2,
                         "defer espera '{ ... };' o 'defer;'");
            }
        }
        p += 5;
    }
    /* _Macro a nivel de fichero debería cerrar con '};' — chequeo blando:
     * si hay _Macro sin "};" posterior en 2000 chars, avisa. */
    for (char *p = m; (p = strstr(p, "_Macro"));) {
        int ok = (p == m || !is_idc(p[-1])) && !is_idc(p[6]);
        if (ok) {
            char *e = strstr(p, "};");
            long l = line_of(t, (size_t)(p - m));
            long col = 0;
            for (const char *r = p; r > t && *(r - 1) != '\n'; r--)
                col++;
            if (!e || (e - p) > 8000)
                diag_add(l, col, l, col + 6, 2,
                         "_Macro sin cierre '};' cercano");
        }
        p += 6;
    }
    free(m);
}

/* Chequeo 3 (opcional): invoca `czet -fsyntax-only` sobre un temporal.
 * Solo si g_use_czet y `czet` existe en PATH. Parseo: file:line:col: sev: msg */
static void check_with_czet(const char *text, const char *uri)
{
    if (!g_use_czet)
        return;
    /* tmp .ct */
    char tmp[] = "/tmp/czet-lsp-XXXXXX.ct";
    int fd = mkstemps(tmp, 3); /* conserva sufijo .ct */
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);

    char cmd[1152];
    snprintf(cmd, sizeof cmd,
             "czet -fsyntax-only -std=czet '%s' 2>&1", tmp);
    FILE *pp = popen(cmd, "r");
    if (!pp) {
        remove(tmp);
        return;
    }
    char line[1024];
    int added = 0;
    while (fgets(line, sizeof line, pp) && added < 40) {
        /* ignora si czet no existe: "sh: czet: command not found" */
        if (strstr(line, "command not found") || strstr(line, "no se encontr"))
            break;
        /* formato gcc: path:linea:col: error|warning: msg */
        char *c1 = strchr(line, ':');
        if (!c1)
            continue;
        long ln = atol(c1 + 1) - 1;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2)
            continue;
        long cc = atol(c2 + 1) - 1;
        char *c3 = strchr(c2 + 1, ':');
        if (!c3)
            continue;
        char *msg = c3 + 1;
        while (*msg == ' ' || *msg == '\t')
            msg++;
        size_t ml = strlen(msg);
        while (ml && (msg[ml - 1] == '\n' || msg[ml - 1] == '\r'))
            msg[--ml] = '\0';
        /* corta "error: "/ "warning: " del severidad */
        int sev = 1;
        if (strncmp(msg, "warning", 7) == 0 || strncmp(msg, "aviso", 5) == 0)
            sev = 2;
        else if (strncmp(msg, "error", 5) == 0)
            sev = 1;
        else if (strstr(line, "rror") && sev == 1) {
            /* deja sev=1 */
        }
        /* quita prefijo "error: " para mensaje más limpio */
        char *colon = strchr(msg, ':');
        char clean[256];
        if (colon && (size_t)(colon - msg) < 12) {
            const char *q = colon + 1;
            while (*q == ' ')
                q++;
            snprintf(clean, sizeof clean, "%s", q);
        } else {
            snprintf(clean, sizeof clean, "%s", msg);
        }
        if (ln < 0)
            ln = 0;
        if (cc < 0)
            cc = 0;
        char final_[256];
        snprintf(final_, sizeof final_, "czet: %.249s", clean);
        diag_add(ln, cc, ln, cc + 1, sev, final_);
        added++;
    }
    pclose(pp);
    remove(tmp);
    (void)uri;
}

/* Publica diagnostics del doc. */
static void publish_diagnostics(Doc *d)
{
    g_ndiag = 0;
    if (d->text) {
        check_balance(d->text);
        check_czet_rules(d->text);
        check_with_czet(d->text, d->uri);
    }
    /* construye JSON */
    size_t cap = 4096 + (size_t)g_ndiag * 512;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return;
    char *esc_uri = json_escape(d->uri);
    int n = snprintf(buf, cap,
                     "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                     "\"params\":{\"uri\":\"%s\",\"diagnostics\":[",
                     esc_uri ? esc_uri : "");
    free(esc_uri);
    size_t pos = n > 0 ? (size_t)n : 0;
    for (int i = 0; i < g_ndiag; i++) {
        Diag *dg = &g_diag[i];
        char *em = json_escape(dg->msg);
        char tmp[768];
        snprintf(tmp, sizeof tmp,
                 "%s{\"range\":{\"start\":{\"line\":%ld,\"character\":%ld},"
                 "\"end\":{\"line\":%ld,\"character\":%ld}},"
                 "\"severity\":%d,\"source\":\"czet-lsp\",\"message\":\"%s\"}",
                 i ? "," : "", dg->line, dg->ch, dg->eline, dg->ech,
                 dg->sev, em ? em : "");
        free(em);
        size_t tl = strlen(tmp);
        if (pos + tl + 8 >= cap) {
            cap = (pos + tl + 8) * 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return;
            }
            buf = nb;
        }
        memcpy(buf + pos, tmp, tl);
        pos += tl;
    }
    if (pos + 4 >= cap) {
        char *nb = (char *)realloc(buf, pos + 8);
        if (!nb) {
            free(buf);
            return;
        }
        buf = nb;
    }
    memcpy(buf + pos, "]}}", 3);
    pos += 3;
    buf[pos] = '\0';
    rpc_send(buf);
    free(buf);
}

/* ============================ handlers ============================ */

static void send_result(char *id_raw, const char *result_json)
{
    size_t cap = strlen(result_json) + 64;
    char *o = (char *)malloc(cap);
    if (!o)
        return;
    snprintf(o, cap, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
             id_raw ? id_raw : "null", result_json);
    rpc_send(o);
    free(o);
}

static void send_error(char *id_raw, int code, const char *msg)
{
    char *e = json_escape(msg);
    size_t cap = 256 + (e ? strlen(e) : 0);
    char *o = (char *)malloc(cap);
    if (!o) {
        free(e);
        return;
    }
    snprintf(o, cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id_raw ? id_raw : "null", code, e ? e : "");
    free(e);
    rpc_send(o);
    free(o);
}

static void handle_initialize(char *id_raw, const char *body)
{
    (void)body;
    send_result(id_raw,
                "{\"capabilities\":{\"textDocumentSync\":1,"
                "\"completionProvider\":{\"triggerCharacters\":[\"<\",\":\",\"_\",\".\",\"/\",\"#\"]},"
                "\"hoverProvider\":true,\"definitionProvider\":true,"
                "\"documentSymbolProvider\":true},"
                "\"serverInfo\":{\"name\":\"czet-lsp\",\"version\":\"" CZET_LSP_VERSION "\"}}");
}

static void handle_did_open(const char *body)
{
    char uri[1024] = { 0 };
    if (js_uri(uri, sizeof uri, body) != 0)
        return;
    /* text está en params.textDocument.text */
    const char *p = js_val(body, "text");
    char *txt = NULL;
    size_t tn = 0;
    if (p && *p == '"') {
        p++;
        const char *e = p;
        while (*e) {
            if (*e == '\\') {
                e += 2;
                continue;
            }
            if (*e == '"')
                break;
            e++;
        }
        size_t raw = (size_t)(e - p);
        char *tmp = (char *)malloc(raw + 1);
        if (tmp) {
            memcpy(tmp, p, raw);
            tmp[raw] = '\0';
            txt = (char *)malloc(raw + 1);
            if (txt) {
                json_unescape_to(tmp, txt, raw + 1);
                tn = strlen(txt);
            }
            free(tmp);
        }
    }
    long ver = 0;
    js_int(body, "version", &ver);
    Doc *d = doc_get_or_create(uri);
    if (!d) {
        free(txt);
        return;
    }
    d->version = (int)ver;
    if (txt) {
        doc_set_text(d, txt, tn);
        free(txt);
    } else {
        doc_set_text(d, "", 0);
    }
    publish_diagnostics(d);
}

static void handle_did_change(const char *body)
{
    char uri[1024] = { 0 };
    if (js_uri(uri, sizeof uri, body) != 0)
        return;
    Doc *d = doc_find(uri);
    if (!d)
        d = doc_get_or_create(uri);
    if (!d)
        return;
    /* contentChanges[0].text : última "text" del body es el contenido nuevo
     * (la primera "text" podría ser otra cosa solo si hay rangos; con sync
     * completa solo hay text). Tomamos la ÚLTIMA ocurrencia. */
    const char *last = NULL;
    const char *q = body;
    for (;;) {
        const char *v = strstr(q, "\"text\"");
        if (!v)
            break;
        last = v;
        q = v + 6;
    }
    if (last) {
        const char *v = strchr(last + 6, ':');
        if (v) {
            v++;
            while (*v && isspace((unsigned char)*v))
                v++;
            if (*v == '"') {
                v++;
                const char *e = v;
                while (*e) {
                    if (*e == '\\') {
                        e += 2;
                        continue;
                    }
                    if (*e == '"')
                        break;
                    e++;
                }
                size_t raw = (size_t)(e - v);
                char *tmp = (char *)malloc(raw + 1);
                if (tmp) {
                    memcpy(tmp, v, raw);
                    tmp[raw] = '\0';
                    char *txt = (char *)malloc(raw + 1);
                    if (txt) {
                        json_unescape_to(tmp, txt, raw + 1);
                        doc_set_text(d, txt, strlen(txt));
                        free(txt);
                    }
                    free(tmp);
                }
            }
        }
    }
    long ver = d->version;
    js_int(body, "version", &ver);
    d->version = (int)ver;
    publish_diagnostics(d);
}

static void handle_did_close(const char *body)
{
    char uri[1024] = { 0 };
    if (js_uri(uri, sizeof uri, body) != 0)
        return;
    Doc *d = doc_find(uri);
    if (d) {
        d->open = 0;
        free(d->text);
        d->text = NULL;
        d->len = 0;
    }
    /* limpia diagnostics */
    char *e = json_escape(uri);
    size_t cap = strlen(uri) + 160;
    char *o = (char *)malloc(cap);
    if (o) {
        snprintf(o, cap,
                 "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                 "\"params\":{\"uri\":\"%s\",\"diagnostics\":[]}}",
                 e ? e : "");
        rpc_send(o);
        free(o);
    }
    free(e);
}

static void handle_completion(char *id_raw, const char *body)
{
    char uri[1024] = { 0 };
    js_uri(uri, sizeof uri, body);
    long line, ch;
    js_position(body, &line, &ch);
    Doc *d = doc_find(uri);
    const char *text = (d && d->text) ? d->text : "";
    char lp[512];
    line_prefix(text, line, ch, lp, sizeof lp);
    /* prefijo: última palabra [A-Za-z_#/<>] */
    char *end = lp + strlen(lp);
    char *s = end;
    while (s > lp && (isalnum((unsigned char)s[-1]) || s[-1] == '_' ||
                      s[-1] == '/' || s[-1] == '#' || s[-1] == '<' ||
                      s[-1] == '.' || s[-1] == ':'))
        s--;
    /* si empieza por < o tras #include, filtra headers; si no, todo */
    int want_header = (strchr(s, '/') != NULL || strchr(s, '<') != NULL);
    /* extrae alfanumérico final para filtrar */
    char *w = end;
    while (w > s && (isalnum((unsigned char)w[-1]) || w[-1] == '_' ))
        w--;
    size_t wlen = (size_t)(end - w);

    size_t cap = 8192;
    char *o = (char *)malloc(cap);
    if (!o) {
        send_error(id_raw, -32603, "out of memory");
        return;
    }
    size_t pos = 0;
    pos += (size_t)snprintf(o + pos, cap - pos, "{\"isIncomplete\":false,\"items\":[");
    int first = 1;
    for (const Compl *c = g_compl; c->label; c++) {
        int is_hdr = (strchr(c->label, '/') != NULL);
        if (want_header && !is_hdr)
            continue;
        if (!want_header && wlen > 0 && strncmp(c->label, w, wlen) != 0)
            continue;
        char *el = json_escape(c->label);
        char *ed = json_escape(c->detail ? c->detail : "");
        char *edoc = json_escape(c->doc ? c->doc : "");
        char tmp[1024];
        snprintf(tmp, sizeof tmp,
                 "%s{\"label\":\"%s\",\"kind\":%d,\"detail\":\"%s\","
                 "\"documentation\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
                 first ? "" : ",", el ? el : "", c->lsp_kind,
                 ed ? ed : "", edoc ? edoc : "");
        free(el);
        free(ed);
        free(edoc);
        size_t tl = strlen(tmp);
        if (pos + tl + 4 >= cap) {
            cap = (pos + tl + 4) * 2;
            char *nb = (char *)realloc(o, cap);
            if (!nb) {
                free(o);
                send_error(id_raw, -32603, "out of memory");
                return;
            }
            o = nb;
        }
        memcpy(o + pos, tmp, tl);
        pos += tl;
        first = 0;
    }
    if (pos + 3 >= cap) {
        char *nb = (char *)realloc(o, pos + 8);
        if (!nb) {
            free(o);
            return;
        }
        o = nb;
    }
    memcpy(o + pos, "]}", 2);
    pos += 2;
    o[pos] = '\0';
    send_result(id_raw, o);
    free(o);
}

static void handle_hover(char *id_raw, const char *body)
{
    char uri[1024] = { 0 };
    js_uri(uri, sizeof uri, body);
    long line, ch;
    js_position(body, &line, &ch);
    Doc *d = doc_find(uri);
    const char *text = (d && d->text) ? d->text : "";
    size_t off = offset_of(text, line, ch);
    char word[128];
    word_at(text, off, word, sizeof word, NULL);
    const char *md = NULL;
    for (const HoverDoc *h = g_hover; h->word; h++) {
        if (strcmp(h->word, word) == 0) {
            md = h->md;
            break;
        }
    }
    if (!md) {
        /* genéricos: si la palabra va seguida de '<' sugiere generic */
        size_t len = strlen(text);
        size_t e = off;
        while (e < len && is_wordc(text[e]))
            e++;
        size_t t = e;
        while (t < len && (text[t] == ' ' || text[t] == '\t'))
            t++;
        if (t < len && text[t] == '<')
            md = "Instanciación genérica CZet (`f<T>(...)`, `struct box<T,U>`): monomorfizada por cada tipo.";
        else if (strncmp(word, "co_", 3) == 0 || strncmp(word, "tcp_", 4) == 0 ||
                 strncmp(word, "ipc_", 4) == 0 || strncmp(word, "hmap_", 5) == 0 ||
                 strncmp(word, "arena_", 6) == 0 || strncmp(word, "str_", 4) == 0 ||
                 strncmp(word, "slice_", 6) == 0 || strncmp(word, "pool_", 5) == 0)
            md = "API de `<utils/...>` de CZet (ver docs/GUIA_STDLIB.md).";
    }
    if (!md) {
        send_result(id_raw, "null");
        return;
    }
    char *e = json_escape(md);
    size_t cap = strlen(md) * 2 + 128;
    char *o = (char *)malloc(cap);
    if (!o) {
        free(e);
        send_error(id_raw, -32603, "out of memory");
        return;
    }
    snprintf(o, cap, "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
             e ? e : "");
    free(e);
    send_result(id_raw, o);
    free(o);
}

/* Busca definición naive: primera línea que declare `word`. */
static int find_definition(const char *text, const char *word,
                           long *dl, long *dc, long *el, long *ec)
{
    char pat_macro[160], pat_struct[160], pat_func[160], pat_td[160];
    snprintf(pat_macro, sizeof pat_macro, "_Macro %s", word);
    snprintf(pat_struct, sizeof pat_struct, "struct %s", word);
    snprintf(pat_func, sizeof pat_func, "%s(", word);
    snprintf(pat_td, sizeof pat_td, "%s;", word);

    long line = 0;
    const char *p = text;
    long best = -1, bc = 0;
    int bkind = 0; /* 0 macro,1 struct,2 func */
    /* pasada por líneas */
    const char *lp = text;
    line = 0;
    while (*lp) {
        const char *le = strchr(lp, '\n');
        size_t ll = le ? (size_t)(le - lp) : strlen(lp);
        char *tmp = (char *)malloc(ll + 1);
        if (!tmp)
            break;
        memcpy(tmp, lp, ll);
        tmp[ll] = '\0';
        int hit = 0, kind = -1;
        if (strstr(tmp, pat_macro)) {
            hit = 1;
            kind = 0;
        } else if (strstr(tmp, "_Operator") && strstr(tmp, word)) {
            hit = 1;
            kind = 0;
        } else if (strstr(tmp, pat_struct)) {
            hit = 1;
            kind = 1;
        } else if (strstr(tmp, pat_func) && (strstr(tmp, "(") != NULL)) {
            /* heurística función: contiene word( y { o ; o return-type */
            hit = 1;
            kind = 2;
        } else if ((strstr(tmp, "typedef") && strstr(tmp, word))) {
            hit = 1;
            kind = 1;
        }
        if (hit && best < 0) {
            best = line;
            const char *f = strstr(tmp, word);
            bc = f ? (long)(f - tmp) : 0;
            bkind = kind;
        }
        free(tmp);
        if (!le)
            break;
        lp = le + 1;
        line++;
        (void)p;
        (void)pat_td;
    }
    if (best < 0)
        return -1;
    *dl = best;
    *dc = bc;
    *el = best;
    *ec = bc + (long)strlen(word);
    (void)bkind;
    return 0;
}

static void handle_definition(char *id_raw, const char *body)
{
    char uri[1024] = { 0 };
    js_uri(uri, sizeof uri, body);
    long line, ch;
    js_position(body, &line, &ch);
    Doc *d = doc_find(uri);
    const char *text = (d && d->text) ? d->text : "";
    size_t off = offset_of(text, line, ch);
    char word[128];
    word_at(text, off, word, sizeof word, NULL);
    if (!word[0]) {
        send_result(id_raw, "null");
        return;
    }
    long dl, dc, el, ec;
    if (find_definition(text, word, &dl, &dc, &el, &ec) != 0) {
        send_result(id_raw, "null");
        return;
    }
    char *e = json_escape(uri);
    size_t cap = strlen(uri) + 256;
    char *o = (char *)malloc(cap);
    if (!o) {
        free(e);
        send_error(id_raw, -32603, "out of memory");
        return;
    }
    snprintf(o, cap,
             "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%ld,\"character\":%ld},"
             "\"end\":{\"line\":%ld,\"character\":%ld}}}",
             e ? e : "", dl, dc, el, ec);
    free(e);
    send_result(id_raw, o);
    free(o);
}

/* Símbolos: parseo por líneas, heurístico pero útil. */
static void handle_symbols(char *id_raw, const char *body)
{
    char uri[1024] = { 0 };
    js_uri(uri, sizeof uri, body);
    Doc *d = doc_find(uri);
    const char *text = (d && d->text) ? d->text : "";
    char *euri = json_escape(uri);

    size_t cap = 16384;
    char *o = (char *)malloc(cap);
    if (!o) {
        free(euri);
        send_error(id_raw, -32603, "out of memory");
        return;
    }
    size_t pos = 0;
    pos += (size_t)snprintf(o + pos, cap - pos, "[");
    int first = 1, count = 0;

    const char *lp = text;
    long line = 0;
    while (*lp && count < MAX_SYMBOLS) {
        const char *le = strchr(lp, '\n');
        size_t ll = le ? (size_t)(le - lp) : strlen(lp);
        if (ll > 0 && ll < 900) {
            char tmp[960];
            memcpy(tmp, lp, ll);
            tmp[ll] = '\0';
            /* trim izq */
            char *t = tmp;
            while (*t == ' ' || *t == '\t')
                t++;
            if (*t && *t != '/' && *t != '*' && *t != '#') {
                char name[128] = { 0 };
                int kind = 0; /* LSP SymbolKind */
                int hit = 0;
                if (strncmp(t, "_Macro", 6) == 0) {
                    /* _Macro NOMBRE<... */
                    const char *q = t + 6;
                    while (*q == ' ' || *q == '\t')
                        q++;
                    size_t k = 0;
                    while (is_idc(*q) && k + 1 < sizeof name)
                        name[k++] = *q++;
                    name[k] = '\0';
                    kind = 11;
                    hit = name[0] != '\0';
                } else if (strstr(t, "_Operator") == t ||
                           (strstr(t, "_Operator") && strstr(t, "("))) {
                    /* void _Operator(Tipo, "op") */
                    const char *q = strstr(t, "\"");
                    char op[32] = { 0 };
                    if (q) {
                        const char *q2 = strchr(q + 1, '"');
                        if (q2 && (size_t)(q2 - q - 1) < sizeof op - 1) {
                            memcpy(op, q + 1, (size_t)(q2 - q - 1));
                            op[q2 - q - 1] = '\0';
                        }
                    }
                    snprintf(name, sizeof name, "operator %s",
                             op[0] ? op : "?");
                    kind = 25;
                    hit = 1;
                } else if (strncmp(t, "struct", 6) == 0 && isspace((unsigned char)t[6])) {
                    const char *q = t + 6;
                    while (isspace((unsigned char)*q))
                        q++;
                    size_t k = 0;
                    while (is_idc(*q) && k + 1 < sizeof name)
                        name[k++] = *q++;
                    name[k] = '\0';
                    kind = 23;
                    hit = name[0] != '\0';
                } else if (strncmp(t, "union", 5) == 0 && isspace((unsigned char)t[5])) {
                    const char *q = t + 5;
                    while (isspace((unsigned char)*q))
                        q++;
                    size_t k = 0;
                    while (is_idc(*q) && k + 1 < sizeof name)
                        name[k++] = *q++;
                    name[k] = '\0';
                    kind = 23;
                    hit = name[0] != '\0';
                } else if (strncmp(t, "enum", 4) == 0 && isspace((unsigned char)t[4])) {
                    const char *q = t + 4;
                    while (isspace((unsigned char)*q))
                        q++;
                    size_t k = 0;
                    while (is_idc(*q) && k + 1 < sizeof name)
                        name[k++] = *q++;
                    name[k] = '\0';
                    if (!name[0])
                        snprintf(name, sizeof name, "enum@%ld", line + 1);
                    kind = 10;
                    hit = 1;
                } else if (strncmp(t, "typedef", 7) == 0 && isspace((unsigned char)t[7])) {
                    /* último ident antes de ; */
                    const char *semi = strchr(t, ';');
                    if (semi) {
                        const char *q = semi - 1;
                        while (q > t && (*q == ' ' || *q == '\t'))
                            q--;
                        const char *we = q + 1;
                        while (q > t && is_idc(*(q - 1)))
                            q--;
                        size_t k = 0;
                        if (is_idc(*q)) {
                            while (q < we && k + 1 < sizeof name)
                                name[k++] = *q++;
                            name[k] = '\0';
                        }
                    }
                    kind = 13;
                    hit = name[0] != '\0';
                } else if (strstr(t, "constexpr") == t ||
                           (strstr(t, "(") && strstr(t, ")") &&
                            (strstr(t, "{") || (le && le[1] == '{')))) {
                    /* posible función: `T name<...>(...)` o `T name(...)` */
                    const char *par = strchr(t, '(');
                    if (par && par != t) {
                        const char *q = par - 1;
                        while (q > t && (*q == ' ' || *q == '\t'))
                            q--;
                        const char *we = q + 1;
                        /* genérico `name<T,U>(...`: si termina en '>',
                         * retrocede al '<' y toma el ident previo. */
                        if (we > t && *(we - 1) == '>') {
                            int depth = 0;
                            const char *r = we - 1;
                            while (r > t) {
                                if (*r == '>')
                                    depth++;
                                else if (*r == '<') {
                                    depth--;
                                    if (depth == 0)
                                        break;
                                }
                                r--;
                            }
                            if (r > t && *r == '<') {
                                q = r - 1;
                                while (q > t && (*q == ' ' || *q == '\t'))
                                    q--;
                                we = q + 1;
                                while (q > t && is_idc(*(q - 1)))
                                    q--;
                            } else {
                                we = q + 1; /* fallback: deja "T>" fuera */
                                q = we;
                            }
                        } else {
                            while (q > t && is_idc(*(q - 1)))
                                q--;
                        }
                        /* salta keywords C */
                        size_t k = 0;
                        const char *qq = q;
                        while (qq < we && k + 1 < sizeof name)
                            name[k++] = *qq++;
                        name[k] = '\0';
                        static const char *kw[] = { "if", "for", "while",
                            "switch", "return", "sizeof", NULL };
                        int iskw = 0;
                        for (int i = 0; kw[i]; i++)
                            if (strcmp(name, kw[i]) == 0)
                                iskw = 1;
                        if (name[0] && !iskw) {
                            kind = 12;
                            hit = 1;
                        }
                    }
                }
                if (hit) {
                    char *en = json_escape(name);
                    long col = (long)(strstr(tmp, name[0] && kind != 25 ? name + (name[0] == 'o' && 0) : name) ? 0 : 0);
                    /* columna real: busca name en línea */
                    const char *f = kind == 25 ? tmp : strstr(tmp, name);
                    col = f ? (long)(f - tmp) : 0;
                    /* ajusta indent */
                    char item[768];
                    snprintf(item, sizeof item,
                             "%s{\"name\":\"%s\",\"kind\":%d,"
                             "\"location\":{\"uri\":\"%s\",\"range\":"
                             "{\"start\":{\"line\":%ld,\"character\":%ld},"
                             "\"end\":{\"line\":%ld,\"character\":%ld}}}}",
                             first ? "" : ",", en ? en : "", kind,
                             euri ? euri : "", line, col, line,
                             col + (long)strlen(name));
                    free(en);
                    size_t tl = strlen(item);
                    if (pos + tl + 4 >= cap) {
                        cap = (pos + tl + 4) * 2;
                        char *nb = (char *)realloc(o, cap);
                        if (!nb) {
                            free(o);
                            free(euri);
                            send_error(id_raw, -32603, "out of memory");
                            return;
                        }
                        o = nb;
                    }
                    memcpy(o + pos, item, tl);
                    pos += tl;
                    first = 0;
                    count++;
                }
            }
        }
        if (!le)
            break;
        lp = le + 1;
        line++;
    }
    if (pos + 3 >= cap) {
        char *nb = (char *)realloc(o, pos + 8);
        if (!nb) {
            free(o);
            free(euri);
            return;
        }
        o = nb;
    }
    memcpy(o + pos, "]", 1);
    pos += 1;
    o[pos] = '\0';
    free(euri);
    send_result(id_raw, o);
    free(o);
}

/* ============================ main loop ============================ */

static void print_help(const char *prog)
{
    printf("Uso: %s [--log=PATH] [--no-czet] [--help]\n"
           "Mini LSP para CZet (.ct/.czet/.c con -std=czet) por stdio.\n"
           "\n"
           "Compilar: cc -O2 -std=c11 -Wall -o czet-lsp czet-lsp.c\n"
           "Vim:      usa este repo como plugin (ver README.md)\n"
           "VSCode:   ver vscode/README.md\n",
           prog);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--log=", 6) == 0) {
            g_log_path = argv[i] + 6;
        } else if (strcmp(argv[i], "--no-czet") == 0) {
            g_use_czet = 0;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }
    if (g_log_path) {
        g_log = fopen(g_log_path, "a");
    }

    /* stdin binario (por si CRLF) */
    lsp_log("czet-lsp " CZET_LSP_VERSION " start (use_czet=%d)", g_use_czet);

    int should_exit = 0;
    int shutdown_req = 0;
    for (;;) {
        char *body = NULL;
        int rc = rpc_read(&body);
        if (rc != 0) {
            free(body);
            break; /* EOF */
        }
        lsp_log("RECV: %s", body);
        char *id_raw = js_id_raw(body);
        char method[128] = { 0 };
        if (js_str(body, "method", method, sizeof method) != 0) {
            /* respuesta a nuestro publish? ignora */
            free(id_raw);
            free(body);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(id_raw, body);
        } else if (strcmp(method, "initialized") == 0) {
            /* noop */
        } else if (strcmp(method, "shutdown") == 0) {
            shutdown_req = 1;
            send_result(id_raw, "null");
        } else if (strcmp(method, "exit") == 0) {
            should_exit = 1;
            free(id_raw);
            free(body);
            break;
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
            handle_did_open(body);
        } else if (strcmp(method, "textDocument/didChange") == 0) {
            handle_did_change(body);
        } else if (strcmp(method, "textDocument/didClose") == 0) {
            handle_did_close(body);
        } else if (strcmp(method, "textDocument/completion") == 0) {
            if (!id_raw)
                ; /* notificación: nada */
            else
                handle_completion(id_raw, body);
        } else if (strcmp(method, "textDocument/hover") == 0) {
            if (id_raw)
                handle_hover(id_raw, body);
        } else if (strcmp(method, "textDocument/definition") == 0) {
            if (id_raw)
                handle_definition(id_raw, body);
        } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
            if (id_raw)
                handle_symbols(id_raw, body);
        } else if (strcmp(method, "$/cancelRequest") == 0) {
            /* ignora */
        } else {
            if (id_raw)
                send_error(id_raw, -32601, "Method not found");
        }
        free(id_raw);
        free(body);
        if (should_exit)
            break;
    }
    if (g_log)
        fclose(g_log);
    return shutdown_req ? 0 : 0;
}
