#ifndef MILP_H
#define MILP_H

/*
 * Branch-and-bound MILP solver (MAXIMIZE) over a subset of binary variables.
 *
 *   maximize   c^T x
 *   subject to A x <= b,  x >= 0
 *              x[bin[i]] in {0,1},  i = 0..nb-1
 *
 * Search strategy: best-bound-first. Open nodes are kept in a max-heap
 * keyed by their LP relaxation upper bound; the most promising node is
 * expanded first and optimality is proven as soon as the best open bound
 * does not exceed the incumbent. A rounding heuristic seeds a strong
 * incumbent from the root relaxation.
 *
 * Returns:
 *    1  optimal incumbent found
 *    0  infeasible / no incumbent
 *   -1  numerical failure or node cap reached without an incumbent
 * If *exact is 0, the node cap was hit and *best_bound is the best upper
 * bound still achievable by any unexplored node (integrality gap = the
 * difference between *best_bound and the returned objective).
 */
typedef struct {
    int n;              /* number of variables                     */
    int m;              /* number of constraints                   */
    const double* c;    /* objective coefficients, length n        */
    const double* A;    /* constraints, m x n row-major            */
    const double* b;    /* RHS, length m                           */
    int nb;             /* number of binary variables              */
    const int* bin;     /* indices of binary variables, length nb  */
    int node_cap;       /* max branch-and-bound nodes (>= 1)       */
} milp_prob;

int milp_solve_max(const milp_prob* p, double* x, double* opt,
                   int* nodes_used, int* exact, double* best_bound);

#endif
