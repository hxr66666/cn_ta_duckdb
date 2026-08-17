#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// 金融统计回归函数注册（基于 Eigen 实现）
// 声明于 cn_ta_stat.cpp，供 cn_ta_extension.cpp 调用
void RegisterCnTaStatFunctions(ExtensionLoader &loader);

} // namespace duckdb
