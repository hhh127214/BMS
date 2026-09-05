/*
 * json_util.c - minimal JSON parser and accessors (RFC 8259 subset).
 * Supports objects, arrays, strings (with escapes incl. \uXXXX), numbers,
 * booleans and null.
 */
#include <stdlib.h>
#include <string.h>

#include "json_util.h"

typedef struct {
    const char* p;
    int err;
} parser;

static void skip_ws(parser* P) {
    while (*P->p == ' ' || *P->p == '\t' || *P->p == '\r' || *P->p == '\n') P->p++;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a JSON string token (including surrounding quotes). */
static char* parse_string_token(parser* P) {
    if (*P->p != '"') { P->err = 1; return NULL; }
    P->p++;
    size_t cap = 64, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) { P->err = 1; return NULL; }
    while (*P->p && *P->p != '"') {
        unsigned cp = (unsigned char)*P->p;
        P->p++;
        if (cp == '\\') {
            char esc = *P->p;
            if (esc == 0) { P->err = 1; break; }
            P->p++;
            switch (esc) {
                case '"': case '\\': case '/': cp = (unsigned char)esc; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break;
                case 't': cp = '\t'; break;
                case 'u': {
                    cp = 0;
                    int ok = 1;
                    for (int k = 0; k < 4; k++) {
                        int h = hexval(*P->p);
                        if (h < 0) { ok = 0; break; }
                        cp = (cp << 4) | (unsigned)h;
                        P->p++;
                    }
                    if (!ok) P->err = 1;
                    break;
                }
                default: P->err = 1; break;
            }
        }
        if (P->err) break;
        if (cp < 0x80) {
            if (len + 2 > cap) {
                cap *= 2;
                char* nb = (char*)realloc(out, cap);
                if (!nb) { P->err = 1; free(out); return NULL; }
                out = nb;
            }
            out[len++] = (char)cp;
        } else if (cp < 0x800) {
            if (len + 3 > cap) {
                cap *= 2;
                char* nb = (char*)realloc(out, cap);
                if (!nb) { P->err = 1; free(out); return NULL; }
                out = nb;
            }
            out[len++] = (char)(0xC0 | (cp >> 6));
            out[len++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (len + 4 > cap) {
                cap *= 2;
                char* nb = (char*)realloc(out, cap);
                if (!nb) { P->err = 1; free(out); return NULL; }
                out = nb;
            }
            out[len++] = (char)(0xE0 | (cp >> 12));
            out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[len++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    if (*P->p != '"') P->err = 1;
    else P->p++;
    if (P->err) { free(out); return NULL; }
    out[len] = 0;
    return out;
}
static json_value* parse_value(parser* P);

static json_value* new_value(int type) {
    json_value* v = (json_value*)calloc(1, sizeof(json_value));
    if (v) v->type = type;
    return v;
}

static int match_literal(parser* P, const char* lit) {
    size_t n = strlen(lit);
    if (strncmp(P->p, lit, n) == 0) { P->p += n; return 1; }
    P->err = 1;
    return 0;
}

static json_value* parse_array(parser* P) {
    P->p++; /* '[' */
    json_value* v = new_value(JSON_ARRAY);
    if (!v) { P->err = 1; return NULL; }
    size_t cap = 8;
    v->array.items = (json_value**)malloc(cap * sizeof(json_value*));
    if (!v->array.items) { json_free(v); P->err = 1; return NULL; }
    skip_ws(P);
    if (*P->p == ']') { P->p++; return v; }
    while (1) {
        json_value* item = parse_value(P);
        if (!item) break;
        if (v->array.count == (int)cap) {
            cap *= 2;
            json_value** ni = (json_value**)realloc(v->array.items, cap * sizeof(json_value*));
            if (!ni) { json_free(item); json_free(v); P->err = 1; return NULL; }
            v->array.items = ni;
        }
        v->array.items[v->array.count++] = item;
        skip_ws(P);
        if (*P->p == ',') { P->p++; continue; }
        if (*P->p == ']') { P->p++; return v; }
        P->err = 1;
        break;
    }
    json_free(v);
    return NULL;
}

static json_value* parse_object(parser* P) {
    P->p++; /* '{' */
    json_value* v = new_value(JSON_OBJECT);
    if (!v) { P->err = 1; return NULL; }
    size_t cap = 8;
    v->object.keys = (char**)malloc(cap * sizeof(char*));
    v->object.values = (json_value**)malloc(cap * sizeof(json_value*));
    if (!v->object.keys || !v->object.values) { json_free(v); P->err = 1; return NULL; }
    skip_ws(P);
    if (*P->p == '}') { P->p++; return v; }
    while (1) {
        skip_ws(P);
        if (*P->p != '"') { P->err = 1; break; }
        char* key = parse_string_token(P);
        if (!key) break;
        skip_ws(P);
        if (*P->p != ':') { free(key); P->err = 1; break; }
        P->p++;
        json_value* val = parse_value(P);
        if (!val) { free(key); break; }
        if (v->object.count == (int)cap) {
            cap *= 2;
            char** nk = (char**)realloc(v->object.keys, cap * sizeof(char*));
            json_value** nv = (json_value**)realloc(v->object.values, cap * sizeof(json_value*));
            if (!nk || !nv) { free(key); json_free(val); json_free(v); P->err = 1; return NULL; }
            v->object.keys = nk;
            v->object.values = nv;
        }
        v->object.keys[v->object.count] = key;
        v->object.values[v->object.count] = val;
        v->object.count++;
        skip_ws(P);
        if (*P->p == ',') { P->p++; continue; }
        if (*P->p == '}') { P->p++; return v; }
        P->err = 1;
        break;
    }
    json_free(v);
    return NULL;
}

static json_value* parse_value(parser* P) {
    skip_ws(P);
    char c = *P->p;
    if (c == 0) { P->err = 1; return NULL; }
    if (c == '{') return parse_object(P);
    if (c == '[') return parse_array(P);
    if (c == '"') {
        json_value* v = new_value(JSON_STRING);
        if (!v) { P->err = 1; return NULL; }
        v->string = parse_string_token(P);
        if (!v->string) { json_free(v); return NULL; }
        return v;
    }
    if (c == 't') {
        if (!match_literal(P, "true")) return NULL;
        json_value* v = new_value(JSON_BOOL);
        if (v) v->boolean = 1;
        return v;
    }
    if (c == 'f') {
        if (!match_literal(P, "false")) return NULL;
        return new_value(JSON_BOOL);
    }
    if (c == 'n') {
        if (!match_literal(P, "null")) return NULL;
        return new_value(JSON_NULL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char* end = NULL;
        double num = strtod(P->p, &end);
        if (end == P->p) { P->err = 1; return NULL; }
        P->p = end;
        json_value* v = new_value(JSON_NUMBER);
        if (v) v->number = num;
        return v;
    }
    P->err = 1;
    return NULL;
}

json_value* json_parse(const char* text) {
    if (!text) return NULL;
    parser P = { text, 0 };
    json_value* v = parse_value(&P);
    if (!v || P.err) { json_free(v); return NULL; }
    skip_ws(&P);
    if (*P.p != 0) { json_free(v); return NULL; }
    return v;
}

void json_free(json_value* v) {
    if (!v) return;
    switch (v->type) {
        case JSON_STRING: free(v->string); break;
        case JSON_ARRAY:
            for (int i = 0; i < v->array.count; i++) json_free(v->array.items[i]);
            free(v->array.items);
            break;
        case JSON_OBJECT:
            for (int i = 0; i < v->object.count; i++) {
                free(v->object.keys[i]);
                json_free(v->object.values[i]);
            }
            free(v->object.keys);
            free(v->object.values);
            break;
        default: break;
    }
    free(v);
}

const json_value* json_get(const json_value* v, const char* key) {
    if (!v || v->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < v->object.count; i++) {
        if (strcmp(v->object.keys[i], key) == 0) return v->object.values[i];
    }
    return NULL;
}

double json_num(const json_value* v, double dflt) {
    if (!v || v->type != JSON_NUMBER) return dflt;
    return v->number;
}

const char* json_str(const json_value* v, const char* dflt) {
    if (!v || v->type != JSON_STRING) return dflt;
    return v->string;
}

int json_array_count(const json_value* v) {
    if (!v || v->type != JSON_ARRAY) return 0;
    return v->array.count;
}

const json_value* json_array_at(const json_value* v, int index) {
    if (!v || v->type != JSON_ARRAY || index < 0 || index >= v->array.count) return NULL;
    return v->array.items[index];
}
