#ifndef LP_H
#define LP_H

/*
 * Dense tableau simplex (two-phase), MAXIMIZE.
 *
 *   maximize   c[0..n-1]^T x
 *   subject to A[i][0..n-1] . x <= b[i],  i = 0..m-1
 *              x[j] >= 0,                 j = 0..n-1
 *
 * Returns:
 *    1  optimal       (x in xout, optimum in *opt)
 *    0  infeasible
 *   -1  unbounded
 *   -2  numerical / iteration-cap failure
 */
int lp_solve_max(int n, int m, const double* c,
                 const double* A, const double* b,
                 double* xout, double* opt);

#endif
