const nodesEl = document.getElementById("nodes");
const actionsContainer = document.getElementById("actionsContainer");
const historyEl = document.getElementById("history");
const healthEl = document.getElementById("health");
const nodeListEl = document.getElementById("nodeList");
const refreshBtn = document.getElementById("refreshConfig");
const clearBtn = document.getElementById("clearHistory");
const outputLimitEl = document.getElementById("outputLimit");
const kvmTestToggle = document.getElementById("kvmTestToggle");
const recoverAllBtn = document.getElementById("recoverAll");

let configCache = null;
let actionTitleMap = {};

function appendHistory(item) {
  historyEl.prepend(item);
}

function buildResult(res) {
  const result = document.createElement("div");
  result.className = "result";
  result.innerHTML = `
      <div class="result-head">
        <span class="result-node">${res.node}@${res.host}</span>
        <span class="result-meta">exit=${res.exit_code} | ${res.elapsed}s</span>
      </div>
      <div class="result-cmd">${res.cmd || ""}</div>
    `;

  const output = document.createElement("pre");
  output.className = "result-output";
  const outText = [res.stdout, res.stderr].filter(Boolean).join("\n");
  output.textContent = outText || "(无输出)";
  result.appendChild(output);

  if (res.truncated) {
    const badge = document.createElement("div");
    badge.className = "result-trunc";
    const meta = res.stdout_meta || {};
    const lines = meta.total_lines || "?";
    const chars = meta.total_chars || "?";
    badge.textContent = `输出已截断 (总计 ${lines} 行 / ${chars} 字符)`;
    result.appendChild(badge);
  }

  return result;
}

function appendResults(container, results = []) {
  results.forEach((r) => {
    container.appendChild(buildResult(r));
  });
}

function renderNodes(nodes) {
  nodesEl.innerHTML = "";
  nodeListEl.innerHTML = "";

  nodes.forEach((n) => {
    const card = document.createElement("div");
    card.className = "node-card";
    card.innerHTML = `
      <div class="node-title">${n.name}</div>
      <div class="node-meta">${n.host}:${n.port}</div>
      <div class="node-meta">角色: ${n.role}</div>
      <div class="node-tag ${n.local ? "tag-local" : "tag-ssh"}">${n.local ? "local" : "ssh"}</div>
    `;
    nodesEl.appendChild(card);

    const opt1 = document.createElement("option");
    opt1.value = n.name;
    opt1.label = `${n.name} (${n.host})`;
    nodeListEl.appendChild(opt1);

    const opt2 = document.createElement("option");
    opt2.value = n.host;
    opt2.label = `${n.host}`;
    nodeListEl.appendChild(opt2);
  });
}

function renderActionSections(groups, actions) {
  actionsContainer.innerHTML = "";
  actionTitleMap = actions.reduce((acc, action) => {
    acc[action.key] = action.title || action.key;
    return acc;
  }, {});
  const actionMap = actions.reduce((acc, action) => {
    if (!acc[action.group]) acc[action.group] = [];
    acc[action.group].push(action);
    return acc;
  }, {});

  groups.forEach((group, idx) => {
    const list = actionMap[group.key] || [];
    if (!list.length) return;

    const section = document.createElement("section");
    section.className = "action-section";
    section.style.setProperty("--delay", `${idx * 60}ms`);

    section.innerHTML = `
      <div class="section-head">
        <div>
          <h3>${group.title}</h3>
          <p>${group.desc}</p>
        </div>
      </div>
      <div class="action-grid"></div>
    `;

    const grid = section.querySelector(".action-grid");
    list.forEach((action) => {
      grid.appendChild(renderActionCard(action));
    });

    actionsContainer.appendChild(section);
  });
}

function renderActionCard(action) {
  const card = document.createElement("div");
  card.className = `action-card ${action.danger ? "danger" : ""}`;

  const form = document.createElement("div");
  form.className = "action-form";

  (action.params || []).forEach((param) => {
    form.appendChild(renderField(param));
  });

  const btn = document.createElement("button");
  btn.textContent = "执行";
  btn.className = "primary";
  btn.addEventListener("click", () => {
    const params = collectParams(form);
    runAction(action.key, action.title, params, btn);
  });

  const quickSpec = getQuickActionSpec(action.key);
  let quickBtn = null;
  if (quickSpec) {
    quickBtn = document.createElement("button");
    quickBtn.textContent = quickSpec.label;
    quickBtn.addEventListener("click", () => {
      const quickTitle = actionTitleMap[quickSpec.action] || quickSpec.title || quickSpec.label;
      runAction(quickSpec.action, quickTitle, quickSpec.params || {}, quickBtn);
    });
  }

  const dangerBadge = action.danger ? '<span class="danger-badge">高风险</span>' : "";
  card.innerHTML = `
    <div class="action-title">${action.title}${dangerBadge}</div>
    <div class="action-desc">${action.desc || ""}</div>
  `;
  card.appendChild(form);

  const footer = document.createElement("div");
  footer.className = "action-footer";
  if (quickBtn) footer.appendChild(quickBtn);
  footer.appendChild(btn);
  card.appendChild(footer);

  if (action.key === "vm_network") {
    bindVmNetworkHints(form);
  }

  return card;
}

function renderField(param) {
  const wrapper = document.createElement("label");
  wrapper.className = "field";

  const label = document.createElement("span");
  label.className = "field-label";
  label.textContent = param.label + (param.required ? "" : " (可选)");

  let input;
  if (param.type === "select") {
    input = document.createElement("select");
    (param.options || []).forEach((opt) => {
      const option = document.createElement("option");
      option.value = opt.value;
      option.textContent = opt.label;
      input.appendChild(option);
    });
    if (param.default !== undefined) input.value = param.default;
  } else if (param.type === "number") {
    input = document.createElement("input");
    input.type = "number";
    if (param.default !== undefined) input.value = param.default;
  } else if (param.type === "node") {
    input = document.createElement("input");
    input.setAttribute("list", "nodeList");
    input.placeholder = param.placeholder || "slave1 / 192.168.1.x";
  } else {
    input = document.createElement("input");
    input.type = "text";
    if (param.default !== undefined) input.value = param.default;
  }

  if (param.placeholder && param.type !== "node") {
    input.placeholder = param.placeholder;
  }

  input.dataset.param = param.name;
  input.dataset.type = param.type || "text";

  wrapper.appendChild(label);
  wrapper.appendChild(input);

  if (param.help) {
    const help = document.createElement("small");
    help.className = "field-help";
    help.textContent = param.help;
    wrapper.appendChild(help);
  }

  return wrapper;
}

function collectParams(form) {
  const params = {};
  form.querySelectorAll("[data-param]").forEach((input) => {
    const name = input.dataset.param;
    const raw = input.value;
    const value = raw !== undefined ? raw.toString().trim() : "";
    if (value !== "") {
      params[name] = value;
    }
  });
  return params;
}

function bindVmNetworkHints(form) {
  const typeSelect = form.querySelector('[data-param="net_type"]');
  const paramInput = form.querySelector('[data-param="net_param"]');
  if (!typeSelect || !paramInput) return;

  const hints = {
    delay: "100ms",
    loss: "10%",
    partition: "8080",
    corrupt: "1%",
    clear: "(无需参数)",
  };

  const updateHint = () => {
    const type = typeSelect.value;
    paramInput.placeholder = hints[type] || "参数";
    if (type === "clear") {
      paramInput.disabled = true;
      paramInput.value = "";
    } else {
      paramInput.disabled = false;
    }
  };

  typeSelect.addEventListener("change", updateHint);
  updateHint();
}

function getQuickActionSpec(actionKey) {
  const map = {
    delay: { action: "delay_clear", label: "清理" },
    loss: { action: "loss_clear", label: "清理" },
    reorder: { action: "reorder_clear", label: "清理" },
    isolate: { action: "isolate_clear", label: "清理" },
    mem_stress: { action: "mem_stress_clear", label: "清理" },
    disk_fill: { action: "disk_fill_clear", label: "清理" },
    vm_network: { action: "vm_network", label: "清理", params: { net_type: "clear" } },
  };
  return map[actionKey] || null;
}

async function fetchJson(url, options = {}) {
  const res = await fetch(url, options);
  if (!res.ok) {
    const data = await res.json().catch(() => ({}));
    throw new Error(data.detail || `请求失败: ${res.status}`);
  }
  return res.json();
}

async function runAction(actionKey, title, params, btn) {
  const startedAt = new Date();
  const btnText = btn ? btn.textContent : "";
  if (btn) {
    btn.textContent = "执行中...";
    btn.disabled = true;
  }

  try {
    const testFlags = {
      kvm: Boolean(kvmTestToggle && kvmTestToggle.checked),
    };
    const data = await fetchJson("/api/action", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: actionKey, params, tests: testFlags }),
    });

    const entry = buildHistoryEntry(title, actionKey, data, startedAt);
    appendHistory(entry);
  } catch (err) {
    const entry = buildErrorEntry(title, actionKey, err, startedAt);
    appendHistory(entry);
  } finally {
    if (btn) {
      btn.textContent = btnText;
      btn.disabled = false;
    }
  }
}

async function runRecoveryAll() {
  if (!recoverAllBtn) return;
  const plan = [
    { action: "delay_clear" },
    { action: "loss_clear" },
    { action: "reorder_clear" },
    { action: "isolate_clear" },
    { action: "mem_stress_clear" },
    { action: "disk_fill_clear" },
    { action: "vm_network", params: { net_type: "clear" } },
    { action: "kvm_clear" },
  ];

  const btnText = recoverAllBtn.textContent;
  recoverAllBtn.textContent = "恢复中...";
  recoverAllBtn.disabled = true;

  for (const step of plan) {
    if (!actionTitleMap[step.action]) continue;
    const startedAt = new Date();
    const title = `一键恢复 - ${actionTitleMap[step.action] || step.action}`;
    try {
      const data = await fetchJson("/api/action", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          action: step.action,
          params: step.params || {},
          tests: { kvm: false },
        }),
      });
      const entry = buildHistoryEntry(title, step.action, data, startedAt);
      appendHistory(entry);
    } catch (err) {
      const entry = buildErrorEntry(title, step.action, err, startedAt);
      appendHistory(entry);
    }
  }

  recoverAllBtn.textContent = btnText;
  recoverAllBtn.disabled = false;
}

function buildHistoryEntry(title, actionKey, data, startedAt) {
  const item = document.createElement("div");
  item.className = `history-item ${data.ok ? "ok" : "bad"}`;

  const header = document.createElement("div");
  header.className = "history-head";
  header.innerHTML = `
    <div>
      <div class="history-title">${title}</div>
      <div class="history-meta">${startedAt.toLocaleString()} | 动作: ${actionKey}</div>
    </div>
    <div class="history-status">${data.ok ? "成功" : "失败"}</div>
  `;

  const body = document.createElement("div");
  body.className = "history-body";

  appendResults(body, data.results || []);

  if (Array.isArray(data.tests) && data.tests.length) {
    const testSection = document.createElement("div");
    testSection.className = "test-section";
    testSection.innerHTML = `<div class="test-head">自动测试</div>`;

    data.tests.forEach((test) => {
      const card = document.createElement("div");
      card.className = `test-card ${test.ok ? "ok" : "bad"}`;
      card.innerHTML = `
        <div class="test-card-head">
          <div class="test-title">${test.title || "自动测试"}</div>
          <div class="test-status">${test.ok ? "成功" : "失败"}</div>
        </div>
      `;

      const testBody = document.createElement("div");
      testBody.className = "test-body";
      appendResults(testBody, test.results || []);
      card.appendChild(testBody);
      testSection.appendChild(card);
    });

    body.appendChild(testSection);
  }

  item.appendChild(header);
  item.appendChild(body);
  return item;
}

function buildErrorEntry(title, actionKey, err, startedAt) {
  const item = document.createElement("div");
  item.className = "history-item bad";
  item.innerHTML = `
    <div class="history-head">
      <div>
        <div class="history-title">${title}</div>
        <div class="history-meta">${startedAt.toLocaleString()} | 动作: ${actionKey}</div>
      </div>
      <div class="history-status">失败</div>
    </div>
    <div class="history-body">
      <pre class="result-output">${err.message || "请求失败"}</pre>
    </div>
  `;
  return item;
}

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

async function loadConfig() {
  configCache = await fetchJson("/api/config");
  renderNodes(configCache.nodes || []);
  renderActionSections(configCache.groups || [], configCache.actions || []);
  const outputCfg = configCache.output || {};
  const maxLines =
    typeof outputCfg.max_lines === "number" ? outputCfg.max_lines : 200;
  const maxChars =
    typeof outputCfg.max_chars === "number" ? outputCfg.max_chars : 8000;
  if ((maxLines || 0) <= 0 && (maxChars || 0) <= 0) {
    outputLimitEl.textContent = "输出限制: 无限制";
  } else {
    outputLimitEl.textContent = `输出限制: ${maxLines} 行 / ${maxChars} 字符`;
  }
}

refreshBtn.addEventListener("click", () => {
  loadConfig();
});

clearBtn.addEventListener("click", () => {
  historyEl.innerHTML = "";
});

if (recoverAllBtn) {
  recoverAllBtn.addEventListener("click", () => {
    runRecoveryAll();
  });
}

(async function init() {
  await healthCheck();
  await loadConfig();
})();
