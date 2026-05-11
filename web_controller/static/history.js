const historyListEl = document.getElementById("historyList");
const historySummaryEl = document.getElementById("historySummary");
const refreshHistoryBtn = document.getElementById("refreshHistory");
const clearDbHistoryBtn = document.getElementById("clearDbHistory");
const backToConsoleBtn = document.getElementById("backToConsole");
const runTypeFilterEl = document.getElementById("runTypeFilter");

function elc(tag, cls, html) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (html !== undefined) e.innerHTML = html;
  return e;
}

function escapeHtml(str) {
  const d = document.createElement("div");
  d.textContent = str == null ? "" : String(str);
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

function formatDate(value) {
  if (!value) return "--";
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString();
}

function runTypeText(type) {
  const map = {
    functest: "功能测试",
    action: "单次动作",
    cleanup: "清理动作",
  };
  return map[type] || type || "未知";
}

function setMessage(message, cls = "history-empty") {
  historyListEl.innerHTML = "";
  historyListEl.appendChild(elc("div", cls, escapeHtml(message)));
}

function resultsByPhase(run) {
  const phases = new Map();
  (run.results || []).forEach(result => {
    const phase = result.phase || "action";
    if (!phases.has(phase)) phases.set(phase, []);
    phases.get(phase).push(result);
  });
  return phases;
}

function phaseText(phase) {
  const map = {
    baseline: "操作前检查",
    action: "故障注入动作",
    verify: "操作后验证",
    cleanup: "清理动作",
    auto_test: "自动验证",
  };
  return map[phase] || phase || "结果";
}

function renderResult(result) {
  const item = elc("div", `node-result ${result.ok ? "nr-ok" : "nr-fail"}`);
  item.appendChild(elc("div", "nr-header", `
    <span class="nr-node">${escapeHtml(result.node || "unknown")}</span>
    <span class="nr-host">${escapeHtml(result.host || "")}</span>
    <span class="nr-meta">exit=${result.exit_code != null ? result.exit_code : "?"} | ${result.elapsed != null ? result.elapsed : "?"}s</span>
    <span class="nr-status ${result.ok ? "status-ok" : "status-fail"}">${result.ok ? "成功" : "失败"}</span>
  `));

  if (result.cmd) {
    item.appendChild(elc("div", "result-cmd", `<code>$ ${escapeHtml(result.cmd)}</code>`));
  }
  if (result.stdout) {
    item.appendChild(elc("pre", "nr-output", escapeHtml(result.stdout)));
  }
  if (result.stderr) {
    item.appendChild(elc("pre", "nr-stderr", escapeHtml(result.stderr)));
  }
  return item;
}

function renderRun(run) {
  const item = elc("div", `history-item ${run.ok ? "ok" : "bad"}`);
  const title = run.title || run.scenario_key || run.action_key || `运行 #${run.id}`;
  const failed = Number(run.failed_count || 0);
  const total = Number(run.result_count || (run.results || []).length || 0);

  item.appendChild(elc("div", "history-head", `
    <div>
      <div class="history-title">${escapeHtml(title)}</div>
      <div class="history-meta">
        #${escapeHtml(run.id)} | ${escapeHtml(runTypeText(run.run_type))}
        ${run.action_key ? ` | 动作: ${escapeHtml(run.action_key)}` : ""}
        ${run.scenario_key ? ` | 场景: ${escapeHtml(run.scenario_key)}` : ""}
        | ${escapeHtml(formatDate(run.started_at || run.created_at))}
      </div>
    </div>
    <div class="history-status">${run.ok ? "成功" : "失败"}</div>
  `));

  const body = elc("div", "history-body");
  body.appendChild(elc("div", "history-run-stats", `
    <span>结果数: ${total}</span>
    <span>失败数: ${failed}</span>
    <span>完成时间: ${escapeHtml(formatDate(run.finished_at))}</span>
  `));

  if (run.params && Object.keys(run.params).length) {
    const params = elc("details", "raw-details");
    params.innerHTML = `<summary>查看参数</summary><pre class="result-output">${escapeHtml(JSON.stringify(run.params, null, 2))}</pre>`;
    body.appendChild(params);
  }

  const phases = resultsByPhase(run);
  if (!phases.size) {
    body.appendChild(elc("div", "history-empty", "暂无命令结果"));
  } else {
    phases.forEach((results, phase) => {
      const details = elc("details", "raw-details");
      details.open = phase === "action" || phase === "cleanup";
      details.innerHTML = `<summary>${escapeHtml(phaseText(phase))} (${results.length})</summary>`;
      results.forEach(result => details.appendChild(renderResult(result)));
      body.appendChild(details);
    });
  }

  item.appendChild(body);
  return item;
}

async function loadHistory() {
  setMessage("正在加载数据库历史...", "history-loading");
  historySummaryEl.textContent = "加载中...";
  const type = runTypeFilterEl.value;
  const url = type ? `/api/history?limit=100&run_type=${encodeURIComponent(type)}` : "/api/history?limit=100";

  try {
    const payload = await fetchJson(url);
    const runs = Array.isArray(payload.runs) ? payload.runs : [];
    historySummaryEl.textContent = `共 ${runs.length} 条记录`;

    if (!runs.length) {
      setMessage("数据库中暂无注入历史");
      return;
    }

    const details = await Promise.all(runs.map(run => (
      run && run.id != null
        ? fetchJson(`/api/history/${encodeURIComponent(run.id)}`).catch(() => run)
        : Promise.resolve(run)
    )));

    historyListEl.innerHTML = "";
    details.forEach(run => historyListEl.appendChild(renderRun(run)));
  } catch (err) {
    historySummaryEl.textContent = "加载失败";
    setMessage(`历史记录加载失败: ${err.message}`, "history-empty history-error");
  }
}

async function clearDbHistory() {
  if (!window.confirm("确定清空数据库中的全部注入历史吗？当前控制台本次记录不会受影响。")) return;
  const btnText = clearDbHistoryBtn.textContent;
  clearDbHistoryBtn.textContent = "清空中...";
  clearDbHistoryBtn.disabled = true;
  try {
    const payload = await fetchJson("/api/history", { method: "DELETE" });
    historySummaryEl.textContent = `已删除 ${payload.deleted || 0} 条记录`;
    setMessage("数据库中暂无注入历史");
  } catch (err) {
    setMessage(`清空失败: ${err.message}`, "history-empty history-error");
  } finally {
    clearDbHistoryBtn.textContent = btnText;
    clearDbHistoryBtn.disabled = false;
  }
}

document.addEventListener("DOMContentLoaded", () => {
  if (backToConsoleBtn) backToConsoleBtn.addEventListener("click", () => { window.location.href = "/"; });
  if (refreshHistoryBtn) refreshHistoryBtn.addEventListener("click", loadHistory);
  if (runTypeFilterEl) runTypeFilterEl.addEventListener("change", loadHistory);
  if (clearDbHistoryBtn) clearDbHistoryBtn.addEventListener("click", clearDbHistory);
  loadHistory();
});
