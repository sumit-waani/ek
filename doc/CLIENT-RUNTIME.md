# Eka — Client Runtime

Eka ships an **embedded client runtime** at `/_eka.js`. It is a hand-written JavaScript file (target: ~500 LOC, 8–12 KB gzipped), embedded in the Eka binary as a string constant. Zero external CDN, zero dependencies.

The runtime implements a **server-driven swap** model: the server returns HTML, the client swaps it into the DOM. There is **no reactive state, no signals, no `x-data`, no virtual DOM.** That is the user's responsibility — write vanilla JS if you need client state.

---

## Runtime Delivery

- **URL:** `/_eka.js?v=<version>` (version = Eka binary version)
- **Served by:** the Eka runtime itself (virtual route)
- **Caching:** `Cache-Control: public, max-age=31536000, immutable`
- **Content-Encoding:** `gzip` when client supports it
- **Browser baseline:** ES2020 (`Proxy`, `fetch`, `IntersectionObserver`, `AbortController`)

## Auto-Injection

When the runtime sends an HTML response, it scans the body for `e-*` attributes. If ≥1 is found, exactly one script tag is injected into the document `<head>`:

```html
<script src="/_eka.js?v=1.0.0" defer></script>
```

**No `e-*` attributes → no script injection → zero overhead.**

**Manual opt-out:** set response header `X-Eka-Runtime: skip`.

**Manual opt-in** (for static HTML in `public/`): include the script tag yourself.

---

## Attribute Set

All attributes are prefixed `e-`. The runtime is dormant until one of these is encountered.

### Core Attributes

| Attribute | Applies to | Description |
|-----------|------------|-------------|
| `e-get="<url>"` | any | Send GET, swap response into `e-target` |
| `e-post="<url>"` | form, any | Send POST (form-encoded or JSON) |
| `e-put="<url>"` | form, any | Send PUT |
| `e-delete="<url>"` | form, any | Send DELETE |
| `e-patch="<url>"` | form, any | Send PATCH |
| `e-target="<selector>"` | any | **Required.** Where to put the response |
| `e-swap="<mode>"` | any | Swap strategy. Default: `innerHTML` |
| `e-trigger="<events>"` | any | Comma-separated events. Defaults by element type |
| `e-include="<selector>"` | any | Include additional form fields from elsewhere |
| `e-confirm="<message>"` | any | Show `confirm()` dialog before sending |
| `e-timeout="<ms>"` | any | Request timeout in milliseconds. Default: 30000 |
| `e-error-target="<selector>"` | any | Where to swap error responses |
| `e-error-swap="<mode>"` | any | Swap mode for errors. Default: `innerHTML` |

No other `e-*` attributes exist. Unknown attributes are ignored.

### `e-target` Selectors

The `e-target` attribute accepts:

| Value pattern | Resolves to |
|--------------|------------|
| Any CSS selector (`#id`, `.class`, `tag`, `[attr]`) | `document.querySelector(selector)` |
| `this` | The triggering element itself |
| `closest <selector>` | `element.closest(selector)` — walks up DOM tree |
| `find <selector>` | `element.querySelector(selector)` — walks down from element |

```html
<!-- Target by ID -->
<button e-post="/add" e-target="#list">Add</button>

<!-- Replace closest parent container -->
<button e-delete="/item/5" e-target="closest article" e-swap="delete">×</button>

<!-- Replace the button itself -->
<button e-get="/refresh" e-target="this" e-swap="outerHTML">Refresh</button>
```

---

## Swap Strategies

| Mode | Behavior |
|------|----------|
| `innerHTML` | (default) Replace `innerHTML` of target |
| `outerHTML` | Replace the target element itself |
| `beforeend` | Append response as last child of target |
| `afterbegin` | Prepend response as first child of target |
| `delete` | Remove target from DOM. Response body ignored |
| `none` | Do nothing with response. Fire-and-forget |

---

## Trigger Events

| Trigger | Fires when | Default for |
|---------|-----------|-------------|
| `click` | Element clicked | `<button>`, `<a>`, `<div>`, etc. |
| `change` | Value changes | `<input>`, `<select>`, `<textarea>` |
| `submit` | Form submitted | `<form>` |
| `load` | Once on DOMContentLoaded | `<body>`, `<html>` |
| `revealed` | Element scrolled into view (IntersectionObserver) | (opt-in only) |

`e-trigger` is a comma-separated list:

```html
<input e-get="/search" e-trigger="change" e-target="#results">
<button e-get="/more" e-trigger="click, revealed" e-target="#list" e-swap="beforeend">
```

No polling trigger. Users needing polling write vanilla JS with `setInterval` + `window.e.fetch`.

---

## Request Encoding

| Element type | Encoding |
|-------------|----------|
| `<form>` with `e-post`/`e-put`/`e-patch`/`e-delete` | `application/x-www-form-urlencoded` (form data). Form default-submit is prevented |
| Non-form trigger (`<button>`, `<div>`, `<a>`) | No body (GET) or empty body (POST) |
| `e-include` selector's values | Added as `?key=value&...` query params for GET, or form body for POST |

### Headers Sent

- All requests: `X-Requested-With: XMLHttpRequest`
- All `e-*` POST/PUT/PATCH/DELETE requests: `X-Eka-Request: 1` (satisfies CSRF check)
- No other custom headers. Users needing auth headers use `window.e.fetch` directly.

---

## Public JS API

The runtime exposes `window.e`:

```js
window.e = {
  version,              // "1.0.0"
  fetch(url, opts),     // fetch wrapper using runtime's pipe
  swap(sel, html, mode) // manual DOM swap
}
```

### `e.fetch(url, opts)`

Like `fetch()` but integrates with the runtime's loading state and event system.

```js
e.fetch("/api/users", { method: "POST", body: JSON.stringify(data) })
  .then(res => res.json())
  .then(data => { ... })
```

Options: standard `fetch` options (`method`, `headers`, `body`, etc.).

### `e.swap(sel, html, mode)`

Manual DOM swap using the runtime's swap engine.

```js
e.swap("#results", "<p>New content</p>", "innerHTML")
```

### Events

All events bubble and are prefixed `e:`:

| Event | When |
|-------|------|
| `e:before-request` | Before `fetch()` is called |
| `e:after-request` | After response received, before swap |
| `e:after-swap` | After swap completes (fires even for `swap="none"` with `html: null`) |
| `e:request-error` | Network error or 4xx/5xx response |

```js
document.addEventListener('e:after-swap', (e) => {
  console.log('Swapped', e.detail.target, 'with', e.detail.html.length, 'chars')
})
```

---

## Loading State

During an in-flight request, the runtime applies one class to the **triggering element**:

```css
.e-busy {
  /* user styles this */
  opacity: 0.4;
  pointer-events: none;
}
```

Removed on completion (success or error).

---

## Redirects

When an `e-*` request receives a redirect (301, 302, 303, 307, 308), the runtime follows the redirect via `fetch()` and swaps the final response into `e-target`. This is standard `fetch` behavior.

For full-page redirects from `e-*` requests, the server should return a normal HTML response with a `<meta http-equiv="refresh">` tag, or the user should use `window.location` in vanilla JS.

---

## Errors

### Default Behavior (no `e-error-target`)

- Network error → `console.warn`, no swap.
- 4xx/5xx response → `console.warn`, no swap.
- `e:request-error` event always fires.

### With `e-error-target`

Error response body is swapped into the error target using `e-error-swap` mode.

```html
<form e-post="/login" e-target="#dashboard" e-error-target="#error" e-error-swap="innerHTML">
  ...
</form>
<div id="error"></div>
```

### Timeout

Default timeout: 30 seconds. Override per element with `e-timeout`:

```html
<button e-get="/slow-report" e-target="#report" e-timeout="60000">Generate</button>
```

---

## Security

- **No auto-CSRF protection in runtime.** CSRF defense is handled server-side. The runtime sends `X-Eka-Request: 1` on all non-GET `e-*` requests, which satisfies the server's CSRF check automatically.
- **innerHTML for swaps.** The server is responsible for escaping HTML. Use `html.escape()` and `{{ }}` (which auto-escapes) for any user-generated content.
- **CSP-friendly.** Runtime uses no `eval`, no `Function()`, no inline scripts. Works with `script-src 'self'`.
- **No external CDN.** `/_eka.js` is served from the same domain. No cross-origin concerns.
