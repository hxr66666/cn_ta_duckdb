// ============================================================
// cn_ta_indicators.cpp
//
// "懒人函数" cn_ta_indicators —— 传入一个子查询（OHLCV 等 K 线数据），
// 一次性批量计算所有可计算的 TA-Lib 指标，输出一张宽表。
//
// 用法：
//   SELECT * FROM cn_ta_indicators((
//       SELECT 日期, 开盘, 最高, 最低, 收盘, 成交量, amount, outstanding_share, turnover
//       FROM OHLC_xxx WHERE xxx = xx
//   ));
//
// 实现：
//   - TableFunction in-out 模式：in_out_function 累积所有输入 chunk 到内存，
//     in_out_function_final 一次性批量调用 TA-Lib（O(N)，性能最优），逐 chunk 输出。
//   - 输入列固定为 9 列：ts, open, high, low, close, volume, amount,
//     outstanding_share, turnover（金额/流通股本/换手率为可空，暂用于 A 股适配指标）。
//   - 输出 = 原始 9 列 + 全部可计算指标列（约 100 列）。
// ============================================================

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/timestamp.hpp"

extern "C" {
#include "ta_libc.h"
}

#include <vector>
#include <string>
#include <cstring>
#include <cmath>

namespace duckdb {

// 输入列名（固定顺序）
static const std::vector<std::string> INPUT_COLS = {
    "ts", "open", "high", "low", "close", "volume", "amount", "outstanding_share", "turnover"
};

// ============================================================
// 指标定义（name, period，用 X-macro 风格的 lambda 表）
// 只列"能基于 OHLCV 计算"的单序列指标。
// ============================================================

// P1：单序列 + period（基于 close）
struct P1Def { const char *name; TA_RetCode (*func)(int, int, const double[], int, int *, int *, double[]); int period; };
static const std::vector<P1Def> P1_INDICATORS = {
    {"sma", TA_SMA, 5}, {"sma", TA_SMA, 10}, {"sma", TA_SMA, 20}, {"sma", TA_SMA, 60},
    {"ema", TA_EMA, 5}, {"ema", TA_EMA, 10}, {"ema", TA_EMA, 20}, {"ema", TA_EMA, 60},
    {"wma", TA_WMA, 10}, {"dema", TA_DEMA, 10}, {"tema", TA_TEMA, 10}, {"trima", TA_TRIMA, 10},
    {"kama", TA_KAMA, 10}, {"midpoint", TA_MIDPOINT, 10},
    {"rsi", TA_RSI, 6}, {"rsi", TA_RSI, 12}, {"rsi", TA_RSI, 24},
    {"cmo", TA_CMO, 14}, {"mom", TA_MOM, 10}, {"roc", TA_ROC, 10}, {"rocp", TA_ROCP, 10},
    {"rocr", TA_ROCR, 10}, {"rocr100", TA_ROCR100, 10}, {"trix", TA_TRIX, 12},
    {"linearreg", TA_LINEARREG, 14}, {"linearreg_slope", TA_LINEARREG_SLOPE, 14},
    {"linearreg_intercept", TA_LINEARREG_INTERCEPT, 14}, {"linearreg_angle", TA_LINEARREG_ANGLE, 14},
    {"tsf", TA_TSF, 14}, {"sum", TA_SUM, 10}, {"max", TA_MAX, 10}, {"min", TA_MIN, 10},
};

// P3：high/low/close + period
struct P3Def { const char *name; TA_RetCode (*func)(int, int, const double[], const double[], const double[], int, int *, int *, double[]); int period; };
static const std::vector<P3Def> P3_INDICATORS = {
    {"atr", TA_ATR, 14}, {"natr", TA_NATR, 14},
    {"willr", TA_WILLR, 14}, {"cci", TA_CCI, 14},
    {"adx", TA_ADX, 14}, {"adxr", TA_ADXR, 14}, {"dx", TA_DX, 14},
    {"plus_di", TA_PLUS_DI, 14}, {"minus_di", TA_MINUS_DI, 14},
};

// P4：high/low/close/volume（无 period）
struct P4Def { const char *name; TA_RetCode (*func)(int, int, const double[], const double[], const double[], const double[], int *, int *, double[]); };
static const std::vector<P4Def> P4_INDICATORS = {
    {"ad", TA_AD},
};

// P5：open/high/low/close（无 period，返回 DOUBLE）
struct P5Def { const char *name; TA_RetCode (*func)(int, int, const double[], const double[], const double[], const double[], int *, int *, double[]); };
static const std::vector<P5Def> P5_INDICATORS = {
    {"avgprice", TA_AVGPRICE}, {"bop", TA_BOP},
};

// P6：high/low（无 period）
struct P6Def { const char *name; TA_RetCode (*func)(int, int, const double[], const double[], int *, int *, double[]); };
static const std::vector<P6Def> P6_INDICATORS = {
    {"medprice", TA_MEDPRICE},
};

// P7：high/low/close（无 period）
struct P7Def { const char *name; TA_RetCode (*func)(int, int, const double[], const double[], const double[], int *, int *, double[]); };
static const std::vector<P7Def> P7_INDICATORS = {
    {"trange", TA_TRANGE}, {"typprice", TA_TYPPRICE}, {"wclprice", TA_WCLPRICE},
};

// P8：high/low + period
struct P8Def { const char *name; TA_RetCode (*func)(int, int, const double[], const double[], int, int *, int *, double[]); int period; };
static const std::vector<P8Def> P8_INDICATORS = {
    {"midprice", TA_MIDPRICE, 14}, {"plus_dm", TA_PLUS_DM, 14}, {"minus_dm", TA_MINUS_DM, 14},
};

// ============================================================
// Bind / State
// ============================================================

struct CnTaIndicatorsBindData : public FunctionData {
    vector<LogicalType> input_types;

    unique_ptr<FunctionData> Copy() const override {
        auto res = make_uniq<CnTaIndicatorsBindData>();
        res->input_types = input_types;
        return std::move(res);
    }
    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<CnTaIndicatorsBindData>();
        return input_types == other.input_types;
    }
};

// 全局状态：累积所有输入数据
struct CnTaIndicatorsGlobalState : public GlobalTableFunctionState {
    // 累积的原始列（0=ts, 1=open, 2=high, 3=low, 4=close, 5=volume,
    //               6=amount, 7=outstanding_share, 8=turnover）
    vector<vector<double>> cols; // 数值列
    vector<timestamp_t> ts_col;  // 时间列

    // 结果列（每列一个 vector<double>，长度 = 行数）
    vector<string> out_names;
    vector<vector<double>> out_cols;

    // 输出游标
    idx_t out_row = 0;
    bool computed = false;
    idx_t input_ncols = 0; // 实际输入列数（由 in_out 第一次调用时设置）

    CnTaIndicatorsGlobalState() : cols(9) {}
    idx_t MaxThreads() const override { return 1; } // 累积需串行
};

// ============================================================
// 计算所有指标（在 final 阶段调用一次）
// ============================================================

// 把 TA-Lib 输出（含 outBeg 前导）展开成与输入等长的列，前导填 NaN
static void ExpandResult(vector<double> &dst, const double *outReal, int outBeg, int outNb, int size) {
    dst.assign(size, std::numeric_limits<double>::quiet_NaN());
    if (outNb <= 0) return;
    for (int i = 0; i < outNb; i++) {
        dst[outBeg + i] = outReal[i];
    }
}

// ============================================================
// Eigen 统计核心（口径与 cn_ta_stat.cpp / cn_ta_stat_agg.cpp 一致）
// ============================================================
static const double STAT_NAN = std::numeric_limits<double>::quiet_NaN();

static double StatMean(const std::vector<double> &x) {
    if (x.empty()) return STAT_NAN;
    double s = 0.0;
    for (double v : x) s += v;
    return s / (double)x.size();
}

static double StatVariance(const std::vector<double> &x) {
    if (x.size() < 2) return STAT_NAN;
    double mean = StatMean(x);
    double s = 0.0;
    for (double v : x) s += (v - mean) * (v - mean);
    return s / (double)(x.size() - 1); // 样本方差
}

static double StatSkew(const std::vector<double> &x) {
    if (x.size() < 3) return STAT_NAN;
    double mean = StatMean(x);
    double m2 = 0.0, m3 = 0.0;
    for (double v : x) {
        double d = v - mean;
        m2 += d * d;
        m3 += d * d * d;
    }
    m2 /= x.size();
    m3 /= x.size();
    if (m2 == 0.0) return STAT_NAN;
    return m3 / std::pow(m2, 1.5); // 矩法偏度
}

static double StatKurtosis(const std::vector<double> &x) {
    if (x.size() < 4) return STAT_NAN;
    double mean = StatMean(x);
    double m2 = 0.0, m4 = 0.0;
    for (double v : x) {
        double d = v - mean;
        m2 += d * d;
        m4 += d * d * d * d;
    }
    m2 /= x.size();
    m4 /= x.size();
    if (m2 == 0.0) return STAT_NAN;
    return m4 / (m2 * m2) - 3.0; // 矩法超额峰度
}

static double StatMaxDrawdown(const std::vector<double> &prices) {
    if (prices.empty()) return STAT_NAN;
    double peak = -std::numeric_limits<double>::infinity();
    double max_dd = 0.0;
    for (double p : prices) {
        if (p > peak) peak = p;
        if (peak > 0.0) {
            double dd = (peak - p) / peak;
            if (dd > max_dd) max_dd = dd;
        }
    }
    return max_dd;
}

static double StatSharpe(const std::vector<double> &returns, double rf, double ppy) {
    if (returns.size() < 2) return STAT_NAN;
    double var = StatVariance(returns);
    double sd = std::sqrt(var);
    if (sd == 0.0) return STAT_NAN;
    return (StatMean(returns) - rf) / sd * std::sqrt(ppy);
}

static double StatCorr(const std::vector<double> &x, const std::vector<double> &y) {
    if (x.size() != y.size() || x.size() < 2) return STAT_NAN;
    double mx = StatMean(x), my = StatMean(y);
    double num = 0.0, dx = 0.0, dy = 0.0;
    for (size_t i = 0; i < x.size(); i++) {
        double ax = x[i] - mx, ay = y[i] - my;
        num += ax * ay;
        dx += ax * ax;
        dy += ay * ay;
    }
    double den = std::sqrt(dx * dy);
    if (den == 0.0) return STAT_NAN;
    return num / den;
}

static double StatBeta(const std::vector<double> &asset, const std::vector<double> &market) {
    if (asset.size() != market.size() || asset.size() < 2) return STAT_NAN;
    double ma = StatMean(asset), mm = StatMean(market);
    double cov = 0.0, var_m = 0.0;
    for (size_t i = 0; i < asset.size(); i++) {
        cov += (asset[i] - ma) * (market[i] - mm);
        var_m += (market[i] - mm) * (market[i] - mm);
    }
    if (var_m == 0.0) return STAT_NAN;
    return cov / var_m;
}

// 单变量 OLS：y = slope*x + intercept，返回 {slope, intercept, r2, std_err}
static void StatRegress(const std::vector<double> &y, const std::vector<double> &x,
                        double &slope, double &intercept, double &r2, double &std_err) {
    slope = intercept = r2 = std_err = STAT_NAN;
    if (x.size() != y.size() || x.size() < 2) return;
    size_t n = x.size();
    double mx = StatMean(x), my = StatMean(y);
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (size_t i = 0; i < n; i++) {
        double dx = x[i] - mx, dy = y[i] - my;
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }
    if (sxx == 0.0) return; // 奇异（x 为常数）
    slope = sxy / sxx;
    intercept = my - slope * mx;
    if (syy != 0.0) r2 = (sxy * sxy) / (sxx * syy);
    // 斜率标准误 = sqrt(mse / sxx)，mse = ss_res/(n-2)
    if (n > 2) {
        double ss_res = 0.0;
        for (size_t i = 0; i < n; i++) {
            double e = y[i] - (slope * x[i] + intercept);
            ss_res += e * e;
        }
        double mse = ss_res / (double)(n - 2);
        std_err = std::sqrt(mse / sxx);
    }
}

static void ComputeAll(CnTaIndicatorsGlobalState &g) {
    int n = (int)g.ts_col.size();
    if (n == 0) {
        g.computed = true;
        return;
    }

    auto &open = g.cols[1], &high = g.cols[2], &low = g.cols[3], &close = g.cols[4], &volume = g.cols[5];
    auto &amount = g.cols[6], &outstanding_share = g.cols[7], &turnover = g.cols[8];
    bool has_amount = (int)amount.size() == n;
    bool has_shares = (int)outstanding_share.size() == n;

    // 输出列命名辅助
    auto add_col = [&](const string &name, vector<double> &&v) {
        g.out_names.push_back(name);
        g.out_cols.push_back(std::move(v));
    };

    // ---- P1（close + period）----
    for (auto &ind : P1_INDICATORS) {
        int outBeg = 0, outNb = 0;
        vector<double> outReal(n);
        TA_RetCode rc = ind.func(0, n - 1, close.data(), ind.period, &outBeg, &outNb, outReal.data());
        vector<double> col;
        ExpandResult(col, outReal.data(), outBeg, outNb, n);
        add_col(string(ind.name) + "_" + std::to_string(ind.period), std::move(col));
    }

    // ---- P3（HLC + period）----
    for (auto &ind : P3_INDICATORS) {
        int outBeg = 0, outNb = 0;
        vector<double> outReal(n);
        TA_RetCode rc = ind.func(0, n - 1, high.data(), low.data(), close.data(), ind.period, &outBeg, &outNb, outReal.data());
        vector<double> col;
        ExpandResult(col, outReal.data(), outBeg, outNb, n);
        add_col(string(ind.name) + "_" + std::to_string(ind.period), std::move(col));
    }

    // ---- P4（HLCV）----
    for (auto &ind : P4_INDICATORS) {
        int outBeg = 0, outNb = 0;
        vector<double> outReal(n);
        TA_RetCode rc = ind.func(0, n - 1, high.data(), low.data(), close.data(), volume.data(), &outBeg, &outNb, outReal.data());
        vector<double> col;
        ExpandResult(col, outReal.data(), outBeg, outNb, n);
        add_col(ind.name, std::move(col));
    }

    // ---- P5（OHLC）----
    for (auto &ind : P5_INDICATORS) {
        int outBeg = 0, outNb = 0;
        vector<double> outReal(n);
        TA_RetCode rc = ind.func(0, n - 1, open.data(), high.data(), low.data(), close.data(), &outBeg, &outNb, outReal.data());
        vector<double> col;
        ExpandResult(col, outReal.data(), outBeg, outNb, n);
        add_col(ind.name, std::move(col));
    }

    // ---- P6（HL）----
    for (auto &ind : P6_INDICATORS) {
        int outBeg = 0, outNb = 0;
        vector<double> outReal(n);
        TA_RetCode rc = ind.func(0, n - 1, high.data(), low.data(), &outBeg, &outNb, outReal.data());
        vector<double> col;
        ExpandResult(col, outReal.data(), outBeg, outNb, n);
        add_col(ind.name, std::move(col));
    }

    // ---- P7（HLC）----
    for (auto &ind : P7_INDICATORS) {
        int outBeg = 0, outNb = 0;
        vector<double> outReal(n);
        TA_RetCode rc = ind.func(0, n - 1, high.data(), low.data(), close.data(), &outBeg, &outNb, outReal.data());
        vector<double> col;
        ExpandResult(col, outReal.data(), outBeg, outNb, n);
        add_col(ind.name, std::move(col));
    }

    // ---- P8（HL + period）----
    for (auto &ind : P8_INDICATORS) {
        int outBeg = 0, outNb = 0;
        vector<double> outReal(n);
        TA_RetCode rc = ind.func(0, n - 1, high.data(), low.data(), ind.period, &outBeg, &outNb, outReal.data());
        vector<double> col;
        ExpandResult(col, outReal.data(), outBeg, outNb, n);
        add_col(string(ind.name) + "_" + std::to_string(ind.period), std::move(col));
    }

    // ============================================================
    // 多输出指标（BBANDS / MACD / STOCH / AROON / MINMAX / MAMA / HT_*）
    // ============================================================
    const double NAN_VAL = std::numeric_limits<double>::quiet_NaN();
    auto expand2 = [&](const string &n1, const string &n2, const double *o1, const double *o2, int outBeg, int outNb) {
        vector<double> c1(n, NAN_VAL), c2(n, NAN_VAL);
        for (int i = 0; i < outNb; i++) {
            c1[outBeg + i] = o1[i];
            c2[outBeg + i] = o2[i];
        }
        add_col(n1, std::move(c1));
        add_col(n2, std::move(c2));
    };
    auto expand3 = [&](const string &n1, const string &n2, const string &n3, const double *o1, const double *o2, const double *o3, int outBeg, int outNb) {
        vector<double> c1(n, NAN_VAL), c2(n, NAN_VAL), c3(n, NAN_VAL);
        for (int i = 0; i < outNb; i++) {
            c1[outBeg + i] = o1[i];
            c2[outBeg + i] = o2[i];
            c3[outBeg + i] = o3[i];
        }
        add_col(n1, std::move(c1));
        add_col(n2, std::move(c2));
        add_col(n3, std::move(c3));
    };

    // BBANDS（布林带）：(close, 20, 2.0, 2.0, SMA)
    {
        int outBeg = 0, outNb = 0;
        vector<double> up(n), mid(n), lo(n);
        TA_BBANDS(0, n - 1, close.data(), 20, 2.0, 2.0, TA_MAType_SMA, &outBeg, &outNb, up.data(), mid.data(), lo.data());
        expand3("bbands_upper", "bbands_middle", "bbands_lower", up.data(), mid.data(), lo.data(), outBeg, outNb);
    }
    // MACD：(close, 12, 26, 9)
    {
        int outBeg = 0, outNb = 0;
        vector<double> macd(n), sig(n), hist(n);
        TA_MACD(0, n - 1, close.data(), 12, 26, 9, &outBeg, &outNb, macd.data(), sig.data(), hist.data());
        expand3("macd", "macd_signal", "macd_hist", macd.data(), sig.data(), hist.data(), outBeg, outNb);
    }
    // STOCH（随机指标）：(HLC, 5, 3, SMA, 3, SMA)
    {
        int outBeg = 0, outNb = 0;
        vector<double> k(n), d(n);
        TA_STOCH(0, n - 1, high.data(), low.data(), close.data(), 5, 3, TA_MAType_SMA, 3, TA_MAType_SMA, &outBeg, &outNb, k.data(), d.data());
        expand2("stoch_k", "stoch_d", k.data(), d.data(), outBeg, outNb);
    }
    // AROON：(HL, 14)
    {
        int outBeg = 0, outNb = 0;
        vector<double> dn(n), up(n);
        TA_AROON(0, n - 1, high.data(), low.data(), 14, &outBeg, &outNb, dn.data(), up.data());
        expand2("aroon_down", "aroon_up", dn.data(), up.data(), outBeg, outNb);
    }
    // MINMAX：(close, 20)
    {
        int outBeg = 0, outNb = 0;
        vector<double> mn(n), mx(n);
        TA_MINMAX(0, n - 1, close.data(), 20, &outBeg, &outNb, mn.data(), mx.data());
        expand2("minmax_min", "minmax_max", mn.data(), mx.data(), outBeg, outNb);
    }
    // MAMA：(close, 0.5, 0.05)
    {
        int outBeg = 0, outNb = 0;
        vector<double> mama(n), fama(n);
        TA_MAMA(0, n - 1, close.data(), 0.5, 0.05, &outBeg, &outNb, mama.data(), fama.data());
        expand2("mama", "fama", mama.data(), fama.data(), outBeg, outNb);
    }
    // HT_PHASOR：(close)
    {
        int outBeg = 0, outNb = 0;
        vector<double> inph(n), quad(n);
        TA_HT_PHASOR(0, n - 1, close.data(), &outBeg, &outNb, inph.data(), quad.data());
        expand2("ht_inphase", "ht_quadrature", inph.data(), quad.data(), outBeg, outNb);
    }
    // HT_SINE：(close)
    {
        int outBeg = 0, outNb = 0;
        vector<double> sine(n), lead(n);
        TA_HT_SINE(0, n - 1, close.data(), &outBeg, &outNb, sine.data(), lead.data());
        expand2("ht_sine", "ht_leadsine", sine.data(), lead.data(), outBeg, outNb);
    }

    // ============================================================
    // A 股适配指标（基于输入列直接派生，无需 TA-Lib）
    // ============================================================

    // 1. 涨跌幅 pct_change = (close - prev_close) / prev_close
    {
        vector<double> col(n, NAN_VAL);
        for (int i = 1; i < n; i++) {
            double prev = close[i - 1];
            if (!std::isnan(prev) && std::fabs(prev) > 1e-12) {
                col[i] = (close[i] - prev) / prev;
            }
        }
        add_col("pct_change", std::move(col));
    }

    // 2. 量比 volume_ratio = 当前量 / 前 5 日均量（若有 volume）
    {
        vector<double> col(n, NAN_VAL);
        for (int i = 0; i < n; i++) {
            if (i < 5) continue;
            double s = 0.0;
            int c = 0;
            for (int k = i - 5; k < i; k++) {
                if (std::isnan(volume[k])) { s = NAN_VAL; break; }
                s += volume[k]; c++;
            }
            if (!std::isnan(s) && c == 5 && std::fabs(s) > 1e-12) {
                col[i] = volume[i] / (s / 5.0);
            }
        }
        add_col("volume_ratio", std::move(col));
    }

    // 3. 成交额均线 amount_ma5 / amount_ma10（若有 amount 列）
    if (has_amount) {
        for (int period : {5, 10}) {
            vector<double> col(n, NAN_VAL);
            for (int i = 0; i < n; i++) {
                if (i < period - 1) continue;
                double s = 0.0;
                int c = 0;
                for (int k = i - period + 1; k <= i; k++) {
                    if (std::isnan(amount[k])) { s = NAN_VAL; break; }
                    s += amount[k]; c++;
                }
                col[i] = (c == period && !std::isnan(s)) ? s / period : NAN_VAL;
            }
            add_col("amount_ma" + std::to_string(period), std::move(col));
        }
    }

    // 4. 换手率重算 turnover_calc = volume / outstanding_share（若有流通股本）
    if (has_shares) {
        vector<double> col(n, NAN_VAL);
        for (int i = 0; i < n; i++) {
            double sh = outstanding_share[i];
            if (!std::isnan(sh) && std::fabs(sh) > 1e-12 && !std::isnan(volume[i])) {
                col[i] = volume[i] / sh;
            }
        }
        add_col("turnover_calc", std::move(col));
    }

    // ============================================================
    // Eigen 金融统计与回归（stat_*）
    // ============================================================

    // ---- 整段统计（常量列，每行同值）----
    // 先构造基于 close 的收益率序列（对数收益）
    vector<double> log_returns(n, NAN_VAL);
    for (int i = 1; i < n; i++) {
        if (!std::isnan(close[i]) && !std::isnan(close[i-1]) && close[i-1] > 0.0 && close[i] > 0.0) {
            log_returns[i] = std::log(close[i] / close[i - 1]);
        }
    }

    auto add_const = [&](const string &name, double v) {
        vector<double> col(n, std::isnan(v) ? NAN_VAL : v);
        add_col(name, std::move(col));
    };

    add_const("stat_var", StatVariance(close));
    add_const("stat_stddev", std::isnan(StatVariance(close)) ? STAT_NAN : std::sqrt(StatVariance(close)));
    add_const("stat_skew", StatSkew(close));
    add_const("stat_kurtosis", StatKurtosis(close));
    add_const("stat_max_drawdown", StatMaxDrawdown(close));
    // sharpe/annual_vol 基于 log_returns（去掉前导 NaN）
    {
        vector<double> r;
        r.reserve(n);
        for (double v : log_returns) if (!std::isnan(v)) r.push_back(v);
        add_const("stat_sharpe", StatSharpe(r, 0.0, 252.0));
        add_const("stat_annual_vol", std::isnan(StatVariance(r)) ? STAT_NAN : std::sqrt(StatVariance(r)) * std::sqrt(252.0));
    }
    add_const("stat_corr_close_vol", StatCorr(close, volume));
    add_const("stat_beta", StatBeta(close, volume));
    // 回归 close ~ volume
    {
        double slope, intercept, r2, std_err;
        StatRegress(close, volume, slope, intercept, r2, std_err);
        add_const("stat_regress_slope", slope);
        add_const("stat_regress_intercept", intercept);
        add_const("stat_regress_r2", r2);
        add_const("stat_regress_std_err", std_err);
        // 趋势强度 = 线性回归 R²（close ~ 时间索引）
        vector<double> idx(n);
        for (int i = 0; i < n; i++) idx[i] = (double)i;
        double ts_slope, ts_intercept, ts_r2, ts_err;
        StatRegress(close, idx, ts_slope, ts_intercept, ts_r2, ts_err);
        add_const("stat_trend_strength", ts_r2);
    }

    // ---- 滚动统计（每行列，窗口 20）----
    const int ROLL_WIN = 20;
    auto rolling = [&](const string &name, const vector<double> &src, auto fn) {
        vector<double> col(n, NAN_VAL);
        for (int i = 0; i < n; i++) {
            if (i < ROLL_WIN - 1) continue;
            vector<double> w(src.begin() + i - ROLL_WIN + 1, src.begin() + i + 1);
            col[i] = fn(w);
        }
        add_col(name, std::move(col));
    };

    rolling("roll_avg_20", close, StatMean);
    rolling("roll_sum_20", close, [](const vector<double> &x) { double s=0; for(double v:x) s+=v; return s; });
    rolling("roll_min_20", low, [](const vector<double> &x) { double m=x[0]; for(double v:x) m=std::min(m,v); return m; });
    rolling("roll_max_20", high, [](const vector<double> &x) { double m=x[0]; for(double v:x) m=std::max(m,v); return m; });
    rolling("roll_momentum_20", close, [](const vector<double> &x) {
        if (x.size() < 2 || std::fabs(x.front()) < 1e-12) return STAT_NAN;
        return (x.back() - x.front()) / std::fabs(x.front());
    });
    rolling("roll_var_20", close, StatVariance);
    rolling("roll_stddev_20", close, [](const vector<double> &x) {
        double v = StatVariance(x); return std::isnan(v) ? v : std::sqrt(v);
    });

    g.computed = true;
}

// ============================================================
// TableFunction 回调
// ============================================================

static unique_ptr<FunctionData> CnTaIndicatorsBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
    // 校验输入列数（子查询的列）
    if (input.input_table_types.size() < 5) {
        throw BinderException("cn_ta_indicators 需要至少 5 列: ts, open, high, low, close");
    }

    auto bind_data = make_uniq<CnTaIndicatorsBindData>();
    bind_data->input_types = input.input_table_types;

    // 输出列 = 原始输入列 + 指标列
    // 原始列
    for (idx_t i = 0; i < input.input_table_types.size(); i++) {
        return_types.push_back(input.input_table_types[i]);
        names.push_back(i < (idx_t)INPUT_COLS.size() ? INPUT_COLS[i] : input.input_table_names[i]);
    }

    // 指标列（全部 DOUBLE）
    auto add_out = [&](const string &name) {
        return_types.push_back(LogicalType::DOUBLE);
        names.push_back(name);
    };
    for (auto &ind : P1_INDICATORS) add_out(string(ind.name) + "_" + std::to_string(ind.period));
    for (auto &ind : P3_INDICATORS) add_out(string(ind.name) + "_" + std::to_string(ind.period));
    for (auto &ind : P4_INDICATORS) add_out(ind.name);
    for (auto &ind : P5_INDICATORS) add_out(ind.name);
    for (auto &ind : P6_INDICATORS) add_out(ind.name);
    for (auto &ind : P7_INDICATORS) add_out(ind.name);
    for (auto &ind : P8_INDICATORS) add_out(string(ind.name) + "_" + std::to_string(ind.period));

    // 多输出指标列
    for (const char *c : {"bbands_upper", "bbands_middle", "bbands_lower",
                          "macd", "macd_signal", "macd_hist",
                          "stoch_k", "stoch_d", "aroon_down", "aroon_up",
                          "minmax_min", "minmax_max", "mama", "fama",
                          "ht_inphase", "ht_quadrature", "ht_sine", "ht_leadsine"}) {
        add_out(c);
    }

    // A 股适配指标列
    add_out("pct_change");
    add_out("volume_ratio");
    if (input.input_table_types.size() >= 7) {  // 有 amount 列
        add_out("amount_ma5");
        add_out("amount_ma10");
    }
    if (input.input_table_types.size() >= 8) {  // 有 outstanding_share 列
        add_out("turnover_calc");
    }

    // Eigen 金融统计（整段常量列）
    for (const char *c : {"stat_var", "stat_stddev", "stat_skew", "stat_kurtosis",
                          "stat_max_drawdown", "stat_sharpe", "stat_annual_vol",
                          "stat_corr_close_vol", "stat_beta",
                          "stat_regress_slope", "stat_regress_intercept",
                          "stat_regress_r2", "stat_regress_std_err", "stat_trend_strength"}) {
        add_out(c);
    }

    // 滚动统计（每行列，窗口 20）
    for (const char *c : {"roll_avg_20", "roll_sum_20", "roll_min_20", "roll_max_20",
                          "roll_momentum_20", "roll_var_20", "roll_stddev_20"}) {
        add_out(c);
    }

    return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> CnTaIndicatorsInit(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<CnTaIndicatorsGlobalState>();
}

// in_out_function：累积输入 chunk，不输出（返回 NEED_MORE_INPUT）
static OperatorResultType CnTaIndicatorsInOut(ExecutionContext &context, TableFunctionInput &data,
                                             DataChunk &input, DataChunk &output) {
    auto &g = data.global_state->Cast<CnTaIndicatorsGlobalState>();
    auto &bind = data.bind_data->Cast<CnTaIndicatorsBindData>();

    idx_t ncols = input.ColumnCount();
    g.input_ncols = ncols;
    for (idx_t c = 0; c < ncols; c++) {
        for (idx_t r = 0; r < input.size(); r++) {
            Value v = input.GetValue(c, r);
            if (c == 0) {
                // ts 列（TIMESTAMP）
                if (v.IsNull()) {
                    g.ts_col.push_back(timestamp_t(0));
                } else {
                    g.ts_col.push_back(v.GetValue<timestamp_t>());
                }
            } else {
                // 数值列：统一转 double，NULL → NaN
                if (v.IsNull()) {
                    g.cols[c].push_back(std::numeric_limits<double>::quiet_NaN());
                } else {
                    g.cols[c].push_back(v.GetValue<double>());
                }
            }
        }
    }
    output.SetCardinality(0);
    return OperatorResultType::NEED_MORE_INPUT;
}

// in_out_function_final：所有数据累积完毕后，批量计算并输出
static OperatorFinalizeResultType CnTaIndicatorsFinal(ExecutionContext &context, TableFunctionInput &data,
                                                      DataChunk &output) {
    auto &g = data.global_state->Cast<CnTaIndicatorsGlobalState>();

    if (!g.computed) {
        ComputeAll(g);
        g.out_row = 0;
    }

    idx_t total = g.ts_col.size();
    if (g.out_row >= total) {
        return OperatorFinalizeResultType::FINISHED;
    }

    // 输出一个 chunk（最多 STANDARD_VECTOR_SIZE 行）
    idx_t remaining = total - g.out_row;
    idx_t chunk_size = std::min<idx_t>(remaining, STANDARD_VECTOR_SIZE);
    output.SetCardinality(chunk_size);

    idx_t ncols = output.ColumnCount();
    // 原始列
    idx_t input_ncols = g.input_ncols;
    for (idx_t c = 0; c < ncols; c++) {
        auto &outvec = output.data[c];
        if (c == 0) {
            // ts
            auto tdata = FlatVector::GetData<timestamp_t>(outvec);
            for (idx_t i = 0; i < chunk_size; i++) {
                tdata[i] = g.ts_col[g.out_row + i];
            }
        } else if (c < input_ncols) {
            // 原始数值列：按列类型动态设置（可能为 DOUBLE/BIGINT 等）
            for (idx_t i = 0; i < chunk_size; i++) {
                double v = g.cols[c][g.out_row + i];
                if (std::isnan(v)) {
                    output.SetValue(c, i, Value());
                } else {
                    output.SetValue(c, i, Value::DOUBLE(v));
                }
            }
        } else {
    // 指标列（NaN → NULL，与 DuckDB 语义一致）
    idx_t indicator_idx = c - input_ncols;
    auto ddata = FlatVector::GetData<double>(outvec);
    auto &validity = FlatVector::Validity(outvec);
    for (idx_t i = 0; i < chunk_size; i++) {
        double v = g.out_cols[indicator_idx][g.out_row + i];
        if (std::isnan(v)) {
            validity.SetInvalid(i);
        } else {
            ddata[i] = v;
        }
    }
        }
    }

    g.out_row += chunk_size;
    return OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
}

// ============================================================
// Registration
// ============================================================

void RegisterCnTaIndicatorsFunction(ExtensionLoader &loader) {
    // 参数类型 TABLE 使函数接受子查询作为表输入（类似 summary）
    TableFunction func("cn_ta_indicators", {LogicalType::TABLE}, nullptr, CnTaIndicatorsBind, CnTaIndicatorsInit);
    func.in_out_function = CnTaIndicatorsInOut;
    func.in_out_function_final = CnTaIndicatorsFinal;
    loader.RegisterFunction(func);
}

} // namespace duckdb
