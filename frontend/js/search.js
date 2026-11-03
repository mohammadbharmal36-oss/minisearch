// MiniSearch - search behaviour for the home page and the results page.

"use strict";

(function () {
  // ------------------------------------------------------------------
  // Home page: hero form + live stats.
  // ------------------------------------------------------------------
  const homeForm = document.getElementById("home-search");
  if (homeForm) {
    const homeInput = document.getElementById("home-query");
    attachSuggestions(homeInput, document.getElementById("home-suggest"), homeForm);
    homeForm.addEventListener("submit", (event) => {
      event.preventDefault();
      const query = homeInput.value.trim();
      if (query) window.location.href = "/search?q=" + encodeURIComponent(query);
    });

    const setStat = (id, value) => {
      const el = document.getElementById(id);
      if (el) el.textContent = value;
    };
    api("/api/stats")
      .then((stats) => {
        setStat("s-docs", formatNumber(stats.documents));
        setStat("s-terms", formatNumber(stats.unique_terms));
        setStat("s-latency", stats.search_count > 0 ? formatMs(stats.average_search_latency_ms) : "—");
        setStat(
          "s-crawled",
          formatNumber(stats.pages_crawled) + (stats.pages_crawled === 1 ? " page" : " pages")
        );
      })
      .catch(() => {
        setStat("s-docs", "unavailable");
      });
    return; // home page needs nothing below
  }

  // ------------------------------------------------------------------
  // Results page.
  // ------------------------------------------------------------------
  const form = document.getElementById("page-search");
  const input = document.getElementById("page-query");
  const suggestBox = document.getElementById("page-suggest");
  const algoSelect = document.getElementById("algo-select");
  const limitSelect = document.getElementById("limit-select");
  const metaEl = document.getElementById("result-meta");
  const resultsEl = document.getElementById("results");
  const paginationEl = document.getElementById("pagination");
  const loadingEl = document.getElementById("loading");
  const noResultsEl = document.getElementById("no-results");
  const errorEl = document.getElementById("error-state");
  const errorMessage = document.getElementById("error-message");
  const initialState = document.getElementById("initial-state");

  attachSuggestions(input, suggestBox, form);

  function show(element) {
    [loadingEl, noResultsEl, errorEl, initialState, resultsEl].forEach((el) => {
      if (el) el.hidden = el !== element;
    });
  }

  /** Wrap words in snippet that begin with a query term. */
  function highlight(snippet, terms) {
    const words = String(snippet).split(/(\s+)/);
    return words
      .map((word) => {
        if (/^\s+$/.test(word)) return word;
        const bare = word.toLowerCase().replace(/^[^\w+#.]+|[^\w+#.]+$/g, "");
        const hit = terms.some((term) => term.length > 1 && bare.startsWith(term));
        return hit ? "<mark>" + escapeHtml(word) + "</mark>" : escapeHtml(word);
      })
      .join("");
  }

  function renderItems(response) {
    resultsEl.innerHTML = "";
    response.items.forEach((item) => {
      const result = document.createElement("article");
      result.className = "result";
      const snippet = item.snippet ? highlight(item.snippet, item.matched_terms) : "";
      const chips = (item.matched_terms || [])
        .map((term) => '<span class="chip">' + escapeHtml(term) + "</span>")
        .join("");
      result.innerHTML =
        '<div class="r-head"><div class="r-main">' +
        '<h3 class="r-title"><a href="' + escapeHtml(item.url) + '" target="_blank" rel="noopener">' +
        escapeHtml(item.title) + "</a></h3>" +
        '<div class="r-url">🔒<span class="host">' + escapeHtml(urlHost(item.url)) + "</span>" +
        (urlPath(item.url) ? " › " + escapeHtml(urlPath(item.url)) : "") +
        "</div>" +
        '<p class="r-snippet">' + snippet + "</p>" +
        (chips ? '<div class="chips">' + chips + "</div>" : "") +
        "</div>" +
        '<div class="r-side"><span class="score-chip" title="relevance score">' +
        item.score.toFixed(3) + "</span></div></div>";
      resultsEl.appendChild(result);
    });
  }

  function renderPagination(query, algo, limit, offset, total) {
    paginationEl.innerHTML = "";
    if (total <= limit) return;
    const pageSize = limit;
    const pageCount = Math.ceil(total / pageSize);
    const currentPage = Math.floor(offset / pageSize) + 1;

    const navigate = (page) => {
      const newOffset = (page - 1) * pageSize;
      const params = new URLSearchParams({ q: query, algorithm: algo, limit: String(limit) });
      if (newOffset > 0) params.set("offset", String(newOffset));
      window.history.pushState({}, "", "/search?" + params.toString());
      runSearch();
    };

    const mkButton = (label, page, opts) => {
      const button = document.createElement("button");
      button.textContent = label;
      if (opts && opts.disabled) button.disabled = true;
      if (opts && opts.current) button.className = "current";
      button.addEventListener("click", () => navigate(page));
      return button;
    };

    paginationEl.appendChild(mkButton("‹ prev", currentPage - 1, { disabled: currentPage <= 1 }));
    for (let page = 1; page <= pageCount; page++) {
      if (pageCount > 9 && Math.abs(page - currentPage) > 3 && page !== 1 && page !== pageCount) {
        if (paginationEl.lastElementChild && paginationEl.lastElementChild.textContent !== "…") {
          const dots = document.createElement("button");
          dots.textContent = "…";
          dots.disabled = true;
          paginationEl.appendChild(dots);
        }
        continue;
      }
      paginationEl.appendChild(mkButton(String(page), page, { current: page === currentPage }));
    }
    paginationEl.appendChild(mkButton("next ›", currentPage + 1, { disabled: currentPage >= pageCount }));
  }

  let searchSequence = 0;

  async function runSearch() {
    const params = new URLSearchParams(window.location.search);
    const query = (params.get("q") || "").trim();
    const algo = params.get("algorithm") === "tfidf" ? "tfidf" : "bm25";
    const limit = Math.min(Math.max(parseInt(params.get("limit") || "10", 10) || 10, 1), 50);
    const offset = Math.max(parseInt(params.get("offset") || "0", 10) || 0, 0);

    input.value = query;
    algoSelect.value = algo;
    limitSelect.value = String(limit);

    if (!query) {
      show(initialState);
      metaEl.textContent = "";
      paginationEl.innerHTML = "";
      return;
    }

    const seq = ++searchSequence;
    show(loadingEl);
    metaEl.textContent = "";
    paginationEl.innerHTML = "";

    try {
      const response = await api(
        "/api/search?q=" + encodeURIComponent(query) +
        "&algorithm=" + algo + "&limit=" + limit + "&offset=" + offset
      );
      if (seq !== searchSequence) return; // a newer search superseded this one
      metaEl.innerHTML =
        "<strong>" + formatNumber(response.total) + "</strong> result" +
        (response.total === 1 ? "" : "s") + " · " + formatMs(response.took_ms) +
        " · " + escapeHtml(response.algorithm);
      if (response.items.length === 0) {
        show(noResultsEl);
      } else {
        renderItems(response);
        show(resultsEl);
        renderPagination(query, algo, limit, offset, response.total);
      }
    } catch (err) {
      if (seq !== searchSequence) return;
      errorMessage.textContent = err.message;
      show(errorEl);
    }
  }

  form.addEventListener("submit", (event) => {
    event.preventDefault();
    const query = input.value.trim();
    const params = new URLSearchParams();
    if (query) params.set("q", query);
    const algo = algoSelect.value;
    const limit = limitSelect.value;
    if (algo) params.set("algorithm", algo);
    if (limit) params.set("limit", limit);
    window.history.pushState({}, "", "/search?" + params.toString());
    runSearch();
  });

  algoSelect.addEventListener("change", () => {
    if (!input.value.trim()) return;
    const params = new URLSearchParams(window.location.search);
    params.set("algorithm", algoSelect.value);
    params.delete("offset");
    window.history.pushState({}, "", "/search?" + params.toString());
    runSearch();
  });

  limitSelect.addEventListener("change", () => {
    if (!input.value.trim()) return;
    const params = new URLSearchParams(window.location.search);
    params.set("limit", limitSelect.value);
    params.delete("offset");
    window.history.pushState({}, "", "/search?" + params.toString());
    runSearch();
  });

  window.addEventListener("popstate", runSearch);
  runSearch();
})();
