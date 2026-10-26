import { DurableObject } from "cloudflare:workers";
import { emptyState, type Intent, type ServerMessage, type SessionState } from "./protocol";
import { handleSearch, type SearchEnv } from "./search";
import { applyIntent, AWAY_GRACE_MS, expireAway, markAway, nextAwayDeadline, removeMember } from "./session";

export interface Env extends SearchEnv {
  SESSIONS: DurableObjectNamespace<SessionRelay>;
}

const SESSION_PATH = /^\/session\/([A-Za-z0-9_-]{1,64})$/;

// Room codes: 6 characters from digits and uppercase letters, leaving out
// the lookalikes 0/O and 1/I/L so codes read aloud and type cleanly.
// 31^6 is about 887 million codes, so guessing a live room is impractical.
// Mirrored in client/src/MainFrame.cpp (kRoomCodeAlphabet, kRoomCodeLength).
const ROOM_CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
const ROOM_CODE_LENGTH = 6;
const ROOM_CODE_TRIES = 25;

function randomRoomCode(): string {
  // Rejection sampling: bytes past the largest multiple of the alphabet size
  // would make the first letters more likely than the rest.
  const limit = 256 - (256 % ROOM_CODE_ALPHABET.length);
  let code = "";
  while (code.length < ROOM_CODE_LENGTH) {
    const bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    for (const b of bytes) {
      if (b < limit && code.length < ROOM_CODE_LENGTH) code += ROOM_CODE_ALPHABET[b % ROOM_CODE_ALPHABET.length];
    }
  }
  return code;
}

// POST /rooms -> {"code":"K7M2QX"}: picks a random code that isn't in use and
// creates the room. Each try asks that code's Durable Object to claim it, so
// two people creating rooms at once can never get the same code.
async function createRoom(env: Env): Promise<Response> {
  for (let i = 0; i < ROOM_CODE_TRIES; i++) {
    const code = randomRoomCode();
    if (await env.SESSIONS.get(env.SESSIONS.idFromName(code)).claim(code)) {
      return Response.json({ code });
    }
  }
  return Response.json({ error: "Couldn't find a free room code. Try again in a moment." }, { status: 503 });
}

// Close code telling a client that the same identity connected again from
// somewhere else. The client must not auto-reconnect (or two instances
// would keep kicking each other off).
const CLOSE_REPLACED = 4000;

const CLIENT_ID = /^[A-Za-z0-9_-]{16,64}$/;

// Public member ID for a secret clientId. Hashed so that other members
// (who see member IDs in the state) can't learn the secret and impersonate.
async function memberIdFor(sessionId: string, clientId: string): Promise<string> {
  const data = new TextEncoder().encode(`${sessionId}:${clientId}`);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", data));
  return [...digest.slice(0, 12)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// How long an empty room is kept, so a brief network blip (or everyone
// restarting the app) doesn't wipe it. Also how long a freshly created room
// waits for its creator to connect.
const EMPTY_SESSION_TTL_MS = 10 * 60 * 1000;

// Entry point: route /session/<id> to that session's Durable Object.
export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    // Handled by the Worker itself: no session (Durable Object) involved.
    if (url.pathname === "/search") return handleSearch(request, env);
    if (url.pathname === "/rooms") {
      if (request.method !== "POST") return new Response("Use POST", { status: 405 });
      return createRoom(env);
    }

    const match = url.pathname.match(SESSION_PATH);
    if (!match) return new Response("Not found", { status: 404 });
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("Expected a WebSocket upgrade", { status: 426 });
    }
    // idFromName is deterministic: everyone using the same join code reaches
    // the same Durable Object instance, wherever they are in the world.
    // Codes are uppercase; a code typed in lowercase still finds its room.
    const stub = env.SESSIONS.get(env.SESSIONS.idFromName(match[1].toUpperCase()));
    return stub.fetch(request);
  },
} satisfies ExportedHandler<Env>;

// Per-socket data that survives hibernation (see serializeAttachment).
interface SocketInfo {
  memberId: string;
  sessionId: string;
  // Joined with a persistent clientId. Only those get a grace period when
  // they disconnect: a throwaway identity can never come back anyway.
  persistent?: boolean;
}

// One instance per session. It's the single authority and the single clock.
//
// Uses the WebSocket Hibernation API: sockets are accepted with
// ctx.acceptWebSocket(), so when the session is idle the object is evicted
// from memory while the sockets stay open, and costs nothing. The catch: any
// in-memory field can vanish between messages. State lives in ctx.storage
// and per-connection info in the socket attachment.
//
// Durable Objects process one event at a time per instance (input gates
// hold new messages while storage operations are in flight), so intents are
// applied strictly in arrival order with no extra locking.
export class SessionRelay extends DurableObject<Env> {
  private state: SessionState | null = null; // cache; storage is the source of truth

  // Creates the room if this code is free. Called via RPC from createRoom().
  // Durable Objects run one event at a time, so check-then-create is atomic.
  async claim(code: string): Promise<boolean> {
    if (this.ctx.getWebSockets().length > 0) return false;
    if (await this.ctx.storage.get<SessionState>("state")) return false;
    const state = emptyState(code);
    this.state = state;
    await this.ctx.storage.put("state", state);
    // Deleted again if nobody connects within the TTL.
    await this.ctx.storage.put("emptyAt", Date.now() + EMPTY_SESSION_TTL_MS);
    await this.scheduleAlarm(state);
    return true;
  }

  async fetch(request: Request): Promise<Response> {
    const sessionId = new URL(request.url).pathname.match(SESSION_PATH)![1].toUpperCase();
    // Only rooms created through POST /rooms exist; a mistyped code must
    // not silently create a new, empty room.
    if (!(await this.ctx.storage.get<SessionState>("state"))) {
      return new Response("No room with that code", { status: 404 });
    }
    const { 0: client, 1: server } = new WebSocketPair();

    this.ctx.acceptWebSocket(server);
    const info: SocketInfo = { memberId: crypto.randomUUID(), sessionId };
    server.serializeAttachment(info);

    console.log(`connect session=${sessionId} sockets=${this.ctx.getWebSockets().length}`);

    // Someone is here again: the session isn't abandoned.
    await this.ctx.storage.delete("emptyAt");
    await this.scheduleAlarm(await this.loadState(sessionId));

    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    // Read the clock first, before any awaits, so serverTime is as close to
    // message arrival as possible.
    const now = Date.now();

    if (typeof message !== "string") return;
    let intent: Intent;
    try {
      intent = JSON.parse(message);
    } catch {
      this.send(ws, { type: "error", message: "invalid JSON" });
      return;
    }
    if (typeof intent !== "object" || intent === null) return;

    // Answer time sync immediately; it never touches state.
    if (intent.type === "timeSync") {
      this.send(ws, { type: "timeSync", serverTime: now, clientSentAt: intent.clientSentAt });
      return;
    }

    const info = ws.deserializeAttachment() as SocketInfo;
    const state = await this.loadState(info.sessionId);

    if (intent.type === "join" && typeof intent.clientId === "string" && CLIENT_ID.test(intent.clientId)) {
      await this.adoptIdentity(ws, info, state, await memberIdFor(info.sessionId, intent.clientId));
    }

    const result = applyIntent(state, info.memberId, intent, now);
    if (!result.ok) {
      this.send(ws, { type: "error", message: result.error });
      return;
    }

    if (intent.type === "join") this.send(ws, { type: "welcome", memberId: info.memberId });
    if (result.changed) {
      await this.commit(state);
    } else if (intent.type === "join") {
      // A re-join that changed nothing still needs a snapshot.
      this.send(ws, { type: "state", version: state.version, state });
    }
  }

  // Binds this socket to a persistent member identity. If another socket
  // already has it (the same client reconnecting before the server noticed
  // the old connection drop, or a second copy of the app), the new socket
  // wins and the old one is closed.
  private async adoptIdentity(ws: WebSocket, info: SocketInfo, state: SessionState, memberId: string): Promise<void> {
    if (info.memberId === memberId) return;
    const previousId = info.memberId;
    info.memberId = memberId;
    info.persistent = true;
    ws.serializeAttachment(info);

    for (const other of this.ctx.getWebSockets()) {
      if (other === ws) continue;
      const otherInfo = other.deserializeAttachment() as SocketInfo | null;
      if (otherInfo?.memberId !== memberId) continue;
      // Detach first so its close event doesn't remove the member.
      other.serializeAttachment({ ...otherInfo, memberId: "" });
      try {
        other.close(CLOSE_REPLACED, "connected from another place");
      } catch {
        // already closed
      }
    }

    // Drop the throwaway identity this socket had before joining (only
    // present if it sent intents before its first join).
    removeMember(state, previousId);
  }

  async webSocketClose(ws: WebSocket, code: number, reason: string): Promise<void> {
    console.log(`close code=${code} reason=${reason}`);
    await this.dropSocket(ws);
    try {
      ws.close(code, reason); // complete the close handshake
    } catch {
      // already closed
    }
  }

  async webSocketError(ws: WebSocket, error: unknown): Promise<void> {
    console.error("websocket error", error);
    await this.dropSocket(ws);
  }

  // One alarm per object, used for two jobs: removing members whose grace
  // period ran out, and deleting a session that has been empty for a while.
  async alarm(): Promise<void> {
    const stored = await this.ctx.storage.get<SessionState>("state");
    if (!stored) return; // already deleted
    const state = await this.loadState(stored.sessionId);
    const now = Date.now();
    if (expireAway(state, now)) await this.commit(state);

    const emptyAt = await this.ctx.storage.get<number>("emptyAt");
    if (emptyAt !== undefined && now >= emptyAt && this.openSockets().length === 0) {
      this.state = null;
      await this.ctx.storage.deleteAll();
      return;
    }
    await this.scheduleAlarm(state);
  }

  // ---- helpers --------------------------------------------------------------

  private async dropSocket(ws: WebSocket): Promise<void> {
    const info = ws.deserializeAttachment() as SocketInfo | null;
    if (!info || !info.memberId) return;
    const state = await this.loadState(info.sessionId);
    // Only act if no other open socket carries the identity.
    const stillHere = this.openSockets(ws).some(
      (s) => (s.deserializeAttachment() as SocketInfo | null)?.memberId === info.memberId,
    );
    if (!stillHere) {
      const changed = info.persistent
        ? markAway(state, info.memberId, Date.now() + AWAY_GRACE_MS)
        : removeMember(state, info.memberId);
      if (changed) await this.commit(state);
    }

    if (this.openSockets(ws).length === 0) await this.ctx.storage.put("emptyAt", Date.now() + EMPTY_SESSION_TTL_MS);
    await this.scheduleAlarm(state);
  }

  // Open sockets, optionally excluding one (getWebSockets() may still include
  // a socket that is closing).
  private openSockets(except?: WebSocket): WebSocket[] {
    return this.ctx.getWebSockets().filter((s) => s !== except && s.readyState === WebSocket.OPEN);
  }

  // Points the alarm at the earliest pending job, or clears it.
  private async scheduleAlarm(state: SessionState): Promise<void> {
    const times: number[] = [];
    const away = nextAwayDeadline(state);
    if (away !== null) times.push(away);
    const emptyAt = await this.ctx.storage.get<number>("emptyAt");
    if (emptyAt !== undefined) times.push(emptyAt);
    if (times.length > 0) await this.ctx.storage.setAlarm(Math.min(...times));
    else await this.ctx.storage.deleteAlarm();
  }

  private async loadState(sessionId: string): Promise<SessionState> {
    if (!this.state) {
      const state = (await this.ctx.storage.get<SessionState>("state")) ?? emptyState(sessionId);
      // Older stored states predate awayUntil.
      for (const m of state.members) m.awayUntil ??= null;
      // If the object restarted without close events (a crash, a migration),
      // stored members may have no socket any more. Treat them as away, so
      // they get the usual grace period to reconnect; the alarm removes them
      // otherwise. Persisted with the next commit.
      const live = new Set(
        this.ctx.getWebSockets().map((ws) => (ws.deserializeAttachment() as SocketInfo | null)?.memberId),
      );
      let ghosts = false;
      for (const m of state.members) if (!live.has(m.id)) ghosts = markAway(state, m.id, Date.now() + AWAY_GRACE_MS) || ghosts;
      this.state = state;
      if (ghosts) await this.scheduleAlarm(state);
    }
    return this.state;
  }

  // Every change bumps the version, is persisted, then goes out to everyone
  // as a full snapshot.
  private async commit(state: SessionState): Promise<void> {
    state.version++;
    this.state = state;
    await this.ctx.storage.put("state", state);
    this.broadcast({ type: "state", version: state.version, state });
  }

  private send(ws: WebSocket, msg: ServerMessage): void {
    try {
      ws.send(JSON.stringify(msg));
    } catch {
      // Socket already closing; webSocketClose will clean up.
    }
  }

  private broadcast(msg: ServerMessage): void {
    const text = JSON.stringify(msg);
    for (const ws of this.ctx.getWebSockets()) {
      try {
        ws.send(text);
      } catch {
        // ignore, same as above
      }
    }
  }
}
