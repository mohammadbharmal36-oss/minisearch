// MiniSearch - shared front-end helpers (vanilla JS, no frameworks).

"use strict";

/** Fetch JSON from the API; throws Error(message) on HTTP errors. */
async function api(path, options) {
  const response = await fetch(path, options);
  let data = null;
  try {
    data = await response.json();
  } catch (err) {
    data = null;
  }
  if (!response.ok) {
    const message = data && data.error ? data.error : "HTTP " + response.status;
    throw new Error(message);
  }
  return data;
}

/** Escape text for safe innerHTML interpolation. */
function escapeHtml(text) {
  return String(text)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

/** Debounce: fn runs `delay` ms after the last call. */
function debounce(fn, delay) {
  let timer = null;
  return function (...args) {
    clearTimeout(timer);
    timer = setTimeout(() => fn.apply(this, args), delay);
  };
}

function formatBytes(bytes) {
  if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB";
  if (bytes >= 1024) return (bytes / 1024).toFixed(1) + " KB";
  return bytes + " B";
}

function formatMs(ms) {
  if (ms < 0.01) return "<0.01 ms";
  if (ms < 1) return ms.toFixed(2) + " ms";
  return ms.toFixed(1) + " ms";
}

function formatNumber(value) {
  return new Intl.NumberFormat().format(value);
}

function formatSeconds(ms) {
  if (ms < 1000) return Math.round(ms) + " ms";
  return (ms / 1000).toFixed(1) + " s";
}

/** Hostname part of a URL, for result URL lines. */
function urlHost(url) {
  try {
    return new URL(url).host;
  } catch (err) {
    return url;
  }
}

/** Path part (without leading slash) for result URL lines. */
function urlPath(url) {
  try {
    const parsed = new URL(url);
    return (parsed.pathname + parsed.search).replace(/^\//, "");
  } catch (err) {
    return "";
  }
}

/**
 * Attaches a suggestion dropdown to an input + container pair.
 * Selecting a suggestion submits the surrounding form.
 */
function attachSuggestions(input, container, form) {
  let items = [];
  let selected = -1;

  const close = () => {
    container.classList.remove("open");
    container.innerHTML = "";
    items = [];
    selected = -1;
  };

  const render = () => {
    container.innerHTML = "";
    items.forEach((item, index) => {
      const row = document.createElement("div");
      row.className = "s-item" + (index === selected ? " sel" : "");
      row.innerHTML =
        '<span class="term">' + escapeHtml(item.term) + "</span>" +
        '<span class="df">' + escapeHtml(item.documents) + " docs</span>";
      row.addEventListener("mousedown", (event) => {
        // mousedown fires before the input's blur.
        event.preventDefault();
        input.value = item.term;
        close();
        if (form) form.requestSubmit();
      });
      container.appendChild(row);
    });
    container.classList.toggle("open", items.length > 0);
  };

  input.addEventListener("input", debounce(async () => {
    const prefix = input.value.trim().toLowerCase();
    if (prefix.length < 2) {
      close();
      return;
    }
    try {
      const data = await api("/api/suggest?q=" + encodeURIComponent(prefix));
      items = data.suggestions || [];
      selected = -1;
      render();
    } catch (err) {
      close();
    }
  }, 140));

  input.addEventListener("keydown", (event) => {
    if (!container.classList.contains("open")) return;
    if (event.key === "ArrowDown") {
      event.preventDefault();
      selected = Math.min(selected + 1, items.length - 1);
      render();
    } else if (event.key === "ArrowUp") {
      event.preventDefault();
      selected = Math.max(selected - 1, 0);
      render();
    } else if (event.key === "Escape") {
      close();
    } else if (event.key === "Enter" && selected >= 0 && items[selected]) {
      event.preventDefault();
      input.value = items[selected].term;
      close();
      if (form) form.requestSubmit();
    }
  });

  input.addEventListener("blur", () => setTimeout(close, 120));
  document.addEventListener("click", (event) => {
    if (!container.contains(event.target) && event.target !== input) close();
  });
}
