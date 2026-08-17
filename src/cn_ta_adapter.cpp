#include "cn_ta_adapter.hpp"
// list_vector included via duckdb.hpp

#include <limits>

namespace duckdb {

std::vector<double> ListToDoubleArray(const list_entry_t &list, const Vector &child) {
    auto size = list.length;
    // 空 LIST 直接返回空数组，避免访问空 child（非 flat）导致 FlatVector 断言崩溃
    if (size == 0) {
        return std::vector<double>();
    }

    std::vector<double> result(size);

    auto &child_validity = FlatVector::Validity(child);
    auto child_data = FlatVector::GetData<double>(child);

    // 注意：TA-Lib 的 double[] 无法表达 NULL。这里把 NULL 元素转成 NaN，
    // 让"数据缺失"在 TA-Lib 内部按 NaN 传播（任何含 NaN 的计算结果仍为 NaN），
    // 而不是把缺失当成 0 参与计算（0 会污染 SMA/EMA/RSI 等指标）。
    const double NAN_VALUE = std::numeric_limits<double>::quiet_NaN();

    for (idx_t i = 0; i < size; i++) {
        auto child_idx = list.offset + i;
        if (child_validity.RowIsValid(child_idx)) {
            result[i] = child_data[child_idx];
        } else {
            result[i] = NAN_VALUE;
        }
    }

    return result;
}

void PackDoubleResult(Vector &result, idx_t idx, int input_size,
                      int out_beg_idx, int out_nb_element, const double *out_array) {
    auto list_size = input_size;
    auto list_data = FlatVector::GetData<list_entry_t>(result);
    list_data[idx].offset = ListVector::GetListSize(result);
    list_data[idx].length = list_size;

    ListVector::Reserve(result, list_data[idx].offset + list_size);
    ListVector::SetListSize(result, list_data[idx].offset + list_size);

    auto &child = ListVector::GetEntry(result);
    auto child_data = FlatVector::GetData<double>(child);
    auto &child_validity = FlatVector::Validity(child);

    auto offset = list_data[idx].offset;

    // Fill lookback period positions with NULL
    for (int i = 0; i < out_beg_idx; i++) {
        child_validity.SetInvalid(offset + i);
    }

    // Fill actual output values
    for (int i = 0; i < out_nb_element; i++) {
        child_data[offset + out_beg_idx + i] = out_array[i];
    }

    // Fill any trailing positions with NULL (if output is shorter than expected)
    for (int i = out_beg_idx + out_nb_element; i < list_size; i++) {
        child_validity.SetInvalid(offset + i);
    }
}

void PackIntResult(Vector &result, idx_t idx, int input_size,
                   int out_beg_idx, int out_nb_element, const int *out_array) {
    auto list_size = input_size;
    auto list_data = FlatVector::GetData<list_entry_t>(result);
    list_data[idx].offset = ListVector::GetListSize(result);
    list_data[idx].length = list_size;

    ListVector::Reserve(result, list_data[idx].offset + list_size);
    ListVector::SetListSize(result, list_data[idx].offset + list_size);

    auto &child = ListVector::GetEntry(result);
    auto child_data = FlatVector::GetData<int32_t>(child);
    auto &child_validity = FlatVector::Validity(child);

    auto offset = list_data[idx].offset;

    // Fill lookback period positions with NULL
    for (int i = 0; i < out_beg_idx; i++) {
        child_validity.SetInvalid(offset + i);
    }

    // Fill actual output values
    for (int i = 0; i < out_nb_element; i++) {
        child_data[offset + out_beg_idx + i] = out_array[i];
    }

    // Fill any trailing positions with NULL (if output is shorter than expected)
    for (int i = out_beg_idx + out_nb_element; i < list_size; i++) {
        child_validity.SetInvalid(offset + i);
    }
}

} // namespace duckdb
