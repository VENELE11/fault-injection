from __future__ import annotations

import math
import re
import textwrap
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "output" / "progress_image_ppt"
IMG_DIR = OUT_DIR / "slides_png"
PPTX_PATH = OUT_DIR / "项目进展汇报_图片版.pptx"

W, H = 1920, 1080
MINT = (18, 204, 185)
MINT_DARK = (8, 142, 132)
MINT_LIGHT = (226, 252, 249)
INK = (23, 40, 52)
MUTED = (94, 111, 124)
LINE = (220, 235, 236)
WHITE = (255, 255, 255)
SOFT = (246, 251, 250)
AMBER = (255, 181, 71)
BLUE = (84, 130, 255)
VIOLET = (139, 112, 246)


FONT_CANDIDATES = [
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/System/Library/Fonts/HelveticaNeue.ttc",
]


def pick_font() -> str:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return path
    raise RuntimeError("No usable font found")


FONT_PATH = pick_font()


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    # Hiragino/STHeiti TTCs render Chinese reliably. Index 0 is enough here.
    return ImageFont.truetype(FONT_PATH, size=size, index=0)


F_TITLE = font(64, True)
F_H1 = font(52, True)
F_H2 = font(40, True)
F_BODY = font(34)
F_BODY_SM = font(29)
F_CAP = font(24)
F_TAG = font(22)
F_NUM = font(76, True)


def text_size(draw: ImageDraw.ImageDraw, text: str, ft: ImageFont.FreeTypeFont) -> tuple[int, int]:
    if not text:
        return 0, 0
    box = draw.textbbox((0, 0), text, font=ft)
    return box[2] - box[0], box[3] - box[1]


def wrap_text(draw: ImageDraw.ImageDraw, text: str, ft: ImageFont.FreeTypeFont, max_width: int) -> list[str]:
    lines: list[str] = []
    for para in text.split("\n"):
        para = para.strip()
        if not para:
            lines.append("")
            continue
        current = ""
        tokens = re.findall(r"[A-Za-z0-9_./:-]+|\s+|.", para)
        for token in tokens:
            if token.isspace():
                token = " "
            test = current + token
            if text_size(draw, test, ft)[0] <= max_width:
                current = test
            else:
                if current:
                    lines.append(current.rstrip())
                if text_size(draw, token, ft)[0] <= max_width:
                    current = token.lstrip()
                else:
                    # Extremely long token fallback.
                    current = ""
                    for ch in token:
                        test_ch = current + ch
                        if text_size(draw, test_ch, ft)[0] <= max_width:
                            current = test_ch
                        else:
                            if current:
                                lines.append(current)
                            current = ch
        if current:
            lines.append(current.rstrip())
    return lines


def draw_wrapped(
    draw: ImageDraw.ImageDraw,
    xy: tuple[int, int],
    text: str,
    ft: ImageFont.FreeTypeFont,
    max_width: int,
    fill: tuple[int, int, int] = INK,
    line_gap: int = 12,
    max_lines: int | None = None,
) -> int:
    x, y = xy
    lines = wrap_text(draw, text, ft, max_width)
    if max_lines is not None and len(lines) > max_lines:
        lines = lines[:max_lines]
        if lines:
            lines[-1] = lines[-1].rstrip("，。；、") + "..."
    for line in lines:
        draw.text((x, y), line, font=ft, fill=fill)
        y += ft.size + line_gap
    return y


def rounded_rect(
    draw: ImageDraw.ImageDraw,
    box: tuple[int, int, int, int],
    radius: int = 28,
    fill: tuple[int, int, int] = WHITE,
    outline: tuple[int, int, int] | None = None,
    width: int = 2,
) -> None:
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def draw_header(draw: ImageDraw.ImageDraw, eyebrow: str, title: str, slide_no: int) -> None:
    draw.rounded_rectangle((96, 58, 96 + 24, 82), radius=12, fill=MINT)
    draw.text((132, 46), eyebrow, font=F_TAG, fill=MINT_DARK)
    draw.text((96, 96), title, font=F_H1, fill=INK)
    draw.text((1712, 58), f"{slide_no:02d}", font=F_TAG, fill=(130, 148, 156))
    draw.line((96, 174, 1824, 174), fill=LINE, width=2)


def add_corner_marks(draw: ImageDraw.ImageDraw) -> None:
    draw.arc((1670, -120, 2100, 310), 90, 190, fill=MINT_LIGHT, width=42)
    draw.arc((-180, 820, 260, 1260), 270, 20, fill=MINT_LIGHT, width=42)


def make_canvas() -> tuple[Image.Image, ImageDraw.ImageDraw]:
    img = Image.new("RGB", (W, H), WHITE)
    draw = ImageDraw.Draw(img)
    add_corner_marks(draw)
    return img, draw


def bullet(draw: ImageDraw.ImageDraw, x: int, y: int, text: str, width: int, accent=MINT) -> int:
    draw.rounded_rectangle((x, y + 10, x + 18, y + 28), radius=9, fill=accent)
    return draw_wrapped(draw, (x + 38, y), text, F_BODY_SM, width - 38, INK, line_gap=9)


def slide_cover(idx: int, title: str, subtitle: str, chips: list[str]) -> Image.Image:
    img, draw = make_canvas()
    draw.rounded_rectangle((96, 92, 318, 128), radius=18, fill=MINT_LIGHT)
    draw.text((124, 98), "PROJECT UPDATE", font=F_TAG, fill=MINT_DARK)
    draw.text((96, 230), title, font=F_TITLE, fill=INK)
    draw_wrapped(draw, (102, 330), subtitle, F_BODY, 1180, MUTED, line_gap=14)
    x = 102
    for chip in chips:
        tw, th = text_size(draw, chip, F_TAG)
        draw.rounded_rectangle((x, 476, x + tw + 46, 526), radius=25, fill=SOFT, outline=LINE, width=2)
        draw.text((x + 23, 489), chip, font=F_TAG, fill=INK)
        x += tw + 66

    # A non-parallel architecture motif.
    center = (1350, 570)
    nodes = [
        ("Web", 1120, 360, BLUE),
        ("FastAPI", 1425, 330, MINT),
        ("SSH", 1630, 560, AMBER),
        ("VM/KVM", 1380, 765, VIOLET),
        ("Logs", 1060, 680, MINT_DARK),
    ]
    for _, x0, y0, color in nodes:
        draw.line((center[0], center[1], x0 + 74, y0 + 74), fill=(207, 224, 226), width=7)
    draw.ellipse((center[0] - 82, center[1] - 82, center[0] + 82, center[1] + 82), fill=MINT, outline=None)
    draw.text((center[0] - 48, center[1] - 17), "控制面", font=F_TAG, fill=WHITE)
    for label, x0, y0, color in nodes:
        draw.rounded_rectangle((x0, y0, x0 + 148, y0 + 148), radius=36, fill=WHITE, outline=(210, 229, 230), width=3)
        draw.ellipse((x0 + 46, y0 + 28, x0 + 102, y0 + 84), fill=color)
        tw, _ = text_size(draw, label, F_TAG)
        draw.text((x0 + (148 - tw) / 2, y0 + 98), label, font=F_TAG, fill=INK)
    draw.text((96, 970), "16:9 图片页 · 白底 · 薄荷色主题 RGB(18,204,185)", font=F_CAP, fill=MUTED)
    return img


def slide_statement(idx: int, eyebrow: str, title: str, body: str, callout: str, pattern: str = "orbit") -> Image.Image:
    img, draw = make_canvas()
    draw_header(draw, eyebrow, title, idx)
    if pattern == "orbit":
        draw.ellipse((1120, 260, 1740, 880), outline=MINT_LIGHT, width=36)
        draw.ellipse((1265, 405, 1595, 735), outline=(214, 244, 241), width=22)
        draw.ellipse((1388, 528, 1472, 612), fill=MINT)
        for angle, color in [(18, BLUE), (118, AMBER), (222, VIOLET), (310, MINT_DARK)]:
            rad = math.radians(angle)
            cx = 1430 + int(math.cos(rad) * 300)
            cy = 570 + int(math.sin(rad) * 300)
            draw.ellipse((cx - 38, cy - 38, cx + 38, cy + 38), fill=color)
    else:
        for i in range(8):
            x = 1120 + i * 76
            y = 315 + (i % 3) * 110
            draw.rounded_rectangle((x, y, x + 280, y + 78), radius=28, fill=SOFT, outline=LINE, width=2)

    draw_wrapped(draw, (104, 274), body, F_BODY, 880, INK, line_gap=18)
    rounded_rect(draw, (104, 750, 960, 914), radius=30, fill=MINT_LIGHT, outline=(183, 238, 232), width=2)
    draw.text((144, 784), "汇报关键词", font=F_TAG, fill=MINT_DARK)
    draw_wrapped(draw, (144, 826), callout, F_BODY_SM, 760, INK, line_gap=10)
    return img


def slide_workpoint(idx: int, section: str, title: str, body: str, keywords: list[str], motif: str) -> Image.Image:
    img, draw = make_canvas()
    draw_header(draw, section, title, idx)

    if motif == "steps":
        positions = [(1130, 290), (1340, 470), (1170, 680), (1515, 710)]
        for i, (x, y) in enumerate(positions):
            draw.rounded_rectangle((x, y, x + 230, y + 118), radius=38, fill=WHITE, outline=LINE, width=3)
            draw.ellipse((x + 24, y + 29, x + 84, y + 89), fill=[MINT, BLUE, AMBER, VIOLET][i])
            draw.text((x + 104, y + 42), f"{i + 1}", font=F_H2, fill=INK)
            if i < len(positions) - 1:
                nx, ny = positions[i + 1]
                draw.line((x + 230, y + 59, nx, ny + 59), fill=(204, 224, 226), width=6)
    elif motif == "timeline":
        draw.line((1110, 330, 1710, 750), fill=(209, 228, 230), width=10)
        for i, (x, y, c) in enumerate([(1110, 330, MINT), (1290, 458, BLUE), (1500, 604, AMBER), (1710, 750, VIOLET)]):
            draw.ellipse((x - 44, y - 44, x + 44, y + 44), fill=c)
            draw.text((x - 11, y - 20), str(i + 1), font=F_TAG, fill=WHITE)
    elif motif == "cards":
        for i in range(5):
            x = 1110 + (i % 2) * 315
            y = 280 + i * 92
            draw.rounded_rectangle((x, y, x + 380, y + 122), radius=28, fill=SOFT if i % 2 else WHITE, outline=LINE, width=2)
            draw.rectangle((x, y, x + 11, y + 122), fill=[MINT, BLUE, AMBER, VIOLET, MINT_DARK][i])
    else:
        for i, r in enumerate([310, 250, 190, 130]):
            draw.ellipse((1410 - r, 565 - r, 1410 + r, 565 + r), outline=[MINT_LIGHT, LINE, MINT_LIGHT, LINE][i], width=18)
        draw.rounded_rectangle((1312, 468, 1508, 662), radius=48, fill=MINT)
        draw.text((1357, 535), "DATA", font=F_TAG, fill=WHITE)

    draw_wrapped(draw, (104, 276), body, F_BODY, 900, INK, line_gap=18)
    y = 710
    for kw in keywords:
        tw, _ = text_size(draw, kw, F_TAG)
        draw.rounded_rectangle((104, y, 104 + tw + 42, y + 48), radius=24, fill=MINT_LIGHT, outline=(194, 240, 236), width=2)
        draw.text((125, y + 12), kw, font=F_TAG, fill=MINT_DARK)
        y += 62
    return img


def slide_tool(idx: int, tool: str, role: str, recovery: list[str], cleanup: list[str], accent=MINT) -> Image.Image:
    img, draw = make_canvas()
    draw_header(draw, "注入工具数据回收", tool, idx)

    draw.rounded_rectangle((104, 236, 1780, 328), radius=36, fill=SOFT, outline=LINE, width=2)
    draw.ellipse((142, 260, 190, 308), fill=accent)
    draw_wrapped(draw, (218, 255), role, F_BODY_SM, 1460, MUTED, line_gap=8, max_lines=2)

    flow = [("回收", "stdout / stderr"), ("标识", "exit_code / elapsed"), ("归档", "node / host / cmd")]
    for i, (a, b) in enumerate(flow):
        x = 260 + i * 485
        y = 382 + (i % 2) * 36
        draw.rounded_rectangle((x, y, x + 310, y + 116), radius=32, fill=WHITE, outline=(204, 228, 228), width=3)
        draw.ellipse((x + 28, y + 30, x + 82, y + 84), fill=[MINT, BLUE, AMBER][i])
        draw.text((x + 104, y + 26), a, font=F_BODY_SM, fill=INK)
        draw.text((x + 104, y + 70), b, font=F_TAG, fill=MUTED)
        if i < 2:
            draw.line((x + 310, y + 58, x + 485, y + 58), fill=(203, 225, 226), width=6)

    left = (104, 590, 910, 930)
    right = (1000, 590, 1804, 930)
    rounded_rect(draw, left, radius=38, fill=MINT_LIGHT, outline=(189, 241, 236), width=2)
    rounded_rect(draw, right, radius=38, fill=WHITE, outline=LINE, width=2)

    draw.text((150, 625), "重点回收数据", font=F_H2, fill=INK)
    y = 695
    for item in recovery:
        y = bullet(draw, 150, y, item, 690, accent=accent) + 16

    draw.text((1046, 625), "清理 / 恢复闭环", font=F_H2, fill=INK)
    y = 695
    for item in cleanup:
        y = bullet(draw, 1046, y, item, 680, accent=MINT_DARK) + 16
    return img


def slide_closing(idx: int) -> Image.Image:
    img, draw = make_canvas()
    draw_header(draw, "下一步", "收尾工作与汇报落点", idx)
    items = [
        ("补齐一键恢复接口", "前端已有按钮，后端可继续实现 /api/recover/all，串联常用清理动作。"),
        ("接上预留场景", "test_hdfs_disk_clear 与 kvm_recover 已在场景中规划，可作为下一步增强。"),
        ("强化指标统计", "把日志对比进一步转为恢复时延、丢包率、CPU/内存变化等图表指标。"),
    ]
    coords = [(140, 280), (685, 420), (1230, 300)]
    colors = [MINT, BLUE, AMBER]
    for i, ((title, body), (x, y)) in enumerate(zip(items, coords)):
        draw.rounded_rectangle((x, y, x + 490, y + 380), radius=44, fill=WHITE, outline=LINE, width=3)
        draw.ellipse((x + 40, y + 42, x + 116, y + 118), fill=colors[i])
        draw.text((x + 148, y + 54), f"{i + 1}", font=F_H2, fill=INK)
        draw.text((x + 42, y + 150), title, font=F_H2, fill=INK)
        draw_wrapped(draw, (x + 42, y + 220), body, F_BODY_SM, 400, MUTED, line_gap=10)
    draw.rounded_rectangle((650, 870, 1270, 930), radius=30, fill=MINT)
    draw.text((714, 884), "重点：平台化、可观测、可恢复", font=F_BODY_SM, fill=WHITE)
    return img


def build_slides() -> list[tuple[str, Image.Image]]:
    slides: list[tuple[str, Image.Image]] = []
    add = slides.append

    add(("00_cover", slide_cover(
        1,
        "云平台故障注入控制平台",
        "前后端集成进展与各注入工具的数据回收闭环",
        ["Hadoop", "CloudStack", "VM", "KVM", "FastAPI + Web"],
    )))

    add(("01_overview", slide_statement(
        2,
        "总体进展",
        "统一 Web 故障注入控制平台",
        "我完成了一个面向 Hadoop / CloudStack / VM / KVM 的统一 Web 故障注入控制平台。前端负责参数录入、场景分组、执行历史、前后对比和清理入口；后端 FastAPI 负责读取配置、校验参数、区分本地/SSH 执行、调用注入工具，并把 stdout、stderr、退出码、耗时、节点信息统一回传给前端。",
        "一句话概括：把分散命令整合成可配置、可执行、可观测、可恢复的平台。",
    )))

    add(("02_frontend", slide_workpoint(
        3,
        "前端部分",
        "统一入口与可视化回收",
        "前端完成了中文控制台、场景分组、参数表单、执行/清理按钮和运行历史。页面从 /api/config 和 /api/testcases 动态拉取配置，执行后展示操作前基线、动作输出、操作后验证，并对 ping、CPU、内存、磁盘、cgroup、HDFS/YARN 等结果做重点判读。",
        ["场景分组", "动态配置", "前后对比", "结果判读"],
        "cards",
    )))

    add(("03_backend", slide_workpoint(
        4,
        "后端部分",
        "FastAPI 调度与统一结果结构",
        "后端建立统一 Action Registry，把各类注入工具封装成相同执行模型：参数定义、所属分组、执行范围、本地/远程、是否 sudo。功能测试按 baseline -> action -> verify -> cleanup 执行，并统一回传 ok、node、host、cmd、exit_code、stdout、stderr、elapsed 等字段。",
        ["Action Registry", "本地 / SSH", "参数校验", "结构化回传"],
        "timeline",
    )))

    add(("04_recovery_platform", slide_tool(
        5,
        "Hadoop / CloudStack",
        "场景化注入层，负责把网络、资源、进程异常映射到 Hadoop 与 CloudStack 的业务语义中。",
        ["Hadoop：jps、HDFS/YARN、任务日志、网络规则、负载、内存、磁盘、cgroup", "CloudStack：组件 PID/状态、API 延迟、网络隔离标记、stdout/stderr"],
        ["Hadoop：resume/重启；网络 clear；资源与 HDFS/YARN 清理", "CloudStack：resume；api-delay-clear；network-clear；db-restore"],
        MINT,
    )))

    add(("05_recovery_vm_kvm", slide_tool(
        6,
        "VM / KVM 注入工具",
        "底层与用户态注入层，覆盖进程、网络、CPU、内存、寄存器、QEMU/KVM 性能故障和 CPU 热插拔。",
        ["VM：ps/pgrep、ping、tc、iptables、loadavg/free、内存与寄存器注入结果", "KVM：QEMU 进程、虚拟机状态、CPU/dd 基准、CPU online 列表"],
        ["VM：SIGCONT；network clear；CPU/内存按进程结束回收", "KVM：perf-clear、cpu-online、clear 恢复 cgroup/cpulimit 与 CPU"],
        BLUE,
    )))

    return slides


def save_images(slides: list[tuple[str, Image.Image]]) -> list[Path]:
    IMG_DIR.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    for i, (name, img) in enumerate(slides, start=1):
        path = IMG_DIR / f"{i:02d}_{name}.png"
        img.save(path, "PNG", optimize=True)
        paths.append(path)
    return paths


def write_xml(path: str, data: str, zf: zipfile.ZipFile) -> None:
    zf.writestr(path, data.encode("utf-8"))


def make_pptx(images: list[Path]) -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if PPTX_PATH.exists():
        PPTX_PATH.unlink()

    slide_w = 12192000
    slide_h = 6858000
    with zipfile.ZipFile(PPTX_PATH, "w", zipfile.ZIP_DEFLATED) as zf:
        content_types = [
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
            '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">',
            '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>',
            '<Default Extension="xml" ContentType="application/xml"/>',
            '<Default Extension="png" ContentType="image/png"/>',
            '<Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>',
            '<Override PartName="/ppt/slideMasters/slideMaster1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml"/>',
            '<Override PartName="/ppt/slideLayouts/slideLayout1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"/>',
            '<Override PartName="/ppt/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>',
            '<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>',
            '<Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>',
        ]
        for i in range(1, len(images) + 1):
            content_types.append(
                f'<Override PartName="/ppt/slides/slide{i}.xml" '
                'ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>'
            )
        content_types.append("</Types>")
        write_xml("[Content_Types].xml", "\n".join(content_types), zf)

        write_xml(
            "_rels/.rels",
            """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>""",
            zf,
        )

        sld_ids = "\n".join(
            f'      <p:sldId id="{255 + i}" r:id="rId{i}"/>' for i in range(1, len(images) + 1)
        )
        write_xml(
            "ppt/presentation.xml",
            f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:presentation xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
  xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
  xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:sldMasterIdLst>
    <p:sldMasterId id="2147483648" r:id="rId{len(images)+1}"/>
  </p:sldMasterIdLst>
  <p:sldIdLst>
{sld_ids}
  </p:sldIdLst>
  <p:sldSz cx="{slide_w}" cy="{slide_h}" type="wide"/>
  <p:notesSz cx="6858000" cy="9144000"/>
</p:presentation>""",
            zf,
        )

        rels = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
                '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">']
        for i in range(1, len(images) + 1):
            rels.append(
                f'  <Relationship Id="rId{i}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide{i}.xml"/>'
            )
        rels.append(
            f'  <Relationship Id="rId{len(images)+1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="slideMasters/slideMaster1.xml"/>'
        )
        rels.append("</Relationships>")
        write_xml("ppt/_rels/presentation.xml.rels", "\n".join(rels), zf)

        write_xml(
            "ppt/slideMasters/slideMaster1.xml",
            """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldMaster xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
  xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
  xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr></p:spTree></p:cSld>
  <p:sldLayoutIdLst><p:sldLayoutId id="2147483649" r:id="rId1"/></p:sldLayoutIdLst>
</p:sldMaster>""",
            zf,
        )
        write_xml(
            "ppt/slideMasters/_rels/slideMaster1.xml.rels",
            """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="../theme/theme1.xml"/>
</Relationships>""",
            zf,
        )
        write_xml(
            "ppt/slideLayouts/slideLayout1.xml",
            """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldLayout xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
  xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
  xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" type="blank" preserve="1">
  <p:cSld name="Blank"><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr></p:spTree></p:cSld>
</p:sldLayout>""",
            zf,
        )
        write_xml(
            "ppt/slideLayouts/_rels/slideLayout1.xml.rels",
            """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="../slideMasters/slideMaster1.xml"/>
</Relationships>""",
            zf,
        )
        write_xml(
            "ppt/theme/theme1.xml",
            """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Mint">
  <a:themeElements>
    <a:clrScheme name="Mint"><a:dk1><a:srgbClr val="172834"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1><a:dk2><a:srgbClr val="0D8E84"/></a:dk2><a:lt2><a:srgbClr val="E2FCF9"/></a:lt2><a:accent1><a:srgbClr val="12CCB9"/></a:accent1><a:accent2><a:srgbClr val="5482FF"/></a:accent2><a:accent3><a:srgbClr val="FFB547"/></a:accent3><a:accent4><a:srgbClr val="8B70F6"/></a:accent4><a:accent5><a:srgbClr val="6E7F8B"/></a:accent5><a:accent6><a:srgbClr val="172834"/></a:accent6><a:hlink><a:srgbClr val="12CCB9"/></a:hlink><a:folHlink><a:srgbClr val="0D8E84"/></a:folHlink></a:clrScheme>
    <a:fontScheme name="MintFonts"><a:majorFont><a:latin typeface="Aptos Display"/></a:majorFont><a:minorFont><a:latin typeface="Aptos"/></a:minorFont></a:fontScheme>
    <a:fmtScheme name="MintFmt"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w="9525"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln></a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme>
  </a:themeElements>
</a:theme>""",
            zf,
        )

        for i, img_path in enumerate(images, start=1):
            zf.write(img_path, f"ppt/media/image{i}.png")
            slide_xml = f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
  xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
  xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld>
    <p:spTree>
      <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr>
      <p:pic>
        <p:nvPicPr><p:cNvPr id="2" name="{escape(img_path.name)}"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
        <p:blipFill><a:blip r:embed="rId1"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>
        <p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="{slide_w}" cy="{slide_h}"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr>
      </p:pic>
    </p:spTree>
  </p:cSld>
</p:sld>"""
            write_xml(f"ppt/slides/slide{i}.xml", slide_xml, zf)
            write_xml(
                f"ppt/slides/_rels/slide{i}.xml.rels",
                f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/image{i}.png"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/>
</Relationships>""",
                zf,
            )

        write_xml(
            "docProps/app.xml",
            f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"
  xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes">
  <Application>Codex image deck generator</Application>
  <PresentationFormat>On-screen Show (16:9)</PresentationFormat>
  <Slides>{len(images)}</Slides>
</Properties>""",
            zf,
        )
        write_xml(
            "docProps/core.xml",
            """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
  xmlns:dc="http://purl.org/dc/elements/1.1/"
  xmlns:dcterms="http://purl.org/dc/terms/"
  xmlns:dcmitype="http://purl.org/dc/dcmitype/"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <dc:title>项目进展汇报 图片版</dc:title>
  <dc:creator>Codex</dc:creator>
  <cp:lastModifiedBy>Codex</cp:lastModifiedBy>
</cp:coreProperties>""",
            zf,
        )


def main() -> None:
    slides = build_slides()
    paths = save_images(slides)
    make_pptx(paths)
    print(f"slides={len(paths)}")
    print(f"pptx={PPTX_PATH}")
    print(f"images={IMG_DIR}")


if __name__ == "__main__":
    main()
