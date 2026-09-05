/*
 * milp.c - branch-and-bound MILP solver (MAXIMIZE) with binary variables.
 *
 * Best-bound-first search:
 *   - open nodes live in a max-heap keyed by their LP relaxation bound;
 *   - the node with the highest upper bound is expanded first;
 *   - optimality is proven as soon as the best open bound <= incumbent;
 *   - a rounding heuristic (fix binaries to nearest, re-solve LP) seeds a
 *     strong incumbent from the root relaxation, so for problems with a
 *     zero integrality gap the solver typically finishes at the root.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "milp.h"
#include "lp.h"

#define BB_INT_TOL 1e-5
#define BB_OPT_TOL 1e-7
#define HEAP_INIT  64

typedef struct node {
    char* fixed;      /* -1 unfixed, 0/1 fixed binary assignment */
    double bound;     /* LP relaxation upper bound of the subtree */
} node;

typedef struct {
    const milp_prob* p;
    node** heap;
    int hsize, hcap;
    double* best_x;
    double best_val;
    int found;
    int nodes;        /* LP nodes solved */
    int cap;
    int aborted;
} bb_ctx;

/* Solve the LP relaxation of the current problem plus fixed-binary rows. */
static int lp_relaxation(const milp_prob* p, const char* fixed,
                         double* x, double* opt) {
    int k = 0;
    for (int i = 0; i < p->n; i++) if (fixed[i] >= 0) k++;
    int m2 = p->m + k;
    double* A = (double*)malloc((size_t)m2 * p->n * sizeof(double));
    double* b = (double*)malloc((size_t)m2 * sizeof(double));
    if (!A || !b) { free(A); free(b); return -2; }
    for (int i = 0; i < p->m; i++) {
        memcpy(A + (size_t)i * p->n, p->A + (size_t)i * p->n,
               (size_t)p->n * sizeof(double));
        b[i] = p->b[i];
    }
    int r = p->m;
    for (int i = 0; i < p->n; i++) {
        if (fixed[i] == 0) {
            memset(A + (size_t)r * p->n, 0, (size_t)p->n * sizeof(double));
            A[(size_t)r * p->n + i] = 1.0;
            b[r] = 0.0;
            r++;
        } else if (fixed[i] == 1) {
            memset(A + (size_t)r * p->n, 0, (size_t)p->n * sizeof(double));
            A[(size_t)r * p->n + i] = -1.0;
            b[r] = -1.0;
            r++;
        }
    }
    int st = lp_solve_max(p->n, m2, p->c, A, b, x, opt);
    free(A);
    free(b);
    return st;
}

/* ---- max-heap of nodes keyed by bound ---- */
static int heap_push(bb_ctx* c, node* nd) {
    if (c->hsize == c->hcap) {
        int nc = c->hcap ? c->hcap * 2 : HEAP_INIT;
        node** h = (node**)realloc(c->heap, (size_t)nc * sizeof(node*));
        if (!h) return -1;
        c->heap = h;
        c->hcap = nc;
    }
    int i = c->hsize++;
    c->heap[i] = nd;
    while (i > 0) {
        int par = (i - 1) / 2;
        if (c->heap[par]->bound >= c->heap[i]->bound) break;
        node* t = c->heap[par]; c->heap[par] = c->heap[i]; c->heap[i] = t;
        i = par;
    }
    return 0;
}

static node* heap_pop(bb_ctx* c) {
    if (c->hsize == 0) return NULL;
    node* top = c->heap[0];
    c->heap[0] = c->heap[--c->hsize];
    int i = 0;
    while (1) {
        int l = 2 * i + 1, rr = 2 * i + 2, m = i;
        if (l < c->hsize && c->heap[l]->bound > c->heap[m]->bound) m = l;
        if (rr < c->hsize && c->heap[rr]->bound > c->heap[m]->bound) m = rr;
        if (m == i) break;
        node* t = c->heap[m]; c->heap[m] = c->heap[i]; c->heap[i] = t;
        i = m;
    }
    return top;
}

static void heap_free_all(bb_ctx* c) {
    while (c->hsize > 0) {
        node* nd = heap_pop(c);
        free(nd->fixed);
        free(nd);
    }
    free(c->heap);
    c->heap = NULL;
    c->hsize = c->hcap = 0;
}
/* ---- helpers ---- */
static int all_integral(const milp_prob* p, const char* fixed, const double* x) {
    for (int i = 0; i < p->nb; i++) {
        int j = p->bin[i];
        if (fixed[j] >= 0) continue;
        if (fabs(x[j] - floor(x[j] + 0.5)) > BB_INT_TOL) return 0;
    }
    return 1;
}

static int most_fractional(const milp_prob* p, const char* fixed, const double* x) {
    int bj = -1;
    double bfr = -1.0;
    for (int i = 0; i < p->nb; i++) {
        int j = p->bin[i];
        if (fixed[j] >= 0) continue;
        double fr = x[j] - floor(x[j]);
        if (fr > 0.5) fr = 1.0 - fr;
        if (fr > bfr) { bfr = fr; bj = j; }
    }
    return bj;
}

static void update_incumbent(bb_ctx* c, const double* x, double val) {
    if (c->found && val <= c->best_val + BB_OPT_TOL) return;
    if (!c->found) {
        c->best_x = (double*)malloc((size_t)c->p->n * sizeof(double));
        if (!c->best_x) return;
        c->found = 1;
    }
    memcpy(c->best_x, x, (size_t)c->p->n * sizeof(double));
    c->best_val = val;
}

/* Rounding heuristic: fix every unfixed binary to its nearest integer and
 * re-solve; if the result is integral, it is a valid incumbent. */
static void rounding_heuristic(bb_ctx* c, const char* fixed, const double* x) {
    const milp_prob* p = c->p;
    if (p->nb == 0) return;
    char* rf = (char*)malloc((size_t)p->n);
    double* rx = (double*)malloc((size_t)p->n * sizeof(double));
    if (!rf || !rx) { free(rf); free(rx); return; }
    memcpy(rf, fixed, (size_t)p->n);
    for (int i = 0; i < p->nb; i++) {
        int j = p->bin[i];
        if (rf[j] < 0) rf[j] = (x[j] >= 0.5) ? 1 : 0;
    }
    double ropt = 0.0;
    if (lp_relaxation(p, rf, rx, &ropt) == 1) {
        int ok = 1;
        for (int i = 0; i < p->nb; i++) {
            int j = p->bin[i];
            if (fabs(rx[j] - floor(rx[j] + 0.5)) > BB_INT_TOL) { ok = 0; break; }
        }
        if (ok) update_incumbent(c, rx, ropt);
    }
    free(rf);
    free(rx);
}

static int push_child(bb_ctx* c, const char* fixed, int j, int val, double bound) {
    node* nd = (node*)malloc(sizeof(node));
    char* f = (char*)malloc((size_t)c->p->n);
    if (!nd || !f) { free(nd); free(f); return -1; }
    memcpy(f, fixed, (size_t)c->p->n);
    f[j] = (char)val;
    nd->fixed = f;
    nd->bound = bound;
    if (heap_push(c, nd) != 0) { free(f); free(nd); return -1; }
    return 0;
}
int milp_solve_max(const milp_prob* p, double* x, double* opt,
                   int* nodes_used, int* exact, double* best_bound) {
    if (!p || p->n <= 0 || p->nb < 0) return -1;

    bb_ctx c;
    memset(&c, 0, sizeof(c));
    c.p = p;
    c.cap = p->node_cap > 0 ? p->node_cap : 1000;
    c.best_val = -1e300;

    char* rootf = (char*)malloc((size_t)p->n);
    double* rx = (double*)malloc((size_t)p->n * sizeof(double));
    if (!rootf || !rx) { free(rootf); free(rx); return -1; }
    memset(rootf, -1, (size_t)p->n);

    double ropt = 0.0;
    int st = lp_relaxation(p, rootf, rx, &ropt);
    if (st == -2) { free(rootf); free(rx); return -1; }
    if (st != 1)  { free(rootf); free(rx); return 0; }   /* infeasible */
    c.nodes = 1;

    rounding_heuristic(&c, rootf, rx);   /* seed a strong incumbent */

    if (all_integral(p, rootf, rx)) {
        update_incumbent(&c, rx, ropt);
        memcpy(x, c.best_x, (size_t)p->n * sizeof(double));
        *opt = c.best_val;
        if (nodes_used) *nodes_used = c.nodes;
        if (exact) *exact = 1;
        if (best_bound) *best_bound = c.best_val;
        free(rootf); free(rx); free(c.best_x);
        return 1;
    }

    int bj = most_fractional(p, rootf, rx);
    if (bj < 0) { free(rootf); free(rx); free(c.best_x); return 1; }
    if (push_child(&c, rootf, bj, 1, ropt) != 0 ||
        push_child(&c, rootf, bj, 0, ropt) != 0) {
        c.aborted = 1;
    }

    double global_bound = ropt;

    while (c.hsize > 0 && !c.aborted) {
        node* nd = heap_pop(&c);
        if (c.found && nd->bound <= c.best_val + BB_OPT_TOL) {
            global_bound = nd->bound;   /* all open nodes are no better */
            free(nd->fixed); free(nd);
            break;
        }
        double* nx = (double*)malloc((size_t)p->n * sizeof(double));
        double nopt = 0.0;
        int st2 = lp_relaxation(p, nd->fixed, nx, &nopt);
        c.nodes++;
        if (c.nodes > c.cap) { c.aborted = 1; free(nx); free(nd->fixed); free(nd); break; }
        if (st2 == -2)       { c.aborted = 1; free(nx); free(nd->fixed); free(nd); break; }
        if (st2 != 1)        { free(nx); free(nd->fixed); free(nd); continue; }  /* infeasible */

        if (nopt > global_bound) global_bound = nopt;

        if (all_integral(p, nd->fixed, nx)) {
            update_incumbent(&c, nx, nopt);
            free(nx); free(nd->fixed); free(nd);
            continue;
        }
        int cj = most_fractional(p, nd->fixed, nx);
        if (cj >= 0) {
            if (push_child(&c, nd->fixed, cj, 1, nopt) != 0 ||
                push_child(&c, nd->fixed, cj, 0, nopt) != 0) {
                c.aborted = 1;
            }
        }
        free(nx); free(nd->fixed); free(nd);
    }

    if (c.aborted && c.hsize > 0) global_bound = c.heap[0]->bound;
    heap_free_all(&c);
    free(rootf);
    free(rx);

    if (c.found) {
        memcpy(x, c.best_x, (size_t)p->n * sizeof(double));
        *opt = c.best_val;
        if (nodes_used) *nodes_used = c.nodes;
        if (exact) *exact = c.aborted ? 0 : 1;
        if (best_bound) {
            double bb = (global_bound > c.best_val + BB_OPT_TOL) ? global_bound : c.best_val;
            *best_bound = bb;
        }
        free(c.best_x);
        return 1;
    }
    free(c.best_x);
    return 0;
}


