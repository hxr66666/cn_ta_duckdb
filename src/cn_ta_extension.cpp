#define DUCKDB_EXTENSION_MAIN

#include "cn_ta_extension.hpp"
#include "cn_ta_stat.hpp"
#include "cn_ta_stat_agg.hpp"
#include "cn_ta_ashare.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// Defined in cn_ta_scalar.cpp
void RegisterCnTaScalarFunctions(ExtensionLoader &loader);
// Defined in cn_ta_aggregate.cpp
void RegisterCnTaAggregateFunctions(ExtensionLoader &loader);
// Defined in cn_ta_multi_output.cpp
void RegisterCnTaMultiOutputFunctions(ExtensionLoader &loader);
// Defined in cn_ta_multi_output_agg.cpp
void RegisterCnTaMultiOutputAggFunctions(ExtensionLoader &loader);
// Defined in cn_ta_stat.cpp
void RegisterCnTaStatFunctions(ExtensionLoader &loader);
// Defined in cn_ta_stat_agg.cpp
void RegisterCnTaStatAggFunctions(ExtensionLoader &loader);
// Defined in cn_ta_ashare.cpp
void RegisterCnTaAshareFunctions(ExtensionLoader &loader);
// Defined in cn_ta_ts_agg.cpp
void RegisterCnTaTsAggFunctions(ExtensionLoader &loader);
// Defined in cn_ta_indicators.cpp
void RegisterCnTaIndicatorsFunction(ExtensionLoader &loader);

static void LoadInternal(ExtensionLoader &loader) {
    RegisterCnTaScalarFunctions(loader);
    RegisterCnTaAggregateFunctions(loader);
    RegisterCnTaMultiOutputFunctions(loader);
    RegisterCnTaMultiOutputAggFunctions(loader);
    RegisterCnTaStatFunctions(loader);
    RegisterCnTaStatAggFunctions(loader);
    RegisterCnTaAshareFunctions(loader);
    RegisterCnTaTsAggFunctions(loader);
    RegisterCnTaIndicatorsFunction(loader);
}

void CnTaExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string CnTaExtension::Name() {
    return "cn_ta";
}

std::string CnTaExtension::Version() const {
#ifdef EXT_VERSION_CN_TA
    return EXT_VERSION_CN_TA;
#else
    return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(cn_ta, loader) {
    duckdb::LoadInternal(loader);
}

}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
