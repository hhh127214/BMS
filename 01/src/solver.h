#ifndef SOLVER_H
#define SOLVER_H

#include <stddef.h>

/*
 * Battery charge/discharge optimization entry point.
 *
 * Parse request_json, solve the optimization, and write a JSON response
 * into out (null-terminated). Returns 0 on success, -1 on request error
 * (in which case out contains an error object).
 */
int run_optimize(const char* request_json, char* out, size_t out_size);

#endif
