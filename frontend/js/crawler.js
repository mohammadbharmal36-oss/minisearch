// MiniSearch - crawler console: starts real crawl jobs via POST /api/crawl
// and follows their progress via GET /api/crawl/status.

"use strict";

(function () {
  const form = document.getElementById("crawl-form");
  const errorEl = document.getElementById("crawl-error");
  const startButton = document.getElementById("crawl-start");
  const progressCard = document.getElementById("progress-card");
  const progressTitle = document.getElementById("progress-title");
  const progressPill = document.getElementById("progress-pill");
  const progressBar = document.getElementById("progress-bar");
  const logBody = document.getElementById("log-body");
  const doneCard = document.getElementById("done-card");
  const doneSummary = document.getElementById("done-summary");

  let pollTimer = null;
  let seenUrls = new Set();

  function setError(message) {
    errorEl.textContent = message || "";
  }

  function validate(seeds) {
    if (seeds.length === 0) return "Enter at least one seed URL.";
    for (const seed of seeds) {
      try {
        const parsed = new URL(seed);
        if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
          return "Only http(s) seed URLs are supported: " + seed;
        }
      } catch (err) {
        return "Not a valid URL: " + seed;
      }
    }
    return "";
  }

  function pill(kind) {
    const classes = { ok: "ok", failed: "failed", skipped: "skipped" };
    return (
      '<span class="status-pill ' + (classes[kind] || "failed") + '">' + escapeHtml(kind) + "</span>"
    );
  }

  function renderStatus(status) {
    progressCard.hidden = false;
    doneCard.hidden = true;

    const running = status.running;
    progressTitle.textContent = running
      ? "Crawl job #" + status.job_id + " in progress"
      : "Crawl job #" + status.job_id + " — finished";
    progressPill.textContent = running ? "running" : "done";
    progressPill.className = "status-pill " + (running ? "running" : "ok");

    const budget = status.config ? status.config.max_pages : 0;
    const progress = budget > 0 ? Math.min(100, Math.round((status.fetched / budget) * 100)) : 0;
    progressBar.style.width = (running || status.fetched > 0 ? progress : 100) + "%";

    document.getElementById("c-queued").textContent = status.queued;
    document.getElementById("c-fetched").textContent = status.fetched;
    document.getElementById("c-ok").textContent = status.succeeded;
    document.getElementById("c-failed").textContent = status.failed;
    document.getElementById("c-skipped").textContent = status.skipped;
    document.getElementById("c-elapsed").textContent = formatSeconds(status.elapsed_ms || 0);

    // Most recent first; only append entries we have not shown yet.
    const recent = status.recent || [];
    for (let i = recent.length - 1; i >= 0; i--) {
      const entry = recent[i];
      if (seenUrls.has(entry.url + "|" + entry.status)) continue;
      seenUrls.add(entry.url + "|" + entry.status);
      const row = document.createElement("tr");
      row.innerHTML =
        "<td>" + pill(entry.status) + "</td>" +
        '<td class="url">' + escapeHtml(entry.url) + "</td>" +
        "<td>" + (entry.http_status || "—") + "</td>" +
        '<td class="note">' + escapeHtml(entry.note || "") + "</td>";
      logBody.insertBefore(row, logBody.firstChild);
    }

    if (!running) {
      clearInterval(pollTimer);
      pollTimer = null;
      startButton.disabled = false;
      doneCard.hidden = false;
      doneSummary.textContent =
        status.succeeded + " pages indexed, " + status.failed + " failed, " +
        status.skipped + " skipped in " + formatSeconds(status.elapsed_ms || 0) + ".";
    }
  }

  async function poll() {
    try {
      renderStatus(await api("/api/crawl/status"));
    } catch (err) {
      setError("status unavailable: " + err.message);
    }
  }

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    setError("");

    const seeds = document
      .getElementById("seed-urls")
      .value.split("\n")
      .map((line) => line.trim())
      .filter((line) => line.length > 0 && !line.startsWith("#"));

    const validationError = validate(seeds);
    if (validationError) {
      setError(validationError);
      return;
    }

    const domains = document
      .getElementById("domains")
      .value.split(",")
      .map((domain) => domain.trim())
      .filter((domain) => domain.length > 0);

    const body = {
      seeds: seeds,
      max_depth: parseInt(document.getElementById("max-depth").value, 10) || 2,
      max_pages: parseInt(document.getElementById("max-pages").value, 10) || 30,
      delay_ms: parseInt(document.getElementById("delay-ms").value, 10) || 0,
    };
    if (domains.length > 0) body.allowed_domains = domains;

    startButton.disabled = true;
    logBody.innerHTML = "";
    seenUrls = new Set();

    try {
      await api("/api/crawl", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (pollTimer) clearInterval(pollTimer);
      pollTimer = setInterval(poll, 1000);
      poll();
    } catch (err) {
      setError(err.message);
      startButton.disabled = false;
    }
  });

  // If a crawl is already running when the page opens, follow it.
  api("/api/crawl/status")
    .then((status) => {
      if (status.has_job) {
        startButton.disabled = status.running;
        if (status.running) {
          if (pollTimer) clearInterval(pollTimer);
          pollTimer = setInterval(poll, 1000);
        }
        renderStatus(status);
      }
    })
    .catch(() => {});
})();
