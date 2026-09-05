/*
 * solver.c - battery charge/discharge MILP optimization core.
 *
 * Three strategies (the "C strategy" set):
 *   1. arbitrage         - peak/valley arbitrage: maximize price-arbitrage profit
 *   2. forecast          - dynamic forecast optimization: minimize energy cost
 *                          given load/PV forecasts (receding-horizon ready)
 *   3. demand_response   - demand response / peak shaving: minimize peak demand
 *                          and energy cost, against a demand limit / DR signals
 *
 * Model (n = horizon, dt = time_step_hours, e[t] = eta_chg*chg[t] - dis[t]/eta_dis):
 *   variables:  chg[t], dis[t] >= 0     grid power into/out of the battery [kW]
 *               u[t]  in {0,1}          charging indicator (forecast/demand_response)
 *               grid[t] >= 0            purchased power (forecast/demand_response)
 *               M                      demand peak (demand_response)
 *   SOC is eliminated:  SOC[t] = soc_init + sum_{k<=t} e[k]*dt/cap, enforced by
 *   cumulative inequalities  soc_min <= SOC[t] <= soc_max  and the terminal
 *   equality  SOC[n-1] = soc_final.
 *
 *   arbitrage:       max  sum_t price[t]*(dis[t]-chg[t])*dt     (pure LP:
 *                    with price >= 0 a simultaneous chg/dis is never optimal, so
 *                    no 0-1 variables are needed; the LP optimum = MILP optimum)
 *   forecast:        min  sum_t price[t]*grid[t]*dt,  grid = load-pv+chg-dis
 *   demand_response: min  demand_price*M + sum_t price[t]*grid[t]*dt
 *
 * Solved by a dense simplex LP plus branch & bound (lp.c / milp.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "json_util.h"
#include "solver.h"
#include "lp.h"
#include "milp.h"

#define RESPONSE_MAX (1u << 17)
#define MAX_HORIZON  96
#define NODE_CAP     4000

enum { STRAT_ARBITRAGE = 0, STRAT_FORECAST = 1, STRAT_DR = 2 };

typedef struct {
    int n;
    int strategy;
    double* price;
    double dt;
    double cap;
    double p_chg, p_dis;
    double eta_chg, eta_dis;
    double soc_init, soc_min, soc_max, soc_final;
    double* load;           /* forecast/dr */
    double* pv;             /* forecast/dr, optional */
    double demand_price;    /* dr */
    double demand_limit;    /* dr, 0 = inactive */
    char* dr_signal;        /* dr, optional */
} opt_input;

typedef struct {
    int nvars;
    int ncons_cap, ncons;
    double* c;
    double* A;
    double* b;
    int nbin, nbin_cap;
    int* bin;
} model;

static double obj_num(const json_value* obj, const char* key, double dflt) {
    return json_num(json_get(obj, key), dflt);
}

static int fail(const char* msg, char* out, size_t out_size) {
    snprintf(out, out_size, "{\"status\":\"error\",\"message\":\"%s\"}", msg);
    return -1;
}

/* ---- variable index layout ----
 * chg(0..n-1) | dis(n..2n-1) | u(2n..3n-1) | grid(3n..4n-1) | M(4n)  */
static inline int vidx_chg(int n, int t)   { (void)n; return t; }
static inline int vidx_dis(int n, int t)   { return n + t; }
static inline int vidx_u(int n, int t)     { return 2 * n + t; }
static inline int vidx_grid(int n, int t)  { return 3 * n + t; }
static inline int vidx_m(int n)            { return 4 * n; }

/* ---- model builder helpers ---- */
static void model_free(model* mo);

static model* model_new(int nvars, int ncons_cap) {
    model* mo = (model*)calloc(1, sizeof(model));
    if (!mo) return NULL;
    mo->nvars = nvars;
    mo->ncons_cap = ncons_cap;
    mo->c = (double*)calloc((size_t)nvars, sizeof(double));
    mo->A = (double*)calloc((size_t)ncons_cap * nvars, sizeof(double));
    mo->b = (double*)malloc((size_t)ncons_cap * sizeof(double));
    mo->nbin_cap = nvars;
    mo->bin = (int*)malloc((size_t)nvars * sizeof(int));
    if (!mo->c || !mo->A || !mo->b || !mo->bin) {
        model_free(mo);
        return NULL;
    }
    return mo;
}

static void model_free(model* mo) {
    if (!mo) return;
    free(mo->c);
    free(mo->A);
    free(mo->b);
    free(mo->bin);
    free(mo);
}

static int model_grow(model* mo) {
    int newcap = mo->ncons_cap ? mo->ncons_cap * 2 : 32;
    double* A2 = (double*)realloc(mo->A, (size_t)newcap * mo->nvars * sizeof(double));
    double* b2 = (double*)realloc(mo->b, (size_t)newcap * sizeof(double));
    if (!A2 || !b2) return -1;
    mo->A = A2;
    mo->b = b2;
    mo->ncons_cap = newcap;
    return 0;
}

static int model_add_con(model* mo, const double* coeff, double rhs) {
    if (mo->ncons >= mo->ncons_cap && model_grow(mo) != 0) return -1;
    for (int j = 0; j < mo->nvars; j++)
        mo->A[(size_t)mo->ncons * mo->nvars + j] = coeff[j];
    mo->b[mo->ncons] = rhs;
    mo->ncons++;
    return 0;
}

static int model_add_eq(model* mo, const double* coeff, double rhs) {
    if (model_add_con(mo, coeff, rhs) != 0) return -1;
    if (mo->ncons >= mo->ncons_cap && model_grow(mo) != 0) return -1;
    for (int j = 0; j < mo->nvars; j++)
        mo->A[(size_t)mo->ncons * mo->nvars + j] = -coeff[j];
    mo->b[mo->ncons] = -rhs;
    mo->ncons++;
    return 0;
}
/* ---- model construction ---- */
/* Power limits; with_binary=1 adds 0-1 charging indicators that forbid
 * simultaneous charging and discharging. */
static int build_power(model* mo, const opt_input* in, double* coeff, int with_binary) {
    int n = in->n;
    for (int t = 0; t < n; t++) {
        memset(coeff, 0, (size_t)mo->nvars * sizeof(double));
        coeff[vidx_chg(n, t)] = 1.0;
        if (with_binary) {
            coeff[vidx_u(n, t)] = -in->p_chg;
            if (model_add_con(mo, coeff, 0.0) != 0) return -1;          /* chg <= p_chg*u */
            memset(coeff, 0, (size_t)mo->nvars * sizeof(double));
            coeff[vidx_dis(n, t)] = 1.0;
            coeff[vidx_u(n, t)] = in->p_dis;
            if (model_add_con(mo, coeff, in->p_dis) != 0) return -1;    /* dis <= p_dis(1-u) */
            memset(coeff, 0, (size_t)mo->nvars * sizeof(double));
            coeff[vidx_u(n, t)] = 1.0;
            if (model_add_con(mo, coeff, 1.0) != 0) return -1;          /* u <= 1 */
            mo->bin[mo->nbin++] = vidx_u(n, t);
        } else {
            if (model_add_con(mo, coeff, in->p_chg) != 0) return -1;    /* chg <= p_chg */
            memset(coeff, 0, (size_t)mo->nvars * sizeof(double));
            coeff[vidx_dis(n, t)] = 1.0;
            if (model_add_con(mo, coeff, in->p_dis) != 0) return -1;    /* dis <= p_dis */
        }
    }
    return 0;
}

/* SOC bounds via cumulative energy (SOC variables eliminated):
 * e[t] = eta_chg*chg[t]*dt - dis[t]*dt/eta_dis  (kWh into battery)
 * soc_min <= soc_init + sum_{k<=t} e[k]/cap <= soc_max
 * SOC[n-1] = soc_final. */
static int build_soc_bounds(model* mo, const opt_input* in, double* coeff) {
    int n = in->n;
    double* cum = (double*)calloc((size_t)mo->nvars, sizeof(double));
    if (!cum) return -1;
    for (int t = 0; t < n; t++) {
        cum[vidx_chg(n, t)] += in->eta_chg * in->dt;
        cum[vidx_dis(n, t)] -= in->dt / in->eta_dis;
        /* upper: sum e <= (soc_max - soc_init)*cap */
        memcpy(coeff, cum, (size_t)mo->nvars * sizeof(double));
        if (model_add_con(mo, coeff, (in->soc_max - in->soc_init) * in->cap) != 0) {
            free(cum); return -1;
        }
        /* lower: -sum e <= (soc_init - soc_min)*cap */
        for (int j = 0; j < mo->nvars; j++) coeff[j] = -cum[j];
        if (model_add_con(mo, coeff, (in->soc_init - in->soc_min) * in->cap) != 0) {
            free(cum); return -1;
        }
    }
    /* terminal: sum e = (soc_final - soc_init)*cap */
    memcpy(coeff, cum, (size_t)mo->nvars * sizeof(double));
    int rc = model_add_eq(mo, coeff, (in->soc_final - in->soc_init) * in->cap);
    free(cum);
    return rc;
}

/* grid balance: grid[t] - chg[t] + dis[t] = load[t] - pv[t]  (forecast/dr) */
static int build_grid(model* mo, const opt_input* in, double* coeff) {
    int n = in->n;
    for (int t = 0; t < n; t++) {
        memset(coeff, 0, (size_t)mo->nvars * sizeof(double));
        coeff[vidx_grid(n, t)] = 1.0;
        coeff[vidx_chg(n, t)] = -1.0;
        coeff[vidx_dis(n, t)] = 1.0;
        double rhs = in->load[t] - (in->pv ? in->pv[t] : 0.0);
        if (model_add_eq(mo, coeff, rhs) != 0) return -1;
    }
    return 0;
}

static int build_arbitrage(model* mo, const opt_input* in, double* coeff) {
    int n = in->n;
    for (int t = 0; t < n; t++) {
        mo->c[vidx_dis(n, t)] += in->price[t] * in->dt;
        mo->c[vidx_chg(n, t)] -= in->price[t] * in->dt;
    }
    if (build_power(mo, in, coeff, 0) != 0) return -1;
    return build_soc_bounds(mo, in, coeff);
}

static int build_forecast(model* mo, const opt_input* in, double* coeff) {
    int n = in->n;
    for (int t = 0; t < n; t++)
        mo->c[vidx_grid(n, t)] = -in->price[t] * in->dt;
    if (build_grid(mo, in, coeff) != 0) return -1;
    if (build_power(mo, in, coeff, 1) != 0) return -1;
    return build_soc_bounds(mo, in, coeff);
}

static int build_dr(model* mo, const opt_input* in, double* coeff) {
    int n = in->n;
    for (int t = 0; t < n; t++)
        mo->c[vidx_grid(n, t)] = -in->price[t] * in->dt;
    mo->c[vidx_m(n)] = -in->demand_price;

    if (build_grid(mo, in, coeff) != 0) return -1;

    /* peak constraints: grid[t] - M <= 0 (DR hours only if dr_signal given) */
    for (int t = 0; t < n; t++) {
        if (in->dr_signal && in->dr_signal[t] == 0) continue;
        memset(coeff, 0, (size_t)mo->nvars * sizeof(double));
        coeff[vidx_grid(n, t)] = 1.0;
        coeff[vidx_m(n)] = -1.0;
        if (model_add_con(mo, coeff, 0.0) != 0) return -1;
    }
    if (in->demand_limit > 0.0) {
        for (int t = 0; t < n; t++) {
            memset(coeff, 0, (size_t)mo->nvars * sizeof(double));
            coeff[vidx_grid(n, t)] = 1.0;
            if (model_add_con(mo, coeff, in->demand_limit) != 0) return -1;
        }
    }
    if (build_power(mo, in, coeff, 1) != 0) return -1;
    return build_soc_bounds(mo, in, coeff);
}
/* ---- solve a parsed request and write the JSON response ---- */
static int solve_response(opt_input* in, const char* strat_s,
                          char* out, size_t out_size) {
    int n = in->n;
    int has_grid = in->strategy != STRAT_ARBITRAGE;
    int nvars = (in->strategy == STRAT_ARBITRAGE) ? 2 * n :
                (in->strategy == STRAT_FORECAST) ? 4 * n : 4 * n + 1;

    model* mo = model_new(nvars, 10 * n + 32);
    double* coeff = (double*)malloc((size_t)nvars * sizeof(double));
    if (!mo || !coeff) {
        model_free(mo);
        free(coeff);
        return fail("out of memory", out, out_size);
    }

    int berr = -1;
    switch (in->strategy) {
        case STRAT_ARBITRAGE: berr = build_arbitrage(mo, in, coeff); break;
        case STRAT_FORECAST:  berr = build_forecast(mo, in, coeff);  break;
        default:              berr = build_dr(mo, in, coeff);        break;
    }
    if (berr != 0) {
        model_free(mo);
        free(coeff);
        return fail("failed to build the MILP model", out, out_size);
    }

    milp_prob prob;
    prob.n = nvars;
    prob.m = mo->ncons;
    prob.c = mo->c;
    prob.A = mo->A;
    prob.b = mo->b;
    prob.nb = mo->nbin;      /* 0 for arbitrage (pure LP) */
    prob.bin = mo->bin;
    prob.node_cap = NODE_CAP;

    double* x = (double*)malloc((size_t)nvars * sizeof(double));
    if (!x) {
        model_free(mo);
        free(coeff);
        return fail("out of memory", out, out_size);
    }
    double opt_val = 0.0, best_bound = 0.0;
    int nodes_used = 0, exact = 1;
    int st = milp_solve_max(&prob, x, &opt_val, &nodes_used, &exact, &best_bound);
    if (st != 1) {
        free(x);
        model_free(mo);
        free(coeff);
        return fail("no feasible solution for the given parameters", out, out_size);
    }

    /* arbitrage is solved as a pure LP: replace any residual simultaneous
     * charge/discharge with the equivalent pure action (same net energy e[t],
     * same SOC trajectory, profit never worse). */
    if (in->strategy == STRAT_ARBITRAGE) {
        for (int t = 0; t < n; t++) {
            double c = x[vidx_chg(n, t)], d = x[vidx_dis(n, t)];
            if (c > 1e-9 && d > 1e-9) {
                double e = in->eta_chg * c - d / in->eta_dis;
                if (e >= 0.0) { x[vidx_chg(n, t)] = e / in->eta_chg; x[vidx_dis(n, t)] = 0.0; }
                else          { x[vidx_dis(n, t)] = -e * in->eta_dis; x[vidx_chg(n, t)] = 0.0; }
            }
        }
    }

    /* SOC trajectory */
    double* soc = (double*)malloc((size_t)n * sizeof(double));
    if (!soc) {
        free(x); model_free(mo); free(coeff);
        return fail("out of memory", out, out_size);
    }
    double s = in->soc_init;
    for (int t = 0; t < n; t++) {
        s += (in->eta_chg * x[vidx_chg(n, t)] - x[vidx_dis(n, t)] / in->eta_dis)
             * in->dt / in->cap;
        soc[t] = s;
    }

    double peak = 0.0, dr_peak = -1.0;
    if (has_grid) {
        for (int t = 0; t < n; t++) {
            double g = x[vidx_grid(n, t)];
            if (g > peak) peak = g;
            if (in->dr_signal && in->dr_signal[t] == 1 && g > dr_peak) dr_peak = g;
        }
    }

    double gap_pct = 0.0;
    if (fabs(opt_val) > 1e-9)
        gap_pct = (best_bound - opt_val) / fabs(opt_val) * 100.0;

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_size - pos,
        "{\"status\":\"ok\","
        "\"strategy\":\"%s\","
        "\"solver\":\"milp_simplex_bb\","
        "\"exact\":%d,"
        "\"nodes_used\":%d,"
        "\"best_bound\":%.10g,"
        "\"gap_pct\":%.6g,"
        "\"horizon\":%d,"
        "\"time_step_hours\":%.10g,",
        strat_s, exact ? 1 : 0, nodes_used, best_bound, gap_pct, n, in->dt);

    if (in->strategy == STRAT_ARBITRAGE) {
        pos += (size_t)snprintf(out + pos, out_size - pos,
            "\"total_profit_yuan\":%.10g,", opt_val);
    } else {
        pos += (size_t)snprintf(out + pos, out_size - pos,
            "\"total_cost_yuan\":%.10g,\"peak_kw\":%.10g,", -opt_val, peak);
        if (in->dr_signal) {
            if (dr_peak < 0.0) dr_peak = 0.0;
            pos += (size_t)snprintf(out + pos, out_size - pos,
                "\"dr_peak_kw\":%.10g,", dr_peak);
        }
    }

    pos += (size_t)snprintf(out + pos, out_size - pos,
        "\"final_soc\":%.10g,\"plan\":[", soc[n - 1]);

    for (int t = 0; t < n && pos < out_size; t++) {
        int w;
        if (has_grid) {
            w = snprintf(out + pos, out_size - pos,
                "%s{\"t\":%d,\"charge_kw\":%.10g,\"discharge_kw\":%.10g,\"grid_kw\":%.10g,\"soc\":%.10g}",
                t ? "," : "", t,
                x[vidx_chg(n, t)], x[vidx_dis(n, t)],
                x[vidx_grid(n, t)], soc[t]);
        } else {
            w = snprintf(out + pos, out_size - pos,
                "%s{\"t\":%d,\"charge_kw\":%.10g,\"discharge_kw\":%.10g,\"soc\":%.10g}",
                t ? "," : "", t, x[vidx_chg(n, t)], x[vidx_dis(n, t)], soc[t]);
        }
        if (w < 0) break;
        pos += (size_t)w;
    }
    if (pos >= out_size) pos = out_size - 1;
    snprintf(out + pos, out_size - pos, "]}");

    free(soc);
    free(x);
    model_free(mo);
    free(coeff);
    return 0;
}
int run_optimize(const char* request_json, char* out, size_t out_size) {
    json_value* root = json_parse(request_json);
    if (!root) return fail("request is not valid JSON", out, out_size);

    const char* strat_s = json_str(json_get(root, "strategy"), "arbitrage");
    int strategy;
    if (strcmp(strat_s, "arbitrage") == 0) strategy = STRAT_ARBITRAGE;
    else if (strcmp(strat_s, "forecast") == 0) strategy = STRAT_FORECAST;
    else if (strcmp(strat_s, "demand_response") == 0) strategy = STRAT_DR;
    else {
        json_free(root);
        return fail("strategy must be one of: arbitrage, forecast, demand_response",
                    out, out_size);
    }

    const json_value* soc = json_get(root, "soc");
    const json_value* bat = json_get(root, "battery");
    if (!soc || soc->type != JSON_OBJECT) {
        json_free(root);
        return fail("missing 'soc' object", out, out_size);
    }
    if (!bat || bat->type != JSON_OBJECT) {
        json_free(root);
        return fail("missing 'battery' object", out, out_size);
    }

    const json_value* price_v = json_get(root, "price");
    if (!price_v) price_v = json_get(root, "prices");
    if (!price_v) price_v = json_get(root, "price_forecast");
    if (!price_v || price_v->type != JSON_ARRAY || price_v->array.count == 0) {
        json_free(root);
        return fail("missing non-empty 'price' array", out, out_size);
    }
    int n = price_v->array.count;
    if (n > MAX_HORIZON) {
        json_free(root);
        return fail("horizon too long (max 96 periods)", out, out_size);
    }

    opt_input in;
    memset(&in, 0, sizeof(in));
    in.n = n;
    in.strategy = strategy;
    in.cap = obj_num(bat, "capacity_kwh", -1.0);
    in.p_chg = obj_num(bat, "max_charge_kw", 0.0);
    in.p_dis = obj_num(bat, "max_discharge_kw", 0.0);
    in.eta_chg = obj_num(bat, "charge_efficiency", 1.0);
    in.eta_dis = obj_num(bat, "discharge_efficiency", 1.0);
    in.soc_init = obj_num(soc, "initial", -1.0);
    in.soc_min = obj_num(soc, "min", 0.0);
    in.soc_max = obj_num(soc, "max", 1.0);
    in.soc_final = obj_num(soc, "final", in.soc_init);
    in.dt = obj_num(root, "time_step_hours", 1.0);
    in.demand_price = obj_num(root, "demand_price_per_kw", 10.0);
    in.demand_limit = obj_num(root, "demand_limit_kw", 0.0);

    if (in.cap <= 0.0) { json_free(root); return fail("battery.capacity_kwh must be > 0", out, out_size); }
    if (in.p_chg < 0.0 || in.p_dis < 0.0) { json_free(root); return fail("max charge/discharge power must be >= 0", out, out_size); }
    if (in.eta_chg <= 0.0 || in.eta_chg > 1.0 || in.eta_dis <= 0.0 || in.eta_dis > 1.0) {
        json_free(root); return fail("efficiencies must be in (0,1]", out, out_size);
    }
    if (in.soc_min < 0.0 || in.soc_max > 1.0 || in.soc_min >= in.soc_max) {
        json_free(root); return fail("invalid soc range (need 0 <= min < max <= 1)", out, out_size);
    }
    if (in.soc_init < in.soc_min || in.soc_init > in.soc_max) {
        json_free(root); return fail("soc.initial must lie in [soc.min, soc.max]", out, out_size);
    }
    if (in.soc_final < in.soc_min || in.soc_final > in.soc_max) {
        json_free(root); return fail("soc.final must lie in [soc.min, soc.max]", out, out_size);
    }
    if (in.dt <= 0.0) { json_free(root); return fail("time_step_hours must be > 0", out, out_size); }
    if (in.demand_price < 0.0 || in.demand_limit < 0.0) {
        json_free(root); return fail("demand_price_per_kw and demand_limit_kw must be >= 0", out, out_size);
    }

    in.price = (double*)malloc((size_t)n * sizeof(double));
    if (!in.price) { json_free(root); return fail("out of memory", out, out_size); }
    for (int i = 0; i < n; i++) {
        const json_value* pv = price_v->array.items[i];
        if (!pv || pv->type != JSON_NUMBER || pv->number < 0.0) {
            free(in.price);
            json_free(root);
            return fail("price entries must be non-negative numbers", out, out_size);
        }
        in.price[i] = pv->number;
    }
    /* forecast / demand_response need a load forecast */
    if (strategy != STRAT_ARBITRAGE) {
        const json_value* load_v = json_get(root, "load_kw");
        if (!load_v) load_v = json_get(root, "load");
        if (!load_v || load_v->type != JSON_ARRAY || load_v->array.count != n) {
            free(in.price);
            json_free(root);
            return fail("forecast/demand_response require 'load_kw' array of same length as 'price'",
                        out, out_size);
        }
        in.load = (double*)malloc((size_t)n * sizeof(double));
        if (!in.load) { free(in.price); json_free(root); return fail("out of memory", out, out_size); }
        for (int i = 0; i < n; i++) {
            const json_value* lv = load_v->array.items[i];
            if (!lv || lv->type != JSON_NUMBER || lv->number < 0.0) {
                free(in.load); free(in.price); json_free(root);
                return fail("load_kw entries must be non-negative numbers", out, out_size);
            }
            in.load[i] = lv->number;
        }

        const json_value* pv_v = json_get(root, "pv_kw");
        if (!pv_v) pv_v = json_get(root, "pv");
        if (pv_v) {
            if (pv_v->type != JSON_ARRAY || pv_v->array.count != n) {
                free(in.load); free(in.price); json_free(root);
                return fail("pv_kw array must have same length as 'price'", out, out_size);
            }
            in.pv = (double*)malloc((size_t)n * sizeof(double));
            if (!in.pv) { free(in.load); free(in.price); json_free(root); return fail("out of memory", out, out_size); }
            for (int i = 0; i < n; i++) {
                const json_value* lv = pv_v->array.items[i];
                if (!lv || lv->type != JSON_NUMBER || lv->number < 0.0) {
                    free(in.pv); free(in.load); free(in.price); json_free(root);
                    return fail("pv_kw entries must be non-negative numbers", out, out_size);
                }
                in.pv[i] = lv->number;
            }
        }

        if (strategy == STRAT_DR) {
            const json_value* dr_v = json_get(root, "dr_signal");
            if (dr_v) {
                if (dr_v->type != JSON_ARRAY || dr_v->array.count != n) {
                    free(in.pv); free(in.load); free(in.price); json_free(root);
                    return fail("dr_signal array must have same length as 'price'", out, out_size);
                }
                in.dr_signal = (char*)malloc((size_t)n);
                if (!in.dr_signal) {
                    free(in.pv); free(in.load); free(in.price); json_free(root);
                    return fail("out of memory", out, out_size);
                }
                for (int i = 0; i < n; i++) {
                    double v = json_num(dr_v->array.items[i], -1.0);
                    if (v != 0.0 && v != 1.0) {
                        free(in.dr_signal); free(in.pv); free(in.load);
                        free(in.price); json_free(root);
                        return fail("dr_signal entries must be 0 or 1", out, out_size);
                    }
                    in.dr_signal[i] = (char)v;
                }
            }
        }
    }

    int rc = solve_response(&in, strat_s, out, out_size);

    free(in.price);
    free(in.load);
    free(in.pv);
    free(in.dr_signal);
    json_free(root);
    return rc;
}




