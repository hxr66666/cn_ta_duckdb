#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// 金融统计聚合/窗口版本（支持 OVER）注册
// 声明于 cn_ta_stat_agg.cpp，供 cn_ta_extension.cpp 调用
void RegisterCnTaStatAggFunctions(ExtensionLoader &loader);

} // namespace duckdb
