"""
examples
========

生成并执行 cn_ta 扩展【全部函数】的调用示例。

工作方式：
1. 连接 DuckDB 并 LOAD 扩展。
2. 从 duckdb_functions() 读取扩展注册的全部函数签名。
3. 根据每个函数的参数类型自动构造合法的 SQL 调用（标量函数用
   list_value(...) 字面量，聚合/窗口函数用 OVER 窗口），逐条执行。
4. 输出每个函数是否执行成功，最后给出汇总。

用法:
    uv run --directory ./benchmark_py examples
    uv run --directory ./benchmark_py examples --ext /abs/path/cn_ta.duckdb_extension
"""

from __future__ import annotations
from re import L
from sre_parse import ANY
from typing import Any

from _duckdb import DuckDBPyConnection
import duckdb
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXT = (
    ROOT / "build" / "release" / "extension" / "cn_ta" / "cn_ta.duckdb_extension"
)

# 排除的辅助/内部函数（stats 是 DuckDB 调试函数，非本扩展）
EXCLUDE = {"stats"}


def setup(ext_path: str) -> DuckDBPyConnection:
    con = duckdb.connect(config={"allow_unsigned_extensions": True})
    if not Path(ext_path).exists():
        raise FileNotFoundError(f"扩展未找到: {ext_path}")

    _ = con.execute(f"LOAD '{ext_path}';")
    return con


def build_sample_table(con: DuckDBPyConnection) -> None:
    """建一张覆盖 OHLCV + 时间戳的示例表。"""
    _ = con.execute("DROP TABLE IF EXISTS ohlc")
    _ = con.execute("""
        CREATE TABLE ohlc AS
        SELECT * FROM (VALUES
            (TIMESTAMP '2026-08-05 09:31:00', 10.0, 11.2,  9.8, 10.5, 120000.0, 100000.0),
            (TIMESTAMP '2026-08-05 09:32:00', 10.5, 11.8, 10.2, 11.3, 135000.0, 105000.0),
            (TIMESTAMP '2026-08-05 09:33:00', 11.3, 12.1, 11.0, 11.8,  98000.0, 110000.0),
            (TIMESTAMP '2026-08-05 09:34:00', 11.8, 12.5, 11.5, 12.2, 145000.0, 115000.0),
            (TIMESTAMP '2026-08-05 09:35:00', 12.2, 13.0, 11.9, 12.7, 167000.0, 120000.0),
            (TIMESTAMP '2026-08-06 09:31:00', 12.7, 13.4, 12.3, 13.1, 112000.0, 125000.0),
            (TIMESTAMP '2026-08-06 09:32:00', 13.1, 13.8, 12.8, 13.5, 130000.0, 130000.0),
            (TIMESTAMP '2026-08-06 09:33:00', 13.5, 14.2, 13.1, 13.9, 118000.0, 135000.0),
            (TIMESTAMP '2026-08-06 09:34:00', 13.9, 14.6, 13.5, 14.3, 155000.0, 140000.0),
            (TIMESTAMP '2026-08-06 09:35:00', 14.3, 15.0, 13.9, 14.8, 142000.0, 145000.0)
        ) AS t(ts, open, high, low, close, volume, float_shares)
    """)


def fetch_functions(con: DuckDBPyConnection) -> list[dict[Any, Any]]:
    """读取扩展注册的全部函数签名（去重函数名，保留标量/聚合各一个代表签名）。

    用 fetchnumpy() 一次性取列式 numpy 数组，避免 fetchall() 逐行构造
    Python tuple 对象（341 行 × 5 列的小元数据，列式读取更快且省内存）。
    """
    cols = con.execute("""
        SELECT function_name, function_type, parameter_types, return_type, parameters
        FROM duckdb_functions()
        WHERE function_name LIKE 'ct_%'
           OR function_name LIKE 'cta_%'
           OR function_name LIKE 'stat_%'
           OR function_name LIKE 'cn_%'
        ORDER BY function_name
    """).fetchnumpy()

    names = cols["function_name"]
    types = cols["function_type"]
    ptypes = cols["parameter_types"]
    rtypes = cols["return_type"]
    params = cols["parameters"]

    funcs: dict[str, dict] = {}
    for i in range(len(names)):
        name = names[i]
        if name in EXCLUDE:
            continue
        # 同一函数名的多重重载（如 _ts 带/不带 bar_period），保留参数最少的一个代表签名
        ptype_list = list(ptypes[i])
        if name in funcs and len(ptype_list) >= len(funcs[name]["ptypes"]):
            continue
        funcs[name] = {
            "name": name,
            "type": types[i],
            "ptypes": ptype_list,
            "rtype": rtypes[i],
            "params": list(params[i]),
        }
    return sorted(funcs.values(), key=lambda f: f["name"])


# --------------------------------------------------------------------------- #
# 参数构造：根据类型映射到合法的示例值
# --------------------------------------------------------------------------- #
def arg_expr(ptype: str, *, list_form: bool = False) -> str:
    """根据 DuckDB 类型名构造一个示例参数表达式。list_form=True 表示
    当前处于标量函数（需要 LIST 输入）的上下文。"""
    base = ptype
    is_list = base.endswith("[]")
    if is_list:
        base = base[:-2]

    # 标量函数里，DOUBLE[] 等数组参数用 list_value(...) 字面量
    if list_form and is_list:
        if base == "DOUBLE":
            return "list_value(10.0, 11.0, 12.0, 13.0, 14.0)"
        if base == "INTEGER":
            return "list_value(1, 2, 3, 4, 5)"
        return f"[]"

    if base == "DOUBLE":
        return "10.0"
    if base == "INTEGER":
        return "5"
    if base == "BIGINT":
        return "5"
    if base == "BOOLEAN":
        return "false"
    if base == "VARCHAR":
        return "'600000'"
    if base == "TIMESTAMP":
        return "TIMESTAMP '2026-08-05 09:31:00'"
    if base == "DATE":
        return "DATE '2026-08-05'"
    return "NULL"


def build_scalar_sql(f: dict) -> str:
    """构造标量函数（ct_*、stat_*、cn_*）的示例 SQL。"""
    name = f["name"]
    ptypes = f["ptypes"]
    args = []
    for ptype in ptypes:
        args.append(arg_expr(ptype, list_form=True))
    # 标量函数直接 SELECT
    return f"SELECT {name}({', '.join(args)})"


def build_aggregate_sql(f: dict) -> str:
    """构造聚合/窗口函数（cta_*）的示例 SQL。用窗口帧避免 O(N²)。"""
    name = f["name"]
    ptypes = f["ptypes"]
    args = []
    for ptype in ptypes:
        args.append(arg_expr(ptype, list_form=False))
    # 聚合函数用 OVER，注意第一列通常是 ts（时间对齐）或 value
    return f"SELECT {name}({', '.join(args)}) OVER (ORDER BY ts) FROM ohlc"


def build_sql(f: dict) -> str:
    if f["type"] == "scalar":
        return build_scalar_sql(f)
    if f["type"] == "table":
        # 表函数（如 cn_ta_indicators）不能用 OVER，需用 FROM ... 方式调用
        return "SELECT * FROM cn_ta_indicators((SELECT * FROM ohlc))"
    return build_aggregate_sql(f)


def run_examples_sql(con, sql_path: Path) -> None:
    """执行人工编写的 examples.sql（带中文注释的可读示例）。"""
    sql = sql_path.read_text(encoding="utf-8")
    count = 0
    errors = []
    buffer = []
    for line in sql.split("\n"):
        s = line.strip()
        if s.startswith("--") or s == "":
            continue
        buffer.append(line)
        if ";" in s:
            stmt = "\n".join(buffer).strip()
            buffer = []
            try:
                con.execute(stmt)
                count += 1
            except Exception as e:
                errors.append((stmt[:70], str(e).split("\n")[0]))
    print(f"=== examples.sql 执行结果 ===\n  成功: {count} 条语句")
    if errors:
        print("  失败:")
        for s, e in errors:
            print(f"    {s} -> {e}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ext", default=str(DEFAULT_EXT))
    ap.add_argument("--list-only", action="store_true", help="只打印函数清单，不执行")
    ap.add_argument(
        "--sql", action="store_true", help="执行 examples.sql（带注释的可读示例）"
    )
    args = ap.parse_args()

    con: DuckDBPyConnection = setup(args.ext)
    build_sample_table(con)
    funcs = fetch_functions(con)

    print(f"共 {len(funcs)} 个函数\n")

    ok, fail = [], []
    for f in funcs:
        sql = build_sql(f)
        if args.list_only:
            print(f"  [{f['type']:9s}] {f['name']}")
            continue
        try:
            con.execute(sql)
            ok.append(f["name"])
        except Exception as e:
            fail.append((f["name"], str(e).split("\n")[0]))

    if args.list_only:
        return

    print(f"=== 自动生成示例执行结果 ===\n  成功: {len(ok)}  失败: {len(fail)}")
    if fail:
        print("\n失败明细:")
        for name, err in fail:
            print(f"  - {name}: {err}")

    if args.sql:
        print()
        run_examples_sql(con, Path(__file__).resolve().parent / "examples.sql")


if __name__ == "__main__":
    main()
