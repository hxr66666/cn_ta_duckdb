#include "cn_ta_stat_agg.hpp"
#include "duckdb.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <vector>
#include <limits>

namespace duckdb {

// ============================================================
// 通用聚合状态：累积窗口内一个或两个 double 序列
// 窗口聚合（OVER）下，每个 state 累积该行窗口 frame 内的值。
// ============================================================
struct StatAggState {
    std::vector<double> *x; // 主序列
    std::vector<double> *y; // 第二序列（双输入函数用），单输入为 nullptr
};

// ============================================================
// 窗口统计计算核心函数（Finalize 用）
// ============================================================
static double WinMean(const std::vector<double> &x) {
    if (x.empty())
        return std::numeric_limits<double>::quiet_NaN();
    double s = 0.0;
    for (double v : x)
        s += v;
    return s / (double)x.size();
}

static double WinSum(const std::vector<double> &x) {
    double s = 0.0;
    for (double v : x)
        s += v;
    return s;
}

static double WinMin(const std::vector<double> &x) {
    if (x.empty())
        return std::numeric_limits<double>::quiet_NaN();
    double m = x[0];
    for (double v : x)
        if (v < m)
            m = v;
    return m;
}

static double WinMax(const std::vector<double> &x) {
    if (x.empty())
        return std::numeric_limits<double>::quiet_NaN();
    double m = x[0];
    for (double v : x)
        if (v > m)
            m = v;
    return m;
}

static double WinVar(const std::vector<double> &x) {
    if (x.size() < 2)
        return std::numeric_limits<double>::quiet_NaN();
    Eigen::Map<const Eigen::VectorXd> v(x.data(), (Eigen::Index)x.size());
    double mean = v.mean();
    return (v.array() - mean).square().sum() / (double)(x.size() - 1);
}

static double WinStddev(const std::vector<double> &x) {
    double var = WinVar(x);
    return std::isnan(var) ? var : std::sqrt(var);
}

// 窗口动量: (last - first) / |first|，用首尾两个元素
static double WinMomentum(const std::vector<double> &x) {
    if (x.size() < 2)
        return std::numeric_limits<double>::quiet_NaN();
    double first = x.front();
    double last = x.back();
    if (std::fabs(first) < 1e-12)
        return std::numeric_limits<double>::quiet_NaN();
    return (last - first) / std::fabs(first);
}

// 窗口 beta: cov(x,y)/var(y) （y 为市场）
static double WinBeta(const std::vector<double> &x, const std::vector<double> &y) {
    if (x.size() != y.size() || x.size() < 2)
        return std::numeric_limits<double>::quiet_NaN();
    Eigen::Map<const Eigen::VectorXd> vx(x.data(), (Eigen::Index)x.size());
    Eigen::Map<const Eigen::VectorXd> vy(y.data(), (Eigen::Index)y.size());
    double mx = vx.mean();
    double my = vy.mean();
    double cov = ((vx.array() - mx) * (vy.array() - my)).sum();
    double var_y = (vy.array() - my).square().sum();
    if (std::fabs(var_y) < 1e-12)
        return std::numeric_limits<double>::quiet_NaN();
    return cov / var_y;
}

// 窗口相关系数
static double WinCorr(const std::vector<double> &x, const std::vector<double> &y) {
    if (x.size() != y.size() || x.size() < 2)
        return std::numeric_limits<double>::quiet_NaN();
    Eigen::Map<const Eigen::VectorXd> vx(x.data(), (Eigen::Index)x.size());
    Eigen::Map<const Eigen::VectorXd> vy(y.data(), (Eigen::Index)y.size());
    double mx = vx.mean();
    double my = vy.mean();
    double num = ((vx.array() - mx) * (vy.array() - my)).sum();
    double dx = (vx.array() - mx).square().sum();
    double dy = (vy.array() - my).square().sum();
    double den = std::sqrt(dx * dy);
    if (std::fabs(den) < 1e-12)
        return std::numeric_limits<double>::quiet_NaN();
    return num / den;
}

// ============================================================
// 单输入聚合框架：Finalize 调用 F(x)
// ============================================================
template <double (*F)(const std::vector<double> &)>
struct StatAggSingle {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(StatAggState); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<StatAggState *>(state);
        s->x = nullptr;
        s->y = nullptr;
    }

    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat vdata, sdata;
        inputs[0].ToUnifiedFormat(count, vdata);
        states_vec.ToUnifiedFormat(count, sdata);
        auto values = UnifiedVectorFormat::GetData<double>(vdata);
        auto states = (StatAggState **)sdata.data;
        for (idx_t i = 0; i < count; i++) {
            auto sidx = sdata.sel->get_index(i);
            auto vidx = vdata.sel->get_index(i);
            auto state = states[sidx];
            if (!state->x)
                state->x = new std::vector<double>();
            if (vdata.validity.RowIsValid(vidx)) {
                state->x->push_back(values[vidx]);
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<StatAggState *>(state_p);
        UnifiedVectorFormat vdata;
        inputs[0].ToUnifiedFormat(count, vdata);
        auto values = UnifiedVectorFormat::GetData<double>(vdata);
        if (!state->x)
            state->x = new std::vector<double>();
        for (idx_t i = 0; i < count; i++) {
            auto vidx = vdata.sel->get_index(i);
            if (vdata.validity.RowIsValid(vidx)) {
                state->x->push_back(values[vidx]);
            }
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto sdata = FlatVector::GetData<StatAggState *>(source);
        auto tdata = FlatVector::GetData<StatAggState *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (sdata[i]->x) {
                if (!tdata[i]->x)
                    tdata[i]->x = new std::vector<double>();
                tdata[i]->x->insert(tdata[i]->x->end(), sdata[i]->x->begin(), sdata[i]->x->end());
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<StatAggState *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);
        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->x || state->x->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            double v = F(*state->x);
            if (std::isnan(v)) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = v;
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<StatAggState *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            if (states[i]->x) {
                delete states[i]->x;
                states[i]->x = nullptr;
            }
        }
    }
};

// ============================================================
// 双输入聚合框架：Finalize 调用 F(x, y)
// ============================================================
template <double (*F)(const std::vector<double> &, const std::vector<double> &)>
struct StatAggPair {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(StatAggState); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<StatAggState *>(state);
        s->x = nullptr;
        s->y = nullptr;
    }

    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat xd, yd, sd;
        inputs[0].ToUnifiedFormat(count, xd);
        inputs[1].ToUnifiedFormat(count, yd);
        states_vec.ToUnifiedFormat(count, sd);
        auto xv = UnifiedVectorFormat::GetData<double>(xd);
        auto yv = UnifiedVectorFormat::GetData<double>(yd);
        auto states = (StatAggState **)sd.data;
        for (idx_t i = 0; i < count; i++) {
            auto sidx = sd.sel->get_index(i);
            auto xidx = xd.sel->get_index(i);
            auto yidx = yd.sel->get_index(i);
            auto state = states[sidx];
            if (!state->x)
                state->x = new std::vector<double>();
            if (!state->y)
                state->y = new std::vector<double>();
            if (xd.validity.RowIsValid(xidx)) {
                state->x->push_back(xv[xidx]);
            }
            if (yd.validity.RowIsValid(yidx)) {
                state->y->push_back(yv[yidx]);
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<StatAggState *>(state_p);
        UnifiedVectorFormat xd, yd;
        inputs[0].ToUnifiedFormat(count, xd);
        inputs[1].ToUnifiedFormat(count, yd);
        auto xv = UnifiedVectorFormat::GetData<double>(xd);
        auto yv = UnifiedVectorFormat::GetData<double>(yd);
        if (!state->x)
            state->x = new std::vector<double>();
        if (!state->y)
            state->y = new std::vector<double>();
        for (idx_t i = 0; i < count; i++) {
            auto xidx = xd.sel->get_index(i);
            auto yidx = yd.sel->get_index(i);
            if (xd.validity.RowIsValid(xidx))
                state->x->push_back(xv[xidx]);
            if (yd.validity.RowIsValid(yidx))
                state->y->push_back(yv[yidx]);
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto sdata = FlatVector::GetData<StatAggState *>(source);
        auto tdata = FlatVector::GetData<StatAggState *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (sdata[i]->x) {
                if (!tdata[i]->x)
                    tdata[i]->x = new std::vector<double>();
                tdata[i]->x->insert(tdata[i]->x->end(), sdata[i]->x->begin(), sdata[i]->x->end());
            }
            if (sdata[i]->y) {
                if (!tdata[i]->y)
                    tdata[i]->y = new std::vector<double>();
                tdata[i]->y->insert(tdata[i]->y->end(), sdata[i]->y->begin(), sdata[i]->y->end());
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<StatAggState *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);
        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->x || !state->y || state->x->empty() || state->y->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            double v = F(*state->x, *state->y);
            if (std::isnan(v)) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = v;
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<StatAggState *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            if (states[i]->x) {
                delete states[i]->x;
                states[i]->x = nullptr;
            }
            if (states[i]->y) {
                delete states[i]->y;
                states[i]->y = nullptr;
            }
        }
    }
};

// ============================================================
// 注册
// ============================================================
void RegisterCnTaStatAggFunctions(ExtensionLoader &loader) {
    // 单输入窗口聚合（全部用独立函数名，避免与标量函数重载冲突）
    loader.RegisterFunction(AggregateFunction(
        "stat_avg", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggSingle<WinMean>::StateSize, StatAggSingle<WinMean>::Initialize, StatAggSingle<WinMean>::Update,
        StatAggSingle<WinMean>::Combine, StatAggSingle<WinMean>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggSingle<WinMean>::SimpleUpdate, nullptr,
        StatAggSingle<WinMean>::Destroy));
    loader.RegisterFunction(AggregateFunction(
        "stat_sum", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggSingle<WinSum>::StateSize, StatAggSingle<WinSum>::Initialize, StatAggSingle<WinSum>::Update,
        StatAggSingle<WinSum>::Combine, StatAggSingle<WinSum>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggSingle<WinSum>::SimpleUpdate, nullptr,
        StatAggSingle<WinSum>::Destroy));
    loader.RegisterFunction(AggregateFunction(
        "stat_min", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggSingle<WinMin>::StateSize, StatAggSingle<WinMin>::Initialize, StatAggSingle<WinMin>::Update,
        StatAggSingle<WinMin>::Combine, StatAggSingle<WinMin>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggSingle<WinMin>::SimpleUpdate, nullptr,
        StatAggSingle<WinMin>::Destroy));
    loader.RegisterFunction(AggregateFunction(
        "stat_max", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggSingle<WinMax>::StateSize, StatAggSingle<WinMax>::Initialize, StatAggSingle<WinMax>::Update,
        StatAggSingle<WinMax>::Combine, StatAggSingle<WinMax>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggSingle<WinMax>::SimpleUpdate, nullptr,
        StatAggSingle<WinMax>::Destroy));
    loader.RegisterFunction(AggregateFunction(
        "stat_momentum", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggSingle<WinMomentum>::StateSize, StatAggSingle<WinMomentum>::Initialize, StatAggSingle<WinMomentum>::Update,
        StatAggSingle<WinMomentum>::Combine, StatAggSingle<WinMomentum>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggSingle<WinMomentum>::SimpleUpdate, nullptr,
        StatAggSingle<WinMomentum>::Destroy));

    // 窗口方差/标准差（独立名字，避免与标量 stat_var/stat_stddev 冲突）
    loader.RegisterFunction(AggregateFunction(
        "stat_rolling_var", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggSingle<WinVar>::StateSize, StatAggSingle<WinVar>::Initialize, StatAggSingle<WinVar>::Update,
        StatAggSingle<WinVar>::Combine, StatAggSingle<WinVar>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggSingle<WinVar>::SimpleUpdate, nullptr,
        StatAggSingle<WinVar>::Destroy));
    loader.RegisterFunction(AggregateFunction(
        "stat_rolling_stddev", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggSingle<WinStddev>::StateSize, StatAggSingle<WinStddev>::Initialize, StatAggSingle<WinStddev>::Update,
        StatAggSingle<WinStddev>::Combine, StatAggSingle<WinStddev>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggSingle<WinStddev>::SimpleUpdate, nullptr,
        StatAggSingle<WinStddev>::Destroy));

    // 双输入窗口聚合（独立名字，避免与标量 stat_beta/stat_corr 冲突）
    loader.RegisterFunction(AggregateFunction(
        "stat_rolling_beta", {LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggPair<WinBeta>::StateSize, StatAggPair<WinBeta>::Initialize, StatAggPair<WinBeta>::Update,
        StatAggPair<WinBeta>::Combine, StatAggPair<WinBeta>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggPair<WinBeta>::SimpleUpdate, nullptr,
        StatAggPair<WinBeta>::Destroy));
    loader.RegisterFunction(AggregateFunction(
        "stat_rolling_corr", {LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE,
        StatAggPair<WinCorr>::StateSize, StatAggPair<WinCorr>::Initialize, StatAggPair<WinCorr>::Update,
        StatAggPair<WinCorr>::Combine, StatAggPair<WinCorr>::Finalize,
        FunctionNullHandling::DEFAULT_NULL_HANDLING, StatAggPair<WinCorr>::SimpleUpdate, nullptr,
        StatAggPair<WinCorr>::Destroy));
}

} // namespace duckdb
