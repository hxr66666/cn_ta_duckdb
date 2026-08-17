#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// A股（沪深/北交所）特有适配函数注册
// 声明于 cn_ta_ashare.cpp，供 cn_ta_extension.cpp 调用
void RegisterCnTaAshareFunctions(ExtensionLoader &loader);

} // namespace duckdb
