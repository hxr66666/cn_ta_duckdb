"""
indicators_demo
===============

演示 cn_ta 扩展的**懒人函数** `cn_ta_indicators`：传入一个 K 线子查询，
一键批量计算全部 TA-Lib 技术指标 + A 股适配 + Eigen 金融统计，生成量化宽表。

用法:
    uv run --directory ./benchmark_py indicators
    uv run --directory ./benchmark_py indicators --ext /abs/path/cn_ta.duckdb_extension
    uv run --directory ./benchmark_py indicators --symbol 601919   # 指定股票
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXT = ROOT / "build" / "release" / "extension" / "cn_ta" / "cn_ta.duckdb_extension"
DATA_DIR = Path(__file__).resolve().parent / "data"
ALIGNED_CSV = DATA_DIR / "minute_bulk_aligned.csv"


def setup(ext_path: str):
    import duckdb

    con = duckdb.connect(config={"allow_unsigned_extensions": True})
    if not Path(ext_path).exists():
        raise FileNotFoundError(f"扩展未找到: {ext_path}")
    con.execute(f"LOAD '{ext_path}';")
    return con


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ext", default=str(DEFAULT_EXT))
    ap.add_argument("--symbol", default="601919", help="股票代码")
    args = ap.parse_args()

    con = setup(args.ext)

    # 1. 从缓存 CSV 建内存表（复用 benchmark 拉取的对齐分钟数据）
    if ALIGNED_CSV.exists():
        con.execute(f"""
            CREATE TABLE ticks AS
            SELECT symbol, ts, open, high, low, close, volume
            FROM read_csv('{ALIGNED_CSV}', auto_detect=true, header=true,
                          types={{'symbol': 'VARCHAR'}})
        """)
        print(f"[数据] 从缓存导入 {ALIGNED_CSV.name}")
    else:
        # 无缓存则造少量示例数据
        con.execute("""
            CREATE TABLE ticks AS
            SELECT * FROM (VALUES
                ('601919', TIMESTAMP '2026-08-05 09:31:00', 10.0, 11.2, 9.8, 10.5, 120000.0),
                ('601919', TIMESTAMP '2026-08-05 09:32:00', 10.5, 11.8, 10.2, 11.3, 135000.0),
                ('601919', TIMESTAMP '2026-08-05 09:33:00', 11.3, 12.1, 11.0, 11.8, 98000.0),
                ('601919', TIMESTAMP '2026-08-05 09:34:00', 11.8, 12.5, 11.5, 12.2, 145000.0),
                ('601919', TIMESTAMP '2026-08-05 09:35:00', 12.2, 13.0, 11.9, 12.7, 167000.0)
            ) AS t(symbol, ts, open, high, low, close, volume)
        """)
        print("[数据] 无缓存，使用示例数据")

    symbol = args.symbol

    # 2. 懒人函数：一键生成 86 列宽表
    print(f"\n=== cn_ta_indicators 懒人函数演示 (symbol={symbol}) ===")
    t0 = time.perf_counter()
    con.execute(f"""
        CREATE TABLE ind_result AS
        SELECT * FROM cn_ta_indicators((
            SELECT ts, open, high, low, close, volume,
                   volume AS amount, volume AS outstanding_share, volume AS turnover
            FROM ticks WHERE symbol = '{symbol}'
        ))
    """)
    elapsed = time.perf_counter() - t0

    nrows = con.execute("SELECT count(*) FROM ind_result").fetchone()[0]
    ncols = len(con.execute("SELECT * FROM ind_result LIMIT 0").description)
    print(f"生成宽表: {nrows} 行 × {ncols} 列，耗时 {elapsed:.3f}s")

    # 3. 列分类统计
    cols = [d[0] for d in con.execute("SELECT * FROM ind_result LIMIT 0").description]
    multi = {"bbands_upper", "bbands_middle", "bbands_lower", "macd", "macd_signal", "macd_hist",
             "stoch_k", "stoch_d", "aroon_down", "aroon_up", "minmax_min", "minmax_max",
             "mama", "fama", "ht_inphase", "ht_quadrature", "ht_sine", "ht_leadsine"}
    cat = {"原始列": 0, "TA-Lib 单输出指标": 0, "TA-Lib 多输出指标": 0, "A股适配": 0, "整段统计": 0, "滚动统计": 0}
    for c in cols:
        if c in ("ts", "open", "high", "low", "close", "volume", "amount", "outstanding_share", "turnover"):
            cat["原始列"] += 1
        elif c.startswith("stat_"):
            cat["整段统计"] += 1
        elif c.startswith("roll_"):
            cat["滚动统计"] += 1
        elif c in ("pct_change", "volume_ratio", "amount_ma5", "amount_ma10", "turnover_calc"):
            cat["A股适配"] += 1
        elif c in multi:
            cat["TA-Lib 多输出指标"] += 1
        else:
            cat["TA-Lib 单输出指标"] += 1
    print("\n列分类:")
    for k, v in cat.items():
        print(f"  {k}: {v} 列")

    # 4. 抽取关键指标展示
    print("\n关键指标预览（前 3 行）:")
    sample = con.execute(f"""
        SELECT ts, close, sma_20, rsi_12, atr_14, pct_change, stat_sharpe
        FROM ind_result ORDER BY ts LIMIT 100
    """).fetchall()
    print("  ts                  close    sma_20    rsi_12   atr_14   pct_change  stat_sharpe")
    for row in sample:
        def fmt(v):
            return "   NULL" if v is None else f"{v:8.4f}"
        print(f"  {str(row[0])[:19]:19s} {fmt(row[1])} {fmt(row[2])} {fmt(row[3])} {fmt(row[4])} {fmt(row[5])} {fmt(row[6])}")

    # 5. 提示：如何查询指定列
    print("\n只取需要的列：")
    print("""  SELECT ts, close, sma_20, rsi_12, atr_14, pct_change
    FROM cn_ta_indicators((SELECT ts, open, high, low, close, volume FROM ticks WHERE symbol = '601919'));""")

    print("\n完成。")


if __name__ == "__main__":
    main()
