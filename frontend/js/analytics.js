// MiniSearch - analytics page: live index statistics from /api/stats and
// /api/crawl/status. Every number shown is measured by the engine.

"use strict";

(function () {
  const set = (id, value) => {
    const el = document.getElementById(id);
    if (el) el.textContent = value;
  };

  function renderCrawlLive(crawl) {
    const body = document.getElementById("crawl-live-body");
    const line = document.getElementById("crawl-status-line");
    if (!crawl.has_job) {
      line.textContent = "No crawl has been started yet.";
      body.innerHTML = "";
      return;
    }
    line.textContent = crawl.running
      ? "A crawl job is running right now."
      : "Last crawl job finished.";
    body.innerHTML =
      '<div class="counter-row">' +
      '<div class="counter"><div class="n">' + crawl.queued + '</div><div class="l">queued</div></div>' +
      '<div class="counter ok"><div class="n">' + crawl.succeeded + '</div><div class="l">indexed</div></div>' +
      '<div class="counter bad"><div class="n">' + crawl.failed + '</div><div class="l">failed</div></div>' +
      '<div class="counter"><div class="n">' + crawl.skipped + '</div><div class="l">skipped</div></div>' +
      "</div>";
  }

  async function refresh() {
    try {
      const stats = await api("/api/stats");
      set("a-docs", formatNumber(stats.documents));
      set("a-terms", formatNumber(stats.unique_terms));
      set("a-postings", formatNumber(stats.total_postings));
      set("a-size", stats.index_size_human || formatBytes(stats.index_size_bytes));
      set("a-avgdoc", stats.average_document_tokens.toFixed(1));
      set("a-latency",
        stats.search_count > 0 ? formatMs(stats.average_search_latency_ms) : "—");
      set("a-searchcount", formatNumber(stats.search_count) + " searches served");
      set("a-crawled", formatNumber(stats.pages_crawled));
      set("a-failed", formatNumber(stats.pages_failed) + " failed");
      set("a-jobs", formatNumber(stats.crawls_run));
      set("a-last-index", stats.last_indexed_at ? "indexed " + stats.last_indexed_at : "never indexed");

      const bars = document.getElementById("term-bars");
      if (stats.top_terms && stats.top_terms.length > 0) {
        const maxDf = stats.top_terms[0].documents || 1;
        bars.innerHTML = stats.top_terms
          .map(
            (t) =>
              '<div class="term-bar"><span class="t">' + escapeHtml(t.term) + "</span>" +
              '<div class="track"><div class="fill" style="width:' +
              Math.max(3, Math.round((t.documents / maxDf) * 100)) + '%"></div></div>' +
              '<span class="d">' + t.documents + "</span></div>"
          )
          .join("");
      } else {
        bars.innerHTML = '<p class="sub">No terms yet — crawl something first.</p>';
      }
    } catch (err) {
      set("a-docs", "unavailable");
    }

    try {
      renderCrawlLive(await api("/api/crawl/status"));
    } catch (err) {
      /* status endpoint unavailable: leave the previous state */
    }
  }

  document.getElementById("refresh-now").addEventListener("click", refresh);

  let timer = null;
  const autoBox = document.getElementById("auto-refresh");
  const setTimer = () => {
    if (timer) clearInterval(timer);
    if (autoBox.checked) timer = setInterval(refresh, 5000);
  };
  autoBox.addEventListener("change", setTimer);

  refresh();
  setTimer();
})();
