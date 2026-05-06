from __future__ import annotations

import json
import os
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


BASE_DIR = Path(__file__).resolve().parent
REPO_ROOT = BASE_DIR.parent
DEFAULT_DB_PATH = REPO_ROOT / ".fi_data" / "fault_history.sqlite3"
DB_ENV = "FI_HISTORY_DB"


def get_db_path() -> Path:
    return Path(os.environ.get(DB_ENV, str(DEFAULT_DB_PATH))).expanduser()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def ts_to_iso(value: Optional[float]) -> str:
    if value is None:
        return utc_now()
    return datetime.fromtimestamp(float(value), tz=timezone.utc).isoformat(timespec="seconds")


def to_json(value: Any) -> str:
    return json.dumps(value if value is not None else {}, ensure_ascii=False, sort_keys=True)


def from_json(value: Optional[str], default: Any) -> Any:
    if not value:
        return default
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return default


def connect() -> sqlite3.Connection:
    db_path = get_db_path()
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def init_db() -> None:
    with connect() as conn:
        conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS fault_runs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                run_type TEXT NOT NULL,
                action_key TEXT,
                scenario_key TEXT,
                title TEXT,
                params_json TEXT NOT NULL DEFAULT '{}',
                ok INTEGER NOT NULL DEFAULT 0,
                started_at TEXT NOT NULL,
                finished_at TEXT NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS fault_results (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                run_id INTEGER NOT NULL,
                phase TEXT NOT NULL,
                check_title TEXT,
                node TEXT,
                host TEXT,
                cmd TEXT,
                stdout TEXT,
                stderr TEXT,
                exit_code INTEGER,
                elapsed REAL,
                ok INTEGER NOT NULL DEFAULT 0,
                truncated INTEGER NOT NULL DEFAULT 0,
                stdout_meta_json TEXT NOT NULL DEFAULT '{}',
                stderr_meta_json TEXT NOT NULL DEFAULT '{}',
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY (run_id) REFERENCES fault_runs(id) ON DELETE CASCADE
            );

            CREATE INDEX IF NOT EXISTS idx_fault_runs_created_at
                ON fault_runs(created_at DESC);
            CREATE INDEX IF NOT EXISTS idx_fault_runs_action
                ON fault_runs(action_key, scenario_key);
            CREATE INDEX IF NOT EXISTS idx_fault_results_run
                ON fault_results(run_id, phase);
            """
        )


def _insert_results(
    conn: sqlite3.Connection,
    run_id: int,
    phase: str,
    results: Iterable[Dict[str, Any]],
    check_title: Optional[str] = None,
) -> None:
    for res in results or []:
        conn.execute(
            """
            INSERT INTO fault_results (
                run_id, phase, check_title, node, host, cmd, stdout, stderr,
                exit_code, elapsed, ok, truncated, stdout_meta_json, stderr_meta_json
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                run_id,
                phase,
                check_title,
                res.get("node"),
                res.get("host"),
                res.get("cmd"),
                res.get("stdout"),
                res.get("stderr"),
                res.get("exit_code"),
                res.get("elapsed"),
                1 if res.get("ok") else 0,
                1 if res.get("truncated") else 0,
                to_json(res.get("stdout_meta", {})),
                to_json(res.get("stderr_meta", {})),
            ),
        )


def record_run(
    *,
    run_type: str,
    action_key: Optional[str] = None,
    scenario_key: Optional[str] = None,
    title: Optional[str] = None,
    params: Optional[Dict[str, Any]] = None,
    ok: bool = False,
    started_at: Optional[float] = None,
    finished_at: Optional[float] = None,
    phases: Optional[List[Dict[str, Any]]] = None,
) -> int:
    init_db()
    with connect() as conn:
        cur = conn.execute(
            """
            INSERT INTO fault_runs (
                run_type, action_key, scenario_key, title, params_json,
                ok, started_at, finished_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                run_type,
                action_key,
                scenario_key,
                title,
                to_json(params or {}),
                1 if ok else 0,
                ts_to_iso(started_at),
                ts_to_iso(finished_at),
            ),
        )
        run_id = int(cur.lastrowid)
        for phase in phases or []:
            _insert_results(
                conn,
                run_id,
                str(phase.get("phase") or "action"),
                phase.get("results", []),
                phase.get("check_title"),
            )
        return run_id


def _run_from_row(row: sqlite3.Row) -> Dict[str, Any]:
    return {
        "id": row["id"],
        "run_type": row["run_type"],
        "action_key": row["action_key"],
        "scenario_key": row["scenario_key"],
        "title": row["title"],
        "params": from_json(row["params_json"], {}),
        "ok": bool(row["ok"]),
        "started_at": row["started_at"],
        "finished_at": row["finished_at"],
        "created_at": row["created_at"],
        "result_count": row["result_count"],
        "failed_count": row["failed_count"],
    }


def list_runs(limit: int = 50, run_type: Optional[str] = None) -> List[Dict[str, Any]]:
    init_db()
    limit = max(1, min(int(limit), 500))
    where = ""
    args: List[Any] = []
    if run_type:
        where = "WHERE r.run_type = ?"
        args.append(run_type)
    args.append(limit)
    with connect() as conn:
        rows = conn.execute(
            f"""
            SELECT
                r.*,
                COUNT(fr.id) AS result_count,
                COALESCE(SUM(CASE WHEN fr.ok = 0 THEN 1 ELSE 0 END), 0) AS failed_count
            FROM fault_runs r
            LEFT JOIN fault_results fr ON fr.run_id = r.id
            {where}
            GROUP BY r.id
            ORDER BY r.id DESC
            LIMIT ?
            """,
            args,
        ).fetchall()
    return [_run_from_row(row) for row in rows]


def get_run(run_id: int) -> Optional[Dict[str, Any]]:
    init_db()
    with connect() as conn:
        row = conn.execute(
            """
            SELECT
                r.*,
                COUNT(fr.id) AS result_count,
                COALESCE(SUM(CASE WHEN fr.ok = 0 THEN 1 ELSE 0 END), 0) AS failed_count
            FROM fault_runs r
            LEFT JOIN fault_results fr ON fr.run_id = r.id
            WHERE r.id = ?
            GROUP BY r.id
            """,
            (run_id,),
        ).fetchone()
        if not row:
            return None
        run = _run_from_row(row)
        result_rows = conn.execute(
            """
            SELECT * FROM fault_results
            WHERE run_id = ?
            ORDER BY id ASC
            """,
            (run_id,),
        ).fetchall()
    run["results"] = [
        {
            "id": r["id"],
            "phase": r["phase"],
            "check_title": r["check_title"],
            "node": r["node"],
            "host": r["host"],
            "cmd": r["cmd"],
            "stdout": r["stdout"],
            "stderr": r["stderr"],
            "exit_code": r["exit_code"],
            "elapsed": r["elapsed"],
            "ok": bool(r["ok"]),
            "truncated": bool(r["truncated"]),
            "stdout_meta": from_json(r["stdout_meta_json"], {}),
            "stderr_meta": from_json(r["stderr_meta_json"], {}),
            "created_at": r["created_at"],
        }
        for r in result_rows
    ]
    return run
