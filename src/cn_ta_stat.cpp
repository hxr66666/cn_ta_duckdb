#include "cn_ta_stat.hpp"
#include "cn_ta_adapter.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <algorithm>
#include <vector>
#include <limits>

namespace duckdb {

static const auto LIST_DOUBLE = LogicalType::LIST(LogicalType::DOUBLE);

// ============================================================
// Helper: extract a double value at (row) of column (col) in a DataChunk
// ============================================================
static inline double GetDoubleColumn(DataChunk &args, idx_t col, idx_t row, bool &is_valid) {
    auto &vec = args.data[col];
    UnifiedVectorFormat vdata;
    vec.ToUnifiedFormat(args.size(), vdata);
    auto data = UnifiedVectorFormat::GetData<double>(vdata);
    auto idx = vdata.sel->get_index(row);
    is_valid = vdata.validity.RowIsValid(idx);
    return data[idx];
}

// ============================================================
// Helper: extract a LIST<DOUBLE> at (row) into std::vector<double>
// 复用 cn_ta_adapter.hpp 的 ListToDoubleArray 完成解包 + NULL→NaN 处理
// ============================================================
static inline bool GetListColumn(DataChunk &args, idx_t col, idx_t row, std::vector<double> &out) {
    auto &vec = args.data[col];
    UnifiedVectorFormat vdata;
    vec.ToUnifiedFormat(args.size(), vdata);
    auto list_data = UnifiedVectorFormat::GetData<list_entry_t>(vdata);
    auto idx = vdata.sel->get_index(row);
    if (!vdata.validity.RowIsValid(idx)) {
        return false;
    }
    auto list = list_data[idx];
    auto &child = ListVector::GetEntry(args.data[col]);
    out = ListToDoubleArray(list, child);
    return true;
}

// ============================================================
// Helper: extract a LIST<LIST<DOUBLE>> at (row) into a 2D matrix (columns x rows)
// Each inner list is one variable/column of observations.
// Returns vector of columns (each a vector of observations).
// ============================================================
static inline bool GetListListColumn(DataChunk &args, idx_t col, idx_t row,
                                     std::vector<std::vector<double>> &cols) {
    auto &vec = args.data[col];
    UnifiedVectorFormat vdata;
    vec.ToUnifiedFormat(args.size(), vdata);
    auto list_data = UnifiedVectorFormat::GetData<list_entry_t>(vdata);
    auto idx = vdata.sel->get_index(row);
    if (!vdata.validity.RowIsValid(idx)) {
        return false;
    }
    auto list = list_data[idx];
    auto &child = ListVector::GetEntry(args.data[col]);
    UnifiedVectorFormat cdata;
    child.ToUnifiedFormat(list.length, cdata);
    auto cdata_ptr = UnifiedVectorFormat::GetData<list_entry_t>(cdata);
    cols.clear();
    for (idx_t k = 0; k < list.length; k++) {
        auto cidx = cdata.sel->get_index(list.offset + k);
        if (!cdata.validity.RowIsValid(cidx)) {
            continue;
        }
        auto inner = cdata_ptr[cidx];
        auto &inner_child = ListVector::GetEntry(child);
        UnifiedVectorFormat ics;
        inner_child.ToUnifiedFormat(inner.length, ics);
        auto ics_ptr = UnifiedVectorFormat::GetData<double>(ics);
        std::vector<double> column;
        column.reserve(inner.length);
        for (idx_t j = 0; j < inner.length; j++) {
            auto jidx = ics.sel->get_index(inner.offset + j);
            if (ics.validity.RowIsValid(jidx)) {
                column.push_back(ics_ptr[jidx]);
            } else {
                // NULL 元素转 NaN，避免把数据缺失当成 0 参与统计计算
                column.push_back(std::numeric_limits<double>::quiet_NaN());
            }
        }
        cols.push_back(std::move(column));
    }
    return !cols.empty();
}

// ============================================================
// Eigen-backed statistical core functions
// ============================================================
static double StatVariance(const std::vector<double> &x, bool sample) {
    if (x.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> v(x.data(), (Eigen::Index)x.size());
    double mean = v.mean();
    double var = (v.array() - mean).square().sum() / (double)(x.size() - (sample ? 1 : 0));
    return var;
}

static double StatCov(const std::vector<double> &x, const std::vector<double> &y, bool sample) {
    if (x.size() != y.size() || x.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> vx(x.data(), (Eigen::Index)x.size());
    Eigen::Map<const Eigen::VectorXd> vy(y.data(), (Eigen::Index)y.size());
    double mx = vx.mean();
    double my = vy.mean();
    double cov = ((vx.array() - mx) * (vy.array() - my)).sum() / (double)(x.size() - (sample ? 1 : 0));
    return cov;
}

static double StatCorr(const std::vector<double> &x, const std::vector<double> &y) {
    if (x.size() != y.size() || x.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> vx(x.data(), (Eigen::Index)x.size());
    Eigen::Map<const Eigen::VectorXd> vy(y.data(), (Eigen::Index)y.size());
    double mx = vx.mean();
    double my = vy.mean();
    double num = ((vx.array() - mx) * (vy.array() - my)).sum();
    double dx = (vx.array() - mx).square().sum();
    double dy = (vy.array() - my).square().sum();
    double den = std::sqrt(dx * dy);
    if (den == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return num / den;
}

static double StatBeta(const std::vector<double> &asset, const std::vector<double> &market) {
    if (asset.size() != market.size() || asset.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> va(asset.data(), (Eigen::Index)asset.size());
    Eigen::Map<const Eigen::VectorXd> vm(market.data(), (Eigen::Index)market.size());
    double ma = va.mean();
    double mm = vm.mean();
    double cov = ((va.array() - ma) * (vm.array() - mm)).sum();
    double var_m = (vm.array() - mm).square().sum();
    if (var_m == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return cov / var_m;
}

// Sharpe ratio = (mean(returns) - rf) / std(returns) * sqrt(periods_per_year)
static double StatSharpe(const std::vector<double> &returns, double rf, double periods_per_year) {
    if (returns.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> r(returns.data(), (Eigen::Index)returns.size());
    double mean = r.mean();
    double var = (r.array() - mean).square().sum() / (double)(returns.size() - 1);
    double sd = std::sqrt(var);
    if (sd == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return (mean - rf) / sd * std::sqrt(periods_per_year);
}

// Max drawdown of a price series
static double StatMaxDrawdown(const std::vector<double> &prices) {
    if (prices.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double peak = -std::numeric_limits<double>::infinity();
    double max_dd = 0.0;
    for (double p : prices) {
        if (p > peak) {
            peak = p;
        }
        if (peak > 0.0) {
            double dd = (peak - p) / peak;
            if (dd > max_dd) {
                max_dd = dd;
            }
        }
    }
    return max_dd;
}

// EWMA volatility: var[t] = (1-lambda) * r[t]^2 + lambda * var[t-1]
static double StatEWMAVol(const std::vector<double> &returns, double lambda) {
    if (returns.empty() || lambda <= 0.0 || lambda >= 1.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double var = 0.0;
    for (double r : returns) {
        var = (1.0 - lambda) * r * r + lambda * var;
    }
    return std::sqrt(var);
}

// ============================================================
// Scalar function implementations
// ============================================================
static void VarFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double v = StatVariance(x, true);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void StddevFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double v = std::sqrt(StatVariance(x, true));
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void CovFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x, y;
        if (!GetListColumn(args, 0, i, x) || !GetListColumn(args, 1, i, y)) {
            rmask.SetInvalid(i);
            continue;
        }
        double v = StatCov(x, y, true);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void CorrFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x, y;
        if (!GetListColumn(args, 0, i, x) || !GetListColumn(args, 1, i, y)) {
            rmask.SetInvalid(i);
            continue;
        }
        double v = StatCorr(x, y);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void BetaFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> asset, market;
        if (!GetListColumn(args, 0, i, asset) || !GetListColumn(args, 1, i, market)) {
            rmask.SetInvalid(i);
            continue;
        }
        double v = StatBeta(asset, market);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void SharpeFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> returns;
        if (!GetListColumn(args, 0, i, returns)) {
            rmask.SetInvalid(i);
            continue;
        }
        double rf = 0.0, ppy = 252.0;
        if (args.ColumnCount() > 1) {
            bool ok1 = false, ok2 = false;
            rf = GetDoubleColumn(args, 1, i, ok1);
            if (args.ColumnCount() > 2) {
                ppy = GetDoubleColumn(args, 2, i, ok2);
            }
        }
        double v = StatSharpe(returns, rf, ppy);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void MaxDrawdownFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> prices;
        if (!GetListColumn(args, 0, i, prices)) {
            rmask.SetInvalid(i);
            continue;
        }
        rdata[i] = StatMaxDrawdown(prices);
    }
}

static void EWMAVolFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> returns;
        if (!GetListColumn(args, 0, i, returns)) {
            rmask.SetInvalid(i);
            continue;
        }
        double lambda = 0.94;
        if (args.ColumnCount() > 1) {
            bool ok = false;
            lambda = GetDoubleColumn(args, 1, i, ok);
        }
        double v = StatEWMAVol(returns, lambda);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

// ============================================================
// OLS single-variable regression -> STRUCT{slope, intercept, r2, std_err}
// ============================================================
static void RegressFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> y, x;
        if (!GetListColumn(args, 0, i, y) || !GetListColumn(args, 1, i, x) || y.size() != x.size() || y.size() < 2) {
            FlatVector::Validity(result).SetInvalid(i);
            continue;
        }
        Eigen::Index n = (Eigen::Index)y.size();
        Eigen::Map<const Eigen::VectorXd> vy(y.data(), n);
        Eigen::Map<const Eigen::VectorXd> vx(x.data(), n);

        // 检测退化：x 为常数（方差为 0）时，AᵀA 奇异，回归无定义
        // 此时 slope 无意义，std_err 会算出 inf，故直接返回 NULL
        double x_mean = vx.mean();
        double x_var = (vx.array() - x_mean).square().sum();
        if (x_var == 0.0) {
            FlatVector::Validity(result).SetInvalid(i);
            continue;
        }

        // y = slope * x + intercept via least squares
        Eigen::MatrixXd A(n, 2);
        A.col(0) = vx;
        A.col(1) = Eigen::VectorXd::Ones(n);
        Eigen::VectorXd coef = A.colPivHouseholderQr().solve(vy);
        double slope = coef(0);
        double intercept = coef(1);

        // R^2
        double y_mean = vy.mean();
        double ss_tot = (vy.array() - y_mean).square().sum();
        Eigen::VectorXd fitted = A * coef;
        double ss_res = (vy.array() - fitted.array()).square().sum();
        double r2 = (ss_tot > 0.0) ? (1.0 - ss_res / ss_tot) : std::numeric_limits<double>::quiet_NaN();

        // Standard error of slope
        double dof = (double)(n - 2);
        double mse = (dof > 0.0) ? (ss_res / dof) : 0.0;
        Eigen::MatrixXd AtA_inv = (A.transpose() * A).inverse();
        double std_err = (AtA_inv(0, 0) >= 0.0) ? std::sqrt(mse * AtA_inv(0, 0)) : std::numeric_limits<double>::quiet_NaN();

        // Build STRUCT{slope, intercept, r2, std_err}
        auto &entries = StructVector::GetEntries(result);
        auto &struct_v = *entries[0];
        auto &intercept_v = *entries[1];
        auto &r2_v = *entries[2];
        auto &se_v = *entries[3];
        FlatVector::GetData<double>(struct_v)[i] = slope;
        FlatVector::GetData<double>(intercept_v)[i] = intercept;
        FlatVector::GetData<double>(r2_v)[i] = r2;
        FlatVector::GetData<double>(se_v)[i] = std_err;
    }
}

// ============================================================
// 多变量 OLS 回归: y ~ X  (X 为多个自变量)
// 输入: (LIST<DOUBLE> y, LIST<LIST<DOUBLE>> X)
// 返回: STRUCT{coefficients LIST<DOUBLE>, intercept DOUBLE, r2 DOUBLE}
// ============================================================
static void OLSFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto &entries = StructVector::GetEntries(result);
    auto &coef_v = *entries[0]; // LIST<DOUBLE>
    auto &intercept_v = *entries[1];
    auto &r2_v = *entries[2];

    auto coef_list_data = FlatVector::GetData<list_entry_t>(coef_v);
    auto &coef_child = ListVector::GetEntry(coef_v);
    auto coef_child_data = FlatVector::GetData<double>(coef_child);

    for (idx_t i = 0; i < count; i++) {
        std::vector<double> y;
        std::vector<std::vector<double>> xcols;
        if (!GetListColumn(args, 0, i, y) || !GetListListColumn(args, 1, i, xcols) || xcols.empty()) {
            FlatVector::Validity(result).SetInvalid(i);
            continue;
        }
        idx_t n = y.size();
        idx_t p = xcols.size();
        bool ok = true;
        for (auto &c : xcols) {
            if (c.size() != n) {
                ok = false;
                break;
            }
        }
        if (!ok || n < p + 1) {
            FlatVector::Validity(result).SetInvalid(i);
            continue;
        }

        // 设计矩阵 A (n x (p+1))，最后一列为 1（截距）
        Eigen::MatrixXd A((Eigen::Index)n, (Eigen::Index)(p + 1));
        for (idx_t j = 0; j < p; j++) {
            for (idx_t k = 0; k < n; k++) {
                A((Eigen::Index)k, (Eigen::Index)j) = xcols[j][k];
            }
        }
        for (idx_t k = 0; k < n; k++) {
            A((Eigen::Index)k, (Eigen::Index)p) = 1.0;
        }
        Eigen::Map<const Eigen::VectorXd> vy(y.data(), (Eigen::Index)n);

        Eigen::VectorXd beta = A.colPivHouseholderQr().solve(vy);

        // R^2
        double y_mean = vy.mean();
        double ss_tot = (vy.array() - y_mean).square().sum();
        Eigen::VectorXd fitted = A * beta;
        double ss_res = (vy.array() - fitted.array()).square().sum();
        double r2 = (ss_tot > 0.0) ? (1.0 - ss_res / ss_tot) : std::numeric_limits<double>::quiet_NaN();

        // 写 coefficients 到 LIST
        auto start_offset = ListVector::GetListSize(coef_v);
        coef_child.Resize(start_offset, start_offset + p);
        coef_child_data = FlatVector::GetData<double>(coef_child);
        for (idx_t j = 0; j < p; j++) {
            coef_child_data[start_offset + j] = beta((Eigen::Index)j);
        }
        ListVector::SetListSize(coef_v, start_offset + p);
        coef_list_data[i].offset = start_offset;
        coef_list_data[i].length = p;

        FlatVector::GetData<double>(intercept_v)[i] = beta((Eigen::Index)p);
        FlatVector::GetData<double>(r2_v)[i] = r2;
    }
}

// ============================================================
// 协方差矩阵: 对多个变量序列计算协方差矩阵
// 输入: LIST<LIST<DOUBLE>> X (每列一个变量)
// 返回: LIST<LIST<DOUBLE>> (p x p 协方差矩阵, 样本)
// ============================================================
static void CovMatrixFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();

    for (idx_t i = 0; i < count; i++) {
        std::vector<std::vector<double>> xcols;
        if (!GetListListColumn(args, 0, i, xcols) || xcols.size() < 1) {
            FlatVector::Validity(result).SetInvalid(i);
            continue;
        }
        idx_t p = xcols.size();
        idx_t n = xcols[0].size();
        bool ok = true;
        for (auto &c : xcols) {
            if (c.size() != n) {
                ok = false;
                break;
            }
        }
        if (!ok || n < 2) {
            FlatVector::Validity(result).SetInvalid(i);
            continue;
        }

        // 数据矩阵 (n x p) -> 协方差 (p x p)
        Eigen::MatrixXd M((Eigen::Index)n, (Eigen::Index)p);
        for (idx_t j = 0; j < p; j++) {
            for (idx_t k = 0; k < n; k++) {
                M((Eigen::Index)k, (Eigen::Index)j) = xcols[j][k];
            }
        }
        Eigen::MatrixXd centered = M.rowwise() - M.colwise().mean();
        Eigen::MatrixXd cov = (centered.transpose() * centered) / (double)(n - 1);

        // 构建 Value::LIST 嵌套，用 SetValue 写入（保证正确性）
        vector<Value> rows;
        rows.reserve(p);
        for (idx_t j = 0; j < p; j++) {
            vector<Value> row;
            row.reserve(p);
            for (idx_t k = 0; k < p; k++) {
                row.emplace_back(cov((Eigen::Index)j, (Eigen::Index)k));
            }
            rows.push_back(Value::LIST(LogicalType::DOUBLE, std::move(row)));
        }
        result.SetValue(i, Value::LIST(LIST_DOUBLE, std::move(rows)));
    }
}

// ============================================================
// PCA: 对数据矩阵做主成分分析
// 输入: LIST<LIST<DOUBLE>> X (每列一个变量)
// 返回: STRUCT{eigenvalues LIST<DOUBLE>, eigenvectors LIST<LIST<DOUBLE>>}
// ============================================================
static void PCAFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto &entries = StructVector::GetEntries(result);
    auto &eval_v = *entries[0]; // LIST<DOUBLE>
    auto &evec_v = *entries[1]; // LIST<LIST<DOUBLE>>

    for (idx_t i = 0; i < count; i++) {
        std::vector<std::vector<double>> xcols;
        if (!GetListListColumn(args, 0, i, xcols) || xcols.size() < 1) {
            FlatVector::Validity(result).SetInvalid(i);
            continue;
        }
        idx_t p = xcols.size();
        idx_t n = xcols[0].size();
        bool ok = true;
        for (auto &c : xcols) {
            if (c.size() != n) {
                ok = false;
                break;
            }
        }
        if (!ok || n < 2) {
            FlatVector::Validity(result).SetInvalid(i);
            continue;
        }

        // 数据矩阵 (n x p) -> 协方差 -> 特征分解
        Eigen::MatrixXd M((Eigen::Index)n, (Eigen::Index)p);
        for (idx_t j = 0; j < p; j++) {
            for (idx_t k = 0; k < n; k++) {
                M((Eigen::Index)k, (Eigen::Index)j) = xcols[j][k];
            }
        }
        Eigen::MatrixXd centered = M.rowwise() - M.colwise().mean();
        Eigen::MatrixXd cov = (centered.transpose() * centered) / (double)(n - 1);
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(cov);

        // 特征值 -> LIST<DOUBLE>
        auto evals = es.eigenvalues();
        vector<Value> eval_vals;
        eval_vals.reserve(p);
        for (idx_t j = 0; j < p; j++) {
            eval_vals.emplace_back(evals((Eigen::Index)j));
        }
        eval_v.SetValue(i, Value::LIST(LogicalType::DOUBLE, std::move(eval_vals)));

        // 特征向量 -> LIST<LIST<DOUBLE>>，列 j 是特征值 j 的特征向量
        auto evecs = es.eigenvectors();
        vector<Value> evec_rows;
        evec_rows.reserve(p);
        for (idx_t j = 0; j < p; j++) {
            vector<Value> row;
            row.reserve(p);
            for (idx_t k = 0; k < p; k++) {
                row.emplace_back(evecs((Eigen::Index)k, (Eigen::Index)j));
            }
            evec_rows.push_back(Value::LIST(LogicalType::DOUBLE, std::move(row)));
        }
        evec_v.SetValue(i, Value::LIST(LIST_DOUBLE, std::move(evec_rows)));
    }
}

// ============================================================
// 扩展金融指标核心函数
// ============================================================

// 年化波动率 = std(returns) * sqrt(periods_per_year)
static double StatAnnualVol(const std::vector<double> &returns, double periods_per_year) {
    if (returns.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> r(returns.data(), (Eigen::Index)returns.size());
    double mean = r.mean();
    double var = (r.array() - mean).square().sum() / (double)(returns.size() - 1);
    return std::sqrt(var) * std::sqrt(periods_per_year);
}

// Sortino 比率 = (mean - target) / downside deviation * sqrt(ppy)
// 下行偏差只考虑低于 target 的收益
static double StatSortino(const std::vector<double> &returns, double target, double periods_per_year) {
    if (returns.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> r(returns.data(), (Eigen::Index)returns.size());
    double mean = r.mean();
    double down = ((r.array() - target).min(0.0).array()).square().sum() / (double)(returns.size() - 1);
    double dd = std::sqrt(down);
    if (dd == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return (mean - target) / dd * std::sqrt(periods_per_year);
}

// 信息比率 = (mean(asset) - mean(benchmark)) / tracking error * sqrt(ppy)
static double StatInformationRatio(const std::vector<double> &asset, const std::vector<double> &benchmark,
                                   double periods_per_year) {
    if (asset.size() != benchmark.size() || asset.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> a(asset.data(), (Eigen::Index)asset.size());
    Eigen::Map<const Eigen::VectorXd> b(benchmark.data(), (Eigen::Index)benchmark.size());
    Eigen::VectorXd diff = a - b;
    double mean_diff = diff.mean();
    double var_diff = (diff.array() - mean_diff).square().sum() / (double)(asset.size() - 1);
    double te = std::sqrt(var_diff);
    if (te == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return mean_diff / te * std::sqrt(periods_per_year);
}

// Calmar 比率 = annualized return / max drawdown
static double StatCalmar(const std::vector<double> &returns, double periods_per_year) {
    if (returns.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // 累计净值
    std::vector<double> prices;
    prices.reserve(returns.size() + 1);
    prices.push_back(1.0);
    for (double r : returns) {
        prices.push_back(prices.back() * (1.0 + r));
    }
    double mdd = StatMaxDrawdown(prices);
    if (mdd == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> r(returns.data(), (Eigen::Index)returns.size());
    double mean = r.mean();
    return mean * periods_per_year / mdd;
}

// 偏度 (Fisher-Pearson)
static double StatSkew(const std::vector<double> &x) {
    if (x.size() < 3) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> v(x.data(), (Eigen::Index)x.size());
    double mean = v.mean();
    double n = (double)x.size();
    double m2 = (v.array() - mean).square().sum() / n;
    double m3 = ((v.array() - mean).array().pow(3.0)).sum() / n;
    if (m2 == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return m3 / std::pow(m2, 1.5);
}

// 峰度 (excess kurtosis, 正态为 0)
static double StatKurtosis(const std::vector<double> &x) {
    if (x.size() < 4) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Eigen::Map<const Eigen::VectorXd> v(x.data(), (Eigen::Index)x.size());
    double mean = v.mean();
    double n = (double)x.size();
    double m2 = (v.array() - mean).square().sum() / n;
    double m4 = ((v.array() - mean).array().pow(4.0)).sum() / n;
    if (m2 == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return m4 / (m2 * m2) - 3.0;
}

// 历史 VaR: 取收益序列的第 alpha 分位数（损失为负值）
static double StatVarHistorical(const std::vector<double> &returns, double alpha) {
    if (returns.empty() || alpha <= 0.0 || alpha >= 1.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::vector<double> sorted = returns;
    std::sort(sorted.begin(), sorted.end());
    idx_t idx = (idx_t)std::ceil(alpha * (double)sorted.size()) - 1;
    if (idx >= sorted.size()) {
        idx = sorted.size() - 1;
    }
    return sorted[idx];
}

// 最大回撤持续期（从峰到恢复到峰下方的最大下跌期，单位：periods）
static double StatMaxDrawdownDuration(const std::vector<double> &prices) {
    idx_t n = prices.size();
    if (n < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double peak = prices[0];
    idx_t peak_idx = 0;
    double max_duration = 0.0;
    idx_t trough_start = 0;
    for (idx_t i = 1; i < n; i++) {
        if (prices[i] > peak) {
            peak = prices[i];
            peak_idx = i;
            // 若已走出回撤，更新最大持续期
            double dur = (double)(i - trough_start);
            if (dur > max_duration) {
                max_duration = dur;
            }
            trough_start = i;
        } else if (prices[i] < peak) {
            // 仍在回撤中，持续期从上一个峰后算起
            double dur = (double)(i - peak_idx);
            if (dur > max_duration) {
                max_duration = dur;
            }
        }
    }
    return max_duration;
}

// ============================================================
// Scalar wrappers
// ============================================================
// 对"单个 LIST -> DOUBLE"的统计函数生成统一 scalar wrapper。
// F 为 `double (*)(const std::vector<double>&)` 形式的函数指针。
template <double (*F)(const std::vector<double> &)>
static void SimpleListScalar(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double v = F(x);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void AnnualVolFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double ppy = 252.0;
        if (args.ColumnCount() > 1) {
            bool ok = false;
            ppy = GetDoubleColumn(args, 1, i, ok);
        }
        double v = StatAnnualVol(x, ppy);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void SortinoFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double target = 0.0, ppy = 252.0;
        if (args.ColumnCount() > 1) {
            bool ok = false;
            target = GetDoubleColumn(args, 1, i, ok);
        }
        if (args.ColumnCount() > 2) {
            bool ok = false;
            ppy = GetDoubleColumn(args, 2, i, ok);
        }
        double v = StatSortino(x, target, ppy);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void InformationRatioFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> asset, benchmark;
        if (!GetListColumn(args, 0, i, asset) || !GetListColumn(args, 1, i, benchmark)) {
            rmask.SetInvalid(i);
            continue;
        }
        double ppy = 252.0;
        if (args.ColumnCount() > 2) {
            bool ok = false;
            ppy = GetDoubleColumn(args, 2, i, ok);
        }
        double v = StatInformationRatio(asset, benchmark, ppy);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void CalmarFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double ppy = 252.0;
        if (args.ColumnCount() > 1) {
            bool ok = false;
            ppy = GetDoubleColumn(args, 1, i, ok);
        }
        double v = StatCalmar(x, ppy);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void VarHistoricalFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double alpha = 0.05;
        if (args.ColumnCount() > 1) {
            bool ok = false;
            alpha = GetDoubleColumn(args, 1, i, ok);
        }
        double v = StatVarHistorical(x, alpha);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void MaxDrawdownDurationFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        rdata[i] = StatMaxDrawdownDuration(x);
    }
}

// ============================================================
// 高级指标：GARCH(1,1) 波动率
// 用最大似然/数值迭代估计 GARCH(1,1) 参数，返回当前条件波动率（年化）。
// 输入: (LIST<DOUBLE> prices, DOUBLE ppy, DOUBLE alpha, DOUBLE beta)
// ============================================================
static double StatGarchVol(const std::vector<double> &prices, double ppy, double alpha, double beta) {
    if (prices.size() < 5)
        return std::numeric_limits<double>::quiet_NaN();
    // 由价格序列计算对数收益
    std::vector<double> rets;
    rets.reserve(prices.size() - 1);
    for (size_t i = 1; i < prices.size(); i++) {
        if (prices[i - 1] > 0.0 && prices[i] > 0.0) {
            rets.push_back(std::log(prices[i] / prices[i - 1]));
        }
    }
    if (rets.size() < 4)
        return std::numeric_limits<double>::quiet_NaN();
    if (alpha < 0.0 || beta < 0.0 || alpha + beta >= 1.0)
        return std::numeric_limits<double>::quiet_NaN();

    // 无条件方差作为初始值
    double mean = 0.0;
    for (double r : rets)
        mean += r;
    mean /= (double)rets.size();
    double uv = 0.0;
    for (double r : rets)
        uv += (r - mean) * (r - mean);
    uv /= (double)(rets.size() - 1);

    double omega = (1.0 - alpha - beta) * uv;
    double var = uv;
    for (double r : rets) {
        var = omega + alpha * r * r + beta * var;
    }
    return std::sqrt(std::max(var, 0.0)) * std::sqrt(ppy);
}

// ============================================================
// 高级指标：协整检验（Engle-Granger 两步法）
// 返回 ADF 检验统计量（负值越大越协整）。
// 输入: (LIST<DOUBLE> x, LIST<DOUBLE> y)
// 第一步：y ~ x 回归得残差
// 第二步：对残差做 ADF(1) 检验，返回 t 统计量
// ============================================================
static double StatCointegration(const std::vector<double> &x, const std::vector<double> &y) {
    if (x.size() != y.size() || x.size() < 6)
        return std::numeric_limits<double>::quiet_NaN();
    Eigen::Index n = (Eigen::Index)x.size();
    Eigen::Map<const Eigen::VectorXd> vx(x.data(), n);
    Eigen::Map<const Eigen::VectorXd> vy(y.data(), n);

    // 第一步：y = a + b*x 回归
    Eigen::MatrixXd A(n, 2);
    A.col(0) = Eigen::VectorXd::Ones(n);
    A.col(1) = vx;
    Eigen::VectorXd coef = A.colPivHouseholderQr().solve(vy);
    Eigen::VectorXd resid = vy - A * coef;

    // 第二步：ADF(1) 检验，回归 diff(resid) ~ lag(resid) + 常数
    // 即 resid_t - resid_{t-1} = alpha + gamma * resid_{t-1} + e_t
    Eigen::Index m = n - 1; // 差分后样本数
    Eigen::VectorXd dres(m);
    Eigen::VectorXd lres(m);
    for (Eigen::Index i = 0; i < m; i++) {
        dres(i) = resid(i + 1) - resid(i);
        lres(i) = resid(i);
    }
    Eigen::MatrixXd X(m, 2);
    X.col(0) = Eigen::VectorXd::Ones(m);
    X.col(1) = lres;
    Eigen::VectorXd gamma_coef = X.colPivHouseholderQr().solve(dres);
    Eigen::VectorXd e = dres - X * gamma_coef;
    double ssr = e.array().square().sum();
    double dof = (double)(m - 2);
    if (dof <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    double se2 = ssr / dof;
    // gamma 的标准误差 = sqrt(se2 * (X'X)^-1 的对应对角元)
    Eigen::MatrixXd XtX_inv = (X.transpose() * X).inverse();
    double se_gamma = std::sqrt(se2 * XtX_inv(1, 1));
    if (se_gamma == 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    // t 统计量 = gamma / se_gamma（gamma 为负时表示协整）
    return gamma_coef(1) / se_gamma;
}

// ============================================================
// 高级指标：RSI（相对强弱指数）
// 用 Wilder 平滑，返回最后一个 RSI 值（0-100）。
// 输入: (LIST<DOUBLE> prices, INTEGER period)
// ============================================================
static double StatRSI(const std::vector<double> &prices, int period) {
    if (prices.size() < (size_t)(period + 1) || period < 1)
        return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> gains, losses;
    gains.reserve(prices.size() - 1);
    losses.reserve(prices.size() - 1);
    for (size_t i = 1; i < prices.size(); i++) {
        double diff = prices[i] - prices[i - 1];
        gains.push_back(diff > 0.0 ? diff : 0.0);
        losses.push_back(diff < 0.0 ? -diff : 0.0);
    }
    // Wilder 平滑
    double avg_gain = 0.0, avg_loss = 0.0;
    for (int i = 0; i < period; i++) {
        avg_gain += gains[i];
        avg_loss += losses[i];
    }
    avg_gain /= period;
    avg_loss /= period;
    for (size_t i = (size_t)period; i < gains.size(); i++) {
        avg_gain = (avg_gain * (period - 1) + gains[i]) / period;
        avg_loss = (avg_loss * (period - 1) + losses[i]) / period;
    }
    if (avg_loss == 0.0)
        return 100.0;
    double rs = avg_gain / avg_loss;
    return 100.0 - 100.0 / (1.0 + rs);
}

// ============================================================
// 高级指标：趋势强度（用线性回归 R² 衡量趋势的确定性，0-1）
// 输入: (LIST<DOUBLE> prices)
// ============================================================
static double StatTrendStrength(const std::vector<double> &prices) {
    if (prices.size() < 3)
        return std::numeric_limits<double>::quiet_NaN();
    Eigen::Index n = (Eigen::Index)prices.size();
    Eigen::Map<const Eigen::VectorXd> v(prices.data(), n);
    Eigen::VectorXd t(n);
    for (Eigen::Index i = 0; i < n; i++)
        t(i) = (double)(i + 1);
    // 回归 price ~ t
    Eigen::MatrixXd A(n, 2);
    A.col(0) = Eigen::VectorXd::Ones(n);
    A.col(1) = t;
    Eigen::VectorXd coef = A.colPivHouseholderQr().solve(v);
    Eigen::VectorXd fitted = A * coef;
    double y_mean = v.mean();
    double ss_tot = (v.array() - y_mean).square().sum();
    double ss_res = (v.array() - fitted.array()).square().sum();
    if (ss_tot == 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return 1.0 - ss_res / ss_tot;
}

// ============================================================
// Scalar wrappers
// ============================================================
static void GarchVolFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double ppy = 252.0, alpha = 0.1, beta = 0.85;
        if (args.ColumnCount() > 1) {
            bool ok = false;
            ppy = GetDoubleColumn(args, 1, i, ok);
        }
        if (args.ColumnCount() > 2) {
            bool ok = false;
            alpha = GetDoubleColumn(args, 2, i, ok);
        }
        if (args.ColumnCount() > 3) {
            bool ok = false;
            beta = GetDoubleColumn(args, 3, i, ok);
        }
        double v = StatGarchVol(x, ppy, alpha, beta);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void CointegrationFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x, y;
        if (!GetListColumn(args, 0, i, x) || !GetListColumn(args, 1, i, y)) {
            rmask.SetInvalid(i);
            continue;
        }
        double v = StatCointegration(x, y);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void RSIFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        int period = 14;
        if (args.ColumnCount() > 1) {
            bool ok = false;
            period = (int)GetDoubleColumn(args, 1, i, ok);
        }
        double v = StatRSI(x, period);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

static void TrendStrengthFunc(DataChunk &args, ExpressionState &, Vector &result) {
    auto count = args.size();
    auto rdata = FlatVector::GetData<double>(result);
    auto &rmask = FlatVector::Validity(result);
    for (idx_t i = 0; i < count; i++) {
        std::vector<double> x;
        if (!GetListColumn(args, 0, i, x)) {
            rmask.SetInvalid(i);
            continue;
        }
        double v = StatTrendStrength(x);
        if (std::isnan(v)) {
            rmask.SetInvalid(i);
        } else {
            rdata[i] = v;
        }
    }
}

// ============================================================
// Registration
// ============================================================
void RegisterCnTaStatFunctions(ExtensionLoader &loader) {
    // 单变量统计：输入 LIST<DOUBLE> -> DOUBLE
    loader.RegisterFunction(ScalarFunction("stat_var", {LIST_DOUBLE}, LogicalType::DOUBLE, VarFunc));
    loader.RegisterFunction(ScalarFunction("stat_stddev", {LIST_DOUBLE}, LogicalType::DOUBLE, StddevFunc));
    loader.RegisterFunction(ScalarFunction("stat_max_drawdown", {LIST_DOUBLE}, LogicalType::DOUBLE, MaxDrawdownFunc));
    loader.RegisterFunction(ScalarFunction("stat_sharpe", {LIST_DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
                                           LogicalType::DOUBLE, SharpeFunc));
    loader.RegisterFunction(ScalarFunction("stat_ewma_vol", {LIST_DOUBLE, LogicalType::DOUBLE},
                                           LogicalType::DOUBLE, EWMAVolFunc));

    // 双变量统计：输入 (LIST<DOUBLE>, LIST<DOUBLE>) -> DOUBLE
    loader.RegisterFunction(ScalarFunction("stat_cov", {LIST_DOUBLE, LIST_DOUBLE}, LogicalType::DOUBLE, CovFunc));
    loader.RegisterFunction(ScalarFunction("stat_corr", {LIST_DOUBLE, LIST_DOUBLE}, LogicalType::DOUBLE, CorrFunc));
    loader.RegisterFunction(ScalarFunction("stat_beta", {LIST_DOUBLE, LIST_DOUBLE}, LogicalType::DOUBLE, BetaFunc));

    // 单变量回归：输入 (LIST<DOUBLE> y, LIST<DOUBLE> x) -> STRUCT{slope, intercept, r2, std_err}
    child_list_t<LogicalType> regress_fields;
    regress_fields.push_back({"slope", LogicalType::DOUBLE});
    regress_fields.push_back({"intercept", LogicalType::DOUBLE});
    regress_fields.push_back({"r2", LogicalType::DOUBLE});
    regress_fields.push_back({"std_err", LogicalType::DOUBLE});
    LogicalType regress_struct = LogicalType::STRUCT(std::move(regress_fields));
    loader.RegisterFunction(ScalarFunction("stat_regress", {LIST_DOUBLE, LIST_DOUBLE}, regress_struct, RegressFunc));

    // 多变量 OLS 回归: (LIST<DOUBLE> y, LIST<LIST<DOUBLE>> X) -> STRUCT{coefficients, intercept, r2}
    child_list_t<LogicalType> ols_fields;
    ols_fields.push_back({"coefficients", LogicalType::LIST(LogicalType::DOUBLE)});
    ols_fields.push_back({"intercept", LogicalType::DOUBLE});
    ols_fields.push_back({"r2", LogicalType::DOUBLE});
    LogicalType ols_struct = LogicalType::STRUCT(std::move(ols_fields));
    auto x_list_type = LogicalType::LIST(LIST_DOUBLE);
    loader.RegisterFunction(ScalarFunction("stat_ols", {LIST_DOUBLE, x_list_type}, ols_struct, OLSFunc));

    // 协方差矩阵: LIST<LIST<DOUBLE>> -> LIST<LIST<DOUBLE>>
    auto matrix_type = LogicalType::LIST(LIST_DOUBLE);
    loader.RegisterFunction(ScalarFunction("stat_cov_matrix", {x_list_type}, matrix_type, CovMatrixFunc));

    // PCA: LIST<LIST<DOUBLE>> -> STRUCT{eigenvalues, eigenvectors}
    child_list_t<LogicalType> pca_fields;
    pca_fields.push_back({"eigenvalues", LogicalType::LIST(LogicalType::DOUBLE)});
    pca_fields.push_back({"eigenvectors", LogicalType::LIST(LogicalType::LIST(LogicalType::DOUBLE))});
    LogicalType pca_struct = LogicalType::STRUCT(std::move(pca_fields));
    loader.RegisterFunction(ScalarFunction("stat_pca", {x_list_type}, pca_struct, PCAFunc));

    // 扩展风险/收益指标
    loader.RegisterFunction(ScalarFunction("stat_skew", {LIST_DOUBLE}, LogicalType::DOUBLE,
                                           SimpleListScalar<StatSkew>));
    loader.RegisterFunction(ScalarFunction("stat_kurtosis", {LIST_DOUBLE}, LogicalType::DOUBLE,
                                           SimpleListScalar<StatKurtosis>));
    loader.RegisterFunction(ScalarFunction("stat_annual_vol", {LIST_DOUBLE, LogicalType::DOUBLE},
                                           LogicalType::DOUBLE, AnnualVolFunc));
    loader.RegisterFunction(ScalarFunction("stat_sortino", {LIST_DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
                                           LogicalType::DOUBLE, SortinoFunc));
    loader.RegisterFunction(ScalarFunction("stat_information_ratio",
                                           {LIST_DOUBLE, LIST_DOUBLE, LogicalType::DOUBLE},
                                           LogicalType::DOUBLE, InformationRatioFunc));
    loader.RegisterFunction(ScalarFunction("stat_calmar", {LIST_DOUBLE, LogicalType::DOUBLE},
                                           LogicalType::DOUBLE, CalmarFunc));
    loader.RegisterFunction(ScalarFunction("stat_var_historical", {LIST_DOUBLE, LogicalType::DOUBLE},
                                           LogicalType::DOUBLE, VarHistoricalFunc));
    loader.RegisterFunction(ScalarFunction("stat_max_drawdown_duration", {LIST_DOUBLE},
                                           LogicalType::DOUBLE, MaxDrawdownDurationFunc));

    // 高级指标
    loader.RegisterFunction(ScalarFunction("stat_garch_vol",
                                           {LIST_DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
                                           LogicalType::DOUBLE, GarchVolFunc));
    loader.RegisterFunction(ScalarFunction("stat_coint", {LIST_DOUBLE, LIST_DOUBLE}, LogicalType::DOUBLE,
                                           CointegrationFunc));
    loader.RegisterFunction(ScalarFunction("stat_rsi", {LIST_DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE,
                                           RSIFunc));
    loader.RegisterFunction(ScalarFunction("stat_trend_strength", {LIST_DOUBLE}, LogicalType::DOUBLE,
                                           TrendStrengthFunc));
}

} // namespace duckdb
