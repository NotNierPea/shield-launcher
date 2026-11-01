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
  label.textContent = "Play Client";

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

$("play-dedicated").addEventListener("click", () => send({ action: "openDedicated" }));
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
  button.textContent = "Downloading...";
  send({ action: "update" });
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
      $("update-text").textContent = `Version ${message.latest} is out, you have ${message.current}. The launcher closes while it updates.`;
      openDialog("update-dialog");
      break;
    case "error":
      if ($("update-now").disabled) {
        $("update-now").disabled = false;
        $("update-now").textContent = "Update now";
      }
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

function previewHost(message) {
  const reply = (m) => setTimeout(() => onHostMessage(m), 0);
  switch (message.action) {
    case "ready":
      reply({ type: "init", name: preview.name, version: "preview", gameFound: true, volume: 5, closeOnPlay: true, language: "enUS", installedLanguages: ["enUS", "itIT"] });
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

$("play-client").addEventListener("click", () => startLaunch({ action: "play", mode: "client" }));
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
