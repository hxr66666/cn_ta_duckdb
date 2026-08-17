#include "cn_ta_ashare.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "cn_ta_trading_calendar.hpp"

#include <cmath>
#include <string>

namespace duckdb {

// ============================================================
// A股涨跌停幅度规则（按股票代码判断板块）
//   60xxxx / 00xxxx : 沪深主板   ±10%
//   30xxxx          : 创业板     ±20%
//   688xxx          : 科创板     ±20%
//   8xxxxx / 4xxxxx : 北交所     ±30%
//   ST/*ST          : 主板 ±5%，创业板/科创板 ±20%
// ============================================================
static double GetLimitPct(const std::string &code, bool is_st) {
	double base = 0.10;
	if (code.size() >= 2) {
		char c0 = code[0];
		if (code.size() >= 3 && code[0] == '6' && code[1] == '8' && code[2] == '8') {
			base = 0.20; // 科创板
		} else if (code.size() >= 2 && code[0] == '3' && code[1] == '0') {
			base = 0.20; // 创业板
		} else if (code[0] == '8' || code[0] == '4' || (code[0] == '9' && code[1] == '2')) {
			base = 0.30; // 北交所
		} else {
			base = 0.10; // 主板
		}
	}
	if (is_st && base == 0.10) {
		base = 0.05; // ST 主板 ±5%
	}
	return base;
}

// 四舍五入到分（0.01）
static double RoundToCent(double v) {
	return std::round(v * 100.0) / 100.0;
}

// ============================================================
// 涨跌停幅度: (stock_code, is_st) -> DOUBLE (如 0.10)
// ============================================================
static void LimitPctFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto rdata = FlatVector::GetData<double>(result);
	auto &rmask = FlatVector::Validity(result);
	UnifiedVectorFormat cdata, sdata;
	args.data[0].ToUnifiedFormat(count, cdata);
	args.data[1].ToUnifiedFormat(count, sdata);
	auto codes = UnifiedVectorFormat::GetData<string_t>(cdata);
	auto sts = UnifiedVectorFormat::GetData<bool>(sdata);
	for (idx_t i = 0; i < count; i++) {
		auto cidx = cdata.sel->get_index(i);
		auto sidx = sdata.sel->get_index(i);
		if (!cdata.validity.RowIsValid(cidx)) {
			rmask.SetInvalid(i);
			continue;
		}
		bool is_st = sdata.validity.RowIsValid(sidx) && sts[sidx];
		rdata[i] = GetLimitPct(codes[cidx].GetString(), is_st);
	}
}

// ============================================================
// 涨跌停价格: (prev_close, stock_code, is_st) -> STRUCT{limit_up, limit_down}
// ============================================================
static void LimitPriceFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto &entries = StructVector::GetEntries(result);
	auto &up_v = *entries[0];
	auto &down_v = *entries[1];
	auto up_data = FlatVector::GetData<double>(up_v);
	auto down_data = FlatVector::GetData<double>(down_v);
	auto &rmask = FlatVector::Validity(result);

	UnifiedVectorFormat pdata, cdata, sdata;
	args.data[0].ToUnifiedFormat(count, pdata);
	args.data[1].ToUnifiedFormat(count, cdata);
	args.data[2].ToUnifiedFormat(count, sdata);
	auto closes = UnifiedVectorFormat::GetData<double>(pdata);
	auto codes = UnifiedVectorFormat::GetData<string_t>(cdata);
	auto sts = UnifiedVectorFormat::GetData<bool>(sdata);

	for (idx_t i = 0; i < count; i++) {
		auto pidx = pdata.sel->get_index(i);
		auto cidx = cdata.sel->get_index(i);
		auto sidx = sdata.sel->get_index(i);
		if (!pdata.validity.RowIsValid(pidx) || !cdata.validity.RowIsValid(cidx)) {
			rmask.SetInvalid(i);
			continue;
		}
		bool is_st = sdata.validity.RowIsValid(sidx) && sts[sidx];
		double pct = GetLimitPct(codes[cidx].GetString(), is_st);
		double prev_close = closes[pidx];
		up_data[i] = RoundToCent(prev_close * (1.0 + pct));
		down_data[i] = RoundToCent(prev_close * (1.0 - pct));
	}
}

// ============================================================
// 是否 ST/*ST: (stock_name) -> BOOLEAN
// ============================================================
static void IsSTFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto rdata = FlatVector::GetData<bool>(result);
	UnifiedVectorFormat ndata;
	args.data[0].ToUnifiedFormat(count, ndata);
	auto names = UnifiedVectorFormat::GetData<string_t>(ndata);
	for (idx_t i = 0; i < count; i++) {
		auto nidx = ndata.sel->get_index(i);
		if (!ndata.validity.RowIsValid(nidx)) {
			rdata[i] = false;
			continue;
		}
		std::string name = names[nidx].GetString();
		rdata[i] = (name.find("ST") != std::string::npos || name.find("st") != std::string::npos);
	}
}

// ============================================================
// 涨跌幅: (close, prev_close) -> DOUBLE（如 0.0523 表示 +5.23%）
// ============================================================
static void ChangePctFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto rdata = FlatVector::GetData<double>(result);
	auto &rmask = FlatVector::Validity(result);
	UnifiedVectorFormat cdata, pdata;
	args.data[0].ToUnifiedFormat(count, cdata);
	args.data[1].ToUnifiedFormat(count, pdata);
	auto closes = UnifiedVectorFormat::GetData<double>(cdata);
	auto prevs = UnifiedVectorFormat::GetData<double>(pdata);
	for (idx_t i = 0; i < count; i++) {
		auto cidx = cdata.sel->get_index(i);
		auto pidx = pdata.sel->get_index(i);
		if (!cdata.validity.RowIsValid(cidx) || !pdata.validity.RowIsValid(pidx)) {
			rmask.SetInvalid(i);
			continue;
		}
		if (std::fabs(prevs[pidx]) < 1e-12) {
			rmask.SetInvalid(i);
			continue;
		}
		rdata[i] = (closes[cidx] - prevs[pidx]) / prevs[pidx];
	}
}

// ============================================================
// 换手率: (volume, float_shares) -> DOUBLE（volume/流通股数）
// ============================================================
static void TurnoverFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto rdata = FlatVector::GetData<double>(result);
	auto &rmask = FlatVector::Validity(result);
	UnifiedVectorFormat vdata, sdata;
	args.data[0].ToUnifiedFormat(count, vdata);
	args.data[1].ToUnifiedFormat(count, sdata);
	auto vols = UnifiedVectorFormat::GetData<double>(vdata);
	auto shares = UnifiedVectorFormat::GetData<double>(sdata);
	for (idx_t i = 0; i < count; i++) {
		auto vidx = vdata.sel->get_index(i);
		auto sidx = sdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(vidx) || !sdata.validity.RowIsValid(sidx)) {
			rmask.SetInvalid(i);
			continue;
		}
		if (std::fabs(shares[sidx]) < 1e-12) {
			rmask.SetInvalid(i);
			continue;
		}
		rdata[i] = vols[vidx] / shares[sidx];
	}
}

// ============================================================
// 是否交易日: (date) -> BOOLEAN
// 复用 cn_ta_trading_calendar.hpp 的统一交易日历（内置 2020-2026 节假日），
// 排除周末 + 法定节假日。与 cn_ta_is_trading_day(ts) 共用同一份日历数据。
// 注：调休补班（周末上班日）暂未纳入，需接入完整交易日历数据。
// ============================================================
static void TradingDayFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto rdata = FlatVector::GetData<bool>(result);
	auto &rmask = FlatVector::Validity(result);
	UnifiedVectorFormat ddata;
	args.data[0].ToUnifiedFormat(count, ddata);
	auto dates = UnifiedVectorFormat::GetData<date_t>(ddata);
	for (idx_t i = 0; i < count; i++) {
		auto didx = ddata.sel->get_index(i);
		if (!ddata.validity.RowIsValid(didx)) {
			rmask.SetInvalid(i);
			continue;
		}
		rdata[i] = IsTradingDay(dates[didx]);
	}
}

// ============================================================
// 辅助：解析 LIST<DOUBLE> 到 vector
// ============================================================
static bool GetDoubleList(DataChunk &args, idx_t col, idx_t row, std::vector<double> &out) {
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
	auto cdata_ptr = UnifiedVectorFormat::GetData<double>(cdata);
	out.reserve(list.length);
	for (idx_t k = 0; k < list.length; k++) {
		auto cidx = cdata.sel->get_index(list.offset + k);
		out.push_back(cdata.validity.RowIsValid(cidx) ? cdata_ptr[cidx] : 0.0);
	}
	return !out.empty();
}

// ============================================================
// 量比: (volume, hist_vol) -> DOUBLE
// 量比 = 当前成交量 / 历史平均成交量（通常用前 5 日均量）
// ============================================================
static void VolumeRatioFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto rdata = FlatVector::GetData<double>(result);
	auto &rmask = FlatVector::Validity(result);
	UnifiedVectorFormat vdata;
	args.data[0].ToUnifiedFormat(count, vdata);
	auto vols = UnifiedVectorFormat::GetData<double>(vdata);
	for (idx_t i = 0; i < count; i++) {
		auto vidx = vdata.sel->get_index(i);
		std::vector<double> hist;
		if (!vdata.validity.RowIsValid(vidx) || !GetDoubleList(args, 1, i, hist) || hist.empty()) {
			rmask.SetInvalid(i);
			continue;
		}
		double sum = 0.0;
		for (double h : hist) {
			sum += h;
		}
		double avg = sum / (double)hist.size();
		if (avg < 1e-12) {
			rmask.SetInvalid(i);
			continue;
		}
		rdata[i] = vols[vidx] / avg;
	}
}

// ============================================================
// 市盈率分位: (pe, pe_hist) -> DOUBLE (0~1)
// 当前 PE 在历史序列中的百分位
// ============================================================
static void PePercentileFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto rdata = FlatVector::GetData<double>(result);
	auto &rmask = FlatVector::Validity(result);
	UnifiedVectorFormat pdata;
	args.data[0].ToUnifiedFormat(count, pdata);
	auto pe = UnifiedVectorFormat::GetData<double>(pdata);
	for (idx_t i = 0; i < count; i++) {
		auto pidx = pdata.sel->get_index(i);
		std::vector<double> hist;
		if (!pdata.validity.RowIsValid(pidx) || !GetDoubleList(args, 1, i, hist) || hist.empty()) {
			rmask.SetInvalid(i);
			continue;
		}
		double cur = pe[pidx];
		// 小于等于当前值的比例
		idx_t below = 0;
		for (double h : hist) {
			if (h <= cur) {
				below++;
			}
		}
		rdata[i] = (double)below / (double)hist.size();
	}
}

// ============================================================
// 复权价: (close, factor) -> DOUBLE
// 除权除息复权价 = close * factor（factor 为复权因子）
// ============================================================
static void AdjustPriceFunc(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	auto rdata = FlatVector::GetData<double>(result);
	auto &rmask = FlatVector::Validity(result);
	UnifiedVectorFormat cdata, fdata;
	args.data[0].ToUnifiedFormat(count, cdata);
	args.data[1].ToUnifiedFormat(count, fdata);
	auto closes = UnifiedVectorFormat::GetData<double>(cdata);
	auto factors = UnifiedVectorFormat::GetData<double>(fdata);
	for (idx_t i = 0; i < count; i++) {
		auto cidx = cdata.sel->get_index(i);
		auto fidx = fdata.sel->get_index(i);
		if (!cdata.validity.RowIsValid(cidx) || !fdata.validity.RowIsValid(fidx)) {
			rmask.SetInvalid(i);
			continue;
		}
		rdata[i] = closes[cidx] * factors[fidx];
	}
}

// ============================================================
// 注册
// ============================================================
void RegisterCnTaAshareFunctions(ExtensionLoader &loader) {
	// cn_limit_pct(stock_code, is_st) -> DOUBLE
	loader.RegisterFunction(ScalarFunction("cn_limit_pct", {LogicalType::VARCHAR, LogicalType::BOOLEAN},
	                                       LogicalType::DOUBLE, LimitPctFunc));

	// cn_limit_price(prev_close, stock_code, is_st) -> STRUCT{limit_up, limit_down}
	child_list_t<LogicalType> limit_fields;
	limit_fields.push_back({"limit_up", LogicalType::DOUBLE});
	limit_fields.push_back({"limit_down", LogicalType::DOUBLE});
	LogicalType limit_struct = LogicalType::STRUCT(std::move(limit_fields));
	loader.RegisterFunction(ScalarFunction("cn_limit_price",
	                                       {LogicalType::DOUBLE, LogicalType::VARCHAR, LogicalType::BOOLEAN},
	                                       limit_struct, LimitPriceFunc));

	// cn_is_st(stock_name) -> BOOLEAN
	loader.RegisterFunction(ScalarFunction("cn_is_st", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, IsSTFunc));

	// cn_change_pct(close, prev_close) -> DOUBLE
	loader.RegisterFunction(ScalarFunction("cn_change_pct", {LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                       LogicalType::DOUBLE, ChangePctFunc));

	// cn_turnover(volume, float_shares) -> DOUBLE
	loader.RegisterFunction(
	    ScalarFunction("cn_turnover", {LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE, TurnoverFunc));

	// cn_trading_day(date) -> BOOLEAN
	loader.RegisterFunction(
	    ScalarFunction("cn_trading_day", {LogicalType::DATE}, LogicalType::BOOLEAN, TradingDayFunc));

	// cn_volume_ratio(volume, hist_vol) -> DOUBLE
	loader.RegisterFunction(ScalarFunction("cn_volume_ratio",
	                                       {LogicalType::DOUBLE, LogicalType::LIST(LogicalType::DOUBLE)},
	                                       LogicalType::DOUBLE, VolumeRatioFunc));

	// cn_pe_percentile(pe, pe_hist) -> DOUBLE
	loader.RegisterFunction(ScalarFunction("cn_pe_percentile",
	                                       {LogicalType::DOUBLE, LogicalType::LIST(LogicalType::DOUBLE)},
	                                       LogicalType::DOUBLE, PePercentileFunc));

	// cn_adjust_price(close, factor) -> DOUBLE
	loader.RegisterFunction(ScalarFunction("cn_adjust_price", {LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                       LogicalType::DOUBLE, AdjustPriceFunc));
}

} // namespace duckdb
