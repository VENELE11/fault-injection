from __future__ import annotations

import math
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


OUTDIR = Path("/Users/venele/Downloads/fault-injection/output/doc/assets/unified")
FONT_PATH = "/System/Library/Fonts/Hiragino Sans GB.ttc"

COL = {
    "blue": "#dbe8f5",
    "green": "#dfeee1",
    "beige": "#f7ecd8",
    "lav": "#e8dff2",
    "ice": "#e6f1f7",
    "line": "#4a4f55",
    "text": "#2b2f33",
    "sub": "#59616a",
    "bg": "#ffffff",
}

F_TITLE = ImageFont.truetype(FONT_PATH, 30)
F_HEAD = ImageFont.truetype(FONT_PATH, 24)
F_BODY = ImageFont.truetype(FONT_PATH, 20)
F_SMALL = ImageFont.truetype(FONT_PATH, 17)


def canvas(w: int, h: int):
    img = Image.new("RGB", (w, h), COL["bg"])
    return img, ImageDraw.Draw(img)


def rr(draw: ImageDraw.ImageDraw, box, fill, outline=None, width=2, r=18):
    draw.rounded_rectangle(
        box,
        radius=r,
        fill=fill,
        outline=outline or COL["line"],
        width=width,
    )


def center_text(draw: ImageDraw.ImageDraw, xy, text, font, fill=None, anchor="mm"):
    draw.text(xy, text, font=font, fill=fill or COL["text"], anchor=anchor)


def wrap_text(draw: ImageDraw.ImageDraw, text: str, font, max_width: int) -> list[str]:
    if "\n" in text:
        lines: list[str] = []
        for seg in text.split("\n"):
            lines.extend(wrap_text(draw, seg, font, max_width))
        return lines
    chars = list(text)
    line = ""
    lines: list[str] = []
    for ch in chars:
        test = line + ch
        bbox = draw.textbbox((0, 0), test, font=font)
        if bbox[2] - bbox[0] <= max_width or not line:
            line = test
        else:
            lines.append(line)
            line = ch
    if line:
        lines.append(line)
    return lines


def fit_lines(
    draw: ImageDraw.ImageDraw,
    text: str,
    preferred_font,
    fallback_fonts: Iterable,
    max_width: int,
    max_lines: int | None = None,
):
    fonts = [preferred_font, *fallback_fonts]
    for font in fonts:
        lines = wrap_text(draw, text, font, max_width)
        if max_lines is None or len(lines) <= max_lines:
            return lines, font
    font = fonts[-1]
    lines = wrap_text(draw, text, font, max_width)
    return lines, font


def label_box(
    draw: ImageDraw.ImageDraw,
    box,
    title: str,
    desc: str | None = None,
    fill="white",
    outline=None,
):
    rr(draw, box, fill, outline)
    x1, y1, x2, y2 = box
    width = x2 - x1
    title_lines, title_font = fit_lines(
        draw, title, F_HEAD, [F_BODY, F_SMALL], width - 40, max_lines=2
    )
    current_y = y1 + 22
    for idx, line in enumerate(title_lines):
        center_text(
            draw,
            ((x1 + x2) // 2, current_y),
            line,
            title_font,
            COL["text"],
            anchor="ma",
        )
        current_y += 28 if idx == 0 else 24
    if desc:
        desc_lines, desc_font = fit_lines(
            draw, desc, F_BODY, [F_SMALL], width - 34, max_lines=4
        )
        current_y += 6
        for line in desc_lines:
            center_text(
                draw,
                ((x1 + x2) // 2, current_y),
                line,
                desc_font,
                COL["sub"],
                anchor="ma",
            )
            current_y += 24


def arrow(draw: ImageDraw.ImageDraw, p1, p2, width=3, fill=None):
    fill = fill or COL["line"]
    x1, y1 = p1
    x2, y2 = p2
    draw.line((x1, y1, x2, y2), fill=fill, width=width)
    ang = math.atan2(y2 - y1, x2 - x1)
    length = 16
    a1 = ang + math.pi * 0.82
    a2 = ang - math.pi * 0.82
    p3 = (x2 + length * math.cos(a1), y2 + length * math.sin(a1))
    p4 = (x2 + length * math.cos(a2), y2 + length * math.sin(a2))
    draw.polygon([p2, p3, p4], fill=fill)


def fig2_1():
    img, draw = canvas(1600, 980)
    boxes = [
        (220, 80, 1380, 180, COL["beige"], "SaaS 层", "面向最终用户的软件服务与业务应用"),
        (
            220,
            230,
            1380,
            330,
            COL["lav"],
            "PaaS 层",
            "Hadoop / Spark 等分布式计算框架与平台服务",
        ),
        (
            220,
            380,
            1380,
            480,
            COL["blue"],
            "IaaS / 管理栈层",
            "CloudStack 等资源调度、实例管理与控制面服务",
        ),
        (
            220,
            530,
            1380,
            630,
            COL["green"],
            "虚拟化层",
            "KVM / Xen 提供计算、存储与网络资源抽象",
        ),
        (
            220,
            680,
            1380,
            780,
            COL["ice"],
            "物理资源层",
            "ARM64 服务器、CPU、内存、磁盘与网络设备",
        ),
    ]
    for x1, y1, x2, y2, fill, title, desc in boxes:
        rr(draw, (x1, y1, x2, y2), fill)
        center_text(draw, (360, (y1 + y2) // 2), title, F_TITLE)
        center_text(draw, (930, (y1 + y2) // 2), desc, F_BODY)
    for y in [180, 330, 480, 630]:
        arrow(draw, (800, y + 10), (800, y + 36), 4)
    center_text(
        draw,
        (800, 900),
        "故障注入测试主要围绕底层虚拟化层、Guest OS 运行层和上层业务/控制面展开",
        F_BODY,
        "#7a3340",
    )
    img.save(OUTDIR / "fig2_1.png")


def fig2_2():
    img, draw = canvas(1700, 1180)
    rr(draw, (560, 40, 1140, 130), COL["blue"])
    center_text(draw, (850, 82), "前端交互层", F_TITLE)
    center_text(draw, (850, 112), "Web 控制台 / 参数配置 / 结果查看", F_BODY, COL["sub"])

    rr(draw, (180, 200, 1520, 480), COL["beige"])
    draw.text((220, 240), "后端控制与数据处理层", font=F_TITLE, fill=COL["text"])
    label_box(draw, (250, 280, 550, 390), "API 服务", "接收实验请求\n统一接口封装")
    label_box(draw, (600, 280, 900, 390), "任务调度器", "生成任务对象\n控制执行顺序")
    label_box(draw, (950, 280, 1250, 390), "SSH 调度器", "无代理远程调用\n下发命令并回收状态")
    label_box(draw, (1300, 280, 1460, 390), "结果管理", "归档实验记录\n输出结果摘要")

    rr(draw, (260, 560, 720, 840), COL["green"])
    draw.text((300, 598), "故障注入执行层", font=F_TITLE, fill=COL["text"])
    label_box(draw, (310, 640, 670, 720), "宿主机注入器", "Kprobes / KVM / VFS / 内存管理链路")
    label_box(draw, (310, 740, 670, 820), "Guest OS 注入器", "CPU / 内存 / 网络 / 进程干预")

    rr(draw, (930, 560, 1450, 840), COL["lav"])
    draw.text((970, 598), "目标物理机与虚拟资源层", font=F_TITLE, fill=COL["text"])
    label_box(draw, (980, 640, 1400, 720), "Ubuntu 宿主机", "KVM Hypervisor / QEMU-KVM")
    label_box(draw, (980, 740, 1400, 820), "Alpine 虚拟机集群", "控制节点 / 工作节点 / 业务组件")

    rr(draw, (500, 930, 1200, 1080), COL["ice"])
    center_text(draw, (700, 968), "结果回收与状态存储层", F_HEAD)
    center_text(draw, (850, 1015), "日志索引、时间线、状态监控结果与实验归档", F_BODY, COL["sub"])

    arrow(draw, (850, 130), (850, 200), 4)
    for x in [400, 750, 1100, 1380]:
        arrow(draw, (x, 480), (x, 560), 4)
    arrow(draw, (720, 680), (930, 680), 4)
    arrow(draw, (720, 780), (930, 780), 4)
    arrow(draw, (500, 840), (720, 930), 4)
    arrow(draw, (1200, 840), (980, 930), 4)
    img.save(OUTDIR / "fig2_2.png")


def fig2_3():
    img, draw = canvas(1200, 1080)
    rr(draw, (435, 40, 765, 170), COL["ice"])
    center_text(draw, (600, 82), "虚拟网桥与 NAT 私有子网", F_HEAD)
    center_text(draw, (600, 118), "隔离实验通信与外部环境", F_SMALL, COL["sub"])

    rr(draw, (110, 230, 1090, 970), "#fffbe8")
    draw.text((150, 255), "轻量级虚拟机集群", font=F_TITLE, fill=COL["text"])
    label_box(draw, (180, 350, 520, 470), "Master 节点", "NameNode / ResourceManager")
    label_box(draw, (680, 640, 1020, 760), "Slave 1 节点", "被隔离的 DataNode")
    label_box(draw, (680, 830, 1020, 950), "Slave 2 节点", "健康 DataNode")

    diamond = [(600, 500), (790, 650), (600, 800), (410, 650)]
    draw.polygon(diamond, fill="#ffe9eb", outline="#b85b66")
    center_text(draw, (600, 615), "局部网络隔离", F_HEAD, "#9a3544")
    center_text(draw, (600, 650), "iptables 定向阻断", F_BODY, "#9a3544")
    center_text(draw, (600, 684), "关键端口通信", F_BODY, "#9a3544")

    arrow(draw, (520, 410), (680, 690), 4, "#2c8b57")
    arrow(draw, (520, 430), (680, 890), 4, "#2c8b57")
    arrow(draw, (600, 170), (360, 350), 2)
    arrow(draw, (600, 170), (850, 640), 2)
    arrow(draw, (600, 170), (850, 830), 2)
    center_text(draw, (330, 535), "正常通信：主从心跳与状态同步", F_SMALL, "#2c8b57")
    center_text(draw, (845, 550), "网络分区：关键报文被阻断", F_SMALL, "#9a3544")
    center_text(draw, (850, 785), "局部存活：节点内部仍可运行", F_SMALL, "#6a5e2b")
    img.save(OUTDIR / "fig2_3.png")


def fig2_4():
    img, draw = canvas(1080, 1760)
    sections = [
        (
            120,
            70,
            960,
            360,
            COL["blue"],
            "上层：云平台应用与分布式组件",
            [("cloudstack-fi", "CloudStack 控制面故障"), ("hadoop-fi", "Hadoop 分布式业务故障")],
        ),
        (
            120,
            430,
            960,
            910,
            COL["lav"],
            "中层：虚拟机内部操作系统",
            [
                ("cpu_injector", "CPU 资源争抢"),
                ("mem_injector", "内存耗尽 / 内存污染"),
                ("network_injector", "网络延迟 / 丢包 / 隔离"),
                ("process_injector", "进程崩溃 / 假死 / 恢复"),
            ],
        ),
        (
            120,
            980,
            960,
            1460,
            COL["green"],
            "底层：虚拟化链路与宿主机",
            [
                ("cpu-reg-fi", "vCPU 寄存器与执行流"),
                ("pt-load-fi", "缺页异常拦截"),
                ("file-rw-fi", "VFS I/O 链路阻断"),
                ("vm-migration-fi", "状态机与热迁移干预"),
            ],
        ),
    ]
    for x1, y1, x2, y2, fill, title, items in sections:
        rr(draw, (x1, y1, x2, y2), fill)
        center_text(draw, ((x1 + x2) // 2, y1 + 38), title, F_HEAD)
        yy = y1 + 92
        for left, right in items:
            label_box(draw, (170, yy, 430, yy + 74), left, fill="white")
            label_box(draw, (640, yy, 910, yy + 74), right, fill="white")
            arrow(draw, (430, yy + 37), (640, yy + 37), 3)
            yy += 98
    for y in [360, 910]:
        arrow(draw, (540, y + 10), (540, y + 45), 4)
    rr(draw, (300, 1560, 780, 1705), COL["ice"])
    center_text(draw, (540, 1616), "底层计算与硬件资源", F_HEAD)
    center_text(draw, (540, 1654), "ARM64 / CPU / RAM / 虚拟化硬件支持", F_BODY)
    arrow(draw, (540, 1460), (540, 1560), 4)
    img.save(OUTDIR / "fig2_4.png")


def fig3_1():
    img, draw = canvas(1700, 1140)
    rr(draw, (600, 40, 1100, 130), COL["blue"])
    center_text(draw, (850, 80), "用户交互层", F_TITLE)
    center_text(draw, (850, 112), "Web 页面 / CLI / 参数配置", F_BODY, COL["sub"])

    rr(draw, (160, 200, 1540, 440), COL["beige"])
    draw.text((190, 230), "控制与调度层", font=F_TITLE, fill=COL["text"])
    label_box(draw, (230, 290, 520, 400), "任务解析器", "读取故障类型\n目标节点与参数")
    label_box(draw, (570, 290, 860, 400), "故障模型管理器", "统一描述 CPU / 内存 / 网络\n进程 / 控制面故障")
    label_box(draw, (910, 290, 1200, 400), "调度与分发器", "组织执行顺序\n远程下发任务")
    label_box(draw, (1250, 290, 1470, 400), "恢复与清理器", "执行回滚、恢复\n与环境清理")

    rr(draw, (120, 520, 760, 840), COL["green"])
    draw.text((150, 550), "故障注入执行层", font=F_TITLE, fill=COL["text"])
    label_box(draw, (160, 600, 720, 680), "宿主机注入器", "Kprobes / KVM / VFS / 内存管理链路")
    label_box(draw, (160, 700, 720, 780), "Guest OS 注入器", "CPU 争抢 / 内存耗尽 / 网络异常 / 进程干预")
    label_box(draw, (160, 800, 720, 880), "场景适配器", "Hadoop / CloudStack 场景化注入")

    rr(draw, (940, 520, 1580, 840), COL["lav"])
    draw.text((970, 550), "目标系统与业务层", font=F_TITLE, fill=COL["text"])
    label_box(draw, (980, 600, 1540, 680), "宿主机与 KVM 环境", "ARM64 宿主机 / KVM / QEMU-KVM")
    label_box(draw, (980, 700, 1540, 780), "轻量级虚拟机集群", "Alpine 节点 / 控制节点 / 工作节点")
    label_box(draw, (980, 800, 1540, 880), "典型上层业务", "Hadoop / CloudStack")

    rr(draw, (420, 930, 1280, 1080), COL["ice"])
    center_text(draw, (850, 980), "结果与数据层", F_TITLE)
    center_text(draw, (850, 1020), "日志回收、状态监控、实验结果存储与分析", F_BODY, COL["sub"])

    arrow(draw, (850, 130), (850, 200), 4)
    for x in [370, 720, 1100, 1360]:
        arrow(draw, (x, 440), (x, 520), 4)
    for y in [640, 740, 840]:
        arrow(draw, (720, y), (980, y), 4)
    arrow(draw, (370, 840), (760, 930), 4)
    arrow(draw, (1360, 840), (940, 930), 4)
    img.save(OUTDIR / "fig3_1.png")


def fig3_2():
    img, draw = canvas(1200, 1500)
    steps = [
        ("开始", COL["blue"], None),
        ("配置故障模型", COL["beige"], "选择故障类型、目标层次和参数"),
        ("校验任务合法性", COL["beige"], "检查节点、权限、参数边界与依赖条件"),
        ("生成执行任务", COL["green"], "形成统一任务描述并绑定时间戳"),
        ("向目标节点分发命令", COL["green"], "通过 SSH / 脚本调用对应注入器"),
        ("局部执行与状态观测", COL["lav"], "注入故障并持续采集日志、性能与返回值"),
        ("故障结束与环境恢复", COL["lav"], "停止注入、回滚规则、恢复服务与网络"),
        ("汇总实验结果", COL["ice"], "分析故障影响、恢复时间与异常传播路径"),
        ("结束", COL["blue"], None),
    ]
    x1 = 240
    box_w = 720
    y = 50
    bottoms = []
    for title, fill, desc in steps:
        rr(draw, (x1, y, x1 + box_w, y + 96), fill)
        center_text(draw, (600, y + 34), title, F_HEAD)
        if desc:
            center_text(draw, (600, y + 66), desc, F_SMALL, COL["sub"])
        bottoms.append((600, y + 96))
        y += 145
    for idx in range(len(bottoms) - 1):
        arrow(draw, bottoms[idx], (600, bottoms[idx + 1][1] - 96), 4)
    for lx, ly, text in [
        (205, 360, "任务描述统一化"),
        (1000, 650, "执行过程可观测"),
        (205, 940, "保证实验可恢复"),
    ]:
        rr(draw, (lx - 95, ly - 24, lx + 95, ly + 24), "#faf7f2", outline="#b66", width=2, r=14)
        center_text(draw, (lx, ly), text, F_SMALL, "#8a3844")
    img.save(OUTDIR / "fig3_2.png")


def fig4_1():
    img, draw = canvas(1120, 1360)
    label_box(draw, (300, 40, 820, 180), "客户机访问尚未映射的内存页", "触发硬件缺页异常 Page Fault", fill=COL["blue"])
    arrow(draw, (560, 180), (560, 260), 4)

    rr(draw, (80, 260, 1040, 1260), COL["beige"])
    draw.text((120, 300), "宿主机内核异常处理路径", font=F_TITLE, fill=COL["text"])
    label_box(draw, (375, 380, 745, 490), "handle_mm_fault", "内核缺页处理函数")
    arrow(draw, (560, 490), (560, 585), 4)

    label_box(draw, (125, 620, 475, 905), "注入分支", "Kretprobes 命中返回路径\n篡改故障码\n伪造 VM_FAULT_OOM")
    label_box(draw, (645, 620, 995, 905), "正常分支", "Stage-2 页表建立成功\n返回正常状态码")
    arrow(draw, (560, 585), (300, 620), 3)
    arrow(draw, (560, 585), (820, 620), 3)

    label_box(draw, (125, 1020, 475, 1215), "异常结果", "客户机内存映射失败\n可能触发崩溃或内核恐慌", fill=COL["lav"])
    label_box(draw, (645, 1020, 995, 1215), "正常结果", "控制权返回\n客户机继续运行", fill=COL["green"])
    arrow(draw, (300, 905), (300, 1020), 4)
    arrow(draw, (820, 905), (820, 1020), 4)
    img.save(OUTDIR / "fig4_1.png")


def fig4_2():
    img, draw = canvas(1120, 860)
    label_box(draw, (120, 40, 1000, 110), "基于 POSIX 信号的进程状态控制", "通过 SIGKILL / SIGSTOP / SIGCONT 控制目标业务进程", fill=COL["blue"])
    label_box(draw, (395, 170, 725, 280), "目标进程", "业务守护进程 / Hadoop 组件 / 服务程序", fill=COL["ice"])

    triples = [
        ((120, 400, 360, 560), "SIGKILL", "Crash\n强制终止进程", COL["beige"], "结果1", "进程立即退出\n系统进入崩溃处理路径"),
        ((440, 400, 680, 560), "SIGSTOP", "Hang\n将进程移出调度队列", COL["lav"], "结果2", "进程仍存活\n但不再响应请求"),
        ((760, 400, 1000, 560), "SIGCONT", "Resume\n恢复挂起进程执行", COL["green"], "结果3", "业务重新进入\n正常调度状态"),
    ]
    for box, title, desc, fill, r_title, r_desc in triples:
        label_box(draw, box, title, desc, fill=fill)
        midx = (box[0] + box[2]) // 2
        arrow(draw, (midx, 560), (midx, 660), 3)
        label_box(draw, (box[0], 660, box[2], 770), r_title, r_desc, fill="white")
    arrow(draw, (560, 280), (240, 400), 3)
    arrow(draw, (560, 280), (560, 400), 3)
    arrow(draw, (560, 280), (880, 400), 3)
    img.save(OUTDIR / "fig4_2.png")


def fig4_3():
    img, draw = canvas(1260, 940)
    label_box(draw, (220, 40, 1040, 120), "CloudStack SystemVM 底层干预机制", "控制节点通过 virsh 直接作用于底层 Hypervisor 管理的系统虚拟机", fill=COL["blue"])
    label_box(draw, (80, 220, 360, 340), "控制节点", "cloudstack-fi / 控制脚本", fill=COL["beige"])
    label_box(draw, (470, 220, 790, 340), "宿主机 Hypervisor", "libvirt / virsh 命令接口", fill=COL["green"])
    label_box(draw, (900, 180, 1180, 300), "SystemVM-A", "正常运行")
    label_box(draw, (900, 360, 1180, 480), "SystemVM-B", "挂起状态")
    label_box(draw, (900, 540, 1180, 660), "SystemVM-C", "销毁 / 异常退出")
    label_box(draw, (500, 460, 760, 660), "执行动作", "virsh resume\nvirsh suspend\nvirsh destroy", fill=COL["lav"])
    label_box(draw, (260, 780, 1000, 890), "CloudStack 控制面响应", "监控链路感知 SystemVM 异常，触发重建、恢复或状态更新逻辑", fill=COL["ice"])
    arrow(draw, (360, 280), (470, 280), 4)
    arrow(draw, (790, 280), (900, 240), 3)
    arrow(draw, (790, 280), (900, 420), 3)
    arrow(draw, (790, 280), (900, 600), 3)
    arrow(draw, (1040, 660), (1040, 780), 4)
    img.save(OUTDIR / "fig4_3.png")


def fig4_4():
    img, draw = canvas(1500, 760)
    cols = [120, 430, 760, 1080, 1380]
    heads = ["测试人员 / 主控脚本", "SSH 隧道", "目标节点", "注入器程序", "目标进程 / 系统内核"]
    for idx, x in enumerate(cols):
        label_box(draw, (x - 110, 30, x + 110, 90), heads[idx], fill=COL["lav"] if idx % 2 else COL["ice"])
        draw.line((x, 90, x, 700), fill="#c9ccd1", width=2)
    steps = [
        ((120, 160), (430, 160), "1. 组装任务与建立连接"),
        ((430, 230), (760, 230), "2. 分发控制指令"),
        ((760, 300), (1080, 300), "3. 调用底层注入工具"),
        ((1080, 380), (1380, 380), "4. 触发系统调用 / 篡改内存 / 挂起进程"),
        ((1380, 470), (1080, 470), "5. 产生异常结果"),
        ((1080, 560), (760, 560), "6. 返回 stdout / stderr / 状态码"),
        ((760, 630), (430, 630), "7. 回传日志与状态"),
        ((430, 690), (120, 690), "8. 汇总结果并记录"),
    ]
    for p1, p2, text in steps:
        arrow(draw, p1, p2, 3)
        center_text(draw, ((p1[0] + p2[0]) // 2, p1[1] - 20), text, F_SMALL)
    img.save(OUTDIR / "fig4_4.png")


def fig5_1():
    img, draw = canvas(1700, 1100)
    rr(draw, (590, 40, 1110, 130), COL["blue"])
    center_text(draw, (850, 80), "前端交互层", F_TITLE)
    center_text(draw, (850, 112), "Web 控制台 / 参数表单 / 结果展示", F_BODY, COL["sub"])

    rr(draw, (190, 210, 1510, 470), COL["beige"])
    draw.text((220, 240), "后端控制中枢", font=F_TITLE, fill=COL["text"])
    label_box(draw, (260, 290, 520, 400), "API 服务层", "接收配置请求\n提供统一 REST 接口")
    label_box(draw, (560, 290, 820, 400), "任务调度器", "生成任务对象\n分配执行顺序与时间")
    label_box(draw, (860, 290, 1120, 400), "SSH 执行器", "无代理远程连接\n分发命令并回收状态")
    label_box(draw, (1160, 290, 1440, 400), "恢复与清理器", "删除规则、恢复网络\n进程与模块状态")

    rr(draw, (150, 560, 780, 840), COL["green"])
    draw.text((180, 590), "日志与监控子系统", font=F_TITLE, fill=COL["text"])
    label_box(draw, (210, 640, 720, 720), "日志回收器", "汇总 stdout、stderr、dmesg 与业务日志")
    label_box(draw, (210, 740, 720, 820), "状态监控器", "采集节点连通性、进程状态与资源指标")

    rr(draw, (910, 560, 1550, 840), COL["lav"])
    draw.text((940, 590), "目标节点与执行环境", font=F_TITLE, fill=COL["text"])
    label_box(draw, (980, 620, 1480, 700), "宿主机节点", "KVM / QEMU / 内核级故障注入模块")
    label_box(draw, (980, 720, 1480, 800), "虚拟机节点", "Guest OS 注入程序 / tc / iptables / ptrace")

    rr(draw, (500, 920, 1200, 1040), COL["ice"])
    center_text(draw, (850, 965), "实验记录与结果存储", F_TITLE)
    center_text(draw, (850, 1015), "任务元数据、日志索引、状态时间线与分析结果", F_BODY, COL["sub"])

    arrow(draw, (850, 130), (850, 210), 4)
    for x in [390, 690, 990, 1300]:
        arrow(draw, (x, 470), (x, 560), 4)
    arrow(draw, (780, 680), (910, 680), 4)
    arrow(draw, (780, 780), (910, 780), 4)
    arrow(draw, (530, 840), (770, 920), 4)
    arrow(draw, (1230, 840), (960, 920), 4)
    img.save(OUTDIR / "fig5_1.png")


def fig5_2():
    img, draw = canvas(1600, 980)
    rr(draw, (70, 160, 520, 820), COL["beige"])
    draw.text((95, 190), "控制节点", font=F_TITLE, fill=COL["text"])
    label_box(draw, (130, 250, 460, 330), "前端页面", "故障参数配置与实验展示")
    label_box(draw, (130, 360, 460, 440), "FastAPI 后端", "接口封装、任务生成与调度")
    label_box(draw, (130, 470, 460, 550), "SSH 连接器", "远程下发命令与接收回执")
    label_box(draw, (130, 580, 460, 660), "结果整理器", "生成实验日志与结果摘要")

    rr(draw, (610, 120, 1510, 360), COL["green"])
    draw.text((640, 150), "宿主机侧执行域", font=F_TITLE, fill=COL["text"])
    label_box(draw, (680, 190, 1070, 290), "宿主机 1", "Kprobes 模块 / KVM 路径 / VFS 注入")
    label_box(draw, (1090, 190, 1440, 290), "宿主机 2", "备用宿主机 / 管理节点 / 监控对象")

    rr(draw, (610, 430, 1510, 820), COL["lav"])
    draw.text((640, 460), "虚拟机侧执行域", font=F_TITLE, fill=COL["text"])
    label_box(draw, (680, 520, 920, 620), "VM 控制节点", "NameNode / ResourceManager")
    label_box(draw, (950, 520, 1190, 620), "VM 工作节点 1", "DataNode / 注入程序")
    label_box(draw, (1220, 520, 1460, 620), "VM 工作节点 2", "DataNode / 注入程序")
    label_box(draw, (820, 675, 1330, 775), "CloudStack 相关节点", "SystemVM / API 服务 / 数据库状态测试")

    arrow(draw, (460, 510), (680, 240), 4)
    arrow(draw, (460, 510), (790, 570), 4)
    arrow(draw, (460, 620), (860, 725), 4)
    img.save(OUTDIR / "fig5_2.png")


def main():
    OUTDIR.mkdir(parents=True, exist_ok=True)
    generators = [
        fig2_1,
        fig2_2,
        fig2_3,
        fig2_4,
        fig3_1,
        fig3_2,
        fig4_1,
        fig4_2,
        fig4_3,
        fig4_4,
        fig5_1,
        fig5_2,
    ]
    for fn in generators:
        fn()
    print(f"generated {len(generators)} unified figures into {OUTDIR}")


if __name__ == "__main__":
    main()
