# Eka — Examples

---

## Example 1: Coming Soon Page

Zero dependencies, zero config. Just HTML.

```eka
-- app.eka

@get /
  <!DOCTYPE html>
  <html>
  <head>
    <title>Launching Soon</title>
    <style>
      body { font-family: system-ui; text-align: center; padding: 20vh 2rem; }
      h1 { font-size: 3rem; margin-bottom: 1rem; }
      p { color: #666; }
    </style>
  </head>
  <body>
    <h1>Coming Soon</h1>
    <p>We are building something awesome.</p>
    <p>Check back on {{ datetime.now().format("MMMM YYYY") }}</p>
  </body>
  </html>
@end
```

---

## Example 2: Todo App

Full CRUD with SQLite and client-side interactivity via `e-*` attributes.

```eka
-- app.eka

@do
  let db = sqlite.open("app.db")
  db.exec("CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY, text TEXT, done INTEGER DEFAULT 0)")
@end

@get /
  <!DOCTYPE html>
  <html>
  <head><title>Todo</title>
    <style>
      .e-busy { opacity: 0.4; }
      li { padding: 4px 8px; list-style: none; }
      .done { text-decoration: line-through; color: #999; }
      form { margin-top: 1rem; }
    </style>
  </head>
  <body>
    <h1>Todos</h1>

    <form e-post="/todo" e-target="#todos" e-swap="afterbegin">
      <input name="text" required autofocus>
      <button>Add</button>
    </form>

    <ul id="todos">
      @do
        let todos = db.query("SELECT * FROM todos ORDER BY id DESC")
      @end
      @for t in todos
        <li id="todo-{{ t.id }}" class="{{ if t.done }}done{{ end }}">
          <input type="checkbox"
                 {{ if t.done }}checked{{ end }}
                 e-post="/todo/{{ t.id }}/toggle"
                 e-target="#todo-{{ t.id }}"
                 e-swap="outerHTML">
          {{ t.text }}
          <button e-delete="/todo/{{ t.id }}"
                  e-target="#todo-{{ t.id }}"
                  e-swap="delete"
                  e-confirm="Delete?">×</button>
        </li>
      @else
        <li>No todos yet. Add one above!</li>
      @end
    </ul>
  </body>
  </html>
@end

@post /todo
  @do
    let text = request.form().text
    db.exec("INSERT INTO todos (text, done) VALUES (?, 0)", [text])
    let id = db.lastId()
  @end
  <li id="todo-{{ id }}">
    <input type="checkbox"
           e-post="/todo/{{ id }}/toggle"
           e-target="#todo-{{ id }}"
           e-swap="outerHTML">
    {{ text }}
    <button e-delete="/todo/{{ id }}"
            e-target="#todo-{{ id }}"
            e-swap="delete"
            e-confirm="Delete?">×</button>
  </li>
@end

@post /todo/[id]/toggle
  @do
    let id = request.param("id")
    db.exec("UPDATE todos SET done = NOT done WHERE id = ?", [id])
    let t = db.query("SELECT * FROM todos WHERE id = ?", [id])[0]
  @end
  <li id="todo-{{ t.id }}" class="{{ if t.done }}done{{ end }}">
    <input type="checkbox"
           {{ if t.done }}checked{{ end }}
           e-post="/todo/{{ t.id }}/toggle"
           e-target="#todo-{{ t.id }}"
           e-swap="outerHTML">
    {{ t.text }}
    <button e-delete="/todo/{{ t.id }}"
            e-target="#todo-{{ t.id }}"
            e-swap="delete"
            e-confirm="Delete?">×</button>
  </li>
@end

@delete /todo/[id]
  @do
    db.exec("DELETE FROM todos WHERE id = ?", [request.param("id")])
  @end
  ""
@end
```

---

## Example 3: Blog with Markdown

Blog with SQLite, markdown parsing, and inline editing.

```eka
-- app.eka

@do
  let db = sqlite.open("blog.db")
  db.exec("CREATE TABLE IF NOT EXISTS posts (id INTEGER PRIMARY KEY, title TEXT, body TEXT, created_at TEXT)")
@end

@get /
  <!DOCTYPE html>
  <html>
  <head><title>My Blog</title>
    <style>
      body { font-family: system-ui; max-width: 40rem; margin: 4rem auto; padding: 0 1rem; }
      article { background: #f9f9f9; padding: 1rem; margin: 1rem 0; border-radius: 4px; }
      form { display: flex; flex-direction: column; gap: 0.5rem; margin-top: 2rem; }
      input, textarea, button { padding: 0.5rem; font: inherit; }
      button { background: #2563eb; color: white; border: none; border-radius: 4px; cursor: pointer; }
    </style>
  </head>
  <body>
    <h1>{{ appName }}</h1>

    <div id="posts">
      @do
        let posts = db.query("SELECT * FROM posts ORDER BY created_at DESC")
      @end
      @for post in posts
        <article id="post-{{ post.id }}">
          <h2>{{ post.title }}</h2>
          <div>{{ markdown.parse(post.body) }}</div>
          <small>{{ post.created_at }}</small>
          <button e-delete="/post/{{ post.id }}"
                  e-target="closest article"
                  e-swap="delete"
                  e-confirm="Delete this post?">Delete</button>
        </article>
      @else
        <p>No posts yet. Write the first one!</p>
      @end
    </div>

    <form e-post="/post" e-target="#posts" e-swap="afterbegin">
      <input name="title" placeholder="Title" required>
      <textarea name="body" placeholder="Body (Markdown)" rows="6"></textarea>
      <button>Publish</button>
    </form>
  </body>
  </html>
@end

@post /post
  @do
    let form = request.form()
    let now = datetime.now().format("YYYY-MM-DD HH:mm:ss")
    db.exec("INSERT INTO posts (title, body, created_at) VALUES (?, ?, ?)",
            [form.title, form.body, now])
    let id = db.lastId()
  @end
  <article id="post-{{ id }}">
    <h2>{{ form.title }}</h2>
    <div>{{ markdown.parse(form.body) }}</div>
    <small>{{ now }}</small>
    <button e-delete="/post/{{ id }}"
            e-target="closest article"
            e-swap="delete"
            e-confirm="Delete this post?">Delete</button>
  </article>
@end

@delete /post/[id]
  @do
    db.exec("DELETE FROM posts WHERE id = ?", [request.param("id")])
  @end
  ""
@end
```

---

## Example 4: Real-Time Chat with SSE

Multi-user chat with SSE for real-time updates.

```eka
-- app.eka

@do
  let db = sqlite.open("chat.db")
  db.exec("CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY, text TEXT, created_at TEXT)")
@end

@get /
  <!DOCTYPE html>
  <html>
  <head><title>Chat</title>
    <style>
      body { font-family: system-ui; max-width: 30rem; margin: 4rem auto; padding: 0 1rem; }
      #messages { display: flex; flex-direction: column; gap: 0.5rem; max-height: 24rem; overflow-y: auto; }
      .msg { background: #f9f9f9; padding: 0.5rem 1rem; border-radius: 4px; }
      form { display: flex; gap: 0.5rem; margin-top: 1rem; }
      input { flex: 1; padding: 0.5rem; }
      button { padding: 0.5rem 1rem; background: #2563eb; color: white; border: none; border-radius: 4px; }
    </style>
  </head>
  <body>
    <h1>Chat</h1>

    <div id="messages">
      @do
        let msgs = db.query("SELECT * FROM messages ORDER BY created_at DESC LIMIT 50")
      @end
      @for msg in msgs
        <div class="msg">
          <p>{{ html.escape(msg.text) }}</p>
          <small>{{ msg.created_at }}</small>
        </div>
      @end
    </div>

    <form e-post="/send" e-target="this" e-swap="none">
      <input name="text" placeholder="Type a message..." required autofocus>
      <button>Send</button>
    </form>

    <script>
      const source = new EventSource('/events')
      source.addEventListener('message', (e) => {
        const msg = JSON.parse(e.data)
        const div = document.createElement('div')
        div.className = 'msg'
        div.innerHTML = '<p>' + msg.text + '</p><small>' + msg.time + '</small>'
        document.getElementById('messages').prepend(div)
      })
    </script>
  </body>
  </html>
@end

@get /events
  sse.connect()
@end

@post /send
  @do
    let text = request.form().text
    if str.trim(text) == ""
      response.status(400)
      return
    end
    let now = datetime.now().format("HH:mm:ss")
    db.exec("INSERT INTO messages (text, created_at) VALUES (?, ?)", [text, now])
    sse.broadcast("message", json.stringify({text: html.escape(text), time: now}))
  @end
  ""
@end
```

---

## Example 5: Contact Form with Email

Form validation, email sending, CSRF protection.

```eka
-- app.eka

@do
  let appName = "My Site"
@end

@get /
  <!DOCTYPE html>
  <html>
  <body style="font-family: system-ui; max-width: 30rem; margin: 4rem auto; padding: 0 1rem;">
    <h1>Contact Us</h1>

    @if request.query("sent")
      <div style="background: #d1fae5; color: #065f46; padding: 0.5rem 1rem; border-radius: 4px; margin-bottom: 1rem;">
        Message sent! We'll get back to you soon.
      </div>
    @end

    <form method="post" action="/contact" style="display: flex; flex-direction: column; gap: 0.5rem;">
      <input type="hidden" name="_csrf" value="{{ session.csrf() }}">
      <input name="name" placeholder="Your name" required>
      <input name="email" type="email" placeholder="Your email" required>
      <textarea name="message" placeholder="Your message" rows="5" required></textarea>
      <button style="background: #2563eb; color: white; border: none; padding: 0.5rem; border-radius: 4px;">
        Send Message
      </button>
    </form>
  </body>
  </html>
@end

@post /contact
  @do
    let form = request.form()

    -- Validate
    if not validate.required(form.name)
      response.status(400)
      response.html("Name is required.")
      return
    end
    if not validate.email(form.email)
      response.status(400)
      response.html("Valid email is required.")
      return
    end
    if not validate.required(form.message)
      response.status(400)
      response.html("Message is required.")
      return
    end

    -- Send email
    email.send({
      to: "admin@example.com",
      from: form.email,
      subject: "Contact from ${form.name}",
      body: form.message,
      smtp: env.get("SMTP_SERVER", "smtp.example.com:587")
    })

    response.redirect("/?sent=1")
  @end
@end
```

---

## Example 6: JSON API

Pure JSON API with no HTML — perfect for mobile app backends.

```eka
-- app.eka

@do
  let db = sqlite.open("api.db")
  db.exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT, price NUMBER)")
  -- Seed data
  if db.query("SELECT COUNT(*) AS c FROM items")[0].c == 0
    db.exec("INSERT INTO items (name, price) VALUES (?, ?)", ["Widget", 9.99])
    db.exec("INSERT INTO items (name, price) VALUES (?, ?)", ["Gadget", 19.99])
  end
@end

-- List all items
@get /api/items
  db.query("SELECT * FROM items ORDER BY id")
@end

-- Get single item
@get /api/items/[id]
  @do
    let item = db.query("SELECT * FROM items WHERE id = ?", [request.param("id")])[0]
    if item == null
      response.status(404)
      response.json({error: "Item not found"})
      return
    end
  @end
  item
@end

-- Create item
@post /api/items
  @csrf off
  @do
    let data = request.json()
    if data?.name == null or data?.price == null
      response.status(400)
      response.json({error: "name and price are required"})
      return
    end
    db.exec("INSERT INTO items (name, price) VALUES (?, ?)", [data.name, data.price])
    let id = db.lastId()
    let item = db.query("SELECT * FROM items WHERE id = ?", [id])[0]
  @end
  item
@end

-- Update item
@put /api/items/[id]
  @csrf off
  @do
    let data = request.json()
    let item = db.query("SELECT * FROM items WHERE id = ?", [request.param("id")])[0]
    if item == null
      response.status(404)
      response.json({error: "Item not found"})
      return
    end
    let name = data?.name ?? item.name
    let price = data?.price ?? item.price
    db.exec("UPDATE items SET name = ?, price = ? WHERE id = ?", [name, price, request.param("id")])
  @end
  db.query("SELECT * FROM items WHERE id = ?", [request.param("id")])[0]
@end

-- Delete item
@delete /api/items/[id]
  @csrf off
  @do
    let item = db.query("SELECT * FROM items WHERE id = ?", [request.param("id")])[0]
    if item == null
      response.status(404)
      response.json({error: "Item not found"})
      return
    end
    db.exec("DELETE FROM items WHERE id = ?", [request.param("id")])
  @end
  {deleted: true}
@end
```

---

## Example 7: Auth (Login + Session)

Simple passwordless or password-based auth using sessions.

```eka
-- app.eka

@do
  let db = sqlite.open("app.db")
  db.exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, password TEXT)")
  let appName = "Members Area"
@end

func hashPassword(password)
  crypto.sha256(password)
end

@get /
  @do
    let userId = session.get("user_id")
  @end
  @if userId
    <!DOCTYPE html>
    <html>
    <body style="font-family: system-ui; max-width: 30rem; margin: 4rem auto; padding: 0 1rem;">
      <h1>Welcome back!</h1>
      <p>You are logged in as user #{{ userId }}.</p>
      <a href="/logout">Log out</a>
    </body>
    </html>
  @else
    <!DOCTYPE html>
    <html>
    <body style="font-family: system-ui; max-width: 30rem; margin: 4rem auto; padding: 0 1rem;">
      <h1>{{ appName }}</h1>

      <h2>Log In</h2>
      <form method="post" action="/login" style="display: flex; flex-direction: column; gap: 0.5rem;">
        <input type="hidden" name="_csrf" value="{{ session.csrf() }}">
        <input name="email" type="email" placeholder="Email" required>
        <input name="password" type="password" placeholder="Password" required>
        <button style="background: #2563eb; color: white; border: none; padding: 0.5rem; border-radius: 4px;">Log In</button>
      </form>

      <h2>Sign Up</h2>
      <form method="post" action="/signup" style="display: flex; flex-direction: column; gap: 0.5rem;">
        <input type="hidden" name="_csrf" value="{{ session.csrf() }}">
        <input name="email" type="email" placeholder="Email" required>
        <input name="password" type="password" placeholder="Password" required minlength="8">
        <button style="background: #16a34a; color: white; border: none; padding: 0.5rem; border-radius: 4px;">Sign Up</button>
      </form>
    </body>
    </html>
  @end
@end

@post /signup
  @do
    let form = request.form()

    if not validate.email(form.email)
      response.status(400)
      response.html("Invalid email.")
      return
    end
    if str.len(form.password) < 8
      response.status(400)
      response.html("Password must be at least 8 characters.")
      return
    end

    let existing = db.query("SELECT id FROM users WHERE email = ?", [form.email])[0]
    if existing
      response.status(400)
      response.html("Email already registered.")
      return
    end

    db.exec("INSERT INTO users (email, password) VALUES (?, ?)", [form.email, hashPassword(form.password)])
    let userId = db.lastId()
    session.set("user_id", userId)
    response.redirect("/")
  @end
@end

@post /login
  @do
    let form = request.form()

    let user = db.query("SELECT id, password FROM users WHERE email = ?", [form.email])[0]
    if user == null or user.password != hashPassword(form.password)
      response.status(401)
      response.html("Invalid email or password.")
      return
    end

    session.set("user_id", user.id)
    response.redirect("/")
  @end
@end

@get /logout
  @do
    session.delete("user_id")
    response.redirect("/")
  @end
@end
```
