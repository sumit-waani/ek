/*
 * Eka Client Runtime — ES2020
 * ~500 LOC, zero dependencies, zero CDN.
 * Embedded in the Eka binary, served at /_eka.js
 *
 * Server-driven swap model: server returns HTML, client swaps it into DOM.
 * No reactive state, no signals, no virtual DOM.
 */
(function () {
  "use strict";

  /* ================================================================
   * Config
   * ================================================================ */

  const VERSION = "1.0.0";
  const DEFAULT_TIMEOUT = 30000;
  const HEADER_REQUEST = "X-Requested-With";
  const HEADER_EKA = "X-Eka-Request";

  /* Swap strategies */
  const SWAP_INNER = "innerHTML";
  const SWAP_OUTER = "outerHTML";
  const SWAP_BEFORE_END = "beforeend";
  const SWAP_AFTER_BEGIN = "afterbegin";
  const SWAP_DELETE = "delete";
  const SWAP_NONE = "none";

  /* Trigger defaults by element tag */
  const TRIGGER_DEFAULTS = {
    FORM: "submit",
    INPUT: "change",
    SELECT: "change",
    TEXTAREA: "change",
    BUTTON: "click",
    A: "click",
  };

  const DEFAULT_TRIGGER = "click";

  /* HTTP method attributes */
  const METHOD_ATTRS = ["e-get", "e-post", "e-put", "e-delete", "e-patch"];

  /* ================================================================
   * State
   * ================================================================ */

  let revealObserver = null;
  const revealedElements = new WeakSet();

  /* ================================================================
   * Utilities
   * ================================================================ */

  function attr(el, name) {
    const v = el.getAttribute(name);
    return v === "" ? true : v; /* empty attr = boolean true */
  }

  function hasAttr(el, name) {
    return el.hasAttribute(name);
  }

  function closestWithAttr(el, attrName) {
    while (el && el !== document.documentElement) {
      if (hasAttr(el, attrName)) return el;
      el = el.parentElement;
    }
    return null;
  }

  /* ================================================================
   * Target Resolver
   *
   * e-target values:
   *   "this"           → the triggering element
   *   "closest <sel>"  → element.closest(selector)
   *   "find <sel>"     → element.querySelector(selector)
   *   "<css selector>" → document.querySelector(selector)
   * ================================================================ */

  function resolveTarget(triggerEl, targetStr) {
    if (!targetStr) return null;
    targetStr = targetStr.trim();

    if (targetStr === "this") {
      return triggerEl;
    }

    if (targetStr.startsWith("closest ")) {
      const sel = targetStr.slice(8).trim();
      return triggerEl.closest(sel);
    }

    if (targetStr.startsWith("find ")) {
      const sel = targetStr.slice(5).trim();
      return triggerEl.querySelector(sel);
    }

    return document.querySelector(targetStr);
  }

  /* ================================================================
   * Swap Engine
   * ================================================================ */

  function performSwap(target, html, mode) {
    if (!target) return;

    switch (mode) {
      case SWAP_INNER:
        target.innerHTML = html;
        break;
      case SWAP_OUTER:
        target.outerHTML = html;
        break;
      case SWAP_BEFORE_END:
        target.insertAdjacentHTML("beforeend", html);
        break;
      case SWAP_AFTER_BEGIN:
        target.insertAdjacentHTML("afterbegin", html);
        break;
      case SWAP_DELETE:
        target.remove();
        break;
      case SWAP_NONE:
        break;
      default:
        target.innerHTML = html;
    }
  }

  /* ================================================================
   * Custom Event Dispatcher
   * ================================================================ */

  function dispatch(el, name, detail) {
    el.dispatchEvent(
      new CustomEvent("e:" + name, {
        bubbles: true,
        detail: detail || {},
      })
    );
  }

  /* ================================================================
   * Loading State (.e-busy)
   * ================================================================ */

  function setBusy(el) {
    el.classList.add("e-busy");
  }

  function clearBusy(el) {
    el.classList.remove("e-busy");
  }

  /* ================================================================
   * Request Builder
   * ================================================================ */

  function buildRequestConfig(el) {
    let method = null;
    let url = null;

    /* Find the method + url from e-get, e-post, etc. */
    for (const m of METHOD_ATTRS) {
      if (hasAttr(el, m)) {
        method = m.slice(2).toUpperCase(); /* "e-get" → "GET" */
        url = attr(el, m);
        break;
      }
    }

    if (!method || !url) return null;

    const config = {
      method: method,
      url: url,
      target: attr(el, "e-target") || null,
      swap: attr(el, "e-swap") || SWAP_INNER,
      trigger: attr(el, "e-trigger") || null,
      include: attr(el, "e-include") || null,
      confirm: attr(el, "e-confirm") || null,
      timeout: parseInt(attr(el, "e-timeout"), 10) || DEFAULT_TIMEOUT,
      errorTarget: attr(el, "e-error-target") || null,
      errorSwap: attr(el, "e-error-swap") || SWAP_INNER,
    };

    return config;
  }

  /* ================================================================
   * Form Data Extraction
   * ================================================================ */

  function getFormData(el) {
    /* If element is a form, use it directly */
    let form = null;
    if (el.tagName === "FORM") {
      form = el;
    } else {
      /* Walk up to find enclosing form */
      form = el.closest("form");
    }

    if (form) {
      return new FormData(form);
    }

    /* No form — return empty */
    return new FormData();
  }

  function formDataToUrlEncoded(formData) {
    const params = new URLSearchParams();
    for (const [key, value] of formData.entries()) {
      params.append(key, value);
    }
    return params.toString();
  }

  /* ================================================================
   * Include Fields
   *
   * e-include="<selector>" — include additional form fields
   * from another part of the page as query params (GET) or
   * form body (POST/PUT/PATCH/DELETE).
   * ================================================================ */

  function getIncludeData(includeSelector) {
    if (!includeSelector) return null;

    const container = document.querySelector(includeSelector);
    if (!container) return null;

    const formData = new FormData();
    const inputs = container.querySelectorAll("input, select, textarea");
    for (const input of inputs) {
      if (input.name) {
        if (input.type === "checkbox" || input.type === "radio") {
          if (input.checked) formData.append(input.name, input.value);
        } else {
          formData.append(input.name, input.value);
        }
      }
    }
    return formData;
  }

  /* ================================================================
   * Request Executor
   * ================================================================ */

  async function executeRequest(triggerEl, config) {
    /* Confirm dialog */
    if (config.confirm) {
      if (!window.confirm(config.confirm)) return;
    }

    const target = resolveTarget(triggerEl, config.target);
    const errorTarget = config.errorTarget
      ? resolveTarget(triggerEl, config.errorTarget)
      : null;

    const isGet = config.method === "GET";
    const isForm =
      triggerEl.tagName === "FORM" || triggerEl.closest("form");

    let url = config.url;
    let body = null;

    /* Build request body / query params */
    if (isGet) {
      /* GET: append form fields + include as query params */
      const params = new URLSearchParams();

      if (isForm) {
        const fd = getFormData(triggerEl);
        for (const [key, value] of fd.entries()) {
          params.append(key, value);
        }
      }

      if (config.include) {
        const incData = getIncludeData(config.include);
        if (incData) {
          for (const [key, value] of incData.entries()) {
            params.append(key, value);
          }
        }
      }

      const qs = params.toString();
      if (qs) {
        url += (url.includes("?") ? "&" : "?") + qs;
      }
    } else {
      /* POST/PUT/PATCH/DELETE: form-encoded body */
      if (isForm) {
        const fd = getFormData(triggerEl);
        if (config.include) {
          const incData = getIncludeData(config.include);
          if (incData) {
            for (const [key, value] of incData.entries()) {
              fd.append(key, value);
            }
          }
        }
        body = formDataToUrlEncoded(fd);
      }
    }

    /* Build fetch options */
    const fetchOpts = {
      method: config.method,
      headers: {},
    };

    fetchOpts.headers[HEADER_REQUEST] = "XMLHttpRequest";

    if (!isGet) {
      fetchOpts.headers[HEADER_EKA] = "1";
    }

    if (body) {
      fetchOpts.headers["Content-Type"] = "application/x-www-form-urlencoded";
      fetchOpts.body = body;
    }

    /* Timeout via AbortController */
    const controller = new AbortController();
    fetchOpts.signal = controller.signal;
    const timeoutId = setTimeout(() => controller.abort(), config.timeout);

    /* Dispatch before-request */
    dispatch(triggerEl, "before-request", {
      url: url,
      method: config.method,
      target: target,
    });

    /* Set loading state */
    setBusy(triggerEl);

    try {
      const response = await window.fetch(url, fetchOpts);
      clearTimeout(timeoutId);

      const responseHtml = await response.text();

      dispatch(triggerEl, "after-request", {
        url: url,
        method: config.method,
        status: response.status,
        target: target,
        html: responseHtml,
      });

      if (response.ok) {
        /* Success: swap into target */
        if (config.swap !== SWAP_DELETE && config.swap !== SWAP_NONE) {
          performSwap(target, responseHtml, config.swap);
        } else if (config.swap === SWAP_DELETE) {
          performSwap(target, null, SWAP_DELETE);
        }

        dispatch(triggerEl, "after-swap", {
          url: url,
          target: target,
          html: responseHtml,
          swap: config.swap,
        });
      } else {
        /* Error response (4xx, 5xx) */
        if (errorTarget) {
          performSwap(errorTarget, responseHtml, config.errorSwap);
        }
        console.warn(
          "e: request failed — " + config.method + " " + url + " → " + response.status
        );
        dispatch(triggerEl, "request-error", {
          url: url,
          method: config.method,
          status: response.status,
          target: target,
          html: responseHtml,
        });
      }
    } catch (err) {
      clearTimeout(timeoutId);

      if (err.name === "AbortError") {
        console.warn("e: request timeout — " + config.method + " " + url);
      } else {
        console.warn("e: network error — " + config.method + " " + url, err);
      }

      dispatch(triggerEl, "request-error", {
        url: url,
        method: config.method,
        status: 0,
        target: target,
        error: err.message,
      });
    } finally {
      clearBusy(triggerEl);
    }
  }

  /* ================================================================
   * Trigger Resolution
   * ================================================================ */

  function resolveTriggers(el, config) {
    if (config && config.trigger) {
      return config.trigger
        .split(",")
        .map((t) => t.trim())
        .filter(Boolean);
    }

    /* Default trigger based on element type */
    const tag = el.tagName;
    if (TRIGGER_DEFAULTS[tag]) {
      return [TRIGGER_DEFAULTS[tag]];
    }

    /* <a> with e-* defaults to click */
    if (tag === "A") return ["click"];

    return [DEFAULT_TRIGGER];
  }

  /* ================================================================
   * Revealed Trigger (IntersectionObserver)
   * ================================================================ */

  function ensureRevealObserver() {
    if (revealObserver) return;

    revealObserver = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting && !revealedElements.has(entry.target)) {
            revealedElements.add(entry.target);

            /* Find the e-* element (may be the observed element or a child) */
            const el = closestWithAttr(entry.target, "e-target") || entry.target;
            const config = buildRequestConfig(el);
            if (config) {
              executeRequest(el, config);
            }

            /* Stop observing once revealed */
            revealObserver.unobserve(entry.target);
          }
        }
      },
      { threshold: 0.1 }
    );
  }

  function observeRevealed(el) {
    ensureRevealObserver();
    revealObserver.observe(el);
  }

  /* ================================================================
   * Event Delegation Setup
   *
   * Single set of listeners on document. Handles all e-* elements,
   * including those added dynamically via swaps.
   * ================================================================ */

  function init() {
    /* Delegated event handlers for all trigger types */
    const triggerEvents = ["click", "submit", "change"];

    for (const eventType of triggerEvents) {
      document.addEventListener(
        eventType,
        (evt) => {
          const el = closestWithAttr(evt.target, "e-target");
          if (!el) return;

          const config = buildRequestConfig(el);
          if (!config) return;

          const triggers = resolveTriggers(el, config);

          /* Check if this event type matches a configured trigger */
          let matched = false;
          for (const t of triggers) {
            if (t === eventType) {
              matched = true;
              break;
            }
            /* Special: "load" is handled separately */
          }

          if (!matched) return;

          /* Prevent default for submit and click on <a> */
          if (eventType === "submit" || (eventType === "click" && el.tagName === "A")) {
            evt.preventDefault();
          }

          executeRequest(el, config);
        },
        eventType === "submit" ? true : undefined /* capture for forms */
      );
    }

    /* "load" trigger on DOMContentLoaded */
    document.addEventListener("DOMContentLoaded", () => {
      const loadEls = document.querySelectorAll("[e-target]");
      for (const el of loadEls) {
        const config = buildRequestConfig(el);
        if (!config) continue;

        const triggers = resolveTriggers(el, config);
        if (triggers.includes("load")) {
          executeRequest(el, config);
        }
        if (triggers.includes("revealed")) {
          observeRevealed(el);
        }
      }
    });

    /* Handle dynamically swapped elements with "revealed" trigger.
     * After a swap, new elements may have e-trigger="revealed".
     * We use a MutationObserver to catch these. */
    const swapObserver = new MutationObserver((mutations) => {
      for (const mutation of mutations) {
        for (const node of mutation.addedNodes) {
          if (node.nodeType !== Node.ELEMENT_NODE) continue;

          /* Check the node itself */
          if (hasAttr(node, "e-target") && hasAttr(node, "e-trigger")) {
            const config = buildRequestConfig(node);
            if (config) {
              const triggers = resolveTriggers(node, config);
              if (triggers.includes("revealed")) {
                observeRevealed(node);
              }
            }
          }

          /* Check children */
          const children = node.querySelectorAll
            ? node.querySelectorAll("[e-target][e-trigger]")
            : [];
          for (const child of children) {
            const config = buildRequestConfig(child);
            if (config) {
              const triggers = resolveTriggers(child, config);
              if (triggers.includes("revealed")) {
                observeRevealed(child);
              }
            }
          }
        }
      }
    });

    swapObserver.observe(document.body, { childList: true, subtree: true });
  }

  /* ================================================================
   * Public API — window.e
   * ================================================================ */

  window.e = {
    version: VERSION,

    /**
     * e.fetch(url, opts) — fetch wrapper using runtime's pipe.
     * Adds X-Requested-With header automatically.
     */
    fetch: function (url, opts) {
      opts = opts || {};
      opts.headers = opts.headers || {};
      if (!opts.headers[HEADER_REQUEST]) {
        opts.headers[HEADER_REQUEST] = "XMLHttpRequest";
      }
      if (opts.method && opts.method.toUpperCase() !== "GET") {
        if (!opts.headers[HEADER_EKA]) {
          opts.headers[HEADER_EKA] = "1";
        }
      }
      return window.fetch(url, opts);
    },

    /**
     * e.swap(selector, html, mode) — manual DOM swap.
     * @param {string} selector - CSS selector for target
     * @param {string} html - HTML to swap in
     * @param {string} mode - Swap strategy (default: innerHTML)
     */
    swap: function (selector, html, mode) {
      const target = document.querySelector(selector);
      if (target) {
        performSwap(target, html, mode || SWAP_INNER);
      }
    },
  };

  /* ================================================================
   * Bootstrap
   * ================================================================ */

  init();
})();
