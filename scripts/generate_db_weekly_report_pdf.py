from __future__ import annotations

import sqlite3
import sys
from pathlib import Path
from typing import Any, Dict, List

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (
    PageBreak,
    Paragraph,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from web_controller.db import get_db_path, list_runs  # noqa: E402


OUT_DIR = ROOT / "output" / "pdf"
OUT_PATH = OUT_DIR / "数据库部分周汇报.pdf"
MINT = colors.Color(18 / 255, 204 / 255, 185 / 255)
MINT_LIGHT = colors.Color(226 / 255, 252 / 255, 249 / 255)
INK = colors.Color(23 / 255, 40 / 255, 52 / 255)
MUTED = colors.Color(92 / 255, 110 / 255, 122 / 255)
LINE = colors.Color(215 / 255, 234 / 255, 235 / 255)


def register_fonts() -> str:
    font_name = "STHeiti-Light"
    font_path = Path("/System/Library/Fonts/STHeiti Light.ttc")
    if font_path.exists():
        pdfmetrics.registerFont(TTFont(font_name, str(font_path)))
    else:
        font_name = "Helvetica"
    return font_name


FONT = register_fonts()


def make_styles() -> Dict[str, ParagraphStyle]:
    styles = getSampleStyleSheet()
    return {
        "title": ParagraphStyle(
            "title",
            parent=styles["Title"],
            fontName=FONT,
            fontSize=25,
            leading=34,
            textColor=INK,
            alignment=TA_CENTER,
            spaceAfter=8,
        ),
        "subtitle": ParagraphStyle(
            "subtitle",
            parent=styles["Normal"],
            fontName=FONT,
            fontSize=11,
            leading=18,
            textColor=MUTED,
            alignment=TA_CENTER,
            spaceAfter=18,
        ),
        "h1": ParagraphStyle(
            "h1",
            parent=styles["Heading1"],
            fontName=FONT,
            fontSize=17,
            leading=24,
            textColor=INK,
            spaceBefore=6,
            spaceAfter=10,
        ),
        "h2": ParagraphStyle(
            "h2",
            parent=styles["Heading2"],
            fontName=FONT,
            fontSize=13,
            leading=20,
            textColor=INK,
            spaceBefore=4,
            spaceAfter=6,
        ),
        "body": ParagraphStyle(
            "body",
            parent=styles["BodyText"],
            fontName=FONT,
            fontSize=10.3,
            leading=17,
            textColor=INK,
            wordWrap="CJK",
            spaceAfter=6,
        ),
        "small": ParagraphStyle(
            "small",
            parent=styles["BodyText"],
            fontName=FONT,
            fontSize=8.7,
            leading=13,
            textColor=INK,
            wordWrap="CJK",
        ),
        "muted": ParagraphStyle(
            "muted",
            parent=styles["BodyText"],
            fontName=FONT,
            fontSize=9.5,
            leading=15,
            textColor=MUTED,
            wordWrap="CJK",
        ),
        "cell": ParagraphStyle(
            "cell",
            parent=styles["BodyText"],
            fontName=FONT,
            fontSize=8.5,
            leading=12.5,
            textColor=INK,
            wordWrap="CJK",
        ),
        "cell_head": ParagraphStyle(
            "cell_head",
            parent=styles["BodyText"],
            fontName=FONT,
            fontSize=8.8,
            leading=12,
            textColor=colors.white,
            alignment=TA_CENTER,
        ),
    }


S = make_styles()


def p(text: str, style: str = "body") -> Paragraph:
    return Paragraph(text, S[style])


def bullets(items: List[str]) -> List[Paragraph]:
    return [p(f"· {item}") for item in items]


def make_table(data: List[List[Any]], widths: List[float], header: bool = True) -> Table:
    converted: List[List[Any]] = []
    for r, row in enumerate(data):
        converted.append([
            cell if hasattr(cell, "wrap") else p(str(cell), "cell_head" if header and r == 0 else "cell")
            for cell in row
        ])
    table = Table(converted, colWidths=widths, hAlign="LEFT")
    style = [
        ("GRID", (0, 0), (-1, -1), 0.4, LINE),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 6),
        ("RIGHTPADDING", (0, 0), (-1, -1), 6),
        ("TOPPADDING", (0, 0), (-1, -1), 6),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 6),
        ("BACKGROUND", (0, 1), (-1, -1), colors.white),
    ]
    if header:
        style += [
            ("BACKGROUND", (0, 0), (-1, 0), MINT),
            ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ]
    table.setStyle(TableStyle(style))
    return table


def db_summary() -> Dict[str, Any]:
    path = get_db_path()
    if not path.exists():
        return {"path": str(path), "run_count": 0, "result_count": 0, "runs": []}
    with sqlite3.connect(path) as conn:
        run_count = conn.execute("SELECT COUNT(*) FROM fault_runs").fetchone()[0]
        result_count = conn.execute("SELECT COUNT(*) FROM fault_results").fetchone()[0]
    return {
        "path": str(path),
        "run_count": run_count,
        "result_count": result_count,
        "runs": list_runs(limit=6),
    }


def header_footer(canvas, doc) -> None:
    canvas.saveState()
    width, height = A4
    line_y = height - 16 * mm
    canvas.setStrokeColor(MINT)
    canvas.setLineWidth(2)
    canvas.line(25 * mm, line_y, width - 22 * mm, line_y)
    canvas.setFillColor(MINT)
    canvas.circle(22 * mm, line_y, 3.2 * mm, fill=1, stroke=0)
    canvas.setFont(FONT, 8.5)
    canvas.setFillColor(MUTED)
    canvas.drawString(28 * mm, height - 25 * mm, "云平台故障注入工具 - 数据库部分周汇报")
    canvas.drawRightString(width - 22 * mm, 13 * mm, f"{doc.page}")
    canvas.restoreState()


def build_story() -> List[Any]:
    summary = db_summary()
    story: List[Any] = []

    story.append(Spacer(1, 18 * mm))
    story.append(p("本周工作汇报：故障注入数据持久化", "title"))
    story.append(p("数据库部分 - SQLite 表结构、历史记录模型与数据回收字段设计", "subtitle"))
    story.append(p("一、本周完成目标", "h1"))
    story += bullets([
        "完成数据库部分的核心代码，确定使用 SQLite 保存故障注入实验历史。",
        "完成 fault_runs 与 fault_results 两张表的设计，用一主多从结构表达“一次实验”和“多条命令结果”。",
        "明确数据回收字段：stdout、stderr、exit_code、elapsed、cmd、node、host、ok 等都可以进入数据库。",
        "当前进度是数据库模块已完成，但还没有正式合并进后端执行路由和前端页面。",
    ])
    story.append(Spacer(1, 6))
    story.append(make_table([
        ["成果项", "说明"],
        ["本周进度", "数据库部分代码完成；尚未合并进前端页面和后端执行流程。"],
        ["数据库文件", f"{summary['path']}（开发验证路径）"],
        ["核心模块", "web_controller/db.py"],
        ["后续合并位置", "web_controller/app.py 的执行接口、前端历史记录/实验详情页面。"],
    ], [42 * mm, 116 * mm]))

    story.append(PageBreak())
    story.append(p("二、实现内容与待合并位置", "h1"))
    story.append(p(
        "数据库部分采用独立模块实现，避免把 SQL 逻辑散落在接口代码中。当前完成的是数据库层代码和接入方案，"
        "还没有正式合并到 FastAPI 执行路由和前端页面。后续合并时，后端只需要把结构化结果交给数据库模块即可。",
    ))
    story.append(make_table([
        ["模块/位置", "当前状态"],
        ["web_controller/db.py", "已完成：SQLite 初始化、运行记录写入、结果写入、历史列表查询、单次详情查询。"],
        ["/api/action", "待合并：单次故障注入执行完成后，保存 action 类型运行记录和命令结果。"],
        ["/api/functest", "待合并：功能测试完成后，按 baseline、action、verify 三个阶段保存数据。"],
        ["/api/functest/cleanup", "待合并：清理恢复完成后，按 cleanup 阶段保存恢复结果。"],
        ["前端历史页", "待合并：展示历史实验列表，点击后查看一次实验下的全部命令结果。"],
        ["前端详情页", "待合并：展示 stdout、stderr、cmd、退出码、耗时、节点和成功状态。"],
    ], [44 * mm, 114 * mm]))
    story.append(Spacer(1, 8))
    story.append(p("三、数据回收闭环", "h1"))
    story.append(p(
        "后端执行故障注入时已经会产生命令输出、退出码、耗时和节点信息。本周数据库部分的工作，是先把这些字段的存储结构准备好。"
        "等后续合并到前后端后，一次实验就不再只存在于当前页面状态中，而是可以长期查询、统计和复现实验过程。",
    ))
    story.append(make_table([
        ["阶段", "示例数据"],
        ["baseline", "注入前 jps、ping、HDFS/YARN 状态、CloudStack 服务状态、CPU/磁盘基准。"],
        ["action", "注入器 stdout/stderr、cmd、exit_code、elapsed、node/host、ok。"],
        ["verify", "注入后 ping、tc/iptables、free、df/du、cgroup、业务状态和日志。"],
        ["cleanup", "resume、clear、cpu-online、api-delay-clear 等恢复动作结果。"],
    ], [34 * mm, 124 * mm]))

    story.append(PageBreak())
    story.append(p("四、数据库表设计", "h1"))
    story.append(p("数据库采用一主一从结构：fault_runs 表保存一次实验，fault_results 表保存该实验下的多条命令结果。"))
    story.append(p("1. fault_runs - 一次故障注入/测试运行记录", "h2"))
    story.append(make_table([
        ["字段", "说明"],
        ["id", "主键，自增。"],
        ["run_type", "运行类型：action、functest、cleanup。"],
        ["action_key", "动作名称，如 vm_network、kvm_perf_delay。"],
        ["scenario_key", "测试场景名称，如 test_vm_network。"],
        ["title", "中文标题，便于前端和汇报展示。"],
        ["params_json", "执行参数，JSON 字符串。"],
        ["ok", "本次运行是否成功，0/1。"],
        ["started_at / finished_at", "开始与结束时间。"],
        ["created_at", "数据库创建时间。"],
    ], [42 * mm, 116 * mm]))
    story.append(Spacer(1, 8))
    story.append(p("2. fault_results - 每条命令的回收结果", "h2"))
    story.append(make_table([
        ["字段", "说明"],
        ["run_id", "关联 fault_runs.id，一次实验对应多条结果。"],
        ["phase", "阶段：baseline、action、verify、cleanup、auto_test。"],
        ["check_title", "检查项标题，如“注入前 ping 测试”。"],
        ["node / host", "执行节点与主机地址。"],
        ["cmd", "实际执行命令。"],
        ["stdout / stderr", "注入工具输出日志和错误信息。"],
        ["exit_code / elapsed", "退出码和执行耗时。"],
        ["ok / truncated", "是否成功、输出是否截断。"],
        ["stdout_meta_json / stderr_meta_json", "输出元数据，包含总字符数、总行数等。"],
    ], [42 * mm, 116 * mm]))

    return story


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    doc = SimpleDocTemplate(
        str(OUT_PATH),
        pagesize=A4,
        leftMargin=22 * mm,
        rightMargin=22 * mm,
        topMargin=28 * mm,
        bottomMargin=20 * mm,
        title="数据库部分周汇报",
        author="Codex",
    )
    doc.build(build_story(), onFirstPage=header_footer, onLaterPages=header_footer)
    print(OUT_PATH)


if __name__ == "__main__":
    main()
