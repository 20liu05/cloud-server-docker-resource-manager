"use strict";

const state = {
  images: [],
  containers: [],
  protected: [],
  user: "admin",
  busy: false,
  resourceTab: "containers",
  query: ""
};

const $ = (selector) => document.querySelector(selector);

function fmtBytes(bytes) {
  if (!Number.isFinite(bytes)) return "--";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(value >= 10 || unit === 0 ? 0 : 1)} ${units[unit]}`;
}

function pct(value) {
  if (!Number.isFinite(value)) return "0%";
  return `${Math.max(0, Math.min(100, value))}%`;
}

function setNotice(text, type = "info") {
  const el = $("#notice");
  el.textContent = text;
  el.style.borderColor = type === "error" ? "rgba(255, 107, 122, .45)" : "var(--line)";
  el.style.color = type === "error" ? "#ffd9de" : "var(--muted)";
}

async function api(path, body) {
  const res = await fetch(path, {
    method: body ? "POST" : "GET",
    headers: body ? { "content-type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined
  });
  const data = await res.json().catch(() => ({}));
  if (res.status === 401) throw new Error("认证已变更，请重新登录");
  if (res.status === 502) throw new Error("面板服务正在重启，请稍后刷新");
  if (!res.ok) throw new Error(data.error || `请求失败：${res.status}`);
  return data;
}

function setBusy(value) {
  state.busy = value;
  document.querySelectorAll("button, input, select").forEach((el) => {
    if (value) {
      if (!el.dataset.busyLock) {
        el.dataset.wasDisabled = el.disabled ? "1" : "0";
        el.dataset.busyLock = "1";
      }
      el.disabled = true;
      return;
    }
    if (!el.dataset.busyLock) return;
    el.disabled = el.dataset.wasDisabled === "1";
    delete el.dataset.wasDisabled;
    delete el.dataset.busyLock;
  });
}

function containerName(item) {
  return (item.names[0] || item.shortId || "").replace(/^\//, "");
}

function isProtected(item) {
  const name = containerName(item);
  return state.protected.includes(name) || state.protected.includes(item.id) || state.protected.includes(item.shortId);
}

function renderStats(data) {
  const cpu = data.host.cpu?.percent;
  $("#cpuValue").textContent = Number.isFinite(cpu) ? `${cpu}%` : "--";
  $("#cpuBar").style.setProperty("--value", pct(cpu || 0));

  const mem = data.host.memory;
  $("#memValue").textContent = `${mem.percent}%`;
  $("#memBar").style.setProperty("--value", pct(mem.percent));

  const disk = data.host.storage;
  const diskPercent = disk ? Number(String(disk.usePercent).replace("%", "")) : 0;
  $("#diskValue").textContent = disk ? disk.usePercent : "--";
  $("#diskBar").style.setProperty("--value", pct(diskPercent));

  $("#dockerValue").textContent = `${data.docker.running}/${data.docker.containers}`;
  $("#dockerSub").textContent = `镜像 ${data.docker.images} 个 / Docker ${data.docker.serverVersion || "--"}`;
}

function renderContainers() {
  const body = $("#containersBody");
  const query = state.query.toLowerCase();
  const rows = state.containers.map(normalizeContainer).filter((item) => {
    const text = `${containerName(item)} ${item.shortId} ${item.image} ${item.state} ${item.status}`.toLowerCase();
    return !query || text.includes(query);
  });
  if (!rows.length) {
    body.innerHTML = `<tr><td colspan="5"><div class="empty"><strong>没有匹配的容器</strong>调整搜索条件或创建一个新容器。</div></td></tr>`;
    return;
  }
  body.innerHTML = rows.map((item) => {
    const name = containerName(item);
    const ports = item.ports.map((port) => {
      if (port.PublicPort) return `${port.PublicPort}:${port.PrivatePort}/${port.Type}`;
      return `${port.PrivatePort}/${port.Type}`;
    }).join(", ") || "--";
    const running = item.state === "running";
    const protectedItem = isProtected(item);
    return `
      <tr>
        <td><strong>${escapeHtml(name)}</strong><br><span class="label">${item.shortId}</span>${protectedItem ? `<br><span class="tag protected">受保护</span>` : ""}</td>
        <td>${escapeHtml(item.image)}</td>
        <td><span class="tag ${running ? "running" : "exited"}">${escapeHtml(item.state)}</span><br><span class="label">${escapeHtml(item.status || "")}</span></td>
        <td>${escapeHtml(ports)}</td>
        <td>
          <div class="actions">
            <button onclick="containerAction('start', '${item.id}')" ${running || protectedItem ? "disabled" : ""}>启用</button>
            <button class="ghost" onclick="containerAction('stop', '${item.id}')" ${running && !protectedItem ? "" : "disabled"}>停用</button>
            <button class="danger" onclick="containerAction('delete', '${item.id}')" ${protectedItem ? "disabled" : ""}>删除</button>
            <button class="ghost" onclick="protectContainer('${item.id}')" ${protectedItem ? "disabled" : ""}>保护</button>
          </div>
        </td>
      </tr>
    `;
  }).join("");
}

function renderImages() {
  const body = $("#imagesBody");
  const query = state.query.toLowerCase();
  const select = $("#imageSelect");
  select.innerHTML = state.images.map(normalizeImage).map((item) => {
    const tag = item.repoTags[0] || item.id;
    return `<option value="${escapeAttr(tag)}">${escapeHtml(tag)}</option>`;
  }).join("");

  const rows = state.images.map(normalizeImage).filter((item) => {
    const text = `${item.repoTags.join(" ")} ${item.shortId} ${item.id}`.toLowerCase();
    return !query || text.includes(query);
  });

  if (!rows.length) {
    body.innerHTML = `<tr><td colspan="4"><div class="empty"><strong>没有匹配的镜像</strong>调整搜索条件或拉取一个新镜像。</div></td></tr>`;
    return;
  }

  body.innerHTML = rows.map((item) => {
    const tag = item.repoTags[0] || item.id;
    return `
      <tr>
        <td>${escapeHtml((item.repoTags || []).join(", ") || "<none>")}</td>
        <td>${escapeHtml(item.shortId)}</td>
        <td>${fmtBytes(item.size)}</td>
        <td><button class="danger" onclick="deleteImage('${encodeURIComponent(tag)}')">删除镜像</button></td>
      </tr>
    `;
  }).join("");
}

function renderProtected() {
  const list = $("#protectedList");
  const select = $("#protectSelect");
  const containers = state.containers.map(normalizeContainer);
  $("#protectedCount").textContent = `${state.protected.length} 个保护`;
  select.innerHTML = containers.map((item) => {
    const name = containerName(item);
    const label = `${name} (${item.shortId})`;
    return `<option value="${escapeAttr(item.id)}" ${isProtected(item) ? "disabled" : ""}>${escapeHtml(label)}</option>`;
  }).join("");

  if (!state.protected.length) {
    list.innerHTML = `<div class="empty">暂无受保护容器</div>`;
    return;
  }

  list.innerHTML = state.protected.map((name) => `
    <div class="list-row">
      <div>
        <strong>${escapeHtml(name)}</strong>
        <div class="mini">控制按钮已锁定</div>
      </div>
      <button class="ghost" onclick="removeProtection('${encodeURIComponent(name)}')">移除</button>
    </div>
  `).join("");
}

function renderResourceSummary() {
  const containers = state.containers.map(normalizeContainer);
  const running = containers.filter((item) => item.state === "running").length;
  const stopped = containers.length - running;
  $("#resourceSummary").textContent = `容器 ${containers.length} 个，运行 ${running} 个，停止 ${stopped} 个；镜像 ${state.images.length} 个`;
}

function normalizeContainer(item) {
  return {
    id: item.id || item.Id,
    shortId: item.shortId || (item.Id || "").slice(0, 12),
    names: item.names || item.Names || [],
    image: item.image || item.Image || "",
    imageId: item.imageId || item.ImageID || "",
    command: item.command || item.Command || "",
    created: item.created || item.Created || 0,
    state: item.state || item.State || "",
    status: item.status || item.Status || "",
    ports: item.ports || item.Ports || []
  };
}

function normalizeImage(item) {
  const id = item.id || item.Id || "";
  return {
    id,
    shortId: item.shortId || id.replace("sha256:", "").slice(0, 12),
    repoTags: item.repoTags || item.RepoTags || [],
    created: item.created || item.Created || 0,
    size: item.size || item.Size || 0
  };
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;"
  })[char]);
}

function escapeAttr(value) {
  return escapeHtml(value).replace(/`/g, "&#96;");
}

async function refresh() {
  try {
    const data = await api("/api/overview");
    state.images = data.images;
    state.containers = data.containers;
    state.protected = data.config?.protected || [];
    state.user = data.config?.user || "admin";
    renderStats(data);
    renderContainers();
    renderImages();
    renderResourceSummary();
    renderProtected();
    $("#accountUser").value = state.user;
    if (state.busy) setBusy(true);
    $("#lastUpdated").textContent = new Date(data.generatedAt).toLocaleString();
    setNotice(`已连接 ${data.host.name}，面板状态正常。`);
  } catch (err) {
    setNotice(err.message, "error");
    throw err;
  }
}

function setOpsTab(name) {
  document.querySelectorAll("[data-ops-tab]").forEach((button) => {
    button.classList.toggle("active", button.dataset.opsTab === name);
  });
  document.querySelectorAll("[data-ops-view]").forEach((view) => {
    view.classList.toggle("active", view.dataset.opsView === name);
  });
}

function setResourceTab(name) {
  state.resourceTab = name;
  document.querySelectorAll("[data-resource-tab]").forEach((button) => {
    button.classList.toggle("active", button.dataset.resourceTab === name);
  });
  document.querySelectorAll("[data-resource-view]").forEach((view) => {
    view.classList.toggle("active", view.dataset.resourceView === name);
  });
}

function openSettings() {
  $("#accountUser").value = state.user;
  $("#settingsModal").classList.add("open");
  $("#settingsModal").setAttribute("aria-hidden", "false");
}

function closeSettings() {
  $("#settingsModal").classList.remove("open");
  $("#settingsModal").setAttribute("aria-hidden", "true");
  $("#accountForm").password.value = "";
}

async function refreshWithRetry(times = 8) {
  for (let i = 0; i < times; i++) {
    try {
      await refresh();
      return;
    } catch {
      await new Promise((resolve) => setTimeout(resolve, 650));
    }
  }
  await refresh();
}

async function containerAction(action, id) {
  if (action === "delete" && !confirm("确认删除该容器？")) return;
  setBusy(true);
  try {
    await api(`/api/container/${action}`, { id });
    setNotice("操作已提交，正在刷新...");
    setResourceTab("containers");
    await refreshWithRetry();
  } catch (err) {
    setNotice(err.message, "error");
  } finally {
    setBusy(false);
  }
}

async function deleteImage(encodedImage) {
  const image = decodeURIComponent(encodedImage);
  if (!confirm(`确认删除镜像 ${image}？`)) return;
  setBusy(true);
  try {
    await api("/api/image/delete", { image });
    setNotice("镜像删除已提交，正在刷新...");
    setResourceTab("images");
    await refreshWithRetry();
  } catch (err) {
    setNotice(err.message, "error");
  } finally {
    setBusy(false);
  }
}

async function protectContainer(id) {
  setBusy(true);
  try {
    await api("/api/protection/add", { id });
    setNotice("已加入保护，正在刷新...");
    await refreshWithRetry();
  } catch (err) {
    setNotice(err.message, "error");
  } finally {
    setBusy(false);
  }
}

async function removeProtection(encodedName) {
  const name = decodeURIComponent(encodedName);
  if (!confirm(`确认移除 ${name} 的保护？`)) return;
  setBusy(true);
  try {
    await api("/api/protection/remove", { name });
    setNotice("保护已移除，正在刷新...");
    await refreshWithRetry();
  } catch (err) {
    setNotice(err.message, "error");
  } finally {
    setBusy(false);
  }
}

$("#refreshBtn").addEventListener("click", () => refresh().catch(() => {}));
$("#settingsBtn").addEventListener("click", openSettings);
$("#closeSettingsBtn").addEventListener("click", closeSettings);
$("#cancelSettingsBtn").addEventListener("click", closeSettings);
$("#settingsModal").addEventListener("click", (event) => {
  if (event.target.id === "settingsModal") closeSettings();
});
document.querySelectorAll("[data-ops-tab]").forEach((button) => {
  button.addEventListener("click", () => setOpsTab(button.dataset.opsTab));
});
document.querySelectorAll("[data-resource-tab]").forEach((button) => {
  button.addEventListener("click", () => setResourceTab(button.dataset.resourceTab));
});
$("#resourceSearch").addEventListener("input", (event) => {
  state.query = event.target.value.trim();
  renderContainers();
  renderImages();
});

$("#createForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  setBusy(true);
  try {
    await api("/api/container/create", Object.fromEntries(form.entries()));
    event.currentTarget.reset();
    setNotice("容器已创建，正在刷新...");
    setResourceTab("containers");
    await refreshWithRetry();
  } catch (err) {
    setNotice(err.message, "error");
  } finally {
    setBusy(false);
  }
});

$("#pullForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  setBusy(true);
  try {
    setNotice("正在拉取镜像，请稍等...");
    await api("/api/image/pull", Object.fromEntries(form.entries()));
    event.currentTarget.reset();
    setResourceTab("images");
    await refreshWithRetry();
  } catch (err) {
    setNotice(err.message, "error");
  } finally {
    setBusy(false);
  }
});

$("#protectForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  setBusy(true);
  try {
    await api("/api/protection/add", Object.fromEntries(form.entries()));
    setNotice("已加入保护，正在刷新...");
    await refreshWithRetry();
  } catch (err) {
    setNotice(err.message, "error");
  } finally {
    setBusy(false);
  }
});

$("#accountForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  setBusy(true);
  try {
    await api("/api/account/update", Object.fromEntries(form.entries()));
    event.currentTarget.password.value = "";
    closeSettings();
    setNotice("账号密码已更新，请用新信息重新登录。");
  } catch (err) {
    setNotice(err.message, "error");
  } finally {
    setBusy(false);
  }
});

refresh().catch(() => {});
setInterval(() => {
  if (!state.busy) refresh().catch(() => {});
}, 5000);

window.containerAction = containerAction;
window.deleteImage = deleteImage;
window.protectContainer = protectContainer;
window.removeProtection = removeProtection;
