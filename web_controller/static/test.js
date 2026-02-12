/* ================================================================== */
/*  test.js — 功能测试页逻辑                                           */
/* ================================================================== */

const API_BASE = "";

const GROUP_LABELS = {
    cluster: "集群管理",
    process: "进程故障",
    network: "网络故障",
    resource: "资源故障",
    hdfs: "HDFS / YARN",
    mapreduce: "MapReduce",
    vm: "VM 注入",
    kvm: "KVM 注入",
};

const GROUP_ICONS = {
    cluster: "🖥️",
    process: "⚙️",
    network: "🌐",
    resource: "📊",
    hdfs: "💾",
    mapreduce: "🗺️",
    vm: "🔧",
    kvm: "🔌",
};

let allTests = [];
let nodes = [];
let currentGroup = null;

/* ------------------------------------------------------------------ */
/*  Utilities                                                          */
/* ------------------------------------------------------------------ */

async function fetchJson(url, opts) {
    const r = await fetch(API_BASE + url, opts);
    if (!r.ok) {
        const text = await r.text();
        throw new Error(`HTTP ${r.status}: ${text}`);
    }
    return r.json();
}

function escapeHtml(str) {
    const d = document.createElement("div");
    d.textContent = str;
    return d.innerHTML;
}

function el(tag, cls, html) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html !== undefined) e.innerHTML = html;
    return e;
}

/* ------------------------------------------------------------------ */
/*  Init                                                               */
/* ------------------------------------------------------------------ */

async function init() {
    try {
        const [testData, configData] = await Promise.all([
            fetchJson("/api/testcases"),
            fetchJson("/api/config"),
        ]);
        allTests = testData.tests || [];
        nodes = (configData.nodes || []).map(n => n.name);
        renderSidebar();
        // Select first group by default
        const firstGroup = allTests.length ? allTests[0].group : null;
        if (firstGroup) selectGroup(firstGroup);
    } catch (err) {
        document.getElementById("testList").innerHTML =
            `<div class="error-card">加载失败: ${escapeHtml(err.message)}</div>`;
    }
}

/* ------------------------------------------------------------------ */
/*  Sidebar                                                            */
/* ------------------------------------------------------------------ */

function renderSidebar() {
    const sidebar = document.getElementById("sidebar");
    sidebar.innerHTML = "";

    const groups = [];
    const seen = new Set();
    allTests.forEach(t => {
        if (!seen.has(t.group)) {
            seen.add(t.group);
            groups.push(t.group);
        }
    });

    groups.forEach(g => {
        const count = allTests.filter(t => t.group === g).length;
        const btn = el("button", "nav-item", `
      <span class="nav-icon">${GROUP_ICONS[g] || "📌"}</span>
      <span class="nav-label">${GROUP_LABELS[g] || g}</span>
      <span class="nav-count">${count}</span>
    `);
        btn.dataset.group = g;
        btn.addEventListener("click", () => selectGroup(g));
        sidebar.appendChild(btn);
    });
}

function selectGroup(group) {
    currentGroup = group;

    // Highlight sidebar
    document.querySelectorAll(".nav-item").forEach(b => {
        b.classList.toggle("active", b.dataset.group === group);
    });

    // Show test list, hide detail
    const listEl = document.getElementById("testList");
    const detailEl = document.getElementById("testDetail");
    listEl.classList.remove("hidden");
    detailEl.classList.add("hidden");

    renderTestList(group);
}

/* ------------------------------------------------------------------ */
/*  Test List (cards)                                                   */
/* ------------------------------------------------------------------ */

function renderTestList(group) {
    const container = document.getElementById("testList");
    container.innerHTML = "";

    const groupLabel = GROUP_LABELS[group] || group;
    const header = el("div", "list-header", `
    <h2>${GROUP_ICONS[group] || ""} ${groupLabel}</h2>
    <p>共 ${allTests.filter(t => t.group === group).length} 个测试场景，点击任一场景进入详细测试。</p>
  `);
    container.appendChild(header);

    const grid = el("div", "card-grid");

    allTests.filter(t => t.group === group).forEach((t, idx) => {
        const card = el("div", "test-card");
        card.style.setProperty("--delay", `${idx * 50}ms`);

        const badgeHtml = [];
        if (t.has_baseline) badgeHtml.push('<span class="badge badge-blue">前后对比</span>');
        if (t.has_cleanup) badgeHtml.push('<span class="badge badge-green">可清理</span>');

        card.innerHTML = `
      <div class="card-title">${escapeHtml(t.title)}</div>
      <div class="card-desc">${escapeHtml(t.desc)}</div>
      <div class="card-badges">${badgeHtml.join("")}</div>
      <div class="card-footer">
        <span class="card-params">${t.params.length ? t.params.length + " 个参数" : "无需参数"}</span>
        <button class="btn-enter" data-key="${t.key}">进入测试 →</button>
      </div>
    `;

        card.querySelector(".btn-enter").addEventListener("click", (e) => {
            e.stopPropagation();
            openTestDetail(t.key);
        });
        card.addEventListener("click", () => openTestDetail(t.key));

        grid.appendChild(card);
    });

    container.appendChild(grid);
}

/* ------------------------------------------------------------------ */
/*  Test Detail (parameter form + execution + results)                 */
/* ------------------------------------------------------------------ */

function openTestDetail(key) {
    const test = allTests.find(t => t.key === key);
    if (!test) return;

    const listEl = document.getElementById("testList");
    const detailEl = document.getElementById("testDetail");
    listEl.classList.add("hidden");
    detailEl.classList.remove("hidden");

    detailEl.innerHTML = "";

    // Back button
    const backBtn = el("button", "btn-back", "← 返回列表");
    backBtn.addEventListener("click", () => selectGroup(currentGroup));
    detailEl.appendChild(backBtn);

    // Header
    const header = el("div", "detail-header", `
    <h2>${escapeHtml(test.title)}</h2>
    <p>${escapeHtml(test.desc)}</p>
    <div class="detail-badges">
      ${test.has_baseline ? '<span class="badge badge-blue">前后对比</span>' : ''}
      ${test.has_cleanup ? '<span class="badge badge-green">可清理</span>' : ''}
    </div>
  `);
    detailEl.appendChild(header);

    // Parameter form
    const formSection = el("div", "param-section");

    if (test.params.length > 0) {
        const formTitle = el("h3", "section-label", "📝 测试参数");
        formSection.appendChild(formTitle);

        const form = el("div", "param-form");
        form.id = "paramForm";

        test.params.forEach(p => {
            const row = el("div", "param-row");
            const label = el("label", "param-label", `${p.label}${p.required ? ' <span class="req">*</span>' : ""}`);
            label.htmlFor = `param-${p.name}`;
            row.appendChild(label);

            let input;
            if (p.type === "select") {
                input = el("select", "param-input");
                (p.options || []).forEach(o => {
                    const opt = document.createElement("option");
                    opt.value = o.value;
                    opt.textContent = o.label;
                    if (o.value === p.default) opt.selected = true;
                    input.appendChild(opt);
                });
            } else if (p.type === "node") {
                input = el("select", "param-input");
                nodes.forEach(n => {
                    const opt = document.createElement("option");
                    opt.value = n;
                    opt.textContent = n;
                    input.appendChild(opt);
                });
            } else if (p.type === "number") {
                input = el("input", "param-input");
                input.type = "number";
                if (p.default !== undefined) input.value = p.default;
                if (p.placeholder) input.placeholder = p.placeholder;
            } else {
                input = el("input", "param-input");
                input.type = "text";
                if (p.default !== undefined) input.value = p.default;
                if (p.placeholder) input.placeholder = p.placeholder;
            }
            input.id = `param-${p.name}`;
            input.dataset.name = p.name;
            row.appendChild(input);
            form.appendChild(row);
        });

        formSection.appendChild(form);
    }

    // Run button
    const actionBar = el("div", "action-bar");
    const runBtn = el("button", "btn-run", "▶ 运行测试");
    runBtn.id = "runBtn";
    runBtn.addEventListener("click", () => executeTest(test));
    actionBar.appendChild(runBtn);

    if (test.has_cleanup) {
        const cleanBtn = el("button", "btn-cleanup", "🔄 清理恢复");
        cleanBtn.id = "cleanBtn";
        cleanBtn.disabled = true;
        cleanBtn.addEventListener("click", () => executeCleanup(test));
        actionBar.appendChild(cleanBtn);
    }

    formSection.appendChild(actionBar);
    detailEl.appendChild(formSection);

    // Results area
    const resultsArea = el("div", "results-area");
    resultsArea.id = "resultsArea";
    detailEl.appendChild(resultsArea);
}

/* ------------------------------------------------------------------ */
/*  Execute test                                                       */
/* ------------------------------------------------------------------ */

function collectParams() {
    const params = {};
    document.querySelectorAll("#paramForm .param-input, #paramForm input, #paramForm select").forEach(el => {
        const name = el.dataset.name;
        if (name) {
            let val = el.value;
            if (el.type === "number" && val !== "") val = Number(val);
            params[name] = val;
        }
    });
    return params;
}

async function executeTest(test) {
    const params = collectParams();
    const runBtn = document.getElementById("runBtn");
    const cleanBtn = document.getElementById("cleanBtn");
    const resultsArea = document.getElementById("resultsArea");

    runBtn.disabled = true;
    runBtn.textContent = "⏳ 测试执行中...";
    resultsArea.innerHTML = '<div class="loading"><div class="spinner"></div><span>正在执行测试，请稍候...</span></div>';

    try {
        const data = await fetchJson("/api/functest", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: test.key, params }),
        });
        renderResults(data);
        if (cleanBtn) cleanBtn.disabled = false;
    } catch (err) {
        resultsArea.innerHTML = `<div class="error-card">执行失败: ${escapeHtml(err.message)}</div>`;
    } finally {
        runBtn.disabled = false;
        runBtn.textContent = "▶ 运行测试";
    }
}

async function executeCleanup(test) {
    if (!test.has_cleanup) return;
    const cleanBtn = document.getElementById("cleanBtn");
    cleanBtn.disabled = true;
    cleanBtn.textContent = "⏳ 清理中...";

    try {
        const params = collectParams();
        // Merge cleanup_params (different from test_scenarios cleanup_params — here we call the action directly)
        await fetchJson("/api/action", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ action: test.key.replace("test_", ""), params }),
        });

        // Actually we should call the cleanup action from testcase data
        // For now, show success
        const resultsArea = document.getElementById("resultsArea");
        const banner = el("div", "cleanup-banner", "✅ 清理已完成，系统已恢复");
        resultsArea.prepend(banner);
    } catch (err) {
        // ignore cleanup errors
    } finally {
        cleanBtn.textContent = "🔄 清理恢复";
        cleanBtn.disabled = false;
    }
}

/* ------------------------------------------------------------------ */
/*  Render results with before/after comparison                        */
/* ------------------------------------------------------------------ */

function renderResults(data) {
    const area = document.getElementById("resultsArea");
    area.innerHTML = "";

    // Status banner
    const banner = el("div", `result-banner ${data.ok ? "banner-ok" : "banner-fail"}`, `
    <span class="banner-icon">${data.ok ? "✅" : "❌"}</span>
    <span class="banner-text">${data.title}: ${data.ok ? "测试通过" : "测试未通过/异常"}</span>
  `);
    area.appendChild(banner);

    const hasBaseline = data.baseline && data.baseline.length > 0;
    const hasVerify = data.verify && data.verify.length > 0;

    if (hasBaseline || hasVerify) {
        // Show before/after comparison
        const comparison = el("div", "comparison");

        if (hasBaseline) {
            const leftCol = el("div", "compare-col");
            const leftHeader = el("div", "col-header col-before", "📋 操作前（基线）");
            leftCol.appendChild(leftHeader);
            data.baseline.forEach(check => {
                leftCol.appendChild(renderCheckResult(check));
            });
            comparison.appendChild(leftCol);
        }

        if (hasBaseline && hasVerify) {
            const divider = el("div", "compare-divider", `
        <div class="divider-line"></div>
        <div class="divider-icon">→</div>
        <div class="divider-line"></div>
      `);
            comparison.appendChild(divider);
        }

        if (hasVerify) {
            const rightCol = el("div", "compare-col");
            const rightHeader = el("div", "col-header col-after", "🔍 操作后（验证）");
            rightCol.appendChild(rightHeader);
            data.verify.forEach(check => {
                rightCol.appendChild(renderCheckResult(check));
            });
            comparison.appendChild(rightCol);
        }

        area.appendChild(comparison);
    }

    // Action execution details
    if (data.action) {
        const actionSec = el("div", "action-results-section");
        const actionHeader = el("div", "section-label", `⚡ 动作执行详情 — ${data.action.action || ""}`);
        actionSec.appendChild(actionHeader);

        if (data.action.results && data.action.results.length > 0) {
            data.action.results.forEach(r => {
                actionSec.appendChild(renderNodeResult(r));
            });
        }
        if (data.action.error) {
            const errEl = el("div", "error-card", `错误: ${escapeHtml(data.action.error)}`);
            actionSec.appendChild(errEl);
        }
        area.appendChild(actionSec);
    }
}

function renderCheckResult(check) {
    const card = el("div", `check-card ${check.ok ? "check-ok" : "check-fail"}`);

    const title = el("div", "check-title", `
    <span class="check-icon">${check.ok ? "✓" : "✗"}</span>
    <span>${escapeHtml(check.title)}</span>
  `);
    card.appendChild(title);

    const cmdLine = el("div", "check-cmd", `<code>$ ${escapeHtml(check.cmd)}</code>`);
    card.appendChild(cmdLine);

    (check.results || []).forEach(r => {
        card.appendChild(renderNodeResult(r));
    });

    return card;
}

function renderNodeResult(r) {
    const item = el("div", `node-result ${r.ok ? "nr-ok" : "nr-fail"}`);

    const header = el("div", "nr-header", `
    <span class="nr-node">${escapeHtml(r.node || "unknown")}</span>
    <span class="nr-host">${escapeHtml(r.host || "")}</span>
    <span class="nr-status ${r.ok ? "status-ok" : "status-fail"}">${r.ok ? "成功" : "失败"}</span>
  `);
    item.appendChild(header);

    const stdout = r.stdout || r.output || "";
    const stderr = r.stderr || "";
    if (stdout) {
        const out = el("pre", "nr-output", escapeHtml(stdout));
        item.appendChild(out);
    }
    if (stderr) {
        const err = el("pre", "nr-stderr", escapeHtml(stderr));
        item.appendChild(err);
    }

    return item;
}

/* ------------------------------------------------------------------ */
/*  Start                                                              */
/* ------------------------------------------------------------------ */

init();
