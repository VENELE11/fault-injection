/* ================================================================== */
/*  app.js — 故障注入控制器 (统一界面)                                    */
/*  合并首页 + 功能测试页：前后对比 · 数据分析 · 清理恢复                 */
/* ================================================================== */

// ============================================================
//  DOM Elements
// ============================================================
const nodesEl = document.getElementById("nodes");
const actionsContainer = document.getElementById("actionsContainer");
const historyEl = document.getElementById("history");
const healthEl = document.getElementById("health");
const nodeListEl = document.getElementById("nodeList");
const refreshBtn = document.getElementById("refreshConfig");
const clearBtn = document.getElementById("clearHistory");
const outputLimitEl = document.getElementById("outputLimit");
const recoverAllBtn = document.getElementById("recoverAll");

// ============================================================
//  Constants
// ============================================================
const GROUP_ICONS = {
  cluster: "🖥️", process: "⚙️", network: "🌐", resource: "📊",
  hdfs: "💾", mapreduce: "🗺️", cloudstack: "☁️", vm: "🔧", kvm: "🔌",
};

// Actions shown as standalone utility cards (not covered by test scenarios)
const UTILITY_ACTIONS = new Set([
  "hadoop_start", "hadoop_stop", "kvm_benchmark", "kvm_clear",
]);

// ============================================================
//  State
// ============================================================
let configCache = null;
let allScenarios = [];
let scenariosByGroup = {};
let actionTitleMap = {};
const lastRunParamsByKey = {};

// ============================================================
//  Utility Functions
// ============================================================
function elc(tag, cls, html) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (html !== undefined) e.innerHTML = html;
  return e;
}

function escapeHtml(str) {
  const d = document.createElement("div");
  d.textContent = str;
  return d.innerHTML;
}

async function fetchJson(url, options = {}) {
  const res = await fetch(url, options);
  if (!res.ok) {
    const data = await res.json().catch(() => ({}));
    throw new Error(data.detail || `请求失败: ${res.status}`);
  }
  return res.json();
}

function appendHistory(item) { historyEl.prepend(item); }

// ============================================================
//  Data Parsing Helpers
// ============================================================
function parsePingStats(text) {
  const raw = String(text || "");
  const stats = {};
  const lossMatch = raw.match(/(\d+)%\s*packet loss/);
  if (lossMatch) stats.loss = parseInt(lossMatch[1]);
  const rttMatch = raw.match(/(?:rtt|round-trip)\s.*?=\s*([0-9.]+)\/([0-9.]+)\/([0-9.]+)/);
  if (rttMatch) {
    stats.rttMin = parseFloat(rttMatch[1]);
    stats.rttAvg = parseFloat(rttMatch[2]);
    stats.rttMax = parseFloat(rttMatch[3]);
  }
  return stats;
}

function parseLoadavg(text) {
  const m = String(text || "").match(/([0-9]+(?:\.[0-9]+)?)/);
  return m ? Number(m[1]) : null;
}

function parseFreeUsedMb(text) {
  const m = String(text || "").match(/Mem:\s+(\d+)\s+(\d+)/);
  return m ? Number(m[2]) : null;
}

function parseMaxCpuPercent(text) {
  let max = null;
  String(text || "").split("\n").forEach(line => {
    const m = line.trim().match(/^\d+\s+([0-9]+(?:\.[0-9]+)?)/);
    if (!m) return;
    const value = Number(m[1]);
    if (!Number.isNaN(value)) max = max === null ? value : Math.max(max, value);
  });
  return max;
}

function parseDdThroughputMBps(text) {
  const m = String(text || "").match(/([0-9]+(?:\.[0-9]+)?)\s*([KMG])B\/s/i);
  if (!m) return null;
  const value = Number(m[1]);
  const unit = String(m[2] || "M").toUpperCase();
  if (Number.isNaN(value)) return null;
  if (unit === "G") return value * 1024;
  if (unit === "K") return value / 1024;
  return value;
}

function getCheckOutput(checks, titleKeyword) {
  const c = (checks || []).find(it => String((it && it.title) || "").includes(titleKeyword));
  if (!c) return "";
  const first = (c.results || [])[0] || {};
  return String(first.stdout || first.output || "");
}

// ============================================================
//  Action Signal Detection
// ============================================================
function detectActionKeywordStats(text) {
  const raw = String(text || "");
  return {
    hang: /(\[hang\]|\bhang\b|挂起|暂停|sigstop|stopped)/i.test(raw),
    resume: /(\[resume\]|\bresume\b|恢复|继续|sigcont|continued)/i.test(raw),
    crash: /(\[crash\]|\bcrash\b|崩溃|killed|terminated|sigkill|sigterm)/i.test(raw),
  };
}

function summarizeActionSignals(results) {
  const summary = { hang: 0, resume: 0, crash: 0 };
  (results || []).forEach(r => {
    const mergedText = ((r && r.stdout) || (r && r.output) || "") + "\n" + ((r && r.stderr) || "");
    const stats = detectActionKeywordStats(mergedText);
    if (stats.hang) summary.hang += 1;
    if (stats.resume) summary.resume += 1;
    if (stats.crash) summary.crash += 1;
  });
  return summary;
}

function isHangLikeScenario(data) {
  const actionName = (data && data.action && data.action.action) || "";
  const cleanupParams = (data && data.cleanup_params) || {};
  const actionMayHang = ["process_fault", "vm_process", "cloudstack_process"].includes(actionName);
  return actionMayHang && (cleanupParams.op === "resume" || cleanupParams.proc_action === "resume");
}

// ============================================================
//  Data Analysis Cards
// ============================================================
function renderActionFocusCard(data, actionSignals) {
  if (!data || !data.action) return null;
  const isHangScenario = isHangLikeScenario(data) || actionSignals.hang > 0;
  const hasMeaningfulSignal = isHangScenario || actionSignals.resume > 0 || actionSignals.crash > 0;
  if (!hasMeaningfulSignal) return null;

  const parts = [];
  if (actionSignals.hang > 0) parts.push(`挂起关键字命中 ${actionSignals.hang} 个节点`);
  if (actionSignals.resume > 0) parts.push(`恢复关键字命中 ${actionSignals.resume} 个节点`);
  if (actionSignals.crash > 0) parts.push(`崩溃关键字命中 ${actionSignals.crash} 个节点`);

  const title = isHangScenario ? "⏸ 挂起结果判读提示" : "📌 动作结果提示";
  const detail = isHangScenario
    ? "挂起场景下，目标进程通常仍会出现在 jps 列表中。请查看下方“动作执行详情”中的标签。"
    : "已从动作日志中提取关键状态，请结合节点标签判断执行效果。";

  const card = elc("div", `result-focus ${isHangScenario ? "focus-hang" : "focus-info"}`);
  card.innerHTML = `
    <div class="focus-title">${title}</div>
    <div class="focus-text">${detail}</div>
    ${parts.length ? `<div class="focus-meta">${escapeHtml(parts.join(" · "))}</div>` : ""}
  `;
  return card;
}

function renderResourceFocusCard(data) {
  const key = (data && data.key) || "";
  if (!["test_cpu_stress", "test_mem_stress", "test_disk_fill", "test_io_slow",
    "test_vm_cpu", "test_vm_mem_leak", "test_kvm_perf_delay", "test_kvm_perf_stress"].includes(key)) return null;

  const tips = [];
  let positive = 0;

  if (key === "test_cpu_stress") {
    const bLbl = "注入前 loadavg";
    const aLbl = "注入后 loadavg";
    const beforeLoad = parseLoadavg(getCheckOutput(data.baseline, bLbl));
    const afterLoad = parseLoadavg(getCheckOutput(data.verify, aLbl));
    if (beforeLoad !== null && afterLoad !== null) {
      const delta = afterLoad - beforeLoad;
      if (delta > 0.1) { positive++; tips.push(`loadavg 上升 ${delta.toFixed(2)}，CPU 压力生效`); }
      else { tips.push(`loadavg 变化较小 (${delta.toFixed(2)})`); }
    }
  }

  if (key === "test_vm_cpu") {
    const beforeCpu = parseMaxCpuPercent(getCheckOutput(data.baseline, "压力前 CPU"));
    const afterCpu = parseMaxCpuPercent(getCheckOutput(data.verify, "压力中 CPU"));
    if (beforeCpu !== null && afterCpu !== null) {
      if (afterCpu >= Math.max(50, beforeCpu + 20)) {
        positive++;
        tips.push(`压力中最大 CPU ${beforeCpu.toFixed(1)}% → ${afterCpu.toFixed(1)}%，CPU 压力可见`);
      } else {
        tips.push(`CPU 变化不明显 (${beforeCpu.toFixed(1)}% → ${afterCpu.toFixed(1)}%)`);
      }
    } else {
      tips.push("未解析到 CPU 采样，请查看 CPU 压力日志");
    }
  }

  if (key === "test_kvm_perf_stress") {
    const beforeCpu = parseMaxCpuPercent(getCheckOutput(data.baseline, "压力前 CPU"));
    const afterCpu = parseMaxCpuPercent(getCheckOutput(data.verify, "压力中 CPU"));
    if (beforeCpu !== null && afterCpu !== null) {
      if (afterCpu >= Math.max(50, beforeCpu + 20)) {
        positive++;
        tips.push(`压力中最大 CPU ${beforeCpu.toFixed(1)}% → ${afterCpu.toFixed(1)}%，CPU 压力可见`);
      } else {
        tips.push(`CPU 变化不明显 (${beforeCpu.toFixed(1)}% → ${afterCpu.toFixed(1)}%)`);
      }
    } else {
      tips.push("未解析到 CPU 采样，请查看压力注入日志");
    }
  }

  if (key === "test_mem_stress" || key === "test_vm_mem_leak") {
    const beforeMem = parseFreeUsedMb(getCheckOutput(data.baseline, "内存"));
    const afterMem = parseFreeUsedMb(getCheckOutput(data.verify, "内存"));
    const delta = beforeMem !== null && afterMem !== null ? afterMem - beforeMem : null;
    if (delta !== null && delta >= 50) { positive++; tips.push(`内存已用量上升 ${delta} MB，注入生效`); }
    else { tips.push("未观察到明显内存压力变化"); }
  }

  if (key === "test_disk_fill") {
    const afterFile = getCheckOutput(data.verify, "填充后磁盘文件");
    if (afterFile && !afterFile.includes("disk_hog_absent")) {
      positive++; tips.push("检测到 /tmp/disk_hog，磁盘填充注入生效");
    } else { tips.push("未检测到磁盘填充文件"); }
  }

  if (key === "test_io_slow") {
    const beforeCg = getCheckOutput(data.baseline, "限速前 cgroup");
    const afterCg = getCheckOutput(data.verify, "限速后 cgroup");
    const beforeOff = beforeCg.includes("io_limit_off");
    const afterOff = afterCg.includes("io_limit_off");
    if (beforeOff && !afterOff) { positive++; tips.push("cgroup 由 off → on，I/O 限速已启用"); }
    else if (!afterOff) { positive++; tips.push("io_limited cgroup 已开启"); }
    else { tips.push("cgroup 仍为 off"); }

    const beforeMb = parseDdThroughputMBps(getCheckOutput(data.baseline, "限速前写入"));
    const afterMb = parseDdThroughputMBps(getCheckOutput(data.verify, "限速后写入"));
    if (beforeMb !== null && afterMb !== null) {
      if (afterMb < beforeMb * 0.5) {
        positive++; tips.push(`写入 ${beforeMb.toFixed(1)} → ${afterMb.toFixed(1)} MB/s，限速明显`);
      } else { tips.push(`写入变化不明显 (${beforeMb.toFixed(1)} → ${afterMb.toFixed(1)} MB/s)`); }
    }
  }

  if (key === "test_kvm_perf_delay") {
    const beforeMb = parseDdThroughputMBps(getCheckOutput(data.baseline, "延迟前任务速度"));
    const afterMb = parseDdThroughputMBps(getCheckOutput(data.verify, "延迟后任务速度"));
    if (beforeMb !== null && afterMb !== null) {
      const ratio = afterMb / Math.max(beforeMb, 0.001);
      if (ratio < 0.85) {
        positive++;
        tips.push(`VM 内任务速度 ${beforeMb.toFixed(1)} → ${afterMb.toFixed(1)} MB/s，性能延迟可见`);
      } else {
        tips.push(`VM 内任务速度变化较小 (${beforeMb.toFixed(1)} → ${afterMb.toFixed(1)} MB/s)`);
      }
    } else {
      tips.push("未解析到 VM 内 CPU+dd 速度，请检查 sshpass、root/123456 登录或节点 SSH 端口");
    }
  }

  if (!tips.length) return null;
  const card = elc("div", `result-focus ${positive > 0 ? "focus-info" : "focus-hang"}`);
  card.innerHTML = `
    <div class="focus-title">📊 资源故障结果判读</div>
    <div class="focus-text">${positive > 0 ? "检测到可观测变化。" : "暂未检测到明确变化。"}</div>
    <div class="focus-meta">${escapeHtml(tips.join(" · "))}</div>
  `;
  return card;
}

function renderNetworkFocusCard(data) {
  const key = (data && data.key) || "";
  if (!["test_delay", "test_loss", "test_reorder", "test_isolate", "test_vm_network"].includes(key)) return null;

  const tips = [];
  let positive = 0;
  const beforePing = getCheckOutput(data.baseline, "ping");
  const afterPing = getCheckOutput(data.verify, "ping");
  const beforeStats = parsePingStats(beforePing);
  const afterStats = parsePingStats(afterPing);

  if (key === "test_delay") {
    if (beforeStats.rttAvg != null && afterStats.rttAvg != null) {
      const delta = afterStats.rttAvg - beforeStats.rttAvg;
      if (delta > 10) { positive++; tips.push(`平均延迟上升 ${delta.toFixed(1)}ms (${beforeStats.rttAvg.toFixed(1)} → ${afterStats.rttAvg.toFixed(1)})，延迟注入生效`); }
      else { tips.push(`延迟变化 ${delta.toFixed(1)}ms`); }
    }
    const beforeTc = getCheckOutput(data.baseline, "tc 规则");
    const afterTc = getCheckOutput(data.verify, "tc 规则");
    if (afterTc.includes("netem") && !beforeTc.includes("netem")) { positive++; tips.push("tc 规则已添加 netem"); }
  }

  if (key === "test_loss") {
    if (beforeStats.loss != null && afterStats.loss != null && afterStats.loss > beforeStats.loss) {
      positive++; tips.push(`丢包率 ${beforeStats.loss}% → ${afterStats.loss}%`);
    }
  }

  if (key === "test_reorder") {
    const afterTc = getCheckOutput(data.verify, "tc 规则");
    if (afterTc.includes("netem") || afterTc.includes("corrupt")) { positive++; tips.push("tc 规则已添加报文损坏"); }
  }

  if (key === "test_isolate") {
    const beforeTcp = getCheckOutput(data.baseline, "TCP 端口连通性");
    const afterTcp = getCheckOutput(data.verify, "TCP 端口连通性");
    if (beforeTcp.includes("TCP_CONNECT_OK") && afterTcp.includes("TCP_CONNECT_FAIL")) {
      positive++; tips.push("端口隔离生效：TCP OK → FAIL");
    }
    const beforeRules = getCheckOutput(data.baseline, "OUTPUT 规则");
    const afterRules = getCheckOutput(data.verify, "OUTPUT 规则");
    if (beforeRules.includes("no partition") && afterRules.includes("--dport")) {
      positive++; tips.push("iptables 隔离规则已添加");
    }
  }

  if (key === "test_vm_network") {
    if (beforeStats.rttAvg != null && afterStats.rttAvg != null) {
      const delta = afterStats.rttAvg - beforeStats.rttAvg;
      if (Math.abs(delta) > 5) { positive++; tips.push(`ping 延迟变化 ${delta.toFixed(1)}ms`); }
    }
    if (beforeStats.loss != null && afterStats.loss != null && afterStats.loss !== beforeStats.loss) {
      positive++; tips.push(`丢包率 ${beforeStats.loss}% → ${afterStats.loss}%`);
    }
  }

  if (!tips.length) return null;
  const card = elc("div", `result-focus ${positive > 0 ? "focus-info" : "focus-hang"}`);
  card.innerHTML = `
    <div class="focus-title">🌐 网络故障结果判读</div>
    <div class="focus-text">${positive > 0 ? "检测到网络特征变化。" : "暂未检测到明确变化。"}</div>
    <div class="focus-meta">${escapeHtml(tips.join(" · "))}</div>
  `;
  return card;
}

function renderHdfsFocusCard(data) {
  const key = (data && data.key) || "";
  if (!["test_hdfs_safe", "test_hdfs_disk", "test_yarn_unhealthy"].includes(key)) return null;

  const tips = [];
  let positive = 0;

  if (key === "test_hdfs_safe") {
    const afterSafe = getCheckOutput(data.verify, "HDFS 状态");
    if (/ON|true|enabled/i.test(afterSafe)) { positive++; tips.push("HDFS 安全模式已进入"); }
    const writeTest = getCheckOutput(data.verify, "写入测试");
    if (/WRITE_BLOCKED_EXPECTED|Cannot|Safe mode|safemode/i.test(writeTest)) { positive++; tips.push("安全模式下写入被拒绝"); }
  }

  if (key === "test_hdfs_disk") {
    const actionText = ((data.action && data.action.results) || [])
      .map(r => `${r.stdout || ""}\n${r.stderr || ""}`)
      .join("\n");
    if (/No space left|DISK_FULL_OR_PARTIAL_FILL_EXPECTED|DD_EXIT=/i.test(actionText)) {
      positive++;
      tips.push("磁盘填充动作已执行，空间不足信号已触发");
    }
    const afterDisk = getCheckOutput(data.verify, "填充后目标磁盘");
    if (/(fi_disk_hog|disk_hog)/.test(afterDisk) && !afterDisk.includes("disk_hog_absent")) {
      positive++;
      tips.push("检测到磁盘填充文件，磁盘占用已生效");
    }
    const beforeReport = getCheckOutput(data.baseline, "HDFS 报告");
    const afterReport = getCheckOutput(data.verify, "HDFS 报告");
    const beforeUsed = beforeReport.match(/DFS Used:\s*([0-9.]+\s*\w+)/);
    const afterUsed = afterReport.match(/DFS Used:\s*([0-9.]+\s*\w+)/);
    if (beforeUsed && afterUsed) { positive++; tips.push(`HDFS 已用: ${beforeUsed[1]} → ${afterUsed[1]}`); }
  }

  if (key === "test_yarn_unhealthy") {
    const afterNm = getCheckOutput(data.verify, "目标 NodeManager");
    if (/NodeManager_stopped|not_running|unhealthy/i.test(afterNm)) {
      positive++;
      tips.push("目标 NodeManager 已停止，节点不健康模拟生效");
    }
    const afterYarn = getCheckOutput(data.verify, "标记后 YARN");
    if (/UNHEALTHY|LOST|SHUTDOWN/i.test(afterYarn)) { positive++; tips.push("YARN 节点列表出现异常状态"); }
  }

  if (!tips.length) return null;
  const card = elc("div", `result-focus ${positive > 0 ? "focus-info" : "focus-hang"}`);
  card.innerHTML = `
    <div class="focus-title">💾 HDFS/YARN 结果判读</div>
    <div class="focus-text">${positive > 0 ? "检测到状态变化。" : "暂未检测到明确变化。"}</div>
    <div class="focus-meta">${escapeHtml(tips.join(" · "))}</div>
  `;
  return card;
}

function renderMapReduceFocusCard(data) {
  const key = (data && data.key) || "";
  if (key !== "test_mapreduce_fault") return null;

  const actionText = ((data.action && data.action.results) || [])
    .map(r => `${r.stdout || ""}\n${r.stderr || ""}`)
    .join("\n");
  const jobLog = getCheckOutput(data.verify, "后台任务日志");
  const tips = [];
  let positive = 0;

  if (/MAPREDUCE_WORDCOUNT_JOB_SUBMITTED|submitted application|Running job/i.test(`${actionText}\n${jobLog}`)) {
    positive++;
    tips.push("后台 wordcount 作业已提交");
  }
  if (/MAPREDUCE_TASK_KILLED_EXPECTED|MAPREDUCE_KILLING_TASK/i.test(actionText)) {
    positive++;
    tips.push("已命中并杀死 YarnChild 任务进程");
  }
  if (/TASK_NOT_FOUND_ON_TARGET|MAPREDUCE_JOB_NOT_RUNNING/i.test(actionText)) {
    tips.push("未在目标节点发现可注入的任务进程");
  }

  if (!tips.length) return null;
  const card = elc("div", `result-focus ${positive > 1 ? "focus-info" : "focus-hang"}`);
  card.innerHTML = `
    <div class="focus-title">🗺️ MapReduce 结果判读</div>
    <div class="focus-text">${positive > 1 ? "后台任务和故障注入均已观察到。" : "后台任务已启动，但注入命中还不明确。"}</div>
    <div class="focus-meta">${escapeHtml(tips.join(" · "))}</div>
  `;
  return card;
}

// ============================================================
//  Node Rendering
// ============================================================
function renderNodes(nodes) {
  nodesEl.innerHTML = "";
  nodeListEl.innerHTML = "";
  nodes.forEach(n => {
    const card = elc("div", "node-card");
    card.innerHTML = `
      <div class="node-title">${n.name}</div>
      <div class="node-meta">${n.host}:${n.port}</div>
      <div class="node-meta">角色: ${n.role}</div>
      <div class="node-tag ${n.local ? "tag-local" : "tag-ssh"}">${n.local ? "local" : "ssh"}</div>
    `;
    nodesEl.appendChild(card);

    const opt1 = document.createElement("option");
    opt1.value = n.name; opt1.label = `${n.name} (${n.host})`;
    nodeListEl.appendChild(opt1);
    const opt2 = document.createElement("option");
    opt2.value = n.host; opt2.label = n.host;
    nodeListEl.appendChild(opt2);
  });
}

// ============================================================
//  Main Panel Rendering
// ============================================================
function renderMainPanel(cfg) {
  actionsContainer.innerHTML = "";
  const groups = cfg.groups || [];
  const actions = cfg.actions || [];

  groups.forEach((group, idx) => {
    const scenarios = scenariosByGroup[group.key] || [];
    const utilities = actions.filter(a => a.group === group.key && UTILITY_ACTIONS.has(a.key));
    if (!scenarios.length && !utilities.length) return;

    const section = elc("section", "action-section");
    section.style.setProperty("--delay", `${idx * 60}ms`);
    section.innerHTML = `
      <div class="section-head">
        <div>
          <h3>${GROUP_ICONS[group.key] || "📌"} ${group.title}</h3>
          <p>${group.desc}</p>
        </div>
      </div>
      <div class="action-grid"></div>
    `;

    const grid = section.querySelector(".action-grid");
    scenarios.forEach(s => grid.appendChild(buildScenarioCard(s)));
    utilities.forEach(a => grid.appendChild(buildUtilityCard(a)));

    actionsContainer.appendChild(section);
  });
}

// ============================================================
//  Card Builders
// ============================================================
function buildScenarioCard(scenario) {
  const card = elc("div", "action-card");

  const badges = [];
  if (scenario.has_baseline) badges.push('<span class="scenario-badge badge-compare">前后对比</span>');
  if (scenario.has_cleanup) badges.push('<span class="scenario-badge badge-cleanup">可清理</span>');

  card.innerHTML = `
    <div class="action-title">${escapeHtml(scenario.title)}${badges.join("")}</div>
    <div class="action-desc">${escapeHtml(scenario.desc)}</div>
  `;

  const form = elc("div", "action-form");
  form.id = `form-${scenario.key}`;
  (scenario.params || []).forEach(p => form.appendChild(renderField(p)));
  card.appendChild(form);

  const footer = elc("div", "action-footer");

  if (scenario.has_cleanup) {
    const cleanBtn = elc("button", "btn-cleanup-small", "🔄 清理");
    cleanBtn.addEventListener("click", () => {
      const params = collectParams(form);
      executeCleanup(scenario, params, cleanBtn);
    });
    footer.appendChild(cleanBtn);
  }

  const runBtn = elc("button", "primary", "▶ 执行");
  runBtn.addEventListener("click", () => {
    const params = collectParams(form);
    executeScenario(scenario, params, runBtn);
  });
  footer.appendChild(runBtn);

  card.appendChild(footer);
  return card;
}

function buildUtilityCard(action) {
  const card = elc("div", "action-card utility-card");
  card.innerHTML = `
    <div class="action-title">${escapeHtml(action.title)}<span class="scenario-badge badge-util">工具</span></div>
    <div class="action-desc">${escapeHtml(action.desc)}</div>
  `;

  const form = elc("div", "action-form");
  (action.params || []).forEach(p => form.appendChild(renderField(p)));
  card.appendChild(form);

  const footer = elc("div", "action-footer");
  const btn = elc("button", "primary", "执行");
  btn.addEventListener("click", () => {
    const params = collectParams(form);
    runSimpleAction(action.key, action.title, params, btn);
  });
  footer.appendChild(btn);
  card.appendChild(footer);
  return card;
}

// ============================================================
//  Form Field Rendering
// ============================================================
function renderField(param) {
  const wrapper = elc("label", "field");
  const label = elc("span", "field-label");
  label.textContent = param.label + (param.required ? "" : " (可选)");

  let input;
  if (param.type === "select") {
    input = document.createElement("select");
    (param.options || []).forEach(opt => {
      const option = document.createElement("option");
      option.value = opt.value; option.textContent = opt.label;
      input.appendChild(option);
    });
    if (param.default !== undefined) input.value = param.default;
  } else if (param.type === "node") {
    input = document.createElement("select");
    ((configCache && configCache.nodes) || []).forEach(n => {
      const option = document.createElement("option");
      option.value = n.name; option.textContent = `${n.name} (${n.host})`;
      input.appendChild(option);
    });
    if (param.default !== undefined) input.value = param.default;
  } else if (param.type === "number") {
    input = document.createElement("input");
    input.type = "number";
    if (param.default !== undefined) input.value = param.default;
    if (param.placeholder) input.placeholder = param.placeholder;
  } else {
    input = document.createElement("input");
    input.type = "text";
    if (param.default !== undefined) input.value = param.default;
    if (param.placeholder) input.placeholder = param.placeholder;
  }

  input.dataset.param = param.name;

  wrapper.appendChild(label);
  wrapper.appendChild(input);

  if (param.help) {
    const help = elc("small", "field-help");
    help.textContent = param.help;
    wrapper.appendChild(help);
  }

  return wrapper;
}

function collectParams(form) {
  const params = {};
  form.querySelectorAll("[data-param]").forEach(input => {
    const name = input.dataset.param;
    let val = input.value;
    if (val !== undefined) val = val.toString().trim();
    if (val !== "" && val !== undefined) {
      if (input.type === "number" && val !== "") val = Number(val);
      params[name] = val;
    }
  });
  return params;
}

// ============================================================
//  Execution Functions
// ============================================================
async function executeScenario(scenario, params, btn) {
  const startedAt = new Date();
  const btnText = btn.textContent;
  btn.textContent = "⏳ 执行中...";
  btn.disabled = true;

  try {
    const data = await fetchJson("/api/functest", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ key: scenario.key, params }),
    });
    lastRunParamsByKey[scenario.key] = { ...params };
    const entry = buildEnhancedHistoryEntry(scenario.title, data, startedAt);
    appendHistory(entry);
  } catch (err) {
    const entry = buildErrorEntry(scenario.title, scenario.key, err, startedAt);
    appendHistory(entry);
  } finally {
    btn.textContent = btnText;
    btn.disabled = false;
  }
}

async function executeCleanup(scenario, params, btn) {
  const startedAt = new Date();
  const btnText = btn.textContent;
  btn.textContent = "⏳ 清理中...";
  btn.disabled = true;

  try {
    const cleanParams = lastRunParamsByKey[scenario.key] || params;
    const data = await fetchJson("/api/functest/cleanup", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ key: scenario.key, params: cleanParams }),
    });
    const entry = buildCleanupHistoryEntry(`${scenario.title} — 清理`, data, startedAt);
    appendHistory(entry);
  } catch (err) {
    const entry = buildErrorEntry(`${scenario.title} — 清理`, scenario.key, err, startedAt);
    appendHistory(entry);
  } finally {
    btn.textContent = btnText;
    btn.disabled = false;
  }
}

async function runSimpleAction(actionKey, title, params, btn) {
  const startedAt = new Date();
  const btnText = btn.textContent;
  btn.textContent = "执行中...";
  btn.disabled = true;

  try {
    const data = await fetchJson("/api/action", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: actionKey, params }),
    });
    const entry = buildSimpleHistoryEntry(title, actionKey, data, startedAt);
    appendHistory(entry);
  } catch (err) {
    const entry = buildErrorEntry(title, actionKey, err, startedAt);
    appendHistory(entry);
  } finally {
    btn.textContent = btnText;
    btn.disabled = false;
  }
}

async function runRecoveryAll() {
  if (!recoverAllBtn) return;
  const btnText = recoverAllBtn.textContent;
  recoverAllBtn.textContent = "恢复中...";
  recoverAllBtn.disabled = true;

  try {
    const payload = await fetchJson("/api/recover/all", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
    });
    const steps = Array.isArray(payload.steps) ? payload.steps : [];
    for (const step of steps) {
      const startedAt = new Date();
      const title = `一键恢复 - ${actionTitleMap[step.action] || step.action}`;
      if (step && Array.isArray(step.results)) {
        const entry = buildSimpleHistoryEntry(title, step.action, step, startedAt);
        appendHistory(entry);
      } else {
        const err = new Error(step.error || "恢复失败");
        const entry = buildErrorEntry(title, step.action, err, startedAt);
        appendHistory(entry);
      }
    }
  } catch (err) {
    const entry = buildErrorEntry("一键恢复", "recover_all", err, new Date());
    appendHistory(entry);
  }

  recoverAllBtn.textContent = btnText;
  recoverAllBtn.disabled = false;
}

// ============================================================
//  History Entry Builders
// ============================================================
function buildEnhancedHistoryEntry(title, data, startedAt) {
  const item = elc("div", `history-item ${data.ok ? "ok" : "bad"}`);

  const header = elc("div", "history-head");
  header.innerHTML = `
    <div>
      <div class="history-title">${escapeHtml(title)}</div>
      <div class="history-meta">${startedAt.toLocaleString()} | 场景: ${escapeHtml(data.key || "")}</div>
    </div>
    <div class="history-status">${data.ok ? "成功" : "失败"}</div>
  `;

  const body = elc("div", "history-body");

  // Status banner
  const banner = elc("div", `result-banner ${data.ok ? "banner-ok" : "banner-fail"}`);
  banner.innerHTML = `
    <span class="banner-icon">${data.ok ? "✅" : "❌"}</span>
    <span class="banner-text">${escapeHtml(title)}: ${data.ok ? "测试通过" : "测试未通过/异常"}</span>
  `;
  body.appendChild(banner);

  // Data analysis cards
  const actionSignals = summarizeActionSignals((data && data.action && data.action.results) || []);
  const focusCard = renderActionFocusCard(data, actionSignals);
  if (focusCard) body.appendChild(focusCard);
  const resourceCard = renderResourceFocusCard(data);
  if (resourceCard) body.appendChild(resourceCard);
  const networkCard = renderNetworkFocusCard(data);
  if (networkCard) body.appendChild(networkCard);
  const hdfsCard = renderHdfsFocusCard(data);
  if (hdfsCard) body.appendChild(hdfsCard);
  const mapReduceCard = renderMapReduceFocusCard(data);
  if (mapReduceCard) body.appendChild(mapReduceCard);

  // Before/after comparison
  const hasBaseline = data.baseline && data.baseline.length > 0;
  const hasVerify = data.verify && data.verify.length > 0;
  if (hasBaseline || hasVerify) {
    body.appendChild(renderComparison(data.baseline, data.verify));
  }

  // Action execution details
  if (data.action) {
    const actionSec = elc("div", "action-results-section");
    actionSec.innerHTML = `<div class="section-label">⚡ 动作执行详情 — ${escapeHtml(data.action.action || "")}</div>`;
    if (data.action.results && data.action.results.length > 0) {
      data.action.results.forEach(r => actionSec.appendChild(renderNodeResult(r, { context: "action" })));
    }
    if (data.action.error) {
      actionSec.appendChild(elc("div", "error-card", `错误: ${escapeHtml(data.action.error)}`));
    }
    body.appendChild(actionSec);
  }

  item.appendChild(header);
  item.appendChild(body);
  return item;
}

function buildSimpleHistoryEntry(title, actionKey, data, startedAt) {
  const item = elc("div", `history-item ${data.ok ? "ok" : "bad"}`);

  const header = elc("div", "history-head");
  header.innerHTML = `
    <div>
      <div class="history-title">${escapeHtml(title)}</div>
      <div class="history-meta">${startedAt.toLocaleString()} | 动作: ${escapeHtml(actionKey)}</div>
    </div>
    <div class="history-status">${data.ok ? "成功" : "失败"}</div>
  `;

  const body = elc("div", "history-body");
  (data.results || []).forEach(r => body.appendChild(renderNodeResult(r)));

  item.appendChild(header);
  item.appendChild(body);
  return item;
}

function buildCleanupHistoryEntry(title, data, startedAt) {
  const item = elc("div", `history-item ${data.ok ? "ok" : "bad"}`);

  const header = elc("div", "history-head");
  header.innerHTML = `
    <div>
      <div class="history-title">${escapeHtml(title)}</div>
      <div class="history-meta">${startedAt.toLocaleString()}</div>
    </div>
    <div class="history-status">${data.ok ? "成功" : "失败"}</div>
  `;

  const body = elc("div", "history-body");
  const statusText = data.ok
    ? `✅ 清理已完成: ${escapeHtml(data.cleanup_action || "")}`
    : `❌ 清理失败: ${escapeHtml(data.error || data.cleanup_action || "")}`;
  body.appendChild(elc("div", "cleanup-banner", statusText));
  (data.results || []).forEach(r => body.appendChild(renderNodeResult(r)));

  item.appendChild(header);
  item.appendChild(body);
  return item;
}

function buildErrorEntry(title, actionKey, err, startedAt) {
  const item = elc("div", "history-item bad");
  item.innerHTML = `
    <div class="history-head">
      <div>
        <div class="history-title">${escapeHtml(title)}</div>
        <div class="history-meta">${startedAt.toLocaleString()} | ${escapeHtml(actionKey)}</div>
      </div>
      <div class="history-status">失败</div>
    </div>
    <div class="history-body">
      <pre class="result-output">${escapeHtml(err.message || "请求失败")}</pre>
    </div>
  `;
  return item;
}

// ============================================================
//  Comparison Rendering
// ============================================================
function renderComparison(baseline, verify) {
  const comparison = elc("div", "comparison");

  if (baseline && baseline.length) {
    const leftCol = elc("div", "compare-col");
    leftCol.appendChild(elc("div", "col-header col-before", "📋 操作前（基线）"));
    baseline.forEach(check => leftCol.appendChild(renderCheckResult(check)));
    comparison.appendChild(leftCol);
  }

  if (baseline && baseline.length && verify && verify.length) {
    const divider = elc("div", "compare-divider");
    divider.innerHTML = `
      <div class="divider-line"></div>
      <div class="divider-icon">→</div>
      <div class="divider-line"></div>
    `;
    comparison.appendChild(divider);
  }

  if (verify && verify.length) {
    const rightCol = elc("div", "compare-col");
    rightCol.appendChild(elc("div", "col-header col-after", "🔍 操作后（验证）"));
    verify.forEach(check => rightCol.appendChild(renderCheckResult(check)));
    comparison.appendChild(rightCol);
  }

  return comparison;
}

function renderCheckResult(check) {
  const card = elc("div", `check-card ${check.ok ? "check-ok" : "check-fail"}`);
  card.appendChild(elc("div", "check-title", `
    <span class="check-icon">${check.ok ? "✓" : "✗"}</span>
    <span>${escapeHtml(check.title)}</span>
  `));
  card.appendChild(elc("div", "check-cmd", `<code>$ ${escapeHtml(check.cmd)}</code>`));
  (check.results || []).forEach(r => card.appendChild(renderNodeResult(r)));
  return card;
}

function renderNodeResult(r, options = {}) {
  const context = options.context || "check";
  const stdout = r.stdout || r.output || "";
  const stderr = r.stderr || "";
  const actionStats = context === "action" ? detectActionKeywordStats(`${stdout}\n${stderr}`) : { hang: false, resume: false, crash: false };

  const classes = ["node-result", r.ok ? "nr-ok" : "nr-fail"];
  if (context === "action") classes.push("nr-action");
  if (actionStats.hang) classes.push("nr-signal-hang");
  if (actionStats.resume) classes.push("nr-signal-resume");
  if (actionStats.crash) classes.push("nr-signal-crash");
  const item = elc("div", classes.join(" "));

  let signalBadge = "";
  if (context === "action") {
    if (actionStats.hang) signalBadge = '<span class="nr-signal-badge sig-hang">挂起成功</span>';
    else if (actionStats.resume) signalBadge = '<span class="nr-signal-badge sig-resume">恢复成功</span>';
    else if (actionStats.crash) signalBadge = '<span class="nr-signal-badge sig-crash">崩溃生效</span>';
  }

  item.appendChild(elc("div", "nr-header", `
    <span class="nr-node">${escapeHtml(r.node || "unknown")}</span>
    <span class="nr-host">${escapeHtml(r.host || "")}</span>
    ${signalBadge}
    <span class="nr-meta">exit=${r.exit_code != null ? r.exit_code : "?"} | ${r.elapsed != null ? r.elapsed : "?"}s</span>
    <span class="nr-status ${r.ok ? "status-ok" : "status-fail"}">${r.ok ? "成功" : "失败"}</span>
  `));

  if (stdout) {
    const outClass = context === "action" && (actionStats.hang || actionStats.resume || actionStats.crash)
      ? "nr-output nr-output-focus" : "nr-output";
    item.appendChild(elc("pre", outClass, escapeHtml(stdout)));
  }
  if (stderr) {
    item.appendChild(elc("pre", "nr-stderr", escapeHtml(stderr)));
  }
  return item;
}

// ============================================================
//  Health Check
// ============================================================
async function healthCheck() {
  try {
    await fetchJson("/api/health");
    healthEl.textContent = "在线";
    healthEl.classList.add("ok");
  } catch (err) {
    healthEl.textContent = "离线";
    healthEl.classList.add("bad");
  }
}

// ============================================================
//  Init & Event Listeners
// ============================================================
document.addEventListener("DOMContentLoaded", function () {
  console.log("[app.js] DOMContentLoaded fired");
  try {
    if (refreshBtn) refreshBtn.addEventListener("click", function () { initLoad(); });
    if (clearBtn) clearBtn.addEventListener("click", function () { historyEl.innerHTML = ""; });
    if (recoverAllBtn) recoverAllBtn.addEventListener("click", function () { runRecoveryAll(); });

    healthCheck().then(function () {
      return initLoad();
    }).catch(function (err) {
      console.error("[app.js] init error:", err);
    });
  } catch (err) {
    console.error("[app.js] startup error:", err);
  }
});

async function initLoad() {
  console.log("[app.js] initLoad starting...");
  try {
    var cfgPromise = fetchJson("/api/config");
    var testPromise = fetchJson("/api/testcases");
    var results = await Promise.all([cfgPromise, testPromise]);
    var cfg = results[0];
    var testData = results[1];

    console.log("[app.js] config loaded, groups:", (cfg.groups || []).length, "actions:", (cfg.actions || []).length);
    console.log("[app.js] testcases loaded:", (testData.tests || []).length);

    configCache = cfg;
    allScenarios = testData.tests || [];

    scenariosByGroup = {};
    allScenarios.forEach(function (s) {
      if (!scenariosByGroup[s.group]) scenariosByGroup[s.group] = [];
      scenariosByGroup[s.group].push(s);
    });

    actionTitleMap = {};
    (cfg.actions || []).forEach(function (a) { actionTitleMap[a.key] = a.title; });

    renderNodes(cfg.nodes || []);
    renderMainPanel(cfg);

    var outputCfg = cfg.output || {};
    var maxLines = typeof outputCfg.max_lines === "number" ? outputCfg.max_lines : 200;
    var maxChars = typeof outputCfg.max_chars === "number" ? outputCfg.max_chars : 8000;
    if ((maxLines || 0) <= 0 && (maxChars || 0) <= 0) {
      outputLimitEl.textContent = "输出限制: 无限制";
    } else {
      outputLimitEl.textContent = "输出限制: " + maxLines + " 行 / " + maxChars + " 字符";
    }
    console.log("[app.js] initLoad complete");
  } catch (err) {
    console.error("[app.js] initLoad error:", err);
    actionsContainer.innerHTML = '<div class="error-card">加载失败: ' + escapeHtml(err.message) + '</div>';
  }
}
