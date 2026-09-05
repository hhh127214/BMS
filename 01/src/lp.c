/*
 * lp.c - dense tableau simplex (MAXIMIZE) with big-M artificials.
 *
 * Converts "A x <= b, x >= 0" into equality form by adding slack
 * variables, negating rows with negative RHS, then adding artificial
 * variables to every row so an initial basic solution exists. Artificials
 * carry a large negative objective penalty (big-M), so the optimum never
 * keeps them positive; any basic artificial left above tolerance means the
 * problem is infeasible. Bland's rule guarantees termination.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lp.h"

#define LP_EPS       1e-9
#define LP_RC_EPS    1e-8
#define LP_OBJ_EPS   1e-7
#define LP_MAX_ITER  200000
#define LP_BIG_M     1e6

static int pivot(double* T, int rows, int cols, int pr, int pc) {
    double pv = T[(size_t)pr * cols + pc];
    if (fabs(pv) < LP_EPS) return -1;
    for (int j = 0; j < cols; j++) T[(size_t)pr * cols + j] /= pv;
    for (int i = 0; i < rows; i++) {
        if (i == pr) continue;
        double f = T[(size_t)i * cols + pc];
        if (f == 0.0) continue;
        for (int j = 0; j < cols; j++)
            T[(size_t)i * cols + j] -= f * T[(size_t)pr * cols + j];
    }
    return 0;
}

/* Run simplex from the current tableau/basis.
 * rows = m+1 (row 0 = objective), cols = ncol+1 (last column = RHS).
 * basis[i] = column index of the basic variable in row i+1.
 * Returns 1 optimal, -1 unbounded, -2 iteration cap exceeded. */
static int run_simplex(double* T, int rows, int cols, int m, int* basis,
                       int* iter) {
    (void)m;
    while (1) {
        (*iter)++;
        if (*iter > LP_MAX_ITER) return -2;

        /* entering column: Bland's rule - smallest index with positive
         * reduced cost (row 0 is at offset 0). */
        int enter = -1;
        for (int j = 0; j < cols - 1; j++) {
            if (T[j] > LP_RC_EPS) { enter = j; break; }
        }
        if (enter < 0) return 1;   /* optimal */

        /* leaving row: minimum ratio test with Bland tie-break */
        int leave = -1;
        double min_ratio = 0.0;
        for (int i = 1; i < rows; i++) {
            double a = T[(size_t)i * cols + enter];
            if (a <= LP_EPS) continue;
            double ratio = T[(size_t)i * cols + (cols - 1)] / a;
            if (leave < 0 || ratio < min_ratio - LP_EPS ||
                (fabs(ratio - min_ratio) <= LP_EPS && basis[i - 1] < basis[leave - 1])) {
                leave = i;
                min_ratio = ratio;
            }
        }
        if (leave < 0) return -1;   /* unbounded */

        if (pivot(T, rows, cols, leave, enter) != 0) return -2;
        basis[leave - 1] = enter;
    }
}
int lp_solve_max(int n, int m, const double* c, const double* A,
                 const double* b, double* xout, double* opt) {
    if (n <= 0 || m < 0) return -2;

    if (m == 0) {
        for (int j = 0; j < n; j++) {
            if (c[j] > LP_OBJ_EPS) return -1;   /* unbounded */
        }
        for (int j = 0; j < n; j++) xout[j] = 0.0;
        *opt = 0.0;
        return 1;
    }

    int nart = 0;
    for (int i = 0; i < m; i++) if (b[i] < 0.0) nart++;
    int ncol = n + m + nart;         /* originals + slacks + artificials (only where needed) */
    int rows = m + 1;
    int cols = ncol + 1;
    double* T = (double*)calloc((size_t)rows * cols, sizeof(double));
    int* basis = (int*)malloc((size_t)m * sizeof(int));
    if (!T || !basis) { free(T); free(basis); return -2; }

    /* build constraint rows; make all RHS >= 0.
     * Rows with b >= 0 keep their slack column as the initial basic
     * variable; rows with b < 0 are negated and get an artificial. */
    int art = 0;
    for (int i = 0; i < m; i++) {
        int neg = b[i] < 0.0;
        double bi = neg ? -b[i] : b[i];
        for (int j = 0; j < n; j++)
            T[(size_t)(i + 1) * cols + j] = neg ? -A[(size_t)i * n + j] : A[(size_t)i * n + j];
        T[(size_t)(i + 1) * cols + (n + i)] = neg ? -1.0 : 1.0;   /* slack */
        T[(size_t)(i + 1) * cols + (cols - 1)] = bi;
        if (neg) {
            T[(size_t)(i + 1) * cols + (n + m + art)] = 1.0;      /* artificial */
            basis[i] = n + m + art;
            art++;
        } else {
            basis[i] = n + i;                                     /* slack basic */
        }
    }

    /* objective: originals get c, slacks 0, artificials -BIG_M */
    for (int j = 0; j < n; j++) T[j] = c[j];
    int artcol = n + m;
    for (int i = 0; i < m; i++) {
        if (b[i] < 0.0) {
            T[artcol] = -LP_BIG_M;               /* artificial penalty */
            artcol++;
            /* make the artificial's reduced cost zero: row0 += M * row */
            for (int j = 0; j < cols; j++)
                T[j] += LP_BIG_M * T[(size_t)(i + 1) * cols + j];
        }
    }

    int iter = 0;
    int st = run_simplex(T, rows, cols, m, basis, &iter);
    if (st == -1) { free(T); free(basis); return -1; }   /* unbounded */
    if (st != 1)  { free(T); free(basis); return -2; }

    /* infeasible iff any basic artificial remains positive */
    for (int i = 0; i < m; i++) {
        if (basis[i] >= n + m && T[(size_t)(i + 1) * cols + (cols - 1)] > LP_OBJ_EPS) {
            free(T);
            free(basis);
            return 0;
        }
    }

    for (int j = 0; j < n; j++) xout[j] = 0.0;
    for (int i = 0; i < m; i++) {
        if (basis[i] < n) xout[basis[i]] = T[(size_t)(i + 1) * cols + (cols - 1)];
    }
    *opt = -T[cols - 1];   /* row 0 stores -z in this convention */

#ifdef LP_DEBUG
    {
        printf("LP_DEBUG: opt=%.12g iter=%d\n", *opt, iter);
        for (int i = 0; i < m; i++) {
            double lhs = 0.0;
            for (int j = 0; j < n; j++) lhs += A[(size_t)i * n + j] * xout[j];
            if (lhs > b[i] + 1e-6) {
                printf("LP_DEBUG: VIOLATION row %d lhs=%.12g rhs=%.12g\n", i, lhs, b[i]);
            }
        }
    }
#endif

    free(T);
    free(basis);
    return 1;
}

