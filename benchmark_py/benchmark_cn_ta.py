"""
benchmark_cn_ta
================

对比测试：Python `talib` (TA-Lib) vs DuckDB `cn_ta` 扩展，对 A股分钟K数据计算技术
指标，校验**结果一致性**并对比**性能**。

重点体现 DuckDB 优势：把大量分钟K线一次性导入 DuckDB 内存表，用 SQL 窗口函数
(`cta_*`) 在数据库内按股票分组向量化计算；对比 Python `talib` 逐股循环计算。

用法:
    uv run main.py                  # 拉取/复用分钟K大表，跑一致性+性能
    uv run main.py --reload        # 强制重新拉取数据
    uv run main.py --ext /abs/path/cn_ta.duckdb_extension

依赖: ta-lib, duckdb, akshare, pandas, numpy
"""

from __future__ import annotations

import argparse
import statistics
import time
from pathlib import Path

import numpy as np
import pandas as pd

try:
    import akshare as ak
except Exception:  # pragma: no cover
    ak = None

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXT = ROOT / "build" / "release" / "extension" / "cn_ta" / "cn_ta.duckdb_extension"
DATA_DIR = Path(__file__).resolve().parent / "data"
CACHE_CSV = DATA_DIR / "minute_bulk.csv"
ALIGNED_CSV = DATA_DIR / "minute_bulk_aligned.csv"

# A股 1 分钟 K 交易时段（分钟），排除午休，共 238 根/交易日
# 上午 09:31–11:30（120 根）
# 下午 13:01–14:57（117 根）+ 15:00 收盘价（1 根）
# 14:58、14:59 为收盘集合竞价，新浪接口不返回这两根分钟线
_MORNING = pd.date_range("09:31", "11:30", freq="1min")       # 120 根
_AFTERNOON = pd.date_range("13:01", "14:57", freq="1min")     # 117 根
_CLOSE_AUCTION = pd.Index([pd.Timestamp("15:00").time()])      # 1 根 (收盘价)
SESSION_MINUTES = pd.Index(
    list(_MORNING.time) + list(_AFTERNOON.time) + list(_CLOSE_AUCTION)
)  # 238 根

# 一篮子 A股（601919 必含，去重），用于拼出足够大的分钟K数据集
SYMBOLS = [
    "601919", "600519", "600036", "601318", "000858", "600276", "601166",
    "600030", "600887", "000001", "601398", "600000", "601888", "600009",
    "000333", "002594", "601012", "601628", "600028", "601166", "600104",
    "000651", "600276", "601857", "600958", "601688", "600000", "000725",
    "601288", "600048", "000002", "601988", "600016", "601169", "600585",
    "000063", "601668", "600031", "601390", "600111", "000776", "601211",
    "600837", "601899", "000568", "600690", "601933", "600660", "002415",
    "601658", "600028", "601186", "600050", "000100", "601138", "600196",
    "000338", "601988", "600009",
]


# --------------------------------------------------------------------------- #
# 数据获取
# --------------------------------------------------------------------------- #
def _sym_prefix(code: str) -> str:
    return "sh" + code if code.startswith("6") else "sz" + code


def fetch_minute_bulk(reload: bool = False) -> pd.DataFrame:
    """拉取一篮子 A股 1 分钟线，拼成大表缓存到 CSV（symbol,ts,open,high,low,close,volume）。"""
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if CACHE_CSV.exists() and not reload:
        print(f"[data] 复用缓存: {CACHE_CSV}")
        return pd.read_csv(CACHE_CSV, parse_dates=["ts"], dtype={"symbol": str})

    if ak is None:
        raise RuntimeError("akshare 未安装")

    frames = []
    seen = set()
    for code in SYMBOLS:
        if code in seen:
            continue
        seen.add(code)
        try:
            df = ak.stock_zh_a_minute(symbol=_sym_prefix(code), period="1", adjust="")
            if df is None or len(df) == 0:
                print(f"[data] {code} 空，跳过")
                continue
            df = df.rename(columns={
                "day": "ts", "open": "open", "high": "high",
                "low": "low", "close": "close", "volume": "volume",
            })
            df["symbol"] = code
            df["ts"] = pd.to_datetime(df["ts"])
            df["symbol"] = code  # 字符串，避免 CSV 读回时被推断为 int
            frames.append(df[["symbol", "ts", "open", "high", "low", "close", "volume"]])
            print(f"[data] {code}: {len(df)} 行")
        except Exception as e:
            print(f"[data] {code} 失败: {e!r}")
    if not frames:
        raise RuntimeError("未拉到任何分钟数据")
    out = pd.concat(frames, ignore_index=True)
    out.to_csv(CACHE_CSV, index=False)
    print(f"[data] 已缓存 {len(out)} 行 / {out['symbol'].nunique()} 只 -> {CACHE_CSV}")
    return out


# --------------------------------------------------------------------------- #
# 时间轴对齐 + 前值填充（量化行业标准）
# --------------------------------------------------------------------------- #
def build_minute_grid(days: pd.DatetimeIndex) -> pd.DatetimeIndex:
    """为给定的交易日集合生成完整分钟时间轴（每个交易日 238 根，排除午休+收盘竞价）。"""
    ticks = []
    for d in days.normalize().unique():
        for t in SESSION_MINUTES:
            ticks.append(d + pd.Timedelta(hours=t.hour, minutes=t.minute))
    return pd.DatetimeIndex(ticks)


def filter_outlier_symbols(df: pd.DataFrame) -> pd.DataFrame:
    """
    过滤日期范围明显偏离主流的异常股票（如接口对某些股票返回过期历史数据）。
    策略：以所有股票的「日期中位数」为锚，丢弃中位数日期偏离锚超过 30 天的股票。
    """
    medians = {}
    for code, grp in df.groupby("symbol", sort=False):
        medians[code] = grp["ts"].dt.normalize().median()
    anchor = pd.Series(list(medians.values())).median()
    keep = [c for c, m in medians.items() if abs((m - anchor).days) <= 30]
    dropped = [c for c in medians if c not in keep]
    if dropped:
        print(f"[align] 过滤异常日期股票 {len(dropped)} 只: {dropped}")
    return df[df["symbol"].isin(keep)].copy()


def align_and_ffill(df: pd.DataFrame) -> pd.DataFrame:
    """
    将每只股票的分钟K对齐到【全局公共交易日】网格，缺失分钟前值填充(ffill)。
    - 先过滤日期范围异常股票（filter_outlier_symbols）。
    - 用所有保留股票的公共交易日集合（min~max 交集）生成统一网格。
    - 缺口处 OHLC 前值填充（无成交期间价格不变，行业标准），volume 填 0。
    - 返回: symbol, ts, open, high, low, close, volume（按 symbol, ts 排序）。
    """
    df = filter_outlier_symbols(df)
    cols = ["symbol", "ts", "open", "high", "low", "close", "volume"]

    # 公共交易日集合：所有股票的日期并集（而非各自首尾），保证同一时间轴
    all_days = sorted(pd.DatetimeIndex(df["ts"].dt.normalize().unique()))
    # 只保留所有股票都覆盖到的公共窗口（首尾对齐到最晚开始/最早结束）
    # 简化：取全局 min~max 日期作为交易日轴，单只股票缺的日期整段留空(ffill 兜底)
    grid = build_minute_grid(pd.DatetimeIndex(all_days))

    out_frames = []
    total_gap = 0
    for code, grp in df.groupby("symbol", sort=False):
        grp = grp.sort_values("ts").drop_duplicates("ts")
        full = pd.DataFrame({"ts": grid})
        full = full.merge(grp[cols], on="ts", how="left")
        gap = full["close"].isna().sum()
        total_gap += int(gap)
        full["symbol"] = code
        full["open"] = full["open"].ffill()
        full["high"] = full["high"].ffill()
        full["low"] = full["low"].ffill()
        full["close"] = full["close"].ffill()
        full["volume"] = full["volume"].fillna(0.0)
        # 首日开盘前无前值可填 => 丢弃该股票首日开盘前的空行（不硬凑）
        full = full.dropna(subset=["close"]).reset_index(drop=True)
        out_frames.append(full[cols])
    out = pd.concat(out_frames, ignore_index=True)
    print(f"[align] 补齐网格: 原 {len(df)} 行 -> {len(out)} 行, 补齐 {total_gap} 个缺失分钟, 交易日 {len(all_days)} 天")
    return out


# --------------------------------------------------------------------------- #
# 参考实现：Python TA-Lib（逐股循环）
# --------------------------------------------------------------------------- #
def compute_talib_grouped(df: pd.DataFrame) -> dict[str, np.ndarray]:
    import talib

    out: dict[str, np.ndarray] = {}
    for key, grp in df.groupby("symbol", sort=False):
        close = grp["close"].to_numpy(dtype=float)
        out[key] = {
            "sma20": talib.SMA(close, 20),
            "ema20": talib.EMA(close, 20),
            "rsi14": talib.RSI(close, 14),
        }
    return out


# --------------------------------------------------------------------------- #
# DuckDB cn_ta 实现（一次性导入内存表，SQL 窗口函数按股票分组）
# --------------------------------------------------------------------------- #
def setup_duckdb(ext_path: str, csv_path: str, raw_csv_path: str):
    """建立 DuckDB 连接并一次性导入两张表（只做一次，不算进基准）：
    - ticks: 对齐后的数据（用于 cta_* 数据层对齐方案）
    - ticks_raw: 原始带缺口数据（用于 cta_*_ts 扩展内自动对齐方案）
    """
    import duckdb

    con = duckdb.connect(config={"allow_unsigned_extensions": True})
    if not Path(ext_path).exists():
        raise FileNotFoundError(f"扩展未找到: {ext_path}")
    con.execute(f"LOAD '{ext_path}';")
    con.execute(f"""
        CREATE TABLE ticks AS
        SELECT symbol, ts, open, high, low, close, volume
        FROM read_csv('{csv_path}', auto_detect=true, header=true,
                      types={{'symbol': 'VARCHAR'}});
    """)
    con.execute(f"""
        CREATE TABLE ticks_raw AS
        SELECT symbol, ts, open, high, low, close, volume
        FROM read_csv('{raw_csv_path}', auto_detect=true, header=true,
                      types={{'symbol': 'VARCHAR'}});
    """)
    n = con.execute("SELECT count(*) AS c FROM ticks;").fetchnumpy()["c"][0]
    nr = con.execute("SELECT count(*) AS c FROM ticks_raw;").fetchnumpy()["c"][0]
    print(f"[duckdb] 内存表 ticks: {n} 行 (对齐) / ticks_raw: {nr} 行 (原始) 一次性导入完成")
    return con


def _columns_to_grouped(con, sql: str) -> dict[str, dict[str, np.ndarray]]:
    """用 fetchnumpy 列式取回结果，再用 pandas groupby 按 symbol 分组，
    避免 fetchall + 逐行 Python 循环装箱。"""
    cols = con.execute(sql).fetchnumpy()
    df = pd.DataFrame({
        "symbol": cols["symbol"].astype(str),
        "sma20": cols["sma20"],
        "ema20": cols["ema20"],
        "rsi14": cols["rsi14"],
    })
    out: dict[str, dict[str, np.ndarray]] = {}
    for sym, grp in df.groupby("symbol", sort=False):
        out[sym] = {
            "sma20": grp["sma20"].to_numpy(dtype=float),
            "ema20": grp["ema20"].to_numpy(dtype=float),
            "rsi14": grp["rsi14"].to_numpy(dtype=float),
        }
    return out


def query_cn_ta(con) -> dict[str, np.ndarray]:
    """仅执行指标计算 SQL（进入性能基准）。按股票分组用 cta_* 窗口聚合。"""
    return _columns_to_grouped(con, """
        SELECT symbol,
               cta_sma(close, 20) OVER (PARTITION BY symbol ORDER BY ts) AS sma20,
               cta_ema(close, 20) OVER (PARTITION BY symbol ORDER BY ts) AS ema20,
               cta_rsi(close, 14) OVER (PARTITION BY symbol ORDER BY ts) AS rsi14
        FROM ticks
        ORDER BY symbol, ts;
    """)


def query_cn_ta_ts(con) -> dict[str, np.ndarray]:
    """用 cta_*_ts 扩展内自动对齐，直接作用于原始（未补齐）数据。
    验证扩展内部时间戳对齐与数据层对齐结果一致。"""
    return _columns_to_grouped(con, """
        SELECT symbol,
               cta_sma_ts(ts, close, 20) OVER (PARTITION BY symbol ORDER BY ts) AS sma20,
               cta_ema_ts(ts, close, 20) OVER (PARTITION BY symbol ORDER BY ts) AS ema20,
               cta_rsi_ts(ts, close, 14) OVER (PARTITION BY symbol ORDER BY ts) AS rsi14
        FROM ticks_raw
        ORDER BY symbol, ts;
    """)


def query_native(con) -> None:
    """DuckDB 原生窗口函数（纯 SQL 向量化，不含 TA-Lib 逐行开销），仅用于性能对照。"""
    con.execute("""
        SELECT symbol,
               mean(close) OVER (
                   PARTITION BY symbol ORDER BY ts
                   ROWS BETWEEN 19 PRECEDING AND CURRENT ROW) AS sma20
        FROM ticks
        ORDER BY symbol, ts;
    """).fetchnumpy()


# --------------------------------------------------------------------------- #
# 一致性校验
# --------------------------------------------------------------------------- #
def max_abs_err(a, b) -> float:
    a = np.asarray(a, dtype=float)
    b = np.asarray(b, dtype=float)
    mask = ~np.isnan(a) & ~np.isnan(b)
    if not mask.any():
        return float("nan")
    return float(np.max(np.abs(a[mask] - b[mask])))


def compare_consistency(talib_out, cn_out) -> None:
    print("\n=== 结果一致性 (talib vs cn_ta, 按股票) ===")
    for key in ["sma20", "ema20", "rsi14"]:
        errs = []
        for sym in talib_out:
            if sym in cn_out:
                errs.append(max_abs_err(talib_out[sym][key], cn_out[sym][key]))
        worst = max(errs) if errs else float("nan")
        status = "OK" if (np.isnan(worst) or worst < 1e-6) else "DIFF"
        print(f"  {key:12s} 最大 max|Δ| = {worst:<12.3e}  股票数={len(errs)}  [{status}]")


# --------------------------------------------------------------------------- #
# 性能基准
# --------------------------------------------------------------------------- #
def bench(fn, repeats: int = 10):
    fn()  # 预热
    ts = [time.perf_counter() for _ in range(repeats)]
    for i in range(repeats):
        t0 = time.perf_counter()
        fn()
        ts[i] = time.perf_counter() - t0
    return statistics.mean(ts), min(ts)


def perf_compare(talib_fn, cn_fn, native_fn, n_rows: int) -> None:
    print(f"\n=== 性能对比 ({n_rows} 行分钟K, 多次平均, 剔除首次) ===")
    t_mean, t_min = bench(talib_fn)
    c_mean, c_min = bench(cn_fn)
    n_mean, n_min = bench(native_fn)
    print(f"  talib       : mean={t_mean*1000:8.3f} ms  best={t_min*1000:8.3f} ms  (Python 逐股循环)")
    print(f"  cn_ta(cta_*) : mean={c_mean*1000:8.3f} ms  best={c_min*1000:8.3f} ms  (SQL+TA-Lib 窗口)")
    print(f"  duckdb(native): mean={n_mean*1000:8.3f} ms  best={n_min*1000:8.3f} ms  (纯 SQL 向量化)")
    print(f"  speedup talib/cn_ta      = {t_mean/c_mean:6.2f}x")
    print(f"  speedup talib/duckdb     = {t_mean/n_mean:6.2f}x")
    print(f"  speedup cn_ta/duckdb     = {c_mean/n_mean:6.2f}x")


# --------------------------------------------------------------------------- #
# 主流程
# --------------------------------------------------------------------------- #
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reload", action="store_true")
    ap.add_argument("--ext", default=str(DEFAULT_EXT))
    ap.add_argument("--repeats", type=int, default=10)
    args = ap.parse_args()

    raw = fetch_minute_bulk(reload=args.reload)
    print(f"[bench] 原始数据集: {raw['symbol'].nunique()} 只股票, {len(raw)} 行 1 分钟K")

    # 时间轴对齐 + 前值填充（量化行业标准），缓存对齐后的 CSV
    if ALIGNED_CSV.exists() and not args.reload:
        df = pd.read_csv(ALIGNED_CSV, parse_dates=["ts"], dtype={"symbol": str})
        print(f"[align] 复用对齐缓存: {ALIGNED_CSV}")
    else:
        df = align_and_ffill(raw)
        df.to_csv(ALIGNED_CSV, index=False)
        print(f"[align] 对齐后 {len(df)} 行 -> {ALIGNED_CSV}")
    print(f"[bench] 对齐数据集: {df['symbol'].nunique()} 只股票, {len(df)} 行 (统一 240 分钟/日网格)")

    # 参考：talib 逐股循环（基于对齐后的连续序列）
    talib_out = compute_talib_grouped(df)

    # 被测 1：cn_ta 数据层对齐 —— 对齐数据 + cta_*
    # 被测 2：cn_ta 扩展内自动对齐 —— 原始数据 + cta_*_ts
    con = setup_duckdb(args.ext, str(ALIGNED_CSV), str(CACHE_CSV))
    cn_out = query_cn_ta(con)
    cn_ts_out = query_cn_ta_ts(con)

    print("\n=== 结果一致性 (talib vs cta_* 数据层对齐) ===")
    compare_consistency(talib_out, cn_out)

    print("\n=== 结果一致性 (talib vs cta_*_ts 扩展内自动对齐) ===")
    # cta_*_ts 作用于原始数据（含 600837），talib 基于对齐数据（已剔除 600837），
    # 只对比两者共同股票
    common_syms = set(talib_out) & set(cn_ts_out)
    print(f"  共同股票数: {len(common_syms)}")
    for key in ["sma20", "ema20", "rsi14"]:
        errs = []
        for sym in common_syms:
            a = talib_out[sym][key]
            b = cn_ts_out[sym][key]
            # 长度可能不同（原始 vs 对齐首日），取 min 长度对齐比较
            n = min(len(a), len(b))
            errs.append(max_abs_err(a[:n], b[:n]))
        worst = max(errs) if errs else float("nan")
        status = "OK" if (np.isnan(worst) or worst < 1e-6) else "DIFF"
        print(f"  {key:12s} 最大 max|Δ| = {worst:<12.3e}  [{status}]")

    perf_compare(
        lambda: compute_talib_grouped(df),   # 纯计算（不含 CSV 解析）
        lambda: query_cn_ta(con),            # 纯 SQL+TA-Lib 指标计算（不含建表）
        lambda: query_native(con),           # 纯 SQL 向量化（不含建表）
        len(df),
    )

    print("\n完成。")


if __name__ == "__main__":
    main()
