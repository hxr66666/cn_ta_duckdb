#include "cn_ta_adapter.hpp"
// list_vector included via duckdb.hpp
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// ============================================================
// Helper: pre-format a list column ONCE (outside the row loop) and expose
// the list_entry_t data + child vector.  The old GetListEntry() called
// ToUnifiedFormat() inside the per-row loop, turning every scalar function
// into O(count^2) work.  Now we format each column exactly once.
// ============================================================
struct ListColView {
	UnifiedVectorFormat vdata;
	const list_entry_t *list_data;
	const Vector *child;

	void Init(Vector &vec, idx_t count) {
		vec.ToUnifiedFormat(count, vdata);
		list_data = UnifiedVectorFormat::GetData<list_entry_t>(vdata);
		child = &ListVector::GetEntry(vec);
	}

	inline list_entry_t Get(idx_t row) const {
		return list_data[vdata.sel->get_index(row)];
	}

	inline const Vector &Child() const {
		return *child;
	}
};

// Pre-format a scalar (non-list) column once.  Returns a pointer to the
// unified-format data and lets callers index by row via sel.
struct ScalarColView {
	UnifiedVectorFormat vdata;
	const int32_t *i32_data;

	void Init(Vector &vec, idx_t count) {
		vec.ToUnifiedFormat(count, vdata);
		i32_data = UnifiedVectorFormat::GetData<int32_t>(vdata);
	}

	inline int32_t GetInt(idx_t row) const {
		return i32_data[vdata.sel->get_index(row)];
	}
};

// ============================================================
// P1: (LIST<DOUBLE>, INTEGER) -> LIST<DOUBLE> or LIST<INTEGER>
// Signature: TA_FUNC(startIdx, endIdx, inReal[], optInTimePeriod, &outBegIdx, &outNBElement, outReal[])
// ============================================================

// P1 returning DOUBLE
template <TA_RetCode (*TA_FUNC)(int, int, const double[], int, int *, int *, double[])>
static void CnTaScalarP1Double(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	ScalarColView period;
	in.Init(args.data[0], count);
	period.Init(args.data[1], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int p = period.GetInt(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outReal(size);
		TA_RetCode rc = TA_FUNC(0, size - 1, input.data(), p, &outBeg, &outNb, outReal.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackDoubleResult(result, i, size, outBeg, outNb, outReal.data());
	}
}

// P1 returning INT
template <TA_RetCode (*TA_FUNC)(int, int, const double[], int, int *, int *, int[])>
static void CnTaScalarP1Int(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	ScalarColView period;
	in.Init(args.data[0], count);
	period.Init(args.data[1], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int p = period.GetInt(i);

		int outBeg = 0, outNb = 0;
		std::vector<int> outInt(size);
		TA_RetCode rc = TA_FUNC(0, size - 1, input.data(), p, &outBeg, &outNb, outInt.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackIntResult(result, i, size, outBeg, outNb, outInt.data());
	}
}

// ============================================================
// P2: (LIST<DOUBLE>) -> LIST<DOUBLE> or LIST<INTEGER>
// Signature: TA_FUNC(startIdx, endIdx, inReal[], &outBegIdx, &outNBElement, outReal[])
// ============================================================

// P2 returning DOUBLE
template <TA_RetCode (*TA_FUNC)(int, int, const double[], int *, int *, double[])>
static void CnTaScalarP2Double(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	in.Init(args.data[0], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int outBeg = 0, outNb = 0;
		std::vector<double> outReal(size);
		TA_RetCode rc = TA_FUNC(0, size - 1, input.data(), &outBeg, &outNb, outReal.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackDoubleResult(result, i, size, outBeg, outNb, outReal.data());
	}
}

// P2 returning INT
template <TA_RetCode (*TA_FUNC)(int, int, const double[], int *, int *, int[])>
static void CnTaScalarP2Int(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	in.Init(args.data[0], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int outBeg = 0, outNb = 0;
		std::vector<int> outInt(size);
		TA_RetCode rc = TA_FUNC(0, size - 1, input.data(), &outBeg, &outNb, outInt.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackIntResult(result, i, size, outBeg, outNb, outInt.data());
	}
}

// ============================================================
// P3: (LIST<DOUBLE>, LIST<DOUBLE>, LIST<DOUBLE>, INTEGER) -> LIST<DOUBLE>
// Signature: TA_FUNC(startIdx, endIdx, inHigh[], inLow[], inClose[], optInTimePeriod, &outBeg, &outNb, outReal[])
// ============================================================
template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], int, int *, int *, double[])>
static void CnTaScalarP3(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_h, in_l, in_c;
	ScalarColView period;
	in_h.Init(args.data[0], count);
	in_l.Init(args.data[1], count);
	in_c.Init(args.data[2], count);
	period.Init(args.data[3], count);
	for (idx_t i = 0; i < count; i++) {
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		auto close = ListToDoubleArray(in_c.Get(i), in_c.Child());
		int size = (int)high.size();

		int p = period.GetInt(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outReal(size);
		TA_RetCode rc = TA_FUNC(0, size - 1, high.data(), low.data(), close.data(), p, &outBeg, &outNb, outReal.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackDoubleResult(result, i, size, outBeg, outNb, outReal.data());
	}
}

// ============================================================
// P4: (LIST<DOUBLE>, LIST<DOUBLE>, LIST<DOUBLE>, LIST<DOUBLE>) -> LIST<DOUBLE>
// Signature: TA_FUNC(startIdx, endIdx, inHigh[], inLow[], inClose[], inVolume[], &outBeg, &outNb, outReal[])
// ============================================================
template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], const double[], int *, int *,
                                double[])>
static void CnTaScalarP4(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_h, in_l, in_c, in_v;
	in_h.Init(args.data[0], count);
	in_l.Init(args.data[1], count);
	in_c.Init(args.data[2], count);
	in_v.Init(args.data[3], count);
	for (idx_t i = 0; i < count; i++) {
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		auto close = ListToDoubleArray(in_c.Get(i), in_c.Child());
		auto volume = ListToDoubleArray(in_v.Get(i), in_v.Child());
		int size = (int)high.size();

		int outBeg = 0, outNb = 0;
		std::vector<double> outReal(size);
		TA_RetCode rc =
		    TA_FUNC(0, size - 1, high.data(), low.data(), close.data(), volume.data(), &outBeg, &outNb, outReal.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackDoubleResult(result, i, size, outBeg, outNb, outReal.data());
	}
}

// ============================================================
// P5 DOUBLE: (LIST<DOUBLE>, LIST<DOUBLE>, LIST<DOUBLE>, LIST<DOUBLE>) -> LIST<DOUBLE>
// Signature: TA_FUNC(startIdx, endIdx, inOpen[], inHigh[], inLow[], inClose[], &outBeg, &outNb, outReal[])
// ============================================================
template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], const double[], int *, int *,
                                double[])>
static void CnTaScalarP5Double(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_o, in_h, in_l, in_c;
	in_o.Init(args.data[0], count);
	in_h.Init(args.data[1], count);
	in_l.Init(args.data[2], count);
	in_c.Init(args.data[3], count);
	for (idx_t i = 0; i < count; i++) {
		auto open_ = ListToDoubleArray(in_o.Get(i), in_o.Child());
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		auto close = ListToDoubleArray(in_c.Get(i), in_c.Child());
		int size = (int)open_.size();

		int outBeg = 0, outNb = 0;
		std::vector<double> outReal(size);
		TA_RetCode rc =
		    TA_FUNC(0, size - 1, open_.data(), high.data(), low.data(), close.data(), &outBeg, &outNb, outReal.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackDoubleResult(result, i, size, outBeg, outNb, outReal.data());
	}
}

// ============================================================
// P5 INT: (LIST<DOUBLE>, LIST<DOUBLE>, LIST<DOUBLE>, LIST<DOUBLE>) -> LIST<INTEGER>
// Signature: TA_FUNC(startIdx, endIdx, inOpen[], inHigh[], inLow[], inClose[], &outBeg, &outNb, outInteger[])
// ============================================================
template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], const double[], int *, int *,
                                int[])>
static void CnTaScalarP5Int(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_o, in_h, in_l, in_c;
	in_o.Init(args.data[0], count);
	in_h.Init(args.data[1], count);
	in_l.Init(args.data[2], count);
	in_c.Init(args.data[3], count);
	for (idx_t i = 0; i < count; i++) {
		auto open_ = ListToDoubleArray(in_o.Get(i), in_o.Child());
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		auto close = ListToDoubleArray(in_c.Get(i), in_c.Child());
		int size = (int)open_.size();

		int outBeg = 0, outNb = 0;
		std::vector<int> outInt(size);
		TA_RetCode rc =
		    TA_FUNC(0, size - 1, open_.data(), high.data(), low.data(), close.data(), &outBeg, &outNb, outInt.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackIntResult(result, i, size, outBeg, outNb, outInt.data());
	}
}

// ============================================================
// P6: (LIST<DOUBLE>, LIST<DOUBLE>) -> LIST<DOUBLE>
// Signature: TA_FUNC(startIdx, endIdx, inHigh[], inLow[], &outBeg, &outNb, outReal[])
// ============================================================
template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], int *, int *, double[])>
static void CnTaScalarP6(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_h, in_l;
	in_h.Init(args.data[0], count);
	in_l.Init(args.data[1], count);
	for (idx_t i = 0; i < count; i++) {
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		int size = (int)high.size();

		int outBeg = 0, outNb = 0;
		std::vector<double> outReal(size);
		TA_RetCode rc = TA_FUNC(0, size - 1, high.data(), low.data(), &outBeg, &outNb, outReal.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackDoubleResult(result, i, size, outBeg, outNb, outReal.data());
	}
}

// ============================================================
// P7: (LIST<DOUBLE>, LIST<DOUBLE>, LIST<DOUBLE>) -> LIST<DOUBLE>
// Signature: TA_FUNC(startIdx, endIdx, inHigh[], inLow[], inClose[], &outBeg, &outNb, outReal[])
// ============================================================
template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], const double[], int *, int *, double[])>
static void CnTaScalarP7(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_h, in_l, in_c;
	in_h.Init(args.data[0], count);
	in_l.Init(args.data[1], count);
	in_c.Init(args.data[2], count);
	for (idx_t i = 0; i < count; i++) {
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		auto close = ListToDoubleArray(in_c.Get(i), in_c.Child());
		int size = (int)high.size();

		int outBeg = 0, outNb = 0;
		std::vector<double> outReal(size);
		TA_RetCode rc = TA_FUNC(0, size - 1, high.data(), low.data(), close.data(), &outBeg, &outNb, outReal.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackDoubleResult(result, i, size, outBeg, outNb, outReal.data());
	}
}

// ============================================================
// P8: (LIST<DOUBLE>, LIST<DOUBLE>, INTEGER) -> LIST<DOUBLE>
// Signature: TA_FUNC(startIdx, endIdx, inHigh[], inLow[], optInTimePeriod, &outBeg, &outNb, outReal[])
// ============================================================
template <TA_RetCode (*TA_FUNC)(int, int, const double[], const double[], int, int *, int *, double[])>
static void CnTaScalarP8(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_h, in_l;
	ScalarColView period;
	in_h.Init(args.data[0], count);
	in_l.Init(args.data[1], count);
	period.Init(args.data[2], count);
	for (idx_t i = 0; i < count; i++) {
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		int size = (int)high.size();

		int p = period.GetInt(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outReal(size);
		TA_RetCode rc = TA_FUNC(0, size - 1, high.data(), low.data(), p, &outBeg, &outNb, outReal.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackDoubleResult(result, i, size, outBeg, outNb, outReal.data());
	}
}

// ============================================================
// Registration
// ============================================================

static const auto LIST_DOUBLE = LogicalType::LIST(LogicalType::DOUBLE);
static const auto LIST_INT = LogicalType::LIST(LogicalType::INTEGER);

void RegisterCnTaScalarFunctions(ExtensionLoader &loader) {
// --- P1 DOUBLE: (LIST<DOUBLE>, INTEGER) -> LIST<DOUBLE> ---
#define TALIB_SCALAR_P1_DOUBLE(sql_name, ta_func)                                                                      \
	loader.RegisterFunction(ScalarFunction("ct_" #sql_name, {LIST_DOUBLE, LogicalType::INTEGER}, LIST_DOUBLE,          \
	                                       CnTaScalarP1Double<ta_func>));

// --- P1 INT: (LIST<DOUBLE>, INTEGER) -> LIST<INTEGER> ---
#define TALIB_SCALAR_P1_INT(sql_name, ta_func)                                                                         \
	loader.RegisterFunction(                                                                                           \
	    ScalarFunction("ct_" #sql_name, {LIST_DOUBLE, LogicalType::INTEGER}, LIST_INT, CnTaScalarP1Int<ta_func>));

// --- P2 DOUBLE: (LIST<DOUBLE>) -> LIST<DOUBLE> ---
#define TALIB_SCALAR_P2_DOUBLE(sql_name, ta_func)                                                                      \
	loader.RegisterFunction(ScalarFunction("ct_" #sql_name, {LIST_DOUBLE}, LIST_DOUBLE, CnTaScalarP2Double<ta_func>));

// --- P2 INT: (LIST<DOUBLE>) -> LIST<INTEGER> ---
#define TALIB_SCALAR_P2_INT(sql_name, ta_func)                                                                         \
	loader.RegisterFunction(ScalarFunction("ct_" #sql_name, {LIST_DOUBLE}, LIST_INT, CnTaScalarP2Int<ta_func>));

// --- P3 DOUBLE: (LIST<DOUBLE> x3, INTEGER) -> LIST<DOUBLE> ---
#define TALIB_SCALAR_P3_DOUBLE(sql_name, ta_func)                                                                      \
	loader.RegisterFunction(ScalarFunction("ct_" #sql_name,                                                            \
	                                       {LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE, LogicalType::INTEGER}, LIST_DOUBLE, \
	                                       CnTaScalarP3<ta_func>));

// --- P4 DOUBLE: (LIST<DOUBLE> x4) -> LIST<DOUBLE> ---
#define TALIB_SCALAR_P4_DOUBLE(sql_name, ta_func)                                                                      \
	loader.RegisterFunction(ScalarFunction("ct_" #sql_name, {LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE},      \
	                                       LIST_DOUBLE, CnTaScalarP4<ta_func>));

// --- P5 DOUBLE: (LIST<DOUBLE> x4) -> LIST<DOUBLE> ---
#define TALIB_SCALAR_P5_DOUBLE(sql_name, ta_func)                                                                      \
	loader.RegisterFunction(ScalarFunction("ct_" #sql_name, {LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE},      \
	                                       LIST_DOUBLE, CnTaScalarP5Double<ta_func>));

// --- P5 INT: (LIST<DOUBLE> x4) -> LIST<INTEGER> ---
#define TALIB_SCALAR_P5_INT(sql_name, ta_func)                                                                         \
	loader.RegisterFunction(ScalarFunction("ct_" #sql_name, {LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE},      \
	                                       LIST_INT, CnTaScalarP5Int<ta_func>));

// --- P6 DOUBLE: (LIST<DOUBLE> x2) -> LIST<DOUBLE> ---
#define TALIB_SCALAR_P6_DOUBLE(sql_name, ta_func)                                                                      \
	loader.RegisterFunction(                                                                                           \
	    ScalarFunction("ct_" #sql_name, {LIST_DOUBLE, LIST_DOUBLE}, LIST_DOUBLE, CnTaScalarP6<ta_func>));

// --- P7 DOUBLE: (LIST<DOUBLE> x3) -> LIST<DOUBLE> ---
#define TALIB_SCALAR_P7_DOUBLE(sql_name, ta_func)                                                                      \
	loader.RegisterFunction(                                                                                           \
	    ScalarFunction("ct_" #sql_name, {LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE}, LIST_DOUBLE, CnTaScalarP7<ta_func>));

// --- P8 DOUBLE: (LIST<DOUBLE> x2, INTEGER) -> LIST<DOUBLE> ---
#define TALIB_SCALAR_P8_DOUBLE(sql_name, ta_func)                                                                      \
	loader.RegisterFunction(ScalarFunction("ct_" #sql_name, {LIST_DOUBLE, LIST_DOUBLE, LogicalType::INTEGER},          \
	                                       LIST_DOUBLE, CnTaScalarP8<ta_func>));

// Dispatch macro: TALIB_FUNC -> TALIB_SCALAR_<pattern>_<ret_type>
#define TALIB_FUNC(sql_name, ta_func, ta_lookback, pattern, ret_type)                                                  \
	TALIB_SCALAR_##pattern##_##ret_type(sql_name, ta_func)

#include "cn_ta_functions.hpp"

#undef TALIB_FUNC
#undef TALIB_SCALAR_P1_DOUBLE
#undef TALIB_SCALAR_P1_INT
#undef TALIB_SCALAR_P2_DOUBLE
#undef TALIB_SCALAR_P2_INT
#undef TALIB_SCALAR_P3_DOUBLE
#undef TALIB_SCALAR_P4_DOUBLE
#undef TALIB_SCALAR_P5_DOUBLE
#undef TALIB_SCALAR_P5_INT
#undef TALIB_SCALAR_P6_DOUBLE
#undef TALIB_SCALAR_P7_DOUBLE
#undef TALIB_SCALAR_P8_DOUBLE
}

} // namespace duckdb
