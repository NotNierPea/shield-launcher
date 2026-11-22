"use strict";

// the C++ host when running in the launcher, a stand-in when the page is opened in a browser
const host = window.chrome && window.chrome.webview;

const $ = (id) => document.getElementById(id);
const dock = document.querySelector(".dock");
const fill = $("play-fill");
const label = $("play-label");
const playButtons = [$("play-client"), $("play-vanilla"), $("play-dedicated")];
const volume = $("volume");
const closeOnPlay = $("close-on-play");
const language = $("language");

let launching = false;
let toastTimer = 0;

function send(message) {
  if (host) {
    host.postMessage(message);
  } else {
    previewHost(message);
  }
}

function toast(text, isError) {
  const el = $("toast");
  el.textContent = text;
  el.classList.toggle("error", !!isError);
  el.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove("show"), isError ? 6000 : 2800);
}

function setName(name) {
  $("player-name").textContent = name;
}

function paintVolume() {
  volume.style.setProperty("--val", volume.value + "%");
}

/* play */

// message is what the launcher is asked to do: a play of client or vanilla, or a dedicated launch
function startLaunch(message) {
  if (launching) return;
  launching = true;
  dock.classList.add("launching");
  playButtons.forEach((b) => (b.disabled = true));
  setProgress(0);
  send(message);
}

function setProgress(value) {
  fill.style.width = value + "%";
  label.textContent = "Starting " + value + "%";
}

function finishLaunch(ok, error, closing) {
  launching = false;
  dock.classList.remove("launching");
  playButtons.forEach((b) => (b.disabled = false));
  fill.style.width = "0";
  // back to the files' state, which may keep the client disabled
  renderPlayState();

  if (!ok) toast(error || "Failed to start the game!", true);
  else toast(closing ? "Game started, closing the launcher." : "Game started.");
}

/* dialogs */

function openDialog(id) {
  const dialog = $(id);
  if (!dialog.open) dialog.showModal();
  return dialog;
}

document.querySelectorAll("[data-close]").forEach((button) => {
  button.addEventListener("click", () => button.closest("dialog").close());
});

// a click on the dimmed area outside a dialog closes it
document.querySelectorAll("dialog").forEach((dialog) => {
  dialog.addEventListener("mousedown", (event) => {
    if (event.target === dialog) dialog.close();
  });
});

$("change-name").addEventListener("click", () => {
  const input = $("name-input");
  input.value = $("player-name").textContent;
  openDialog("name-dialog");
  input.select();
});

$("name-form").addEventListener("submit", (event) => {
  const name = $("name-input").value.trim();
  if (!name) {
    event.preventDefault();
    toast("Name cannot be empty!", true);
    return;
  }
  send({ action: "setName", name });
});

/* client settings, one row per value of project-bo4.json */

let settingsEntries = [];

function titleCase(key) {
  return key.replace(/_/g, " ").replace(/([a-z])([A-Z])/g, "$1 $2").replace(/^./, (c) => c.toUpperCase());
}

function renderSettings(entries) {
  settingsEntries = entries;
  const body = $("settings-body");
  body.textContent = "";

  let section = null;
  let sectionKey = null;

  entries.forEach((entry, index) => {
    const parent = entry.path.slice(0, -1).join(" / ");
    if (parent !== sectionKey) {
      sectionKey = parent;
      section = document.createElement("fieldset");
      section.className = "settings-section";
      const legend = document.createElement("legend");
      legend.textContent = parent ? parent.split(" / ").map(titleCase).join(" / ") : "General";
      section.append(legend);
      body.append(section);
    }

    const key = entry.path[entry.path.length - 1];
    const row = document.createElement("label");
    row.className = "settings-row";

    const name = document.createElement("span");
    name.className = "settings-key";
    name.textContent = titleCase(key);
    name.title = entry.path.join(".");
    row.append(name);

    if (entry.type === "bool") {
      const toggle = document.createElement("span");
      toggle.className = "toggle";
      const input = document.createElement("input");
      input.type = "checkbox";
      input.checked = entry.value === "true";
      input.dataset.index = index;
      const sw = document.createElement("span");
      sw.className = "switch";
      toggle.append(input, sw);
      row.append(toggle);
    } else {
      const input = document.createElement("input");
      input.className = "field";
      input.value = entry.value;
      input.dataset.index = index;
      input.spellcheck = false;
      if (entry.type !== "string") {
        input.inputMode = entry.type === "int" ? "numeric" : "decimal";
        input.pattern = entry.type === "int" ? "-?\\d+" : "-?\\d+(\\.\\d+)?([eE][-+]?\\d+)?";
        input.title = entry.type === "int" ? "A whole number" : "A number";
      }
      row.append(input);
    }

    section.append(row);
  });

  if (entries.length === 0) {
    body.textContent = "project-bo4.json has no settings yet.";
  }
}

$("open-settings").addEventListener("click", () => send({ action: "openSettings" }));

/* dedicated server, its section of project-bo4.json and the launch with -dedicated */

const dedicatedForm = $("dedicated-form");
const field = (name) => dedicatedForm.elements.namedItem(name);

// only the rows of the chosen lobby show, hidden ones are disabled so they don't block the form
function showLobbyRows() {
  const lobby = field("lobby").value;
  dedicatedForm.querySelectorAll("[data-lobby]").forEach((row) => {
    row.hidden = row.dataset.lobby !== lobby;
    row.querySelectorAll("input, select").forEach((input) => (input.disabled = row.hidden));
  });
}

function fillDedicated(values) {
  for (const [key, value] of Object.entries(values)) {
    const input = field(key);
    if (!input) continue;
    if (input.type === "checkbox") input.checked = value;
    else input.value = value;
  }
  showLobbyRows();
}

$("play-dedicated").addEventListener("click", () => {
  if (needsFiles()) openFiles();
  else send({ action: "openDedicated" });
});
field("lobby").addEventListener("change", showLobbyRows);

dedicatedForm.addEventListener("submit", (event) => {
  event.preventDefault();
  if (!dedicatedForm.reportValidity()) return;

  const values = {
    name: field("name").value.trim(),
    lobby: field("lobby").value,
    playlist: Number(field("playlist").value),
    rotation: field("rotation").value.trim(),
    lobby_seconds: Number(field("lobby_seconds").value),
    min_players: Number(field("min_players").value),
    draw: field("draw").value,
    max_fps: Number(field("max_fps").value),
    streaming: field("streaming").checked,
    background: field("background").checked,
    minimize: field("minimize").checked,
  };

  $("dedicated-dialog").close();
  startLaunch({ action: "launchDedicated", values });
});

$("settings-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  if (!form.reportValidity()) return;

  const entries = settingsEntries.map((entry, index) => {
    const input = form.querySelector(`[data-index="${index}"]`);
    const value = entry.type === "bool" ? String(input.checked) : input.value.trim();
    return { path: entry.path, type: entry.type, value };
  });
  send({ action: "saveSettings", entries });
});

$("settings-reset").addEventListener("click", () => openDialog("confirm-dialog"));

$("confirm-reset").addEventListener("click", () => {
  $("confirm-dialog").close();
  send({ action: "resetSettings" });
});

$("update-now").addEventListener("click", () => {
  const button = $("update-now");
  button.disabled = true;
  $("update-later").disabled = true;
  button.textContent = "Downloading…";
  send({ action: "update" });
});

function resetUpdateDialog() {
  $("update-now").disabled = false;
  $("update-later").disabled = false;
  $("update-now").textContent = "Update now";
}

/* game files: what the manifest lists, and one button that downloads everything missing */

let content = null; // the last check from the launcher
let contentTask = "check"; // "check", "download" or "launcher" while one runs (the launcher checks on start)
let contentProgress = null;
let filesOpenedOnce = false;
const openGroups = new Set(); // components whose file list is unfolded

function formatSize(bytes) {
  if (bytes >= 1024 ** 3) return (bytes / 1024 ** 3).toFixed(2) + " GB";
  if (bytes >= 1024 ** 2) return (bytes / 1024 ** 2).toFixed(1) + " MB";
  if (bytes >= 1024) return Math.round(bytes / 1024) + " KB";
  return bytes + " B";
}

const stateText = { installed: "Installed", missing: "Not downloaded", outdated: "Update" };

// something to download or unpack again (a package's folder can need fixing with nothing to download)
const hasWork = () => !!content && content.ok && content.components.some((group) => group.state !== "installed");

// the client can't start until every required file is there (an offline check doesn't block it)
function needsFiles() {
  return contentTask === "download" || (!!content && content.ok && !content.ready);
}

function openFiles() {
  renderFiles();
  openDialog("files-dialog");
}

function percent(progress) {
  return progress && progress.total ? Math.floor((progress.done / progress.total) * 100) : 0;
}

function renderPlayState() {
  if (launching) return;
  // the client waits for the check; vanilla needs none of our files
  const checking = contentTask === "check";
  $("play-client").disabled = checking;
  $("play-dedicated").disabled = checking;
  if (checking) {
    fill.style.width = "0";
    label.textContent = content || contentProgress ? "Checking files " + percent(contentProgress) + "%" : "Checking files…";
  } else if (contentTask === "download") {
    const value = percent(contentProgress);
    fill.style.width = value + "%";
    label.textContent = (contentProgress && contentProgress.phase === "installing" ? "Installing " : "Downloading ") + value + "%";
  } else {
    fill.style.width = "0";
    label.textContent = needsFiles() ? "Download files" : "Play Client";
  }
}

function renderChip() {
  const text = $("files-chip-text");
  const dot = $("files-dot");
  let state = "busy";
  if (contentTask === "check" && !content) text.textContent = "Checking game files…";
  else if (contentTask === "check") text.textContent = "Checking game files " + percent(contentProgress) + "%";
  else if (contentTask === "download") text.textContent = "Downloading " + percent(contentProgress) + "%";
  else if (contentTask === "launcher") text.textContent = "Updating launcher…";
  else if (!content) text.textContent = "Game files";
  else if (!content.ok) {
    text.textContent = "Couldn't check game files";
    state = "error";
  } else if (hasWork()) {
    text.textContent = content.download === 0 ? "Game files need fixing" : (content.ready ? "Update: " : "Download: ") + formatSize(content.download);
    state = content.ready ? "update" : "missing";
  } else {
    text.textContent = "Game files up to date";
    state = "ok";
  }
  dot.dataset.state = state;
}

function fileRow(file) {
  const row = document.createElement("div");
  row.className = "file-row";
  const path = document.createElement("span");
  path.className = "file-path";
  // a package is one zip unpacked into its folder
  path.textContent = file.extract || file.path;
  path.title = file.extract ? "Package of " + file.entries + " files (" + file.path + ")" : file.path;
  if (file.extract) {
    const tag = document.createElement("span");
    tag.className = "package-tag";
    tag.textContent = file.entries + " files";
    row.classList.add("package");
    path.append(tag);
  }
  const size = document.createElement("span");
  size.className = "file-size";
  size.textContent = file.state === "outdated" && file.download === 0 ? "unpack again"
    : file.state === "outdated" && file.download < file.size ? formatSize(file.download) + " of " + formatSize(file.size)
    : formatSize(file.size);
  const badge = document.createElement("span");
  badge.className = "badge " + file.state;
  badge.textContent = stateText[file.state];
  row.append(path, size, badge);
  return row;
}

function componentRow(group) {
  const box = document.createElement("div");
  box.className = "component" + (openGroups.has(group.id) ? " open" : "");

  const head = document.createElement("button");
  head.type = "button";
  head.className = "component-head";
  head.addEventListener("click", () => {
    if (openGroups.has(group.id)) openGroups.delete(group.id);
    else openGroups.add(group.id);
    box.classList.toggle("open");
  });

  const chevron = document.createElement("span");
  chevron.className = "chevron";
  chevron.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>';

  const text = document.createElement("span");
  text.className = "component-text";
  const name = document.createElement("span");
  name.className = "component-name";
  name.textContent = group.name + (group.required ? "" : " (optional)");
  const detail = document.createElement("small");
  const count = group.files.length + (group.files.length === 1 ? " file" : " files");
  detail.textContent = (group.description ? group.description + " · " : "") + count;
  text.append(name, detail);

  const size = document.createElement("span");
  size.className = "file-size";
  size.textContent = group.state === "installed" ? formatSize(group.size)
    : group.download === 0 ? "unpack again" : formatSize(group.download) + " to download";

  const badge = document.createElement("span");
  badge.className = "badge " + group.state;
  badge.textContent = stateText[group.state];

  head.append(chevron, text, size, badge);

  const list = document.createElement("div");
  list.className = "file-list";
  // files that need something first
  const order = { missing: 0, outdated: 1, installed: 2 };
  [...group.files].sort((a, b) => order[a.state] - order[b.state] || a.path.localeCompare(b.path)).forEach((file) => list.append(fileRow(file)));

  box.append(head, list);
  return box;
}

function renderFiles() {
  const busy = contentTask !== "";
  const body = $("files-body");
  const summary = $("files-summary");
  body.textContent = "";

  $("files-version").textContent = content && content.version ? "content " + content.version : "";

  if (!content) {
    summary.textContent = "Checking game files…";
  } else if (!content.ok) {
    summary.textContent = content.error || "The game files couldn't be checked.";
    summary.classList.add("error");
  } else {
    summary.classList.remove("error");
    const missing = content.components.filter((group) => group.state !== "installed");
    if (missing.length === 0) summary.textContent = "Everything is installed and up to date.";
    else if (content.download === 0) summary.textContent = "Some files need unpacking again, nothing to download.";
    else if (!content.ready) summary.textContent = "These files are needed to play. " + formatSize(content.download) + " to download.";
    else summary.textContent = "Updates are ready. " + formatSize(content.download) + " to download.";
    content.components.forEach((group) => body.append(componentRow(group)));
  }

  if (content && content.launcherUpdate) {
    const note = document.createElement("button");
    note.type = "button";
    note.className = "launcher-note";
    note.textContent = "Launcher " + content.launcherLatest + " is out, click to update it.";
    note.addEventListener("click", () => openUpdateDialog(content.launcherLatest, $("version").textContent));
    body.prepend(note);
  }

  const progress = $("files-progress");
  progress.hidden = !busy || !contentProgress;
  if (!progress.hidden) {
    const p = contentProgress;
    $("files-bar").style.width = percent(p) + "%";
    const phase = { checking: "Checking", downloading: "Downloading", installing: "Installing", launcher: "Downloading" }[p.phase] || "";
    $("files-progress-file").textContent = phase + (p.file ? " " + p.file : "");
    $("files-progress-numbers").textContent = p.phase === "installing"
      ? p.done + " / " + p.total + " files"
      : formatSize(p.done) + " / " + formatSize(p.total) + (p.speed > 0 && p.phase !== "checking" ? " · " + formatSize(p.speed) + "/s" : "");
  }

  const download = $("files-download");
  const canDownload = hasWork();
  download.hidden = (busy && contentTask !== "check") || (!!content && content.ok && !canDownload);
  download.disabled = busy || !canDownload;
  download.textContent = !canDownload ? "Download all" : content.download === 0 ? "Fix files" : "Download all (" + formatSize(content.download) + ")";
  $("files-cancel").hidden = !busy || contentTask === "launcher";
  $("files-verify").textContent = content && !content.ok ? "Check again" : "Verify files";
  $("files-verify").disabled = busy;
}

function renderContent() {
  renderChip();
  renderPlayState();
  if ($("files-dialog").open) renderFiles();
}

function openUpdateDialog(latest, current) {
  resetUpdateDialog();
  $("update-text").textContent = `Launcher ${latest} is out, you have ${current}. It restarts itself once updated.`;
  openDialog("update-dialog");
}

$("files-chip").addEventListener("click", openFiles);
$("files-download").addEventListener("click", () => {
  contentTask = "download";
  contentProgress = { phase: "downloading", file: "", done: 0, total: content ? content.download : 0, speed: 0 };
  send({ action: "contentDownload" });
  renderContent();
});
$("files-cancel").addEventListener("click", () => send({ action: "contentCancel" }));
$("files-verify").addEventListener("click", () => {
  contentTask = "check";
  contentProgress = null;
  send({ action: "contentCheck", verify: !!content && content.ok });
  renderContent();
});

/* messages from the launcher */

function onHostMessage(message) {
  switch (message.type) {
    case "init":
      setName(message.name);
      $("version").textContent = message.version;
      volume.value = message.volume;
      paintVolume();
      closeOnPlay.checked = message.closeOnPlay;
      language.value = message.language;
      markLanguages(message.installedLanguages);
      if (!message.gameFound) toast("BlackOps4.exe not found! Make sure the launcher is in the game folder.", true);
      requestAnimationFrame(playIntro);
      break;
    case "name":
      setName(message.name);
      break;
    case "languages":
      markLanguages(message.installedLanguages);
      break;
    case "progress":
      setProgress(message.value);
      break;
    case "window":
      document.body.classList.toggle("maximized", message.maximized);
      break;
    case "playDone":
      finishLaunch(message.ok, message.error, message.closing);
      break;
    case "dedicated":
      fillDedicated(message.values);
      openDialog("dedicated-dialog");
      break;
    case "settings":
      renderSettings(message.entries);
      openDialog("settings-dialog");
      break;
    case "settingsSaved":
      $("settings-dialog").close();
      toast("Settings saved.");
      break;
    case "update":
      openUpdateDialog(message.latest, message.current);
      break;
    case "content":
      content = message;
      contentTask = "";
      contentProgress = null;
      renderContent();
      // the list opens by itself once when something is needed to play
      if (content.ok && !content.ready && !content.launcherUpdate && !filesOpenedOnce && !launching) {
        filesOpenedOnce = true;
        openFiles();
      }
      break;
    case "contentProgress":
      contentTask = { checking: "check", downloading: "download", installing: "download", launcher: "launcher" }[message.phase] || contentTask;
      contentProgress = message;
      if (contentTask === "launcher") $("update-now").textContent = "Downloading " + percent(message) + "%";
      renderContent();
      break;
    case "contentDone":
      // a check follows a download, its "content" message ends it
      if (message.task === "download") contentTask = "check";
      if (message.task === "launcher") {
        contentTask = "";
        resetUpdateDialog();
      }
      if (message.cancelled) toast(message.task === "launcher" ? "Launcher update cancelled." : "Download paused, it picks up where it left off next time.");
      else if (!message.ok) toast(message.error || "The download failed.", true);
      else toast("Game files installed.");
      renderContent();
      break;
    case "error":
      if ($("update-now").disabled) resetUpdateDialog();
      toast(message.message, true);
      break;
  }
}

// browser preview only: fakes what the C++ side answers
const preview = {
  name: "Unknown Soldier",
  config: { demonware: { ipv4: "78.157.42.107" }, identity: { name: "Unknown Soldier" } },
};

function previewEntries(object, path = [], out = []) {
  for (const [key, value] of Object.entries(object)) {
    const at = path.concat(key);
    if (value && typeof value === "object") previewEntries(value, at, out);
    else if (typeof value === "boolean") out.push({ path: at, type: "bool", value: String(value) });
    else if (typeof value === "number") out.push({ path: at, type: Number.isInteger(value) ? "int" : "double", value: String(value) });
    else out.push({ path: at, type: "string", value: String(value) });
  }
  return out;
}

// a fake manifest check: support mod missing, the rest installed (or all installed once "downloaded")
function previewContent(installed) {
  installed = installed || preview.installed;
  const file = (path, size, state, extract, entries) => ({ path, size, download: state === "installed" ? 0 : size, state, extract: extract || "", entries: entries || 0 });
  const group = (id, name, description, files) => {
    const state = files.some((f) => f.state === "missing") ? "missing" : files.some((f) => f.state === "outdated") ? "outdated" : "installed";
    return { id, name, description, required: true, state, files,
      size: files.reduce((a, f) => a + f.size, 0), download: files.reduce((a, f) => a + f.download, 0) };
  };
  const s = (state) => (installed ? "installed" : state);
  const components = [
    group("game", "Game", "Black Ops 4 exe, client, LPC", [
      file("project-bo4/launcher/exe.zip", 109493532, "installed"),
      file("project-bo4/launcher/XInput9_1_0.dll", 5211648, s("outdated")),
      file("discord_game_sdk.dll", 3488768, "installed"),
    ]),
    group("support", "Support mod", "Lua, scripts and fastfiles", [
      file("project-bo4/zone/support.ff", 50619336, s("missing")),
      file("project-bo4/zone/support.xpak", 709623808, s("missing")),
      file("project-bo4/zone/snd/all/support.all.sabs", 789563008, s("missing")),
      file("project-bo4/zone_mods/T8ShieldSupport_FF/config.json", 154875, s("missing")),
      file("project-bo4/launcher/packages/T8ShieldSupport.zip", 8412311, s("outdated"), "project-bo4/internals/T8ShieldSupport", 358),
    ]),
    group("mp_targetrange", "Target Range", "The map the support mod's Target Range plays on", [
      file("project-bo4/zone/mp_targetrange.ff", 195989368, s("missing")),
      file("project-bo4/zone/mp_targetrange.xpak", 63635456, s("missing")),
      file("project-bo4/zone/mp_targetrange_ui.ff", 585884, s("missing")),
      file("project-bo4/package_index/mp_targetrange.wni", 26108, s("missing")),
      file("project-bo4/package_index/mp_targetrange_ui.wni", 68, s("missing")),
      file("project-bo4/zone_mods/mp_targetrange/config.json", 641, s("missing")),
    ]),
  ];
  const download = components.reduce((a, g) => a + g.download, 0);
  setTimeout(() => onHostMessage({ type: "content", ok: true, error: "", version: "2026.11.17-1", ready: download === 0,
    download, launcherUpdate: false, launcherLatest: "1.2.0", components }), 300);
}

function previewDownload() {
  const total = 1805000000;
  let done = 0;
  clearInterval(preview.timer);
  preview.timer = setInterval(() => {
    done = Math.min(total, done + 90000000);
    onHostMessage({ type: "contentProgress", phase: "downloading", file: "project-bo4/zone/support.xpak", done, total, speed: 45000000 });
    if (done >= total) {
      clearInterval(preview.timer);
      preview.installed = true;
      onHostMessage({ type: "contentDone", task: "download", ok: true, error: "", cancelled: false });
      previewContent(true);
    }
  }, 250);
}

function previewHost(message) {
  const reply = (m) => setTimeout(() => onHostMessage(m), 0);
  switch (message.action) {
    case "ready":
      reply({ type: "init", name: preview.name, version: "preview", gameFound: true, volume: 5, closeOnPlay: true, language: "enUS", installedLanguages: ["enUS", "itIT"] });
      setTimeout(() => previewContent(false), 600);
      break;
    case "languages":
      reply({ type: "languages", installedLanguages: ["enUS", "itIT"] });
      break;
    case "openDedicated":
      reply({ type: "dedicated", values: { name: "Dedicated", lobby: "public", playlist: 0, rotation: "mp_urban tdm, mp_seaside tdm, mp_frenetic tdm", min_players: 2, lobby_seconds: 20, draw: "ui", max_fps: 60, minimize: false, streaming: false, background: true } });
      break;
    case "launchDedicated":
    case "play": {
      const steps = [[15, "Copying LPC files"], [25, "Copying the client"], [50, "Preparing BlackOps4.exe"], [75, "Starting the game"], [100, "Game started"]];
      steps.forEach(([value, text], i) => setTimeout(() => onHostMessage({ type: "progress", value, text }), 300 * (i + 1)));
      setTimeout(() => onHostMessage({ type: "playDone", ok: true, error: "", closing: false }), 300 * (steps.length + 1));
      break;
    }
    case "setName":
      preview.name = message.name;
      reply({ type: "name", name: message.name });
      break;
    case "openSettings":
      reply({ type: "settings", entries: previewEntries(preview.config) });
      break;
    case "saveSettings":
      reply({ type: "settingsSaved" });
      break;
    case "resetSettings":
      reply({ type: "settings", entries: previewEntries(preview.config) });
      break;
    case "contentCheck":
      previewContent(false);
      break;
    case "contentDownload":
      previewDownload();
      break;
    case "contentCancel":
      clearInterval(preview.timer);
      reply({ type: "contentDone", task: "download", ok: false, error: "", cancelled: true });
      previewContent(false);
      break;
    case "update":
      reply({ type: "contentDone", task: "launcher", ok: false, error: "Preview: no launcher to swap.", cancelled: false });
      break;
    case "volume":
    case "closeOnPlay":
    case "language":
      break;
    default:
      toast("Preview: \"" + message.action + "\" goes to the launcher.");
  }
}

if (host) {
  host.addEventListener("message", (event) => onHostMessage(event.data));
}

// dragging the title bar moves the window (a double click maximizes), its buttons and language stay clickable
$("titlebar").addEventListener("mousedown", (event) => {
  if (event.button === 0 && !event.target.closest("button, .language")) send({ action: "drag" });
});

// the edges size the window, the launcher hides them while maximized
document.querySelectorAll("[data-edge]").forEach((handle) => {
  handle.addEventListener("mousedown", (event) => {
    if (event.button === 0) send({ action: "resize", edge: handle.dataset.edge });
  });
});

document.querySelectorAll("[data-send]").forEach((button) => {
  button.addEventListener("click", () => send({ action: button.dataset.send }));
});

$("play-client").addEventListener("click", () => {
  if (needsFiles()) openFiles();
  else startLaunch({ action: "play", mode: "client" });
});
$("play-vanilla").addEventListener("click", () => startLaunch({ action: "play", mode: "vanilla" }));

volume.addEventListener("input", () => {
  paintVolume();
  send({ action: "volume", value: Number(volume.value), save: false });
});
volume.addEventListener("change", () => send({ action: "volume", value: Number(volume.value), save: true }));

closeOnPlay.addEventListener("change", () => send({ action: "closeOnPlay", value: closeOnPlay.checked }));

/* game language: what battle.net downloaded is marked, null means it couldn't be told */

let installedLanguages = null;

[...language.options].forEach((option) => (option.dataset.name = option.textContent));

const isInstalled = (code) => !installedLanguages || installedLanguages.includes(code);

function markLanguages(installed) {
  installedLanguages = installed;
  [...language.options].forEach((option) => {
    option.textContent = option.dataset.name + (isInstalled(option.value) ? "" : " (not downloaded)");
  });
}

// checked again on each open, a download may have finished since
language.addEventListener("mousedown", () => send({ action: "languages" }));

language.addEventListener("change", () => {
  const name = language.selectedOptions[0].dataset.name;
  send({ action: "language", value: language.value });
  if (isInstalled(language.value)) toast("Language set to " + name + ".");
  else toast(name + " isn't downloaded. Switch to it once in Battle.net to get it, until then the game falls back to an installed language.", true);
});

document.addEventListener("contextmenu", (event) => {
  if (!event.target.closest("input")) event.preventDefault();
});

// the window shows its own splash until the page and the art are loaded, then this intro takes over
function playIntro() {
  if (!document.body.classList.contains("booting")) return;

  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
  document.body.classList.replace("booting", "opening");
  setTimeout(() => {
    document.body.classList.remove("opening");
    $("splash").remove();
  }, reduce ? 0 : 1600);
}

window.addEventListener("load", () => {
  let sent = false;
  const ready = () => {
    if (sent) return;
    sent = true;
    send({ action: "ready" });
  };

  const art = new Image();
  art.onload = art.onerror = ready;
  art.src = "launcher.png";
  setTimeout(ready, 1500);

  // never stay hidden if the launcher doesn't answer
  setTimeout(playIntro, 4000);
});

paintVolume();
renderPlayState();
