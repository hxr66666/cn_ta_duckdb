#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

extern "C" {
#include "ta_libc.h"
}

#include <vector>
#include <string>

namespace duckdb {

// TA-Lib 输入模式分类（文档性枚举，描述各函数族的输入数组签名）
// 注：当前未被运行时引用，仅作为输入模式的统一说明，避免各实现文件口径不一致。
enum class CnTaPattern {
    P1, // 单数组 + 周期: (inReal[], timePeriod)         e.g. sma/ema/rsi
    P2, // 单数组，无周期: (inReal[])                      e.g. sin/cos/ln
    P3, // HLC + 周期: (high[], low[], close[], period)    e.g. atr/willr/cci
    P4, // HLCV，无周期: (high[], low[], close[], volume[]) e.g. ad
    P5, // OHLC，无周期: (open[], high[], low[], close[])   e.g. avgprice/bop/cdl*
    P6, // HL，无周期: (high[], low[])                     e.g. medprice
    P7, // HLC，无周期: (high[], low[], close[])           e.g. trange/typprice
    P8  // HL + 周期: (high[], low[], period)             e.g. midprice/plus_dm
};

// Extract a double array from a DuckDB LIST vector entry
std::vector<double> ListToDoubleArray(const list_entry_t &list, const Vector &child);

// Pack a double output array from TA-Lib back into a DuckDB LIST, respecting outBegIdx
void PackDoubleResult(Vector &result, idx_t idx, int input_size,
                      int out_beg_idx, int out_nb_element, const double *out_array);

// Pack an integer output array (candlestick patterns)
void PackIntResult(Vector &result, idx_t idx, int input_size,
                   int out_beg_idx, int out_nb_element, const int *out_array);

} // namespace duckdb
