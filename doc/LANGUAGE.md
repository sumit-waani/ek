# Eka — Language Reference

## Comments

`--` starts a line comment. Everything from `--` to the end of the line is ignored by the compiler. Works everywhere — code blocks, HTML sections, inside `{{ }}`.

```eka
-- This is a comment
let name = "Alice"  -- inline comment
```

Inside raw passthrough tags (`<script>`, `<style>`, `<pre>`, `<textarea>`, `<code>`), `--` is treated as literal text — the parser does not look inside these tags.

For HTML comments that should appear in browser output, use `<!-- -->`.

## Variables

```eka
let name = "Alice"       -- mutable, request-local or init-global
const PI = 3.14159       -- immutable, cannot reassign
```

- `let` at init scope is effectively read-only from request scope (the binding cannot be changed by request handlers).
- `let` at request scope is request-local — lives and dies with the request.
- `const` is always immutable — cannot be reassigned anywhere.

Variables must be declared before use. Undeclared variables are a compile-time error caught by `eka check`.

## Types

### Built-in Types

| Type | Description | Example |
|------|-------------|---------|
| `string` | UTF-8 text | `"hello"` |
| `number` | 64-bit float (IEEE 754) | `42`, `3.14`, `0xFF` |
| `bool` | Boolean | `true`, `false` |
| `list` | Ordered collection | `[1, 2, 3]` |
| `map` | Key-value collection | `{name: "Alice", age: 30}` |
| `null` | Null value | `null` |
| `RawString` | Unescaped HTML wrapper (internal) | Created by `html.raw()` |

### Optional Type Annotations

```eka
let str: string = "hello"
let num: number = 42
let bool: bool = true
let items: list<string> = ["a", "b", "c"]
let user: map = {name: "Bob", age: 25}
let maybeName: maybe<string> = null
```

**Annotations are optional and stripped at compile time.** The runtime is fully dynamic. Annotations are used by `eka check` for linting and IDE support. Annotated and unannotated code produces identical bytecode.

```eka
-- These produce identical bytecode:
func greet(name: string): string
  "Hello, ${name}!"
end

func greet(name)
  "Hello, ${name}!"
end
```

## Strings

```eka
let single = "hello"
let interp = "Hello, ${name}!"
let multiline = "Line 1
Line 2
Line 3"
let escaped = "Use \${name} for literal dollar-brace"
```

- String interpolation uses `${expression}` inside double-quoted strings.
- To escape: `\${}` produces literal `${`.
- Strings are UTF-8.
- String indexing: `"hello"[0]` → `"h"`, `"hello"[-1]` → `"o"`. Returns single-character string, or `null` if out of bounds.
- `.length` is a property (not a method): `"hello".length` → `5`.

See [Builtins → str](BUILTINS.md#25-str) for string manipulation functions.

## Numbers

```eka
let n = 42
let f = 3.14
let hex = 0xFF
let neg = -5
```

All numbers are 64-bit floats (IEEE 754). There is no integer type.

To parse a string to a number:

```eka
let id = number.parse("42")         -- 42
let invalid = number.parse("hello") -- null
```

Arithmetic coercion: `"5" + 2` → `7`, `"5" * "2"` → `10`. Strings are automatically coerced to numbers in arithmetic contexts.

## Booleans & Logic

```eka
let a = true
let b = false

if a and b   -- logical AND
if a or b    -- logical OR
if not a     -- logical NOT
```

## Null

`null` represents the absence of a value. Functions return `null` when nothing is found. Accessing a missing key on a map returns `null`.

## Lists

```eka
let items = [1, 2, 3]
let mixed = [1, "hello", true, {name: "Alice"}]
```

### List Operations

| Operation | Returns | Description |
|-----------|---------|-------------|
| `items.length` | number | Number of elements (property) |
| `items[n]` | value or null | Index access, 0-based. `-1` = last |
| `items.push(item)` | null | Append to end |
| `items.pop()` | value or null | Remove and return last element |
| `items.insert(idx, item)` | null | Insert at index |
| `items.removeAt(idx)` | null | Remove element at index |
| `items.removeValue(val)` | null | Remove first occurrence of value |
| `items.indexOf(val)` | number or null | First index of value, or null |
| `items.contains(val)` | bool | Whether value exists in list |

**Design note:** Mutation methods (`push`, `pop`, `insert`, `removeAt`, `removeValue`) return `null` to prevent accidental method chaining. If you need the value after mutation, access it explicitly.

List iteration uses `for` loops or `@for` in templates.

## Maps

```eka
let user = {name: "Alice", age: 30}
let empty = {}
```

### Map Operations

| Operation | Returns | Description |
|-----------|---------|-------------|
| `map.key` | value or null | Dot access |
| `map["key"]` | value or null | Bracket access (for dynamic keys) |
| `map.key = value` | — | Assignment |
| `map["key"] = value` | — | Bracket assignment |
| `map.keys()` | list\<string\> | All keys |
| `map.values()` | list | All values |
| `map.has(key)` | bool | Whether key exists |
| `map.delete(key)` | null | Remove key |

Accessing a missing key returns `null` with no error.

## Comparison

| Operator | Description |
|----------|-------------|
| `==` | Strict equality (type + value) |
| `~=` | Loose equality (coerced) |
| `!=` | Strict inequality |
| `>` | Greater than |
| `>=` | Greater than or equal |
| `<` | Less than |
| `<=` | Less than or equal |

## Null-Coalescing

```eka
let name = request.query("name") ?? "World"
```

`??` checks for `null` only. `0`, `""`, `false` are NOT coalesced — only `null`.

## Null-Safe Access

```eka
let name = user?.name              -- null if user is null
let first = items?.[0]             -- null if items is null
let displayName = user?.profile?.name ?? "Anonymous"
```

Use `?.` when the left-hand side may be null. Use `.` when you're confident it isn't. If `.` is used on null, the runtime throws: `"null reference: cannot access 'name' on null"`.

## Functions

```eka
func greet(name)
  "Hello, ${name}!"
end

func add(a, b)
  a + b
end
```

**Implicit return:** The last expression in a function is its return value.

**Explicit return:** Use `return value` for early exit. `return` alone returns `null`.

```eka
func findUser(id)
  if id < 1
    return null
  end
  db.query("SELECT * FROM users WHERE id = ?", [id])[0]
end
```

**Named parameters:** All function parameters are named. Callers must use names:

```eka
func createUser(name, age, role = "user")
  {name: name, age: age, role: role}
end

let u = createUser(name: "Alice", age: 30)
-- let u = createUser("Alice", 30)  ← ERROR: must use named params
```

**Default values:** Parameters can have defaults with `= value`. Parameters with defaults must come after required parameters.

**Type annotations (optional):**

```eka
func greet(name: string): string
  "Hello, ${name}!"
end
```

## Control Flow

### if / else if / else

```eka
if condition
  ...
else if other
  ...
else
  ...
end
```

### for-in

```eka
for item in list
  ...
end
```

### while

```eka
while condition
  ...
end
```

## Error Handling

```eka
try
  let result = risky()
catch err
  print("Error: ${err}")
end
```

- `err` is always a **string** (the error message).
- Catchable: runtime errors (null reference, DB error, file not found, etc.).
- Uncatchable: syntax errors (caught at `eka check` time).
- No re-throw. If you need to propagate, return the error or call another function.

## HTML Integration

### Variable Interpolation

```eka
<h1>Hello, {{ name }}</h1>
<p>Count: {{ items.length }}</p>
```

**Auto-escaped by default.** HTML special characters (`<`, `>`, `&`, `"`, `'`) are escaped. XSS-safe.

### Interpolation in Attributes

```eka
<a href="/user/{{ user.id }}">{{ user.name }}</a>
<button e-delete="/todo/{{ todo.id }}">Delete</button>
```

`{{ }}` works in all text positions — body text and attribute values alike. There is only one interpolation syntax.

### Raw HTML Output

```eka
<p>{{ html.raw(user.bio) }}</p>
```

`html.raw()` returns a `RawString` wrapper. The template engine detects it and skips escaping. Only use with trusted content.

### Template Control Blocks

```eka
@if user.isAdmin
  <div class="admin-panel">Admin tools</div>
@else if user.isModerator
  <div class="mod-panel">Mod tools</div>
@else
  <div>Welcome, {{ user.name }}</div>
@end

@for post in posts
  <article>
    <h2>{{ post.title }}</h2>
  </article>
@else
  <p>No posts yet.</p>
@end
```

`@else` on `@for` runs when the list is empty or null. If omitted, nothing renders for empty lists.

### Silent Code Blocks

```eka
@do
  let posts = db.query("SELECT * FROM posts ORDER BY created_at DESC")
  let count = posts.length
@end
```

`@do` executes code without producing output. Only valid inside method blocks (`@get`, `@post`, etc.). Banned at top level — use bare code for init.

### Raw Passthrough Tags

Contents of these HTML tags are passed through **verbatim** (no Eka parsing inside):

- `<script>` — JavaScript code
- `<style>` — CSS code
- `<pre>` — Preformatted text
- `<textarea>` — Form text
- `<code>` — Inline code

```eka
<script>
  // This is JavaScript, NOT Eka
  let x = "{{ name }}";  -- NOT interpolated!
</script>
```

For dynamic values in `<script>`, use vanilla JS to read from `window.e` or your own data attributes.

### Comments in HTML

`--` comments work inside HTML sections:

```eka
@get /
  -- This route renders the home page
  <h1>Home</h1>  -- main heading
@end
```

### Fault Tolerance

If an expression inside `{{ }}` throws (null reference, out of bounds, etc.), it renders as an empty string and logs a warning. The page continues rendering. This prevents one broken expression from 500-ing the entire page.

## Interpolation: Complete Rules

| Context | Syntax | Example |
|---------|--------|---------|
| Eka string literals | `${expr}` | `"Hello ${name}"` |
| HTML template output | `{{ expr }}` | `<h1>{{ name }}</h1>` |
| HTML attribute values | `{{ expr }}` | `href="/user/{{ id }}"` |

`${}` is ONLY for string interpolation in Eka code. `{{ }}` is ONLY for HTML template output. They never overlap.
