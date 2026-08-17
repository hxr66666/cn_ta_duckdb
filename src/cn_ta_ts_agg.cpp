// ============================================================
// cn_ta_ts_agg.cpp
//
// 带时间戳自动对齐的 TA-Lib 窗口聚合函数族：cta_*_ts
//
// 背景：
//   普通 cta_* 聚合函数只接收 value 数组，TA-Lib 按"数组元素相对顺序"滑动，
//   不感知时间轴。当分钟 K 存在缺口（收盘竞价 14:58/14:59、盘中停牌、节假日
//   休市等）时，缺口会被"挤掉"，导致指标值与日期错位。
//
// 方案（量化行业标准）：
//   新增 cta_<name>_ts(ts, ...) 窗口聚合，在 Update 阶段检测相邻时间戳之间的
//   交易日 bar 缺口，缺口处用"上一个有效值"前值填充(ffill)，使 TA-Lib 基于
//   "补齐后的连续交易日 bar 序列"计算，输出与日历时间对齐。
//
// 特性：
//   1. 交易日历：内置 2020~2026 A股节假日 + 用户可 cn_ta_set_holidays() 覆盖。
//   2. K 线周期：可选 bar_period 参数（默认 1 分钟），支持 5/15/30 分钟等。
//   3. 多输入：P1(value+period)、P3(high/low/close+period)、P5(open/high/low/close)。
//
// 交易日分钟定义（A股，1 分钟 bar）：
//   周一~周五 且 非节假日，且满足下列任一：
//     - 09:31 ~ 11:30（含两端，120 根）
//     - 13:01 ~ 14:57（含两端，117 根）
//     - 15:00（收盘竞价撮合价，1 根）
//   即排除：午休 11:31~12:59、收盘竞价 14:58/14:59、非交易时段、周末、节假日。
// ============================================================

#include "duckdb.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/date.hpp"
#include "cn_ta_trading_calendar.hpp"

extern "C" {
#include "ta_libc.h"
}

#include <vector>
#include <cstring>
#include <algorithm>
#include <set>

namespace duckdb {

// ============================================================
// 交易日 bar 判定（支持任意 bar_period 分钟）
// ============================================================

// 将 timestamp_t 分解为 weekday(0=周日) + 当日已过秒数
static inline void DecomposeMinute(timestamp_t ts, int &weekday, int &seconds_of_day) {
    date_t d;
    dtime_t t;
    Timestamp::Convert(ts, d, t);
    int dow = Date::ExtractISODayOfTheWeek(d); // 1=周一..7=周日
    weekday = (dow % 7);                        // 0=周日,1=周一..6=周六
    seconds_of_day = (int)(t.micros / 1000000);
}

// 判断某个时间戳是否是"交易日 bar 的起点/落点"（分钟整点，且落在交易时段内）
// 用于 1 分钟粒度。对更大 bar_period，判定逻辑在 TradingBarsBetween 中处理。
static bool IsTradingMinuteOnGrid(timestamp_t ts) {
    int weekday, seconds;
    DecomposeMinute(ts, weekday, seconds);
    if (weekday == 0 || weekday == 6) {
        return false;
    }
    // 节假日
    date_t d;
    dtime_t t;
    Timestamp::Convert(ts, d, t);
    if (IsHoliday(d)) {
        return false;
    }
    int hh = seconds / 3600;
    int mm = (seconds % 3600) / 60;
    int total_min = hh * 60 + mm;
    if (total_min >= 9 * 60 + 31 && total_min <= 11 * 60 + 30) {
        return true;
    }
    if (total_min >= 13 * 60 + 1 && total_min <= 14 * 60 + 57) {
        return true;
    }
    if (total_min == 15 * 60) {
        return true;
    }
    return false;
}

// 计算两个时间戳之间"应存在的交易日 bar 数"（不含两端端点本身）。
// bar_period：bar 的分钟粒度（1/5/15/30）。
// 实现：按 bar_period 分钟步进枚举，统计落在交易时段内的 bar 起点数。
static int64_t TradingBarsBetween(timestamp_t prev, timestamp_t cur, int bar_period) {
    if (cur <= prev || bar_period <= 0) {
        return 0;
    }
    const int64_t MICROS_PER_MINUTE = 60000000LL;
    // 防止 bar_period 过大导致 MICROS_PER_MINUTE * bar_period 溢出（有符号溢出是 UB）。
    // 一天最多 1440 分钟，bar_period 超过 1440 已无意义（缺口检测按分钟粒度），
    // 直接按 bar_period = 1440 封顶即可保证不溢出。
    if (bar_period > 1440) {
        bar_period = 1440;
    }
    int64_t step_micros = MICROS_PER_MINUTE * (int64_t)bar_period;
    int64_t prev_micros = (int64_t)prev.value;
    int64_t cur_micros = (int64_t)cur.value;

    int64_t diff_minutes = (cur_micros - prev_micros) / MICROS_PER_MINUTE;
    // 保护：跨度超过 1 年视为异常，不补齐
    if (diff_minutes <= 0 || diff_minutes > 366 * 24 * 60) {
        return 0;
    }

    // 枚举 prev 之后到 cur 之前的每个 bar 起点，统计交易日 bar
    int64_t count = 0;
    timestamp_t t = prev;
    int64_t max_steps = diff_minutes / bar_period + 2;
    for (int64_t i = 0; i < max_steps; i++) {
        t.value += step_micros;
        if (t.value >= cur_micros) {
            break;
        }
        if (IsTradingMinuteOnGrid(t)) {
            count++;
        }
    }
    return count;
}

// ============================================================
// 运行时辅助：用户自定义休市日
// ============================================================

// 标量函数 cn_ta_is_trading_day(ts) -> BOOLEAN
static void IsTradingDayFunc(DataChunk &args, ExpressionState &, Vector &result) {
    UnifiedVectorFormat tdata;
    args.data[0].ToUnifiedFormat(args.size(), tdata);
    auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
    auto &rmask = FlatVector::Validity(result);
    auto rdata = FlatVector::GetData<bool>(result);

    for (idx_t i = 0; i < args.size(); i++) {
        auto idx = tdata.sel->get_index(i);
        if (!tdata.validity.RowIsValid(idx)) {
            rmask.SetInvalid(i);
            continue;
        }
        rdata[i] = IsTradingDay(tss[idx]);
    }
}

// 标量函数 cn_ta_set_holiday(DATE) -> BIGINT（返回 1 表示已添加，0 表示重复）
// 追加一个用户自定义休市日到全局集合（线程安全）。
static void SetHolidayFunc(DataChunk &args, ExpressionState &, Vector &result) {
    UnifiedVectorFormat ddata;
    args.data[0].ToUnifiedFormat(args.size(), ddata);
    auto dvals = UnifiedVectorFormat::GetData<date_t>(ddata);
    auto &rmask = FlatVector::Validity(result);
    auto rdata = FlatVector::GetData<int64_t>(result);

    std::lock_guard<std::mutex> lock(GetHolidaysMutex());
    auto &holidays = GetCustomHolidays();
    for (idx_t i = 0; i < args.size(); i++) {
        auto idx = ddata.sel->get_index(i);
        if (!ddata.validity.RowIsValid(idx)) {
            rmask.SetInvalid(i);
            continue;
        }
        auto ret = holidays.insert(dvals[idx].days);
        rdata[i] = ret.second ? 1 : 0;
    }
}

// 标量函数 cn_ta_clear_holidays() -> BIGINT（清空用户自定义休市日，返回清除数量）
static void ClearHolidaysFunc(DataChunk &args, ExpressionState &, Vector &result) {
    std::lock_guard<std::mutex> lock(GetHolidaysMutex());
    auto &holidays = GetCustomHolidays();
    idx_t n = holidays.size();
    holidays.clear();
    auto rdata = FlatVector::GetData<int64_t>(result);
    for (idx_t i = 0; i < args.size(); i++) {
        rdata[i] = (int64_t)n;
    }
}

// ============================================================
// State（通用，支持 P1/P3/P5 通过模板共享对齐逻辑）
// ============================================================

// 对齐状态：记录上一个 bar 的时间戳与各输入值，用于缺口 ffill。
struct CnTaTsAlignState {
    bool has_prev;
    timestamp_t prev_ts;
    // 前值（按输入列保存，最多 4 列：value / high/low/close / open,high,low,close）
    double prev_vals[4];
    int n_cols;
};

// ============================================================
// 尾截断优化白名单
// ============================================================
static bool TsIsTruncatableLookback(int (*lb)(int)) {
    static const std::set<int (*)(int)> truncatable = {
        TA_SMA_Lookback, TA_WMA_Lookback, TA_TRIMA_Lookback, TA_MIDPOINT_Lookback,
        TA_MOM_Lookback, TA_ROC_Lookback, TA_ROCP_Lookback, TA_ROCR_Lookback,
        TA_ROCR100_Lookback, TA_SUM_Lookback, TA_MAX_Lookback, TA_MIN_Lookback,
        TA_MAXINDEX_Lookback, TA_MININDEX_Lookback, TA_TSF_Lookback,
        TA_LINEARREG_Lookback, TA_LINEARREG_ANGLE_Lookback,
        TA_LINEARREG_INTERCEPT_Lookback, TA_LINEARREG_SLOPE_Lookback,
    };
    return truncatable.count(lb) > 0;
}

// 向 values 追加并做尾截断（仅白名单）
static inline void TsAppendBounded(std::vector<double> *values, double value, int period, int (*LOOKBACK)(int)) {
    values->push_back(value);
    if (LOOKBACK && TsIsTruncatableLookback(LOOKBACK)) {
        int max_len = LOOKBACK(period) + 1;
        if ((int)values->size() >= 2 * max_len) {
            values->erase(values->begin(), values->begin() + max_len);
        }
    }
}

// ============================================================
// P1 DOUBLE: (ts, value, period[, bar_period])
// ============================================================
struct CnTaTsState1 {
    std::vector<double> *values;
    int period;
    int bar_period;
    CnTaTsAlignState align;
};

template <TA_RetCode (*TA_FUNC)(int, int, const double[], int, int*, int*, double[]),
          int (*LOOKBACK)(int) = nullptr>
struct CnTaTsAggP1 {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(CnTaTsState1); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<CnTaTsState1 *>(state);
        s->values = nullptr;
        s->period = 0;
        s->bar_period = 1;
        s->align.has_prev = false;
        s->align.prev_ts = timestamp_t(0);
        s->align.n_cols = 1;
    }

    // inputs[0]=ts, inputs[1]=value, inputs[2]=period, inputs[3]=bar_period(可选)
    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat tdata, vdata, pdata, bdata, sdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, vdata);
        inputs[2].ToUnifiedFormat(count, pdata);
        bool has_bar = input_count > 3;
        if (has_bar) {
            inputs[3].ToUnifiedFormat(count, bdata);
        }
        states_vec.ToUnifiedFormat(count, sdata);

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto values = UnifiedVectorFormat::GetData<double>(vdata);
        auto periods = UnifiedVectorFormat::GetData<int32_t>(pdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;
        auto states = (CnTaTsState1 **)sdata.data;

        for (idx_t i = 0; i < count; i++) {
            auto sidx = sdata.sel->get_index(i);
            auto tidx = tdata.sel->get_index(i);
            auto vidx = vdata.sel->get_index(i);
            auto pidx = pdata.sel->get_index(i);
            auto state = states[sidx];

            if (!state->values) {
                state->values = new std::vector<double>();
            }
            state->period = periods[pidx];
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (vdata.validity.RowIsValid(vidx) && tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double cur_value = values[vidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        TsAppendBounded(state->values, state->align.prev_vals[0], state->period, LOOKBACK);
                    }
                }

                TsAppendBounded(state->values, cur_value, state->period, LOOKBACK);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = cur_value;
                state->align.has_prev = true;
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<CnTaTsState1 *>(state_p);
        UnifiedVectorFormat tdata, vdata, pdata, bdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, vdata);
        inputs[2].ToUnifiedFormat(count, pdata);
        bool has_bar = input_count > 3;
        if (has_bar) {
            inputs[3].ToUnifiedFormat(count, bdata);
        }

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto values = UnifiedVectorFormat::GetData<double>(vdata);
        auto periods = UnifiedVectorFormat::GetData<int32_t>(pdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;

        if (!state->values) {
            state->values = new std::vector<double>();
        }

        for (idx_t i = 0; i < count; i++) {
            auto tidx = tdata.sel->get_index(i);
            auto vidx = vdata.sel->get_index(i);
            auto pidx = pdata.sel->get_index(i);
            state->period = periods[pidx];
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (vdata.validity.RowIsValid(vidx) && tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double cur_value = values[vidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        TsAppendBounded(state->values, state->align.prev_vals[0], state->period, LOOKBACK);
                    }
                }
                TsAppendBounded(state->values, cur_value, state->period, LOOKBACK);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = cur_value;
                state->align.has_prev = true;
            }
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto src = FlatVector::GetData<CnTaTsState1 *>(source);
        auto tgt = FlatVector::GetData<CnTaTsState1 *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (src[i]->values && !src[i]->values->empty()) {
                if (!tgt[i]->values) {
                    tgt[i]->values = new std::vector<double>();
                }
                tgt[i]->values->insert(tgt[i]->values->end(), src[i]->values->begin(), src[i]->values->end());
                tgt[i]->period = src[i]->period;
                tgt[i]->bar_period = src[i]->bar_period;
                tgt[i]->align = src[i]->align;
                if (LOOKBACK && TsIsTruncatableLookback(LOOKBACK)) {
                    int max_len = LOOKBACK(tgt[i]->period) + 1;
                    if ((int)tgt[i]->values->size() >= 2 * max_len) {
                        tgt[i]->values->erase(tgt[i]->values->begin(), tgt[i]->values->begin() + max_len);
                    }
                }
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<CnTaTsState1 *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);

        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->values || state->values->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            int size = (int)state->values->size();
            int outBeg = 0, outNb = 0;
            int start = 0;
            if (LOOKBACK && TsIsTruncatableLookback(LOOKBACK)) {
                int lb = LOOKBACK(state->period);
                start = std::max(0, size - lb - 1);
            }
            std::vector<double> outReal(size);
            TA_RetCode rc = TA_FUNC(start, size - 1, state->values->data(), state->period, &outBeg, &outNb, outReal.data());
            if (rc != TA_SUCCESS || outNb == 0) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = outReal[outNb - 1];
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<CnTaTsState1 *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            if (states[i]->values) {
                delete states[i]->values;
                states[i]->values = nullptr;
            }
        }
    }
};

// ============================================================
// P3 DOUBLE: (ts, high, low, close, period[, bar_period])
// ============================================================
struct CnTaTsState3 {
    std::vector<double> *high;
    std::vector<double> *low;
    std::vector<double> *close;
    int period;
    int bar_period;
    CnTaTsAlignState align;
};

template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], int, int*, int*, double[])>
struct CnTaTsAggP3 {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(CnTaTsState3); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<CnTaTsState3 *>(state);
        s->high = nullptr;
        s->low = nullptr;
        s->close = nullptr;
        s->period = 0;
        s->bar_period = 1;
        s->align.has_prev = false;
        s->align.prev_ts = timestamp_t(0);
        s->align.n_cols = 3;
    }

    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat tdata, hdata, ldata, cdata, pdata, bdata, sdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        inputs[3].ToUnifiedFormat(count, cdata);
        inputs[4].ToUnifiedFormat(count, pdata);
        bool has_bar = input_count > 5;
        if (has_bar) {
            inputs[5].ToUnifiedFormat(count, bdata);
        }
        states_vec.ToUnifiedFormat(count, sdata);

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto cvals = UnifiedVectorFormat::GetData<double>(cdata);
        auto periods = UnifiedVectorFormat::GetData<int32_t>(pdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;
        auto states = (CnTaTsState3 **)sdata.data;

        for (idx_t i = 0; i < count; i++) {
            auto sidx = sdata.sel->get_index(i);
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto cidx = cdata.sel->get_index(i);
            auto pidx = pdata.sel->get_index(i);
            auto state = states[sidx];

            if (!state->high) {
                state->high = new std::vector<double>();
                state->low = new std::vector<double>();
                state->close = new std::vector<double>();
            }
            state->period = periods[pidx];
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                cdata.validity.RowIsValid(cidx) && tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx], cv = cvals[cidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                        state->close->push_back(state->align.prev_vals[2]);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->close->push_back(cv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.prev_vals[2] = cv;
                state->align.has_prev = true;
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<CnTaTsState3 *>(state_p);
        UnifiedVectorFormat tdata, hdata, ldata, cdata, pdata, bdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        inputs[3].ToUnifiedFormat(count, cdata);
        inputs[4].ToUnifiedFormat(count, pdata);
        bool has_bar = input_count > 5;
        if (has_bar) {
            inputs[5].ToUnifiedFormat(count, bdata);
        }

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto cvals = UnifiedVectorFormat::GetData<double>(cdata);
        auto periods = UnifiedVectorFormat::GetData<int32_t>(pdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;

        if (!state->high) {
            state->high = new std::vector<double>();
            state->low = new std::vector<double>();
            state->close = new std::vector<double>();
        }

        for (idx_t i = 0; i < count; i++) {
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto cidx = cdata.sel->get_index(i);
            auto pidx = pdata.sel->get_index(i);
            state->period = periods[pidx];
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                cdata.validity.RowIsValid(cidx) && tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx], cv = cvals[cidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                        state->close->push_back(state->align.prev_vals[2]);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->close->push_back(cv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.prev_vals[2] = cv;
                state->align.has_prev = true;
            }
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto src = FlatVector::GetData<CnTaTsState3 *>(source);
        auto tgt = FlatVector::GetData<CnTaTsState3 *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (src[i]->high && !src[i]->high->empty()) {
                if (!tgt[i]->high) {
                    tgt[i]->high = new std::vector<double>();
                    tgt[i]->low = new std::vector<double>();
                    tgt[i]->close = new std::vector<double>();
                }
                tgt[i]->high->insert(tgt[i]->high->end(), src[i]->high->begin(), src[i]->high->end());
                tgt[i]->low->insert(tgt[i]->low->end(), src[i]->low->begin(), src[i]->low->end());
                tgt[i]->close->insert(tgt[i]->close->end(), src[i]->close->begin(), src[i]->close->end());
                tgt[i]->period = src[i]->period;
                tgt[i]->bar_period = src[i]->bar_period;
                tgt[i]->align = src[i]->align;
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<CnTaTsState3 *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);
        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->high || state->high->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            int size = (int)state->high->size();
            int outBeg = 0, outNb = 0;
            std::vector<double> outReal(size);
            TA_RetCode rc = TA_FUNC(0, size - 1, state->high->data(), state->low->data(), state->close->data(),
                                    state->period, &outBeg, &outNb, outReal.data());
            if (rc != TA_SUCCESS || outNb == 0) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = outReal[outNb - 1];
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<CnTaTsState3 *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            delete states[i]->high;
            delete states[i]->low;
            delete states[i]->close;
            states[i]->high = nullptr;
            states[i]->low = nullptr;
            states[i]->close = nullptr;
        }
    }
};

// ============================================================
// P5 DOUBLE: (ts, open, high, low, close[, bar_period])
// ============================================================
struct CnTaTsState5 {
    std::vector<double> *open_;
    std::vector<double> *high;
    std::vector<double> *low;
    std::vector<double> *close;
    int bar_period;
    CnTaTsAlignState align;
};

template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], const double[], int*, int*, double[])>
struct CnTaTsAggP5Double {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(CnTaTsState5); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<CnTaTsState5 *>(state);
        s->open_ = nullptr;
        s->high = nullptr;
        s->low = nullptr;
        s->close = nullptr;
        s->bar_period = 1;
        s->align.has_prev = false;
        s->align.prev_ts = timestamp_t(0);
        s->align.n_cols = 4;
    }

    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat tdata, odata, hdata, ldata, cdata, bdata, sdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, odata);
        inputs[2].ToUnifiedFormat(count, hdata);
        inputs[3].ToUnifiedFormat(count, ldata);
        inputs[4].ToUnifiedFormat(count, cdata);
        bool has_bar = input_count > 5;
        if (has_bar) {
            inputs[5].ToUnifiedFormat(count, bdata);
        }
        states_vec.ToUnifiedFormat(count, sdata);

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto ovals = UnifiedVectorFormat::GetData<double>(odata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto cvals = UnifiedVectorFormat::GetData<double>(cdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;
        auto states = (CnTaTsState5 **)sdata.data;

        for (idx_t i = 0; i < count; i++) {
            auto sidx = sdata.sel->get_index(i);
            auto tidx = tdata.sel->get_index(i);
            auto oidx = odata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto cidx = cdata.sel->get_index(i);
            auto state = states[sidx];

            if (!state->open_) {
                state->open_ = new std::vector<double>();
                state->high = new std::vector<double>();
                state->low = new std::vector<double>();
                state->close = new std::vector<double>();
            }
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (odata.validity.RowIsValid(oidx) && hdata.validity.RowIsValid(hidx) &&
                ldata.validity.RowIsValid(lidx) && cdata.validity.RowIsValid(cidx) &&
                tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double ov = ovals[oidx], hv = hvals[hidx], lv = lvals[lidx], cv = cvals[cidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->open_->push_back(state->align.prev_vals[0]);
                        state->high->push_back(state->align.prev_vals[1]);
                        state->low->push_back(state->align.prev_vals[2]);
                        state->close->push_back(state->align.prev_vals[3]);
                    }
                }
                state->open_->push_back(ov);
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->close->push_back(cv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = ov;
                state->align.prev_vals[1] = hv;
                state->align.prev_vals[2] = lv;
                state->align.prev_vals[3] = cv;
                state->align.has_prev = true;
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<CnTaTsState5 *>(state_p);
        UnifiedVectorFormat tdata, odata, hdata, ldata, cdata, bdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, odata);
        inputs[2].ToUnifiedFormat(count, hdata);
        inputs[3].ToUnifiedFormat(count, ldata);
        inputs[4].ToUnifiedFormat(count, cdata);
        bool has_bar = input_count > 5;
        if (has_bar) {
            inputs[5].ToUnifiedFormat(count, bdata);
        }

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto ovals = UnifiedVectorFormat::GetData<double>(odata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto cvals = UnifiedVectorFormat::GetData<double>(cdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;

        if (!state->open_) {
            state->open_ = new std::vector<double>();
            state->high = new std::vector<double>();
            state->low = new std::vector<double>();
            state->close = new std::vector<double>();
        }

        for (idx_t i = 0; i < count; i++) {
            auto tidx = tdata.sel->get_index(i);
            auto oidx = odata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto cidx = cdata.sel->get_index(i);
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (odata.validity.RowIsValid(oidx) && hdata.validity.RowIsValid(hidx) &&
                ldata.validity.RowIsValid(lidx) && cdata.validity.RowIsValid(cidx) &&
                tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double ov = ovals[oidx], hv = hvals[hidx], lv = lvals[lidx], cv = cvals[cidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->open_->push_back(state->align.prev_vals[0]);
                        state->high->push_back(state->align.prev_vals[1]);
                        state->low->push_back(state->align.prev_vals[2]);
                        state->close->push_back(state->align.prev_vals[3]);
                    }
                }
                state->open_->push_back(ov);
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->close->push_back(cv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = ov;
                state->align.prev_vals[1] = hv;
                state->align.prev_vals[2] = lv;
                state->align.prev_vals[3] = cv;
                state->align.has_prev = true;
            }
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto src = FlatVector::GetData<CnTaTsState5 *>(source);
        auto tgt = FlatVector::GetData<CnTaTsState5 *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (src[i]->open_ && !src[i]->open_->empty()) {
                if (!tgt[i]->open_) {
                    tgt[i]->open_ = new std::vector<double>();
                    tgt[i]->high = new std::vector<double>();
                    tgt[i]->low = new std::vector<double>();
                    tgt[i]->close = new std::vector<double>();
                }
                tgt[i]->open_->insert(tgt[i]->open_->end(), src[i]->open_->begin(), src[i]->open_->end());
                tgt[i]->high->insert(tgt[i]->high->end(), src[i]->high->begin(), src[i]->high->end());
                tgt[i]->low->insert(tgt[i]->low->end(), src[i]->low->begin(), src[i]->low->end());
                tgt[i]->close->insert(tgt[i]->close->end(), src[i]->close->begin(), src[i]->close->end());
                tgt[i]->bar_period = src[i]->bar_period;
                tgt[i]->align = src[i]->align;
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<CnTaTsState5 *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);
        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->open_ || state->open_->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            int size = (int)state->open_->size();
            int outBeg = 0, outNb = 0;
            std::vector<double> outReal(size);
            TA_RetCode rc = TA_FUNC(0, size - 1, state->open_->data(), state->high->data(),
                                    state->low->data(), state->close->data(), &outBeg, &outNb, outReal.data());
            if (rc != TA_SUCCESS || outNb == 0) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = outReal[outNb - 1];
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<CnTaTsState5 *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            delete states[i]->open_;
            delete states[i]->high;
            delete states[i]->low;
            delete states[i]->close;
            states[i]->open_ = nullptr;
            states[i]->high = nullptr;
            states[i]->low = nullptr;
            states[i]->close = nullptr;
        }
    }
};

// ============================================================
// P4 DOUBLE: (ts, high, low, close, volume[, bar_period])
// ============================================================
struct CnTaTsState4 {
    std::vector<double> *high;
    std::vector<double> *low;
    std::vector<double> *close;
    std::vector<double> *volume;
    int bar_period;
    CnTaTsAlignState align;
};

template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], const double[], int*, int*, double[])>
struct CnTaTsAggP4 {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(CnTaTsState4); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<CnTaTsState4 *>(state);
        s->high = nullptr;
        s->low = nullptr;
        s->close = nullptr;
        s->volume = nullptr;
        s->bar_period = 1;
        s->align.has_prev = false;
        s->align.prev_ts = timestamp_t(0);
        s->align.n_cols = 4;
    }

    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat tdata, hdata, ldata, cdata, vdata, bdata, sdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        inputs[3].ToUnifiedFormat(count, cdata);
        inputs[4].ToUnifiedFormat(count, vdata);
        bool has_bar = input_count > 5;
        if (has_bar) {
            inputs[5].ToUnifiedFormat(count, bdata);
        }
        states_vec.ToUnifiedFormat(count, sdata);

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto cvals = UnifiedVectorFormat::GetData<double>(cdata);
        auto vvals = UnifiedVectorFormat::GetData<double>(vdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;
        auto states = (CnTaTsState4 **)sdata.data;

        for (idx_t i = 0; i < count; i++) {
            auto sidx = sdata.sel->get_index(i);
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto cidx = cdata.sel->get_index(i);
            auto vidx = vdata.sel->get_index(i);
            auto state = states[sidx];

            if (!state->high) {
                state->high = new std::vector<double>();
                state->low = new std::vector<double>();
                state->close = new std::vector<double>();
                state->volume = new std::vector<double>();
            }
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                cdata.validity.RowIsValid(cidx) && vdata.validity.RowIsValid(vidx) &&
                tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx], cv = cvals[cidx], vv = vvals[vidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                        state->close->push_back(state->align.prev_vals[2]);
                        state->volume->push_back(0.0); // 缺口处成交量填 0
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->close->push_back(cv);
                state->volume->push_back(vv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.prev_vals[2] = cv;
                state->align.prev_vals[3] = vv;
                state->align.has_prev = true;
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<CnTaTsState4 *>(state_p);
        UnifiedVectorFormat tdata, hdata, ldata, cdata, vdata, bdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        inputs[3].ToUnifiedFormat(count, cdata);
        inputs[4].ToUnifiedFormat(count, vdata);
        bool has_bar = input_count > 5;
        if (has_bar) {
            inputs[5].ToUnifiedFormat(count, bdata);
        }

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto cvals = UnifiedVectorFormat::GetData<double>(cdata);
        auto vvals = UnifiedVectorFormat::GetData<double>(vdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;

        if (!state->high) {
            state->high = new std::vector<double>();
            state->low = new std::vector<double>();
            state->close = new std::vector<double>();
            state->volume = new std::vector<double>();
        }

        for (idx_t i = 0; i < count; i++) {
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto cidx = cdata.sel->get_index(i);
            auto vidx = vdata.sel->get_index(i);
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                cdata.validity.RowIsValid(cidx) && vdata.validity.RowIsValid(vidx) &&
                tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx], cv = cvals[cidx], vv = vvals[vidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                        state->close->push_back(state->align.prev_vals[2]);
                        state->volume->push_back(0.0);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->close->push_back(cv);
                state->volume->push_back(vv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.prev_vals[2] = cv;
                state->align.prev_vals[3] = vv;
                state->align.has_prev = true;
            }
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto src = FlatVector::GetData<CnTaTsState4 *>(source);
        auto tgt = FlatVector::GetData<CnTaTsState4 *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (src[i]->high && !src[i]->high->empty()) {
                if (!tgt[i]->high) {
                    tgt[i]->high = new std::vector<double>();
                    tgt[i]->low = new std::vector<double>();
                    tgt[i]->close = new std::vector<double>();
                    tgt[i]->volume = new std::vector<double>();
                }
                tgt[i]->high->insert(tgt[i]->high->end(), src[i]->high->begin(), src[i]->high->end());
                tgt[i]->low->insert(tgt[i]->low->end(), src[i]->low->begin(), src[i]->low->end());
                tgt[i]->close->insert(tgt[i]->close->end(), src[i]->close->begin(), src[i]->close->end());
                tgt[i]->volume->insert(tgt[i]->volume->end(), src[i]->volume->begin(), src[i]->volume->end());
                tgt[i]->bar_period = src[i]->bar_period;
                tgt[i]->align = src[i]->align;
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<CnTaTsState4 *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);
        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->high || state->high->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            int size = (int)state->high->size();
            int outBeg = 0, outNb = 0;
            std::vector<double> outReal(size);
            TA_RetCode rc = TA_FUNC(0, size - 1, state->high->data(), state->low->data(), state->close->data(),
                                    state->volume->data(), &outBeg, &outNb, outReal.data());
            if (rc != TA_SUCCESS || outNb == 0) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = outReal[outNb - 1];
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<CnTaTsState4 *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            delete states[i]->high;
            delete states[i]->low;
            delete states[i]->close;
            delete states[i]->volume;
            states[i]->high = nullptr;
            states[i]->low = nullptr;
            states[i]->close = nullptr;
            states[i]->volume = nullptr;
        }
    }
};

// ============================================================
// P6 DOUBLE: (ts, high, low[, bar_period])
// ============================================================
struct CnTaTsState6 {
    std::vector<double> *high;
    std::vector<double> *low;
    int bar_period;
    CnTaTsAlignState align;
};

template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], int*, int*, double[])>
struct CnTaTsAggP6 {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(CnTaTsState6); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<CnTaTsState6 *>(state);
        s->high = nullptr;
        s->low = nullptr;
        s->bar_period = 1;
        s->align.has_prev = false;
        s->align.prev_ts = timestamp_t(0);
        s->align.n_cols = 2;
    }

    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat tdata, hdata, ldata, bdata, sdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        bool has_bar = input_count > 3;
        if (has_bar) {
            inputs[3].ToUnifiedFormat(count, bdata);
        }
        states_vec.ToUnifiedFormat(count, sdata);

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;
        auto states = (CnTaTsState6 **)sdata.data;

        for (idx_t i = 0; i < count; i++) {
            auto sidx = sdata.sel->get_index(i);
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto state = states[sidx];

            if (!state->high) {
                state->high = new std::vector<double>();
                state->low = new std::vector<double>();
            }
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.has_prev = true;
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<CnTaTsState6 *>(state_p);
        UnifiedVectorFormat tdata, hdata, ldata, bdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        bool has_bar = input_count > 3;
        if (has_bar) {
            inputs[3].ToUnifiedFormat(count, bdata);
        }

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;

        if (!state->high) {
            state->high = new std::vector<double>();
            state->low = new std::vector<double>();
        }

        for (idx_t i = 0; i < count; i++) {
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.has_prev = true;
            }
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto src = FlatVector::GetData<CnTaTsState6 *>(source);
        auto tgt = FlatVector::GetData<CnTaTsState6 *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (src[i]->high && !src[i]->high->empty()) {
                if (!tgt[i]->high) {
                    tgt[i]->high = new std::vector<double>();
                    tgt[i]->low = new std::vector<double>();
                }
                tgt[i]->high->insert(tgt[i]->high->end(), src[i]->high->begin(), src[i]->high->end());
                tgt[i]->low->insert(tgt[i]->low->end(), src[i]->low->begin(), src[i]->low->end());
                tgt[i]->bar_period = src[i]->bar_period;
                tgt[i]->align = src[i]->align;
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<CnTaTsState6 *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);
        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->high || state->high->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            int size = (int)state->high->size();
            int outBeg = 0, outNb = 0;
            std::vector<double> outReal(size);
            TA_RetCode rc = TA_FUNC(0, size - 1, state->high->data(), state->low->data(), &outBeg, &outNb, outReal.data());
            if (rc != TA_SUCCESS || outNb == 0) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = outReal[outNb - 1];
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<CnTaTsState6 *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            delete states[i]->high;
            delete states[i]->low;
            states[i]->high = nullptr;
            states[i]->low = nullptr;
        }
    }
};

// ============================================================
// P7 DOUBLE: (ts, high, low, close[, bar_period])
// ============================================================
struct CnTaTsState7 {
    std::vector<double> *high;
    std::vector<double> *low;
    std::vector<double> *close;
    int bar_period;
    CnTaTsAlignState align;
};

template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], int*, int*, double[])>
struct CnTaTsAggP7 {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(CnTaTsState7); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<CnTaTsState7 *>(state);
        s->high = nullptr;
        s->low = nullptr;
        s->close = nullptr;
        s->bar_period = 1;
        s->align.has_prev = false;
        s->align.prev_ts = timestamp_t(0);
        s->align.n_cols = 3;
    }

    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat tdata, hdata, ldata, cdata, bdata, sdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        inputs[3].ToUnifiedFormat(count, cdata);
        bool has_bar = input_count > 4;
        if (has_bar) {
            inputs[4].ToUnifiedFormat(count, bdata);
        }
        states_vec.ToUnifiedFormat(count, sdata);

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto cvals = UnifiedVectorFormat::GetData<double>(cdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;
        auto states = (CnTaTsState7 **)sdata.data;

        for (idx_t i = 0; i < count; i++) {
            auto sidx = sdata.sel->get_index(i);
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto cidx = cdata.sel->get_index(i);
            auto state = states[sidx];

            if (!state->high) {
                state->high = new std::vector<double>();
                state->low = new std::vector<double>();
                state->close = new std::vector<double>();
            }
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                cdata.validity.RowIsValid(cidx) && tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx], cv = cvals[cidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                        state->close->push_back(state->align.prev_vals[2]);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->close->push_back(cv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.prev_vals[2] = cv;
                state->align.has_prev = true;
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<CnTaTsState7 *>(state_p);
        UnifiedVectorFormat tdata, hdata, ldata, cdata, bdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        inputs[3].ToUnifiedFormat(count, cdata);
        bool has_bar = input_count > 4;
        if (has_bar) {
            inputs[4].ToUnifiedFormat(count, bdata);
        }

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto cvals = UnifiedVectorFormat::GetData<double>(cdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;

        if (!state->high) {
            state->high = new std::vector<double>();
            state->low = new std::vector<double>();
            state->close = new std::vector<double>();
        }

        for (idx_t i = 0; i < count; i++) {
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto cidx = cdata.sel->get_index(i);
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                cdata.validity.RowIsValid(cidx) && tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx], cv = cvals[cidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                        state->close->push_back(state->align.prev_vals[2]);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->close->push_back(cv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.prev_vals[2] = cv;
                state->align.has_prev = true;
            }
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto src = FlatVector::GetData<CnTaTsState7 *>(source);
        auto tgt = FlatVector::GetData<CnTaTsState7 *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (src[i]->high && !src[i]->high->empty()) {
                if (!tgt[i]->high) {
                    tgt[i]->high = new std::vector<double>();
                    tgt[i]->low = new std::vector<double>();
                    tgt[i]->close = new std::vector<double>();
                }
                tgt[i]->high->insert(tgt[i]->high->end(), src[i]->high->begin(), src[i]->high->end());
                tgt[i]->low->insert(tgt[i]->low->end(), src[i]->low->begin(), src[i]->low->end());
                tgt[i]->close->insert(tgt[i]->close->end(), src[i]->close->begin(), src[i]->close->end());
                tgt[i]->bar_period = src[i]->bar_period;
                tgt[i]->align = src[i]->align;
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<CnTaTsState7 *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);
        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->high || state->high->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            int size = (int)state->high->size();
            int outBeg = 0, outNb = 0;
            std::vector<double> outReal(size);
            TA_RetCode rc = TA_FUNC(0, size - 1, state->high->data(), state->low->data(), state->close->data(),
                                    &outBeg, &outNb, outReal.data());
            if (rc != TA_SUCCESS || outNb == 0) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = outReal[outNb - 1];
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<CnTaTsState7 *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            delete states[i]->high;
            delete states[i]->low;
            delete states[i]->close;
            states[i]->high = nullptr;
            states[i]->low = nullptr;
            states[i]->close = nullptr;
        }
    }
};

// ============================================================
// P8 DOUBLE: (ts, high, low, period[, bar_period])
// ============================================================
struct CnTaTsState8 {
    std::vector<double> *high;
    std::vector<double> *low;
    int period;
    int bar_period;
    CnTaTsAlignState align;
};

template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], int, int*, int*, double[])>
struct CnTaTsAggP8 {
    static idx_t StateSize(const AggregateFunction &) { return sizeof(CnTaTsState8); }

    static void Initialize(const AggregateFunction &, data_ptr_t state) {
        auto s = reinterpret_cast<CnTaTsState8 *>(state);
        s->high = nullptr;
        s->low = nullptr;
        s->period = 0;
        s->bar_period = 1;
        s->align.has_prev = false;
        s->align.prev_ts = timestamp_t(0);
        s->align.n_cols = 2;
    }

    static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &states_vec, idx_t count) {
        UnifiedVectorFormat tdata, hdata, ldata, pdata, bdata, sdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        inputs[3].ToUnifiedFormat(count, pdata);
        bool has_bar = input_count > 4;
        if (has_bar) {
            inputs[4].ToUnifiedFormat(count, bdata);
        }
        states_vec.ToUnifiedFormat(count, sdata);

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto periods = UnifiedVectorFormat::GetData<int32_t>(pdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;
        auto states = (CnTaTsState8 **)sdata.data;

        for (idx_t i = 0; i < count; i++) {
            auto sidx = sdata.sel->get_index(i);
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto pidx = pdata.sel->get_index(i);
            auto state = states[sidx];

            if (!state->high) {
                state->high = new std::vector<double>();
                state->low = new std::vector<double>();
            }
            state->period = periods[pidx];
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.has_prev = true;
            }
        }
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, data_ptr_t state_p, idx_t count) {
        auto state = reinterpret_cast<CnTaTsState8 *>(state_p);
        UnifiedVectorFormat tdata, hdata, ldata, pdata, bdata;
        inputs[0].ToUnifiedFormat(count, tdata);
        inputs[1].ToUnifiedFormat(count, hdata);
        inputs[2].ToUnifiedFormat(count, ldata);
        inputs[3].ToUnifiedFormat(count, pdata);
        bool has_bar = input_count > 4;
        if (has_bar) {
            inputs[4].ToUnifiedFormat(count, bdata);
        }

        auto tss = UnifiedVectorFormat::GetData<timestamp_t>(tdata);
        auto hvals = UnifiedVectorFormat::GetData<double>(hdata);
        auto lvals = UnifiedVectorFormat::GetData<double>(ldata);
        auto periods = UnifiedVectorFormat::GetData<int32_t>(pdata);
        auto bars = has_bar ? UnifiedVectorFormat::GetData<int32_t>(bdata) : nullptr;

        if (!state->high) {
            state->high = new std::vector<double>();
            state->low = new std::vector<double>();
        }

        for (idx_t i = 0; i < count; i++) {
            auto tidx = tdata.sel->get_index(i);
            auto hidx = hdata.sel->get_index(i);
            auto lidx = ldata.sel->get_index(i);
            auto pidx = pdata.sel->get_index(i);
            state->period = periods[pidx];
            if (has_bar) {
                auto bidx = bdata.sel->get_index(i);
                state->bar_period = bars[bidx] > 0 ? bars[bidx] : 1;
            }

            if (hdata.validity.RowIsValid(hidx) && ldata.validity.RowIsValid(lidx) &&
                tdata.validity.RowIsValid(tidx)) {
                timestamp_t cur_ts = tss[tidx];
                double hv = hvals[hidx], lv = lvals[lidx];

                if (state->align.has_prev) {
                    int64_t gap = TradingBarsBetween(state->align.prev_ts, cur_ts, state->bar_period);
                    for (int64_t g = 0; g < gap; g++) {
                        state->high->push_back(state->align.prev_vals[0]);
                        state->low->push_back(state->align.prev_vals[1]);
                    }
                }
                state->high->push_back(hv);
                state->low->push_back(lv);
                state->align.prev_ts = cur_ts;
                state->align.prev_vals[0] = hv;
                state->align.prev_vals[1] = lv;
                state->align.has_prev = true;
            }
        }
    }

    static void Combine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
        auto src = FlatVector::GetData<CnTaTsState8 *>(source);
        auto tgt = FlatVector::GetData<CnTaTsState8 *>(target);
        for (idx_t i = 0; i < count; i++) {
            if (src[i]->high && !src[i]->high->empty()) {
                if (!tgt[i]->high) {
                    tgt[i]->high = new std::vector<double>();
                    tgt[i]->low = new std::vector<double>();
                }
                tgt[i]->high->insert(tgt[i]->high->end(), src[i]->high->begin(), src[i]->high->end());
                tgt[i]->low->insert(tgt[i]->low->end(), src[i]->low->begin(), src[i]->low->end());
                tgt[i]->period = src[i]->period;
                tgt[i]->bar_period = src[i]->bar_period;
                tgt[i]->align = src[i]->align;
            }
        }
    }

    static void Finalize(Vector &states_vec, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
        auto states = FlatVector::GetData<CnTaTsState8 *>(states_vec);
        auto rdata = FlatVector::GetData<double>(result);
        auto &rmask = FlatVector::Validity(result);
        for (idx_t i = 0; i < count; i++) {
            auto state = states[i];
            idx_t ridx = i + offset;
            if (!state->high || state->high->empty()) {
                rmask.SetInvalid(ridx);
                continue;
            }
            int size = (int)state->high->size();
            int outBeg = 0, outNb = 0;
            std::vector<double> outReal(size);
            TA_RetCode rc = TA_FUNC(0, size - 1, state->high->data(), state->low->data(),
                                    state->period, &outBeg, &outNb, outReal.data());
            if (rc != TA_SUCCESS || outNb == 0) {
                rmask.SetInvalid(ridx);
            } else {
                rdata[ridx] = outReal[outNb - 1];
            }
        }
    }

    static void Destroy(Vector &states_vec, AggregateInputData &, idx_t count) {
        auto states = FlatVector::GetData<CnTaTsState8 *>(states_vec);
        for (idx_t i = 0; i < count; i++) {
            delete states[i]->high;
            delete states[i]->low;
            states[i]->high = nullptr;
            states[i]->low = nullptr;
        }
    }
};

// ============================================================
// Registration
// ============================================================

void RegisterCnTaTsAggFunctions(ExtensionLoader &loader) {

    // ---- 标量辅助函数 ----
    // cn_ta_is_trading_day(ts) -> BOOLEAN
    loader.RegisterFunction(
        ScalarFunction("cn_ta_is_trading_day", {LogicalType::TIMESTAMP}, LogicalType::BOOLEAN, IsTradingDayFunc));

    // cn_ta_set_holiday(DATE) -> BIGINT
    loader.RegisterFunction(
        ScalarFunction("cn_ta_set_holiday", {LogicalType::DATE}, LogicalType::BIGINT, SetHolidayFunc));

    // cn_ta_clear_holidays() -> BIGINT
    loader.RegisterFunction(
        ScalarFunction("cn_ta_clear_holidays", {}, LogicalType::BIGINT, ClearHolidaysFunc));

    // ---- P1 DOUBLE: cta_<name>_ts(ts, value, period[, bar_period]) ----
    #define TALIB_TS_AGG_P1_DOUBLE(sql_name, ta_func, ta_lookback) \
        { \
            using OP = CnTaTsAggP1<ta_func, ta_lookback>; \
            AggregateFunctionSet set("cta_" #sql_name "_ts"); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            loader.RegisterFunction(set); \
        }

    // ---- P3 DOUBLE: cta_<name>_ts(ts, high, low, close, period[, bar_period]) ----
    #define TALIB_TS_AGG_P3_DOUBLE(sql_name, ta_func) \
        { \
            using OP = CnTaTsAggP3<ta_func>; \
            AggregateFunctionSet set("cta_" #sql_name "_ts"); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            loader.RegisterFunction(set); \
        }

    // ---- P5 DOUBLE: cta_<name>_ts(ts, open, high, low, close[, bar_period]) ----
    #define TALIB_TS_AGG_P5_DOUBLE(sql_name, ta_func) \
        { \
            using OP = CnTaTsAggP5Double<ta_func>; \
            AggregateFunctionSet set("cta_" #sql_name "_ts"); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            loader.RegisterFunction(set); \
        }

    // ---- P4 DOUBLE: cta_<name>_ts(ts, high, low, close, volume[, bar_period]) ----
    #define TALIB_TS_AGG_P4_DOUBLE(sql_name, ta_func) \
        { \
            using OP = CnTaTsAggP4<ta_func>; \
            AggregateFunctionSet set("cta_" #sql_name "_ts"); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            loader.RegisterFunction(set); \
        }

    // ---- P6 DOUBLE: cta_<name>_ts(ts, high, low[, bar_period]) ----
    #define TALIB_TS_AGG_P6_DOUBLE(sql_name, ta_func) \
        { \
            using OP = CnTaTsAggP6<ta_func>; \
            AggregateFunctionSet set("cta_" #sql_name "_ts"); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            loader.RegisterFunction(set); \
        }

    // ---- P7 DOUBLE: cta_<name>_ts(ts, high, low, close[, bar_period]) ----
    #define TALIB_TS_AGG_P7_DOUBLE(sql_name, ta_func) \
        { \
            using OP = CnTaTsAggP7<ta_func>; \
            AggregateFunctionSet set("cta_" #sql_name "_ts"); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            loader.RegisterFunction(set); \
        }

    // ---- P8 DOUBLE: cta_<name>_ts(ts, high, low, period[, bar_period]) ----
    #define TALIB_TS_AGG_P8_DOUBLE(sql_name, ta_func) \
        { \
            using OP = CnTaTsAggP8<ta_func>; \
            AggregateFunctionSet set("cta_" #sql_name "_ts"); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            set.AddFunction(AggregateFunction( \
                {LogicalType::TIMESTAMP, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::INTEGER}, \
                LogicalType::DOUBLE, \
                OP::StateSize, OP::Initialize, OP::Update, OP::Combine, OP::Finalize, \
                FunctionNullHandling::SPECIAL_HANDLING, \
                OP::SimpleUpdate, nullptr, OP::Destroy)); \
            loader.RegisterFunction(set); \
        }

    // ---- P1 指标 ----
    TALIB_TS_AGG_P1_DOUBLE(sma,      TA_SMA,      TA_SMA_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(ema,      TA_EMA,      TA_EMA_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(wma,      TA_WMA,      TA_WMA_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(dema,     TA_DEMA,     TA_DEMA_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(tema,     TA_TEMA,     TA_TEMA_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(trima,    TA_TRIMA,    TA_TRIMA_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(kama,     TA_KAMA,     TA_KAMA_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(midpoint, TA_MIDPOINT, TA_MIDPOINT_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(rsi,      TA_RSI,      TA_RSI_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(cmo,      TA_CMO,      TA_CMO_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(mom,      TA_MOM,      TA_MOM_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(roc,      TA_ROC,      TA_ROC_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(rocp,     TA_ROCP,     TA_ROCP_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(rocr,     TA_ROCR,     TA_ROCR_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(rocr100,  TA_ROCR100,  TA_ROCR100_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(trix,     TA_TRIX,     TA_TRIX_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(linearreg,           TA_LINEARREG,           TA_LINEARREG_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(linearreg_angle,     TA_LINEARREG_ANGLE,     TA_LINEARREG_ANGLE_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(linearreg_intercept, TA_LINEARREG_INTERCEPT, TA_LINEARREG_INTERCEPT_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(linearreg_slope,     TA_LINEARREG_SLOPE,     TA_LINEARREG_SLOPE_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(tsf,                 TA_TSF,                 TA_TSF_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(sum,                 TA_SUM,                 TA_SUM_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(max,                 TA_MAX,                 TA_MAX_Lookback)
    TALIB_TS_AGG_P1_DOUBLE(min,                 TA_MIN,                 TA_MIN_Lookback)

    // ---- P3 指标（high/low/close + period）----
    TALIB_TS_AGG_P3_DOUBLE(atr,  TA_ATR)
    TALIB_TS_AGG_P3_DOUBLE(natr, TA_NATR)
    TALIB_TS_AGG_P3_DOUBLE(willr, TA_WILLR)
    TALIB_TS_AGG_P3_DOUBLE(cci,   TA_CCI)
    TALIB_TS_AGG_P3_DOUBLE(adx,   TA_ADX)
    TALIB_TS_AGG_P3_DOUBLE(adxr,  TA_ADXR)
    TALIB_TS_AGG_P3_DOUBLE(dx,    TA_DX)
    TALIB_TS_AGG_P3_DOUBLE(plus_di,  TA_PLUS_DI)
    TALIB_TS_AGG_P3_DOUBLE(minus_di, TA_MINUS_DI)

    // ---- P5 指标（open/high/low/close）----
    TALIB_TS_AGG_P5_DOUBLE(avgprice, TA_AVGPRICE)
    TALIB_TS_AGG_P5_DOUBLE(bop,      TA_BOP)

    // ---- P4 指标（high/low/close/volume）----
    TALIB_TS_AGG_P4_DOUBLE(ad, TA_AD)

    // ---- P6 指标（high/low）----
    TALIB_TS_AGG_P6_DOUBLE(medprice, TA_MEDPRICE)

    // ---- P7 指标（high/low/close）----
    TALIB_TS_AGG_P7_DOUBLE(trange,   TA_TRANGE)
    TALIB_TS_AGG_P7_DOUBLE(typprice, TA_TYPPRICE)
    TALIB_TS_AGG_P7_DOUBLE(wclprice, TA_WCLPRICE)

    // ---- P8 指标（high/low/period）----
    TALIB_TS_AGG_P8_DOUBLE(midprice, TA_MIDPRICE)
    TALIB_TS_AGG_P8_DOUBLE(plus_dm,  TA_PLUS_DM)
    TALIB_TS_AGG_P8_DOUBLE(minus_dm, TA_MINUS_DM)

    #undef TALIB_TS_AGG_P1_DOUBLE
    #undef TALIB_TS_AGG_P3_DOUBLE
    #undef TALIB_TS_AGG_P5_DOUBLE
    #undef TALIB_TS_AGG_P4_DOUBLE
    #undef TALIB_TS_AGG_P6_DOUBLE
    #undef TALIB_TS_AGG_P7_DOUBLE
    #undef TALIB_TS_AGG_P8_DOUBLE
}

} // namespace duckdb
