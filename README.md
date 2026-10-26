# Planar

<img src="client/assets/planar.svg" width="56" align="right" alt="Planar logo">

A desktop app (C++ / wxWidgets) for listening together. Someone creates a
room, shares its 6-character code, everyone adds songs to a shared queue, and
playback stays in sync on every computer.

```
client/      C++ wxWidgets app (CMake), builds Planar.exe
  src/       app code
  web/       player.html: hosts the YouTube IFrame player
  assets/    logo (planar.ico / planar.icns / planar.svg) and its generator, make_icon.py
server/      Cloudflare Worker + Durable Object relay (TypeScript, wrangler)
protocol/    PROTOCOL.md: message and state formats
```

## Architecture

**Audio source.** Playback goes through the official YouTube IFrame Player
API, embedded in a `wxWebView`. Nothing is downloaded, ripped or proxied.
Songs are identified by YouTube video ID only; the only data clients share
is IDs plus small control messages. YouTube's policies require a visible
player, so the UI always shows the video panel.

**Server-authoritative relay.** Each session has one relay (a Cloudflare
Durable Object) that holds the authoritative state and is the single clock.
Clients send *intents* ("add X", "pause", "seek to 120s"). The server applies
them in arrival order, bumps `version`, and broadcasts a full state snapshot.
Clients don't act optimistically: they change what they play only when a
broadcast arrives. Conflicts resolve by arrival order.

**Networking.** One outbound WebSocket per client, so there are no NAT or
port-forwarding issues. The host is Cloudflare Workers + Durable Objects on
the free plan, one Durable Object per session, using the WebSocket
Hibernation API so idle sessions cost nothing. Free-plan limits fail with
errors rather than bills, so clients must reconnect gracefully.

**Shared state.** `sessionId`, `members`, `queue` (`videoId`, `addedBy`,
`title`, `durationSec`, `thumbnailUrl`), `currentIndex`, `startedAt`,
`pausedAt`, `version`, and a `hostId` hook for permissions. Volume and
window layout stay local. See [protocol/PROTOCOL.md](protocol/PROTOCOL.md).

**Timing model.** Position is derived from server timestamps, never stored:

| | |
|---|---|
| playing | `position = now - startedAt` |
| paused | `position = pausedAt - startedAt` |
| play from start | `startedAt = now + LEAD`, `pausedAt = null` |
| pause | `pausedAt = now` |
| resume | `startedAt += now - pausedAt`, `pausedAt = null` |
| seek t (playing) | `startedAt = now - t` |
| seek t (paused) | `startedAt = pausedAt - t` |

Messages say *when* something happens, not "do it now". Starts are scheduled
`LEAD` (~200 ms) in the future so everyone gets the message first. On pause,
clients seek to `pausedAt - startedAt` so they all stop at the same spot.

**Clock sync.** Clients estimate their offset from the server with Cristian's
algorithm (`offset = serverTime + rtt/2 - localNow`, keeping the
smallest-RTT sample of several) and re-sync periodically. "now" on the
client always means `localNow + offset`.

### Client components

| file | role | status |
|---|---|---|
| `YouTubePlayer` | wxWebView panel hosting `web/player.html`; C++→JS via `RunScriptAsync`, JS→C++ via script message handler |
| `YouTubeBrowser` | second wxWebView showing m.youtube.com for finding songs; an injected script turns clicks on video links into "add to queue" picks |
| `MainFrame` | UI (join row, queue, transport, volume), sync rounds, and the reconcile loop that keeps the player matching the server |
| `SessionState` | mirror of server state + JSON (de)serialization |
| `SessionConnection` | IXWebSocket wrapper: intent builders, version gating, reconnect with backoff, re-join with the persistent identity, stop on "replaced" (4000), dispatch to the GUI thread |
| `ClockSync` | Cristian's algorithm: 8-sample first round, 3-sample resync every 30 s, best-RTT sample from the last 2 min |
| `PlaybackController` | (state, synced clock) → desired video, position, playing, time until a scheduled start |

### How the client follows the server

`MainFrame::Reconcile` runs on every new state, at scheduled starts, and on
every player status report (polled every 500 ms):

1. **Different track:** load it at the computed position (or cue it, if the
   start is still in the future).
2. **Scheduled start or resume:** the player waits, parked, and presses play
   just early enough to be moving at the scheduled server time.
3. **Pause:** pause and park at exactly `pausedAt - startedAt`, so every
   client stops at the same frame.
4. **Drift:** if the playhead is more than 120 ms off, seek. A smaller
   offset (over 60 ms) that stays in the same direction for 4 s is also
   corrected once. That's usually left over from a start or seek.
5. **Local pauses:** if something outside the app pauses the player, such as
   clicking on the video, the client doesn't fight it. It holds ("Paused on
   this computer"), and Play rejoins the group at the right position.
   Windows media keys are kept away from the embedded player entirely, via a
   WebView2 flag in `App.cpp`.

Every player command takes some time to take effect: about 100–300 ms for a
play, more for a cold start. The client measures how far off it lands after
each command and learns a latency per command kind (start, resume, seek),
then issues commands that much early or seeks that much ahead. After a few
commands, the clients settle within a few tens of ms of the server
timeline. The "Sync" status bar field shows the current RTT and drift.

Tracks end when a client's player reports `ended`. The server advances the
queue on the first report and ignores duplicates. Videos nobody can play
(removed, embedding disabled) are skipped the same way via `unplayable`.

### Using Planar

- **Start:** enter your name, then **Create a room** (you get a random
  6-character code of letters and numbers; **Copy** puts it on the clipboard), or type a friend's code
  and **Join**. Unknown codes are refused, so a typo can't create an empty
  room by accident.
- **Add songs:** type in the search box and press Enter, or click **Browse
  YouTube**. Either opens YouTube (the mobile site) in the side column;
  clicking a video adds it to the queue instead of opening it, so you can
  pick several in a row. Pasting a YouTube link adds it directly. Browsing
  uses no API quota.
- **Queue:** drag to reorder (host), and right-click for Move up/down and
  Remove. The Delete key removes too. You can remove songs you added; the
  host can remove any.
- **Playback:** one Play/Pause button (it pauses for everyone), Skip and the
  seek bar are for the host, and volume is local. If something on your
  computer pauses the video (say, a click on it), only you pause; Play
  rejoins.
- **Leave** (or closing the window) removes you at once. A dropped
  connection instead keeps your place, and host role, for 30 s.
- The footer shows sync status; hover it for the round-trip time and offset.

### Identity, permissions, settings

- On first run the app generates a secret `clientId` and stores it with
  its settings in `HKCU\Software\Planar`. Reconnecting keeps you the same
  member, so your queue entries stay yours and a reconnecting host stays
  host. Opening the same identity twice moves you to the newest window.
- Permissions are "host controls" (see `protocol/PROTOCOL.md`). The server
  enforces them, and the UI greys out what you can't do. The header shows
  whether you're the host; hover "N listening" to see who's there.
- Your name, volume and learned player latencies are remembered.
- The relay is fixed to Planar's Cloudflare Worker (`kDefaultRelayUrl` in
  `MainFrame.h`); it isn't shown in the UI.

Command-line options, for testing only (normal use needs none):

```
Planar --relay ws://localhost:8787 --create --name Ann --client-id ann_test_identity_01
Planar --relay ws://localhost:8787 --room K7M2QX --name Bob --client-id bob_test_identity_01 --volume 0 --log bob.csv
```

`--relay` points at another relay (e.g. `wrangler dev`), and `--create` /
`--room <code>` create or join a room on startup.

`--log` writes a CSV row for every state, player command and status poll,
with the desired position, the reported position and the drift. Two more
options are for testing only:

- `--client-id <id>`: use this identity instead of the saved one, and don't
  save settings. Use it to run several members on one machine.
- `--clock-drift-ppm <n>`: make the local clock run fast by n parts per
  million, to exercise periodic resync. Real PC clocks drift by about
  10–50 ppm.

## Building the client (Windows)

Requirements:

- **Visual Studio 2022 Build Tools with "Desktop development with C++"**
  (MSVC v143 + Windows SDK). MSVC rather than MinGW because wxWebView's
  Edge/WebView2 backend is only dependable with MSVC; without it you get the
  IE backend, which YouTube doesn't support.
- CMake ≥ 3.24 and Ninja (both bundled with CLion), and git.
- The Microsoft Edge WebView2 Runtime (preinstalled on Windows 11).

All C++ dependencies are fetched by CMake `FetchContent`, pinned to exact
versions: wxWidgets 3.2.9, IXWebSocket 12.1.0, mbedTLS 3.6.7 (TLS for
`wss://`), nlohmann/json 3.12.0. wx downloads the WebView2 SDK from NuGet
itself. **The first configure and build take a while** (wxWidgets is large),
and later builds are incremental.

**CLion:** Settings → Build, Execution, Deployment → Toolchains → add
*Visual Studio*, move it to the top (or select it in your CMake profile),
then reload the CMake project. Run the `music_player` target, which
builds `Planar.exe`.

**Command line** (from an "x64 Native Tools Command Prompt for VS 2022"):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target music_player
build\client\Planar.exe
```

The build copies `client/web/` next to the executable. The app serves it from
`https://musicplayer.example/` via WebView2's virtual-host mapping.

## Building the client (macOS)

Requirements: Xcode Command Line Tools (`xcode-select --install`), CMake ≥
3.24 and Ninja (both bundled with CLion), and git. On macOS the webview is
WKWebView and TLS uses the system's Secure Transport, so mbedTLS isn't
fetched.

**CLion:** open the folder and run the `music_player` target, which builds
`Planar.app`.

**Command line:**

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target music_player
open build/client/Planar.app
```

The build copies `client/web/` into `Planar.app/Contents/MacOS/web`. The
player page is loaded with `https://planar.example/player.html` as its base
URL, which gives it a real origin without any folder mapping. Settings are
stored in `~/Library/Preferences/Planar Preferences` instead of the registry.

### wxWebView notes

- **Origin.** YouTube embeds refuse to play from a page with no real origin
  (`file://`, `about:blank`, `SetPage()` HTML): you get error 153. So
  `YouTubePlayer` maps the virtual host `musicplayer.example` onto the
  `web/` folder (`ICoreWebView2_3::SetVirtualHostNameToFolderMapping`) and
  loads `https://musicplayer.example/player.html`, which is a proper HTTPS
  origin.
- **Startup order.** wx 3.2 creates the WebView2 asynchronously and has no
  "created" event, so the view first loads `about:blank`. Its first
  `wxEVT_WEBVIEW_LOADED` means the native object exists, and then the
  mapping is set up and the real page is loaded.
- **Autoplay.** Chromium blocks unmuted `playVideo()` that isn't triggered by
  a click inside the page. Native buttons and server broadcasts don't count,
  so `App` sets `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--autoplay-policy=no-user-gesture-required`
  before any webview is created.
- **`RunScriptAsync` over `RunScript`.** In wx 3.2 the synchronous
  `RunScript` on Edge pumps the event loop while waiting, which can re-enter
  your handlers.
- **Threads.** IXWebSocket calls back on its own thread. `SessionConnection`
  hops everything to the GUI thread with `wxTheApp->CallAfter`.

## Running the server

```
cd server
npm install
npm run dev          # wrangler dev on http://localhost:8787
```

Clients connect to `ws://localhost:8787/session/<id>` locally.

**Deployed relay:** `wss://music-player-relay.bennyjjumpin05.workers.dev`
(the app's default). To redeploy after server changes: `npm test`, then
`npm run deploy`. Watch live logs with `npx wrangler tail`; the relay logs
each connect and close.

**Search setup (no longer used by the app):** the client now finds songs
through its in-app YouTube browser, so the relay's `/search` endpoint is
unused. To keep it working anyway, create a YouTube Data API v3 key in Google Cloud
(APIs & Services → enable "YouTube Data API v3" → Credentials → API key,
restricted to that API), then store it on the relay:

```
"YOUR_KEY" | npx wrangler secret put YOUTUBE_API_KEY
```

The free quota is 10,000 units/day. A search costs about 101 units, but
results are cached for a day in the `SEARCH_CACHE` KV namespace, so repeated
searches are free. When the quota runs out, search shows a message until
midnight Pacific time; nothing is billed. For local development, run
`wrangler dev --var YOUTUBE_API_KEY:<key>`, or point `YOUTUBE_API_BASE` at a
mock.

Deploys don't drop connected clients: Cloudflare keeps hibernated
WebSockets open across code updates, so sessions keep playing through a
redeploy.

## Distributing a test build

Build the `Release` configuration (for example into
`cmake-build-release-visual-studio`), then ship `Planar.exe`, the
`web/` folder, and the three VC++ runtime DLLs (`msvcp140.dll`,
`vcruntime140.dll`, `vcruntime140_1.dll`, from
`VC\Redist\MSVC\<version>\x64\Microsoft.VC143.CRT`). Those are the only
non-Windows dependencies. `dist/Planar-win64.zip` is such a package.

`server/src/session.ts` holds the pure state logic (no I/O), and
`server/src/index.ts` is the Durable Object around it: sockets, storage,
broadcast. Unit tests for the logic:

```
npm test
```

The relay applies every intent, bumps `version`, persists state to
Durable Object storage and broadcasts a full snapshot. Members are removed
when their socket closes. When the object restarts without close events,
members with no live socket are pruned the next time state is loaded. An
empty session is deleted after 10 minutes, using a Durable Object alarm.

Server notes:

- The free plan requires **SQLite-backed** Durable Objects, hence
  `new_sqlite_classes` in `wrangler.toml`.
- With hibernation, in-memory fields can vanish between messages. Keep state
  in `ctx.storage` and per-socket info in `serializeAttachment`.
- `Date.now()` in a Worker only advances between I/O events. That's fine for
  timeSync because it's read once when a message arrives.
- The main module may only export handlers and Durable Object classes, so no
  exported constants.
