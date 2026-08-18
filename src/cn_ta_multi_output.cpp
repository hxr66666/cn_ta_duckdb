#include "cn_ta_adapter.hpp"
// list_vector included via duckdb.hpp

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// ============================================================
// Helper: pre-format columns ONCE (outside the row loop).
// The old helpers called ToUnifiedFormat() inside the per-row loop,
// turning every scalar function into O(count^2) work.
// ============================================================
struct IntColView {
	UnifiedVectorFormat vdata;
	const int32_t *data;
	void Init(Vector &vec, idx_t count) {
		vec.ToUnifiedFormat(count, vdata);
		data = UnifiedVectorFormat::GetData<int32_t>(vdata);
	}
	inline int32_t Get(idx_t row) const {
		return data[vdata.sel->get_index(row)];
	}
};

struct DoubleColView {
	UnifiedVectorFormat vdata;
	const double *data;
	void Init(Vector &vec, idx_t count) {
		vec.ToUnifiedFormat(count, vdata);
		data = UnifiedVectorFormat::GetData<double>(vdata);
	}
	inline double Get(idx_t row) const {
		return data[vdata.sel->get_index(row)];
	}
};

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

// ============================================================
// Helper: pack multi-output results into LIST<STRUCT>
// Each output array has the same outBeg/outNb; we build
// a list of structs where lookback positions have NULL fields.
// ============================================================

// Pack 2-output result
static void PackStruct2Result(Vector &result, idx_t idx, int input_size, int out_beg, int out_nb, const double *out1,
                              const double *out2, const string &name1, const string &name2) {
	// Set list entry
	auto list_data = FlatVector::GetData<list_entry_t>(result);
	auto offset = ListVector::GetListSize(result);
	list_data[idx].offset = offset;
	list_data[idx].length = input_size;

	ListVector::Reserve(result, offset + input_size);
	ListVector::SetListSize(result, offset + input_size);

	// Get the struct child vector (the entry of the LIST)
	auto &struct_vec = ListVector::GetEntry(result);
	auto &entries = StructVector::GetEntries(struct_vec);

	// entries[0] = first field, entries[1] = second field
	auto data0 = FlatVector::GetData<double>(*entries[0]);
	auto data1 = FlatVector::GetData<double>(*entries[1]);
	auto &validity0 = FlatVector::Validity(*entries[0]);
	auto &validity1 = FlatVector::Validity(*entries[1]);

	// Fill lookback NULLs
	for (int i = 0; i < out_beg; i++) {
		validity0.SetInvalid(offset + i);
		validity1.SetInvalid(offset + i);
	}
	// Fill values
	for (int i = 0; i < out_nb; i++) {
		data0[offset + out_beg + i] = out1[i];
		data1[offset + out_beg + i] = out2[i];
	}
	// Fill trailing NULLs
	for (int i = out_beg + out_nb; i < input_size; i++) {
		validity0.SetInvalid(offset + i);
		validity1.SetInvalid(offset + i);
	}
}

// Pack 3-output result
static void PackStruct3Result(Vector &result, idx_t idx, int input_size, int out_beg, int out_nb, const double *out1,
                              const double *out2, const double *out3) {
	auto list_data = FlatVector::GetData<list_entry_t>(result);
	auto offset = ListVector::GetListSize(result);
	list_data[idx].offset = offset;
	list_data[idx].length = input_size;

	ListVector::Reserve(result, offset + input_size);
	ListVector::SetListSize(result, offset + input_size);

	auto &struct_vec = ListVector::GetEntry(result);
	auto &entries = StructVector::GetEntries(struct_vec);

	auto data0 = FlatVector::GetData<double>(*entries[0]);
	auto data1 = FlatVector::GetData<double>(*entries[1]);
	auto data2 = FlatVector::GetData<double>(*entries[2]);
	auto &validity0 = FlatVector::Validity(*entries[0]);
	auto &validity1 = FlatVector::Validity(*entries[1]);
	auto &validity2 = FlatVector::Validity(*entries[2]);

	for (int i = 0; i < out_beg; i++) {
		validity0.SetInvalid(offset + i);
		validity1.SetInvalid(offset + i);
		validity2.SetInvalid(offset + i);
	}
	for (int i = 0; i < out_nb; i++) {
		data0[offset + out_beg + i] = out1[i];
		data1[offset + out_beg + i] = out2[i];
		data2[offset + out_beg + i] = out3[i];
	}
	for (int i = out_beg + out_nb; i < input_size; i++) {
		validity0.SetInvalid(offset + i);
		validity1.SetInvalid(offset + i);
		validity2.SetInvalid(offset + i);
	}
}

// ============================================================
// MACD: (LIST<DOUBLE>, INT, INT, INT) -> LIST<STRUCT(macd, signal, hist)>
// ============================================================
static void CnTaMacdScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	IntColView fp, sp, sigp;
	in.Init(args.data[0], count);
	fp.Init(args.data[1], count);
	sp.Init(args.data[2], count);
	sigp.Init(args.data[3], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int fast_period = fp.Get(i);
		int slow_period = sp.Get(i);
		int signal_period = sigp.Get(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outMACD(size), outSignal(size), outHist(size);

		TA_RetCode rc = TA_MACD(0, size - 1, input.data(), fast_period, slow_period, signal_period, &outBeg, &outNb,
		                        outMACD.data(), outSignal.data(), outHist.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackStruct3Result(result, i, size, outBeg, outNb, outMACD.data(), outSignal.data(), outHist.data());
	}
}

// ============================================================
// BBANDS: (LIST<DOUBLE>, INT, DOUBLE, DOUBLE, INT) -> LIST<STRUCT(upper, middle, lower)>
// ============================================================
static void CnTaBbandsScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	IntColView tp, mt;
	DoubleColView du, dd;
	in.Init(args.data[0], count);
	tp.Init(args.data[1], count);
	du.Init(args.data[2], count);
	dd.Init(args.data[3], count);
	mt.Init(args.data[4], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int time_period = tp.Get(i);
		double nb_dev_up = du.Get(i);
		double nb_dev_dn = dd.Get(i);
		int ma_type = mt.Get(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outUpper(size), outMiddle(size), outLower(size);

		TA_RetCode rc = TA_BBANDS(0, size - 1, input.data(), time_period, nb_dev_up, nb_dev_dn, (TA_MAType)ma_type,
		                          &outBeg, &outNb, outUpper.data(), outMiddle.data(), outLower.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackStruct3Result(result, i, size, outBeg, outNb, outUpper.data(), outMiddle.data(), outLower.data());
	}
}

// ============================================================
// STOCH: (LIST<DOUBLE> x3, INT, INT, INT, INT, INT) -> LIST<STRUCT(slowk, slowd)>
// ============================================================
static void CnTaStochScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_h, in_l, in_c;
	IntColView fk, sk, skm, sd, sdm;
	in_h.Init(args.data[0], count);
	in_l.Init(args.data[1], count);
	in_c.Init(args.data[2], count);
	fk.Init(args.data[3], count);
	sk.Init(args.data[4], count);
	skm.Init(args.data[5], count);
	sd.Init(args.data[6], count);
	sdm.Init(args.data[7], count);
	for (idx_t i = 0; i < count; i++) {
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		auto close = ListToDoubleArray(in_c.Get(i), in_c.Child());
		int size = (int)high.size();

		int fastk_period = fk.Get(i);
		int slowk_period = sk.Get(i);
		int slowk_matype = skm.Get(i);
		int slowd_period = sd.Get(i);
		int slowd_matype = sdm.Get(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outSlowK(size), outSlowD(size);

		TA_RetCode rc = TA_STOCH(0, size - 1, high.data(), low.data(), close.data(), fastk_period, slowk_period,
		                         (TA_MAType)slowk_matype, slowd_period, (TA_MAType)slowd_matype, &outBeg, &outNb,
		                         outSlowK.data(), outSlowD.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackStruct2Result(result, i, size, outBeg, outNb, outSlowK.data(), outSlowD.data(), "slowk", "slowd");
	}
}

// ============================================================
// AROON: (LIST<DOUBLE> x2, INT) -> LIST<STRUCT(aroon_down, aroon_up)>
// ============================================================
static void CnTaAroonScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in_h, in_l;
	IntColView tp;
	in_h.Init(args.data[0], count);
	in_l.Init(args.data[1], count);
	tp.Init(args.data[2], count);
	for (idx_t i = 0; i < count; i++) {
		auto high = ListToDoubleArray(in_h.Get(i), in_h.Child());
		auto low = ListToDoubleArray(in_l.Get(i), in_l.Child());
		int size = (int)high.size();

		int time_period = tp.Get(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outDown(size), outUp(size);

		TA_RetCode rc =
		    TA_AROON(0, size - 1, high.data(), low.data(), time_period, &outBeg, &outNb, outDown.data(), outUp.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackStruct2Result(result, i, size, outBeg, outNb, outDown.data(), outUp.data(), "aroon_down", "aroon_up");
	}
}

// ============================================================
// MINMAX: (LIST<DOUBLE>, INT) -> LIST<STRUCT(min, max)>
// ============================================================
static void CnTaMinMaxScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	IntColView tp;
	in.Init(args.data[0], count);
	tp.Init(args.data[1], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int time_period = tp.Get(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outMin(size), outMax(size);

		TA_RetCode rc =
		    TA_MINMAX(0, size - 1, input.data(), time_period, &outBeg, &outNb, outMin.data(), outMax.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackStruct2Result(result, i, size, outBeg, outNb, outMin.data(), outMax.data(), "min", "max");
	}
}

// ============================================================
// MAMA: (LIST<DOUBLE>, DOUBLE, DOUBLE) -> LIST<STRUCT(mama, fama)>
// ============================================================
static void CnTaMamaScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	DoubleColView fl, sl;
	in.Init(args.data[0], count);
	fl.Init(args.data[1], count);
	sl.Init(args.data[2], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		double fast_limit = fl.Get(i);
		double slow_limit = sl.Get(i);

		int outBeg = 0, outNb = 0;
		std::vector<double> outMAMA(size), outFAMA(size);

		TA_RetCode rc =
		    TA_MAMA(0, size - 1, input.data(), fast_limit, slow_limit, &outBeg, &outNb, outMAMA.data(), outFAMA.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackStruct2Result(result, i, size, outBeg, outNb, outMAMA.data(), outFAMA.data(), "mama", "fama");
	}
}

// ============================================================
// HT_PHASOR: (LIST<DOUBLE>) -> LIST<STRUCT(inphase, quadrature)>
// ============================================================
static void CnTaHtPhasorScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	in.Init(args.data[0], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int outBeg = 0, outNb = 0;
		std::vector<double> outInPhase(size), outQuadrature(size);

		TA_RetCode rc =
		    TA_HT_PHASOR(0, size - 1, input.data(), &outBeg, &outNb, outInPhase.data(), outQuadrature.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackStruct2Result(result, i, size, outBeg, outNb, outInPhase.data(), outQuadrature.data(), "inphase",
		                  "quadrature");
	}
}

// ============================================================
// HT_SINE: (LIST<DOUBLE>) -> LIST<STRUCT(sine, leadsine)>
// ============================================================
static void CnTaHtSineScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	ListColView in;
	in.Init(args.data[0], count);
	for (idx_t i = 0; i < count; i++) {
		auto input = ListToDoubleArray(in.Get(i), in.Child());
		int size = (int)input.size();

		int outBeg = 0, outNb = 0;
		std::vector<double> outSine(size), outLeadSine(size);

		TA_RetCode rc = TA_HT_SINE(0, size - 1, input.data(), &outBeg, &outNb, outSine.data(), outLeadSine.data());
		if (rc != TA_SUCCESS) {
			outNb = 0;
			outBeg = 0;
		}
		PackStruct2Result(result, i, size, outBeg, outNb, outSine.data(), outLeadSine.data(), "sine", "leadsine");
	}
}

// ============================================================
// Registration
// ============================================================

static const auto LIST_DOUBLE = LogicalType::LIST(LogicalType::DOUBLE);

static LogicalType MakeStructList(const vector<pair<string, LogicalType>> &fields) {
	child_list_t<LogicalType> children;
	for (auto &f : fields) {
		children.push_back(make_pair(f.first, f.second));
	}
	return LogicalType::LIST(LogicalType::STRUCT(children));
}

void RegisterCnTaMultiOutputFunctions(ExtensionLoader &loader) {
	// MACD: (LIST<DOUBLE>, INT, INT, INT) -> LIST<STRUCT(macd, signal, hist)>
	loader.RegisterFunction(ScalarFunction(
	    "ct_macd", {LIST_DOUBLE, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER},
	    MakeStructList({{"macd", LogicalType::DOUBLE}, {"signal", LogicalType::DOUBLE}, {"hist", LogicalType::DOUBLE}}),
	    CnTaMacdScalar));

	// BBANDS: (LIST<DOUBLE>, INT, DOUBLE, DOUBLE, INT) -> LIST<STRUCT(upper, middle, lower)>
	loader.RegisterFunction(ScalarFunction(
	    "ct_bbands",
	    {LIST_DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER},
	    MakeStructList(
	        {{"upper", LogicalType::DOUBLE}, {"middle", LogicalType::DOUBLE}, {"lower", LogicalType::DOUBLE}}),
	    CnTaBbandsScalar));

	// STOCH: (LIST<DOUBLE> x3, INT, INT, INT, INT, INT) -> LIST<STRUCT(slowk, slowd)>
	loader.RegisterFunction(ScalarFunction(
	    "ct_stoch",
	    {LIST_DOUBLE, LIST_DOUBLE, LIST_DOUBLE, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER,
	     LogicalType::INTEGER, LogicalType::INTEGER},
	    MakeStructList({{"slowk", LogicalType::DOUBLE}, {"slowd", LogicalType::DOUBLE}}), CnTaStochScalar));

	// AROON: (LIST<DOUBLE> x2, INT) -> LIST<STRUCT(aroon_down, aroon_up)>
	loader.RegisterFunction(ScalarFunction(
	    "ct_aroon", {LIST_DOUBLE, LIST_DOUBLE, LogicalType::INTEGER},
	    MakeStructList({{"aroon_down", LogicalType::DOUBLE}, {"aroon_up", LogicalType::DOUBLE}}), CnTaAroonScalar));

	// MINMAX: (LIST<DOUBLE>, INT) -> LIST<STRUCT(min, max)>
	loader.RegisterFunction(ScalarFunction("ct_minmax", {LIST_DOUBLE, LogicalType::INTEGER},
	                                       MakeStructList({{"min", LogicalType::DOUBLE}, {"max", LogicalType::DOUBLE}}),
	                                       CnTaMinMaxScalar));

	// MAMA: (LIST<DOUBLE>, DOUBLE, DOUBLE) -> LIST<STRUCT(mama, fama)>
	loader.RegisterFunction(
	    ScalarFunction("ct_mama", {LIST_DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   MakeStructList({{"mama", LogicalType::DOUBLE}, {"fama", LogicalType::DOUBLE}}), CnTaMamaScalar));

	// HT_PHASOR: (LIST<DOUBLE>) -> LIST<STRUCT(inphase, quadrature)>
	loader.RegisterFunction(ScalarFunction(
	    "ct_ht_phasor", {LIST_DOUBLE},
	    MakeStructList({{"inphase", LogicalType::DOUBLE}, {"quadrature", LogicalType::DOUBLE}}), CnTaHtPhasorScalar));

	// HT_SINE: (LIST<DOUBLE>) -> LIST<STRUCT(sine, leadsine)>
	loader.RegisterFunction(ScalarFunction(
	    "ct_ht_sine", {LIST_DOUBLE}, MakeStructList({{"sine", LogicalType::DOUBLE}, {"leadsine", LogicalType::DOUBLE}}),
	    CnTaHtSineScalar));
}

} // namespace duckdb
