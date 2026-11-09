"use strict";

const el = (id) => document.getElementById(id);
const state = {
  status: null,
  lastUid: "",
  browserPath: "/sdcard",
  mountPoint: "/sdcard",
  busy: false,
};

function toast(message, isError = false) {
  const node = el("toast");
  node.textContent = message;
  node.classList.toggle("error", isError);
  node.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => {
    node.hidden = true;
  }, 3200);
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  let body = null;
  try {
    body = await response.json();
  } catch {
    body = null;
  }
  if (!response.ok) {
    throw new Error(body && body.error ? body.error : `HTTP ${response.status}`);
  }
  return body;
}

function setPill(id, ok, label) {
  const node = el(id);
  node.textContent = `${label}: ${ok ? "ready" : "unavailable"}`;
  node.classList.toggle("ok", ok);
  node.classList.toggle("bad", !ok);
}

function formatTime(ms) {
  if (!Number.isFinite(ms) || ms <= 0) return "0:00";
  const total = Math.floor(ms / 1000);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}

function updateStatus(status) {
  state.status = status;
  state.mountPoint = status.mount_point || "/sdcard";
  setPill("pill-sd", status.sd_mounted, "SD card");
  setPill("pill-rfid", status.rfid_ready, "RFID");

  const player = status.player || {};
  const playing = player.state === "playing";
  el("btn-toggle").textContent = playing ? "\u23F8" : "\u25B6";
  el("now-title").textContent = player.title || player.path || "Nothing playing";
  el("now-detail").textContent =
    player.count > 0
      ? `${player.state} \u00B7 ${formatTime(player.position_ms)} \u00B7 track ${player.index + 1}/${player.count}`
      : player.state || "";

  const progress = player.count > 0 ? ((player.index + 1) / player.count) * 100 : 0;
  el("progress-bar").style.width = `${Math.min(100, Math.max(0, progress))}%`;

  const volume = el("volume");
  if (document.activeElement !== volume) {
    volume.value = status.volume ?? 0;
  }
  el("volume-value").textContent = volume.value;

  const tag = status.last_tag || {};
  if (tag.uid) {
    el("last-tag").textContent = tag.assigned
      ? `${tag.uid} \u2192 ${tag.name || tag.path}`
      : `${tag.uid} (not assigned)`;
  } else {
    el("last-tag").textContent = "No card scanned yet";
  }

  if (tag.uid && tag.uid !== state.lastUid) {
    state.lastUid = tag.uid;
    el("tag-uid").value = tag.uid;
    el("tag-path").value = tag.path || "";
    el("tag-name").value = tag.name || "";
  }
}

async function refreshStatus() {
  if (state.busy) return;
  try {
    updateStatus(await api("/api/status"));
  } catch (error) {
    console.error(error);
  }
}

async function control(action, extra = {}) {
  try {
    await api("/api/control", {
      method: "POST",
      body: JSON.stringify({ action, ...extra }),
    });
    await refreshStatus();
  } catch (error) {
    toast(error.message, true);
  }
}

async function playPath(path) {
  try {
    await api("/api/play", {
      method: "POST",
      body: JSON.stringify({ path }),
    });
    toast(`Playing ${path}`);
    await refreshStatus();
  } catch (error) {
    toast(error.message, true);
  }
}

async function browse(path) {
  try {
    const data = await api(`/api/fs?path=${encodeURIComponent(path)}`);
    state.browserPath = data.path || path;
    el("browser-path").textContent = state.browserPath;
    const list = el("browser");
    list.replaceChildren();

    if (!data.entries || data.entries.length === 0) {
      const item = document.createElement("li");
      item.className = "muted";
      item.textContent = "No folders or supported audio files";
      list.append(item);
      return;
    }

    for (const entry of data.entries) {
      const item = document.createElement("li");

      const name = document.createElement("button");
      name.className = "small";
      name.style.border = "none";
      name.style.background = "transparent";
      name.textContent = entry.dir ? `\u{1F4C1} ${entry.name}` : entry.name;
      name.title = entry.path;
      name.addEventListener("click", () => {
        if (entry.dir) {
          browse(entry.path);
        } else {
          el("tag-path").value = entry.path;
          if (!el("tag-name").value) el("tag-name").value = entry.name;
          playPath(entry.path);
        }
      });

      const actions = document.createElement("div");
      actions.className = "actions";

      const use = document.createElement("button");
      use.className = "small";
      use.textContent = "Use";
      use.addEventListener("click", () => {
        el("tag-path").value = entry.path;
        if (!el("tag-name").value) {
          el("tag-name").value = entry.name;
        }
      });

      const play = document.createElement("button");
      play.className = "small";
      play.textContent = "Play";
      play.disabled = !entry.playable && !entry.dir;
      play.addEventListener("click", () => playPath(entry.path));

      actions.append(use, play);
      item.append(name, actions);
      list.append(item);
    }
  } catch (error) {
    toast(error.message, true);
  }
}

async function refreshTags() {
  try {
    const tags = await api("/api/tags");
    const list = el("tags");
    list.replaceChildren();
    if (tags.length === 0) {
      const item = document.createElement("li");
      item.className = "muted";
      item.textContent = "No cards assigned yet";
      list.append(item);
      return;
    }
    for (const tag of tags) {
      const item = document.createElement("li");
      const text = document.createElement("div");
      text.className = "name";
      const title = document.createElement("div");
      title.textContent = tag.name || tag.path;
      const sub = document.createElement("div");
      sub.className = "sub mono";
      sub.textContent = `${tag.uid} \u00B7 ${tag.path}`;
      text.append(title, sub);

      const actions = document.createElement("div");
      actions.className = "actions";
      const play = document.createElement("button");
      play.className = "small";
      play.textContent = "Play";
      play.addEventListener("click", () => playPath(tag.path));

      const remove = document.createElement("button");
      remove.className = "small danger";
      remove.textContent = "Delete";
      remove.addEventListener("click", async () => {
        if (!confirm(`Remove assignment for ${tag.uid}?`)) return;
        try {
          await api(`/api/tags?uid=${encodeURIComponent(tag.uid)}`, { method: "DELETE" });
          await refreshTags();
          await refreshStatus();
        } catch (error) {
          toast(error.message, true);
        }
      });

      actions.append(play, remove);
      item.append(text, actions);
      list.append(item);
    }
  } catch (error) {
    toast(error.message, true);
  }
}

el("btn-previous").addEventListener("click", () => control("previous"));
el("btn-next").addEventListener("click", () => control("next"));
el("btn-stop").addEventListener("click", () => control("stop"));
el("btn-toggle").addEventListener("click", () => {
  const playing = state.status?.player?.state === "playing";
  control(playing ? "pause" : "resume");
});

el("volume").addEventListener("input", (event) => {
  el("volume-value").textContent = event.target.value;
});
el("volume").addEventListener("change", (event) => {
  control("volume", { volume: Number(event.target.value) });
});

el("assign-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const uid = el("tag-uid").value.trim();
  const path = el("tag-path").value.trim();
  const name = el("tag-name").value.trim();
  if (!uid || !path) {
    toast("Scan a card and choose a folder or file first", true);
    return;
  }
  try {
    await api("/api/tags", {
      method: "POST",
      body: JSON.stringify({ uid, path, name }),
    });
    toast("Assignment saved");
    await refreshTags();
  } catch (error) {
    toast(error.message, true);
  }
});

el("assign-play").addEventListener("click", () => {
  const path = el("tag-path").value.trim();
  if (path) playPath(path);
});

el("browser-up").addEventListener("click", () => {
  const current = state.browserPath.replace(/\/+$/, "");
  if (current === state.mountPoint) return;
  const parent = current.slice(0, current.lastIndexOf("/")) || state.mountPoint;
  browse(parent);
});
el("browser-root").addEventListener("click", () => browse(state.mountPoint));

refreshStatus().then(async () => {
  await Promise.all([browse(state.mountPoint), refreshTags()]);
});
setInterval(refreshStatus, 2000);
