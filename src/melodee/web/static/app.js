// Melodee's web page: the link to the MPC in the header on every page, the
// device page's update and plug-in uploads, and the docs' side lists.
"use strict";
const $ = (s, root = document) => root.querySelector(s);
const $$ = (s, root = document) => [...root.querySelectorAll(s)];
const tones = ["var(--coral)", "var(--amber)", "var(--mint)", "var(--lilac)"];

// The MPC answers one request at a time: no status asks while a file is on its way, and one at a time.
let sending = false, asking = false, last = null;

function link(state, word) {
  const el = $("#link");
  el.dataset.state = state;
  $("#link-word").textContent = word;
}

async function status() {
  try {
    const s = await (await fetch("/status", {cache: "no-store"})).json();
    link(update.watching ? "busy" : "on", "MPC " + s.version);
    device.show(s);
    last = s;
    return s;
  } catch (e) {
    link(update.watching === "install" ? "busy" : "off", update.watching === "install" ? "Restarting" : "Not connected");
    device.lost();
    return null;
  }
}

function gb(bytes) { return bytes >= 1e9 ? (bytes / 1e9).toFixed(1) + " GB" : Math.round(bytes / 1e6) + " MB"; }

// ---- The device page ----

const device = {
  here: !!$("#device"),
  show(s) {
    if (!this.here) return;
    $("#v-version").textContent = s.version;
    $("#g-version").style.width = "100%";
    if (s.card) {
      $("#v-card").innerHTML = gb(s.card_free) + "<small>free</small>";
      $("#g-card").style.width = (s.card_total ? 100 * (1 - s.card_free / s.card_total) : 0) + "%";
    } else {
      $("#v-card").textContent = "None";
      $("#g-card").style.width = "0";
    }
    if (s.card !== this.card) { this.card = s.card; plugins(); }
    update.follow(s);
    panels.forEach(p => p.ready());
  },
  lost() {
    if (!this.here) return;
    this.card = false;
    panels.forEach(p => p.ready());
  },
};

// One upload panel: its drop zone, file, gauge, button and line of words.
function Panel(el, path) {
  const input = $("input", el), drop = $("[data-drop]", el);
  const self = {
    el, file: null, busy: false,
    say(text, kind) { const p = $("[data-say]", el); p.textContent = text || ""; p.className = "say" + (kind ? " " + kind : ""); },
    bar(part) { $("[data-bar]", el).style.width = (part * 100) + "%"; },
    ready() {
      el.classList.toggle("busy", self.busy);
      $("[data-send]", el).disabled = self.busy || !self.file || !device.card;
      if (!device.card && !self.busy && last) self.say(last.card === false ? "No SD card in the MPC" : "", "bad");
    },
    pick(file) {
      if (file && !/\.zip$/i.test(file.name)) { self.say("Not a .zip", "bad"); return; }
      self.file = file || null;
      $("[data-name]", el).textContent = file ? file.name : $("[data-name]", el).dataset.empty;
      $("[data-hint]", el).textContent = file ? gb(file.size) : "Drop here or choose";
      drop.classList.toggle("chosen", !!file);
      self.say("");
      self.bar(0);
      self.ready();
    },
    send(done) {
      const file = self.file;
      self.busy = sending = true;
      self.ready();
      self.say("Sending…");
      const xhr = new XMLHttpRequest();
      xhr.open("POST", path + "?name=" + encodeURIComponent(file.name));
      xhr.setRequestHeader("X-Melodee", "1");
      xhr.setRequestHeader("Content-Type", "application/zip");
      xhr.upload.onprogress = e => { if (e.lengthComputable) self.bar(e.loaded / e.total); };
      xhr.upload.onload = () => { self.bar(1); self.say(path === "/update" ? "Checking the zip…" : "Unzipping…"); };
      xhr.onerror = () => { sending = self.busy = false; self.say("The MPC stopped answering", "bad"); self.ready(); done(null); };
      xhr.onload = () => {
        sending = false;
        let r = {};
        try { r = JSON.parse(xhr.responseText); } catch (e) {}
        self.pick(null);
        done(xhr.status === 200 ? r : {error: r.error || "Failed"});
      };
      xhr.send(file);
    },
  };
  $("[data-name]", el).dataset.empty = $("[data-name]", el).textContent;
  input.addEventListener("change", () => self.pick(input.files[0]));
  drop.addEventListener("dragover", e => { e.preventDefault(); drop.classList.add("over"); });
  drop.addEventListener("dragleave", () => drop.classList.remove("over"));
  drop.addEventListener("drop", e => { e.preventDefault(); drop.classList.remove("over"); if (!self.busy) self.pick(e.dataTransfer.files[0]); });
  $("[data-send]", el).addEventListener("click", () => self.file && self.send(el.id === "update" ? update.sent : installed));
  return self;
}

const panels = device.here ? [Panel($("#update"), "/update"), Panel($("#plugins"), "/plugin")] : [];
const [updatePanel, pluginPanel] = panels;

// The update: sent, asked on the MPC, then the MPC's own steps, then the restart.
const update = {
  watching: null, from: "",
  order: ["send", "ask", "saving", "checking", "writing", "starting"],
  step(name, how) {
    const at = this.order.indexOf(name);
    $$("#update .steps li").forEach((li, i) => {
      li.className = i < at ? "done" : i === at ? how || "now" : "";
    });
  },
  sent(r) {
    if (!r) { update.step("send", "failed"); return; }
    if (r.error) { updatePanel.busy = false; updatePanel.say(r.error, "bad"); update.step("send", "failed"); updatePanel.ready(); return; }
    update.watching = "offer";
    update.from = last ? last.version : "";
    update.step("ask", "now wait");
    updatePanel.say("Press INSTALL on the MPC to update to " + r.offered, "ask");
  },
  follow(s) {
    if (!updatePanel) return;
    if (this.watching && this.from && s.version !== this.from) {
      this.watching = null;
      $$("#update .steps li").forEach(li => li.className = "done");
      updatePanel.busy = false;
      updatePanel.say("Updated to " + s.version, "ok");
      return;
    }
    if (!this.watching) return;
    const words = {saving: "Saving the project…", checking: "Checking the update…", writing: "Copying to the card…", starting: "Restarting…"};
    if (s.phase === "failed") {
      this.step(this.order[Math.max(2, this.order.indexOf(this.lastPhase || "saving"))], "failed");
      updatePanel.say(s.error || "Update failed", "bad");
      this.watching = null;
      updatePanel.busy = false;
    } else if (words[s.phase]) {
      this.watching = "install";
      this.lastPhase = s.phase;
      this.step(s.phase);
      updatePanel.say(words[s.phase], "ask");
    } else if (this.watching === "offer" && !s.offer) {
      this.watching = null;
      this.step("ask", "failed");
      updatePanel.busy = false;
      updatePanel.say("LATER on the MPC: not installed", "");
    }
  },
};
if (device.here) update.step("none");

function installed(r) {
  pluginPanel.busy = false;
  if (!r) return;
  if (r.error) { pluginPanel.say(r.error, "bad"); pluginPanel.ready(); return; }
  pluginPanel.say("Installed " + (r.installed || []).join(", "), "ok");
  plugins(r.installed || []);
}

async function plugins(fresh = []) {
  if (!device.here) return;
  const list = $("#plugin-list");
  try {
    const p = await (await fetch("/plugins", {cache: "no-store"})).json();
    list.innerHTML = "";
    const real = p.plugins.filter(x => !x.folder || x.loadable);
    const ready = real.filter(x => x.loadable).length;
    $("#v-plugins").innerHTML = p.card ? ready + (real.length > ready ? "<small>of " + real.length + "</small>" : "") : "–";
    $("#g-plugins").style.width = real.length ? (100 * ready / real.length) + "%" : "0";
    if (!p.plugins.length) {
      const li = document.createElement("li");
      li.className = "empty";
      li.textContent = p.card ? "None yet" : "No SD card";
      list.append(li);
      return;
    }
    p.plugins.forEach((item, n) => {
      const li = document.createElement("li");
      li.style.setProperty("--stripe", tones[n % 4]);
      if (item.why) li.classList.add("off");
      if (fresh.includes(item.file)) li.classList.add("new");
      const name = document.createElement("span");
      name.className = "name";
      name.textContent = item.name;
      li.append(name);
      if (item.why) {
        const why = document.createElement("span");
        why.className = "why";
        why.textContent = item.why;
        li.append(why);
      }
      const chip = document.createElement("span");
      chip.className = "chip";
      chip.textContent = item.folder ? "FOLDER" : item.format;
      li.append(chip);
      list.append(li);
    });
  } catch (e) { /* the next status ask says whether the MPC is there */ }
}

// ---- The docs ----

function docs() {
  // On a phone the chapter list starts closed.
  const menu = $(".chapters-menu");
  if (menu && matchMedia("(max-width: 860px)").matches) menu.open = false;
  // The section in view is marked in the side list.
  const links = $$("aside .toc a");
  if (!links.length || !("IntersectionObserver" in window)) return;
  const byId = new Map(links.map(a => [decodeURIComponent(a.hash.slice(1)), a]));
  const seen = new Set();
  const watch = new IntersectionObserver(entries => {
    entries.forEach(e => e.isIntersecting ? seen.add(e.target.id) : seen.delete(e.target.id));
    const first = $$(".doc h2[id]").find(h => seen.has(h.id));
    if (!first) return;
    links.forEach(a => a.classList.toggle("here", a === byId.get(first.id)));
  }, {rootMargin: "-70px 0px -55% 0px"});
  $$(".doc h2[id]").forEach(h => watch.observe(h));
}

docs();
status().then(() => plugins());
setInterval(() => {
  if (sending || asking) return;
  asking = true;
  status().finally(() => { asking = false; });
}, 1500);
