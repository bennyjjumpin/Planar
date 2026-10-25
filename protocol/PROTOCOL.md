# Wire protocol (draft)

JSON text frames over one WebSocket per client, at `/session/<code>`.

## Rooms (HTTP)

`POST /rooms` returns `{"code":"4821"}`: a random, unused 4-digit code
(1000–9999), with the room already created. Only created rooms exist.
Connecting to `/session/<code>` for an unknown code fails the WebSocket
handshake with **HTTP 404**, and clients must not retry. A room is deleted
after being empty for 10 minutes (including a new room nobody joined), and
its code becomes free again.

Mirrored in code by `server/src/protocol.ts` and `client/src/SessionState.h`
/ `SessionConnection.h`. Change all three together.

## Time

All timestamps are **server time**: milliseconds since the Unix epoch, as
returned by `Date.now()` in the Durable Object. Clients convert using their
`ClockSync` offset (`serverNow = localNow + offset`).

## Session state

```jsonc
{
  "sessionId": "abc123",
  "members": [{ "id": "3f9a…", "name": "Ben", "awayUntil": null }],
  // id: public member ID. awayUntil: server ms, set while disconnected
  // within the 30 s grace period, null while connected.
  "queue": [
    {
      "videoId": "M7lc1UVf-VE",
      "addedBy": "uuid",          // Member.id
      "title": "…" | null,         // null until a client reports it ("meta")
      "durationSec": 213 | null,   // likewise
      "thumbnailUrl": "…" | null   // set by the server on "add"
    }
  ],
  "currentIndex": 0,              // -1 = nothing playing
  "startedAt": 1792899907822,     // server ms
  "pausedAt": null,               // server ms, or null while playing
  "version": 17,                  // +1 on every change
  "hostId": "…" | null            // see Permissions below
}
```

Position is derived, never sent:

- playing: `position = now - startedAt`
- paused: `position = pausedAt - startedAt`

## Client → server (intents)

| message | meaning |
|---|---|
| `{"type":"join","name":"Ben","clientId":"…"}` | Introduce yourself. Sent after every (re)connect. `clientId` (16–64 chars of `A-Za-z0-9_-`, optional) is a secret persistent identity, see Identity below. |
| `{"type":"leave"}` | Leaving on purpose: removed immediately (no grace period). |
| `{"type":"add","videoId":"…","title":"…","durationSec":213}` | Append to the queue. `title`/`durationSec` are optional hints (e.g. from search) shown until a player reports the real values. |
| `{"type":"play"}` / `{"type":"pause"}` | Resume / pause. |
| `{"type":"skip"}` | Advance to the next entry. |
| `{"type":"seek","positionSec":120}` | Seek the current entry. |
| `{"type":"remove","index":3}` | Remove a queue entry. |
| `{"type":"move","from":3,"to":1}` | Reorder. |
| `{"type":"ended","index":2}` | My player finished `queue[index]`. Every client sends it; the server advances only if `index` is still current and not paused, so duplicates are harmless. |
| `{"type":"unplayable","index":2,"videoId":"…","code":150}` | The player can't play `queue[index]`. Only codes 2/100/101/150 count (bad ID, removed, embedding disabled); the server skips if that entry is still current **and** still `videoId`. The ID matters: a player error can arrive after the client has already moved on to the next entry. |
| `{"type":"meta","index":2,"videoId":"…","title":"…","durationSec":213}` | Metadata learned from the player. Fills only empty fields, and only if `queue[index].videoId` still matches. |
| `{"type":"timeSync","clientSentAt":12345.6}` | Clock sync ping (client's local ms, echoed back). |

Intents are requests. The server applies them in arrival order and may
reject them (`error`). Clients never apply them locally.

Server rules (see `server/src/session.ts`):

- A new track starts `TRACK_LEAD_MS` (1500 ms) in the future, so every
  client can load it first; a resume starts `LEAD_MS` (200 ms) in the future.
- `add` to an empty or finished queue starts the new entry.
- `skip`, `ended`, or removing the current entry starts the next entry, or
  sets `currentIndex` to -1 at the end of the queue.
- `remove` and `move` keep `currentIndex` on the same entry.
- `seek` clamps to `[0, durationSec]` once the duration is known.
- The first member to join becomes host; when the host leaves, the next
  member does.

## Permissions ("host controls")

| intent | who |
|---|---|
| `add`, `play`, `pause` | everyone |
| `skip`, `seek`, `move` | host |
| `remove` | host, or whoever added that entry |
| `join`, `ended`, `meta`, `unplayable`, `timeSync` | everyone |

Refused intents get an `error` reply. The rules live in `permissionError()`
in `server/src/session.ts`.

## Grace period

When a member who joined with a `clientId` disconnects, they aren't removed
right away. Their `awayUntil` is set to 30 s from now, they keep their place,
and the host keeps the host role. Joining again within that window clears
`awayUntil`; otherwise a Durable Object alarm removes them, and the host role
passes to the longest-present connected member. Members without a
`clientId` are removed immediately.

## Search (HTTP, not WebSocket)

`GET /search?q=<query>` on the relay's host (`https://…` for a `wss://`
relay) returns `{"results":[{"videoId","title","channel","durationSec",
"thumbnailUrl"}]}` or `{"error":"…"}`. It's backed by the YouTube Data API.
The key is a Worker secret (`YOUTUBE_API_KEY`), never part of the app.
Results are embeddable, non-live videos only, and cached in KV for a day.

## Identity

A client generates a random `clientId` once, keeps it, and sends it in
every `join`. The server maps it to a public member ID,
`hex(SHA-256(sessionId + ":" + clientId))[0..24]`, so reconnecting keeps
the same member (and host role, and `addedBy` attribution). Other members
never see the secret. A `join` without a valid `clientId` gets a throwaway
identity for that socket.

If a second socket joins with an identity that's already connected, the
newer one wins: the older socket is closed with **code 4000** ("connected
from another place"). Clients must not auto-reconnect after a 4000, or two
copies of the app would keep kicking each other off.

## Server → client

| message | meaning |
|---|---|
| `{"type":"state","version":N,"state":{…}}` | Full snapshot after any change, and on join. Clients ignore `version` ≤ the one they have. |
| `{"type":"timeSync","serverTime":…,"clientSentAt":…}` | Reply to the sender only. |
| `{"type":"welcome","memberId":"…"}` | Reply to `join`: your public member ID (to compare with `hostId`, `addedBy`). |
| `{"type":"error","message":"…"}` | Rejected intent, malformed message, etc. |

Clients must ignore unknown `type`s so the server can add messages first.
