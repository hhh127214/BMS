#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stddef.h>

enum {
    JSON_NULL = 0,
    JSON_BOOL = 1,
    JSON_NUMBER = 2,
    JSON_STRING = 3,
    JSON_ARRAY = 4,
    JSON_OBJECT = 5
};

typedef struct json_value {
    int type;
    int boolean;
    double number;
    char* string;
    struct {
        struct json_value** items;
        int count;
    } array;
    struct {
        char** keys;
        struct json_value** values;
        int count;
    } object;
} json_value;

/* Parse a JSON document. Returns NULL on malformed input. */
json_value* json_parse(const char* text);

/* Free a parsed value tree. */
void json_free(json_value* v);

/* Object member lookup (NULL if v is not an object or key absent). */
const json_value* json_get(const json_value* v, const char* key);

/* Scalar accessors with defaults. */
double json_num(const json_value* v, double dflt);
const char* json_str(const json_value* v, const char* dflt);

/* Array accessors. */
int json_array_count(const json_value* v);
const json_value* json_array_at(const json_value* v, int index);

#endif
