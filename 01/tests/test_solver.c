/* Standalone sanity tests for the LP and MILP engines.
 * Build:  gcc -O2 -Wall -o test_solver.exe test_solver.c lp.c milp.c
 * Run:    test_solver.exe
 */
#include <stdio.h>
#include <math.h>
#include "lp.h"
#include "milp.h"

int main(void) {
    double x[8], opt, bb;
    int st, nodes, exact;

    /* 1) LP: max x1+x2  s.t. x1<=2, x2<=2   -> x=(2,2), opt=4 */
    {
        double c[2] = {1, 1};
        double A[2][2] = {{1, 0}, {0, 1}};
        double b[2] = {2, 2};
        st = lp_solve_max(2, 2, c, &A[0][0], b, x, &opt);
        printf("1) st=%d x=(%g,%g) opt=%g (expect 2,2 / 4)\n", st, x[0], x[1], opt);
    }

    /* 2) LP with negative RHS: max x1 s.t. x1<=1, -x1<=-1  -> x1=1, opt=1 */
    {
        double c[1] = {1};
        double A[2][1] = {{1}, {-1}};
        double b[2] = {1, -1};
        st = lp_solve_max(1, 2, c, &A[0][0], b, x, &opt);
        printf("2) st=%d x=%g opt=%g (expect 1 / 1)\n", st, x[0], opt);
    }

    /* 3) MILP: max x1, x1 binary, x1<=0.5 -> x1=0, opt=0 */
    {
        double c[1] = {1};
        double A[1][1] = {{1}};
        double b[1] = {0.5};
        int bin[1] = {0};
        milp_prob p = {1, 1, c, &A[0][0], b, 1, bin, 100};
        st = milp_solve_max(&p, x, &opt, &nodes, &exact, &bb);
        printf("3) st=%d x=%g opt=%g nodes=%d exact=%d (expect 0 / 0)\n", st, x[0], opt, nodes, exact);
    }

    /* 4) MILP: max x1+x2, binary, x1+x2<=1 -> one of them 1, opt=1 */
    {
        double c[2] = {1, 1};
        double A[1][2] = {{1, 1}};
        double b[1] = {1};
        int bin[2] = {0, 1};
        milp_prob p = {2, 1, c, &A[0][0], b, 2, bin, 100};
        st = milp_solve_max(&p, x, &opt, &nodes, &exact, &bb);
        printf("4) st=%d x=(%g,%g) opt=%g nodes=%d exact=%d (expect sum=1, opt=1)\n",
               st, x[0], x[1], opt, nodes, exact);
    }

    /* 5) battery arbitrage n=2: price [0.2,0.8], cap 10, power 5,
          eta 1.0, soc 0.5/0.2/0.8/0.5
          -> charge 3 kWh at t0, discharge 3 kWh at t1, opt=1.8
          Variables: chg0,chg1,dis0,dis1,u0,u1,soc0,soc1 = 8 vars
          Constraints:
            chg0-5u0<=0, chg1-5u1<=0
            dis0+5u0<=5, dis1+5u1<=5
            u0<=1, u1<=1
            soc0<=0.8, soc1<=0.8
            -soc0<=-0.2, -soc1<=-0.2
            soc0 - chg0 + dis0 = 0.5   (eta=1,dt=1,cap=10)
            soc1 - soc0 - chg1 + dis1 = 0
            soc1 = 0.5
          objective: max 0.8dis0+0.8dis1 - 0.2chg0 - 0.2chg1 (wait dt=1, price)
          Actually objective max sum price[t]*(dis-chg) = 0.8*(dis0+dis1)-0.2*(chg0+chg1)
          With price=[0.2,0.8]: dis0,dis1 coeff 0.2,0.8; chg0,chg1 coeff -0.2,-0.8 */
    {
        int nv = 8;
        double c[8] = {0};
        /* indices: 0,1=chg0,chg1; 2,3=dis0,dis1; 4,5=u0,u1; 6,7=soc0,soc1 */
        c[0] = -0.2; c[1] = -0.8;   /* chg0, chg1 */
        c[2] = 0.2;  c[3] = 0.8;    /* dis0, dis1 */
        double A[16][8];
        double b[16];
        int m = 0;
        for (int t = 0; t < 2; t++) {
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][t] = 1; A[m][4 + t] = -5; b[m] = 0; m++;          /* chg - 5u <= 0 */
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][2 + t] = 1; A[m][4 + t] = 5; b[m] = 5; m++;       /* dis + 5u <= 5 */
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][4 + t] = 1; b[m] = 1; m++;                        /* u <= 1 */
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][6 + t] = 1; b[m] = 0.8; m++;                      /* soc <= 0.8 */
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][6 + t] = -1; b[m] = -0.2; m++;                    /* soc >= 0.2 */
        }
        /* soc0 - 0.1*chg0 + 0.1*dis0 = 0.5   (eta=1, dt=1, cap=10) */
        for (int j = 0; j < nv; j++) A[m][j] = 0;
        A[m][6] = 1; A[m][0] = -0.1; A[m][2] = 0.1; b[m] = 0.5; m++;
        for (int j = 0; j < nv; j++) A[m][j] = 0;
        A[m][6] = -1; A[m][0] = 0.1; A[m][2] = -0.1; b[m] = -0.5; m++;
        /* soc1 - soc0 - 0.1*chg1 + 0.1*dis1 = 0 */
        for (int j = 0; j < nv; j++) A[m][j] = 0;
        A[m][7] = 1; A[m][6] = -1; A[m][1] = -0.1; A[m][3] = 0.1; b[m] = 0; m++;
        for (int j = 0; j < nv; j++) A[m][j] = 0;
        A[m][7] = -1; A[m][6] = 1; A[m][1] = 0.1; A[m][3] = -0.1; b[m] = 0; m++;
        /* soc1 = 0.5 */
        for (int j = 0; j < nv; j++) A[m][j] = 0;
        A[m][7] = 1; b[m] = 0.5; m++;
        for (int j = 0; j < nv; j++) A[m][j] = 0;
        A[m][7] = -1; b[m] = -0.5; m++;
        int bin[2] = {4, 5};
        milp_prob p = {nv, m, c, &A[0][0], b, 2, bin, 100};
        st = milp_solve_max(&p, x, &opt, &nodes, &exact, &bb);
        printf("5) st=%d chg=(%g,%g) dis=(%g,%g) u=(%g,%g) soc=(%g,%g) opt=%g\n",
               st, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], opt);
        printf("   expect chg0=3, dis1=3, opt=1.8\n");

        /* 6) same problem as 5 but pure LP (u continuous), then check constraints */
        {
            int nv = 8;
            double c[8] = {0};
            c[0] = -0.2; c[1] = -0.8;
            c[2] = 0.2;  c[3] = 0.8;
            double A[16][8];
            double b[16];
            int m = 0;
            for (int t = 0; t < 2; t++) {
                for (int j = 0; j < nv; j++) A[m][j] = 0;
                A[m][t] = 1; A[m][4 + t] = -5; b[m] = 0; m++;
                for (int j = 0; j < nv; j++) A[m][j] = 0;
                A[m][2 + t] = 1; A[m][4 + t] = 5; b[m] = 5; m++;
                for (int j = 0; j < nv; j++) A[m][j] = 0;
                A[m][4 + t] = 1; b[m] = 1; m++;
                for (int j = 0; j < nv; j++) A[m][j] = 0;
                A[m][6 + t] = 1; b[m] = 0.8; m++;
                for (int j = 0; j < nv; j++) A[m][j] = 0;
                A[m][6 + t] = -1; b[m] = -0.2; m++;
            }
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][6] = 1; A[m][0] = -0.1; A[m][2] = 0.1; b[m] = 0.5; m++;
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][6] = -1; A[m][0] = 0.1; A[m][2] = -0.1; b[m] = -0.5; m++;
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][7] = 1; A[m][6] = -1; A[m][1] = -0.1; A[m][3] = 0.1; b[m] = 0; m++;
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][7] = -1; A[m][6] = 1; A[m][1] = 0.1; A[m][3] = -0.1; b[m] = 0; m++;
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][7] = 1; b[m] = 0.5; m++;
            for (int j = 0; j < nv; j++) A[m][j] = 0;
            A[m][7] = -1; b[m] = -0.5; m++;
            st = lp_solve_max(nv, m, c, &A[0][0], b, x, &opt);
            printf("6) LP st=%d chg=(%g,%g) dis=(%g,%g) u=(%g,%g) soc=(%g,%g) opt=%g\n",
                   st, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], opt);
            printf("   terminal check: soc1=0.5? %s\n",
                   (fabs(x[7] - 0.5) < 1e-6) ? "YES" : "NO");
        }
    }
    return 0;
}
