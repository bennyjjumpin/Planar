# Project: Group Listening Music Player (C++ / wxWidgets)

I'm building a desktop app where multiple people join a shared "session," add songs to a common queue, and listen together in sync. I've already worked through the design; your job is to set up a solid **foundation** for the project based on the decisions below. Please read the whole prompt before starting.

## How I want to work

- I want to write most of the core logic myself. Build the scaffolding, build system, and the first milestone (below) so I have a working base, and leave the sync/state logic as clearly marked stubs with `TODO` comments explaining what each piece needs to do and why.
- Explain non-obvious choices briefly as you go (build config, library choices, wxWebView quirks).
- Before setting up the build, ask me which OS I'm developing on, and confirm library choices with me if you want to deviate from the ones below.

## Core design decisions (already made — don't redesign these)

### 1. Audio source: YouTube embedded player, not downloads
- Songs are played through the **official YouTube IFrame Player API**, embedded in the app. We never download, rip, or proxy audio.
- Songs are identified only by **YouTube video ID**. The only data shared between users is IDs plus small control messages.
- YouTube's API policies require the player to be **visible** — design the UI with a visible video panel (it can be small). No hidden/audio-only player.

### 2. Architecture: server-authoritative relay
- One relay server per session holds the **authoritative state** and is the **single clock**. Clients never trust each other directly.
- Clients send **intents** (e.g., "add video X", "pause", "seek to 120s"). The server applies them in arrival order, updates state, bumps a version number, and broadcasts the result to all clients.
- Clients do **not** act optimistically: they update/play/pause only when the server's broadcast arrives. (Optimistic updates may come later; don't build them now.)
- Conflicts (two people acting at once) are resolved by server arrival order.

### 3. Networking: WebSockets
- Each client holds **one WebSocket** to its session's relay. This avoids NAT/router issues because clients always connect outward.
- Planned host: **Cloudflare Workers + Durable Objects (free plan)**, with **one Durable Object per session**, using the WebSocket Hibernation API (`acceptWebSocket`) so idle sessions cost nothing. Server code is TypeScript.
- Free-plan limits fail with errors rather than billing, so the client should handle disconnects/errors gracefully and reconnect.

### 4. Shared session state
Everything that must be identical on every client:
- `sessionId` / join code
- `members` (who's in the session)
- `queue`: ordered list of entries, each with `videoId`, `addedBy`, and metadata (`title`, `durationSec`, `thumbnailUrl`)
- `currentIndex` (which queue entry is playing)
- `startedAt` (server time, ms)
- `pausedAt` (server time, ms; `null` when playing)
- `version` (integer, incremented on every change)
- Permissions/host (who can skip/remove — to be decided; leave a hook for it)

**Not shared (local only):** volume, window layout, etc.

### 5. Playback timing model
Position is never stored directly — it's derived from fixed timestamps:
- Playing: `position = now - startedAt`
- Paused: `position = pausedAt - startedAt`

Operations (all performed by the server, using server time):
- **Play from start:** `startedAt = serverNow + LEAD`, `pausedAt = null`
- **Pause:** `pausedAt = serverNow`
- **Resume:** `startedAt += (serverNow - pausedAt)`, then `pausedAt = null` (optionally schedule slightly in the future with `LEAD`)
- **Seek to `t` while playing:** `startedAt = serverNow - t`
- **Seek to `t` while paused:** `startedAt = pausedAt - t`

**Scheduling principle:** messages describe *when* something happens in server time, not "do it now." The server schedules starts slightly in the future (`LEAD`, e.g. 200 ms) so every client receives the message before the moment arrives and starts together. On pause, clients seek to `pausedAt - startedAt` so they all stop at the same position even if the message arrived at different times.

### 6. Clock synchronization
- All timestamps are in **server time**. Each client estimates its clock offset from the server using Cristian's algorithm:
  - Send a ping, receive `serverTime`, measure round-trip time `rtt`.
  - `offset = (serverTime + rtt/2) - localNow`
  - Take several samples and keep the one with the **smallest RTT**.
  - Re-sync periodically to correct drift.
- Everywhere the formulas say `now`, the client uses `localNow + offset`.

## Client tech stack
- **C++17 or later**, **wxWidgets 3.2+**, **CMake**.
- **`wxWebView`** hosts a small local HTML page that loads the YouTube IFrame API.
  - C++ → player via `RunScript(...)` (`playVideo`, `pauseVideo`, `seekTo`, `loadVideoById`).
  - Player → C++ via a script message handler (`AddScriptMessageHandler`) for events like state changes, video ended, errors.
  - Known issue: YouTube embeds can refuse to play when the page has no proper origin (e.g., loaded from a bare local file). Load the HTML with a real base URL or otherwise give it a valid origin. Verify this works.
  - On Windows, use the Edge WebView2 backend.
- **WebSocket client:** IXWebSocket (preferred, lightweight) or Boost.Beast.
- **JSON:** nlohmann/json.
- Fetch dependencies in a reproducible way (CMake FetchContent or vcpkg).

## Draft message protocol (JSON over WebSocket)
Client → server (intents):
- `{"type":"join","name":"..."}`
- `{"type":"add","videoId":"..."}`
- `{"type":"play"}`, `{"type":"pause"}`, `{"type":"skip"}`, `{"type":"seek","positionSec":120}`
- `{"type":"remove","index":3}`, `{"type":"move","from":3,"to":1}`
- `{"type":"timeSync","clientSentAt":...}`

Server → clients:
- `{"type":"state","version":N,"state":{...full state...}}` (full snapshot; fine for now since state is small, and simplest for reconnects/late joiners)
- `{"type":"timeSync","serverTime":...,"clientSentAt":...}`
- `{"type":"error","message":"..."}`

Clients ignore any `state` message with a `version` lower than the one they already have.

## Suggested repo layout
```
/client        C++ wxWidgets app (CMake)
  /src
  /web         HTML/JS page hosting the YouTube player
/server        Cloudflare Worker + Durable Object (TypeScript, wrangler)
/protocol      Shared message definitions / docs
README.md
```

## Scope for this task

**Milestone 1 (fully implement):** A wxWidgets window with an embedded `wxWebView` that plays a hardcoded YouTube video, plus native wxWidgets buttons for Play, Pause, and "Seek to 1:00" that control the player through `RunScript`. Player state-change events should flow back to C++ and be shown somewhere (status bar is fine). This proves the riskiest part of the stack.

**Scaffolding only (structure + stubs with TODOs, no full logic):**
- `SessionState` struct mirroring the state above, with JSON (de)serialization.
- A `ClockSync` class with the Cristian's algorithm interface stubbed out.
- A `PlaybackController` that, given state + synced time, decides what the player should be doing (position, playing/paused). Stub the math with TODOs.
- A `SessionConnection` wrapper around the WebSocket client (connect, send intent, receive state callback), stubbed.
- A minimal `/server` Worker + Durable Object skeleton that accepts WebSocket connections per session ID and echoes/broadcasts messages, with TODOs where intents get applied to state.
- A README describing the architecture (summarize the decisions above) and how to build/run the client and server.

When you're done, give me a short summary of what's implemented vs. stubbed, and suggest what I should write first.
