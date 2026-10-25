// Pure session logic: applies intents to state. No I/O, so it can be tested
// without sockets or Durable Objects (see test/session.test.ts).

import type { Intent, SessionState } from "./protocol";

// How far in the future a resume is scheduled, so every client receives the
// message before the moment arrives and they start together.
export const LEAD_MS = 200;

// Starting a *new* track needs more slack than a resume: every client must
// load and buffer the video first. Clients cue it immediately and press play
// at startedAt.
export const TRACK_LEAD_MS = 1500;

// How long a disconnected member keeps their place (and host role) before
// being removed. Covers network blips, app restarts and Wi-Fi roaming.
export const AWAY_GRACE_MS = 30_000;

export const MAX_QUEUE = 200;
export const MAX_NAME = 32;

const VIDEO_ID = /^[A-Za-z0-9_-]{11}$/;

export type ApplyResult =
  | { ok: true; changed: boolean }
  | { ok: false; error: string };

const unchanged: ApplyResult = { ok: true, changed: false };
const changed: ApplyResult = { ok: true, changed: true };
const fail = (error: string): ApplyResult => ({ ok: false, error });

function isInt(n: unknown): n is number {
  return typeof n === "number" && Number.isInteger(n);
}

// YouTube errors that mean "nobody can play this", as opposed to a local
// glitch on one client (5 = HTML5 player error, 153 = our own origin setup).
export const UNPLAYABLE_CODES = new Set([2, 100, 101, 150]);

// Permissions ("host controls"): anyone can add tracks and play/pause; only
// the host can skip, seek and reorder; entries can be removed by the host or
// by whoever added them. Player reports (ended/meta/unplayable) come from
// everyone. Returns an error message, or null if allowed.
export function permissionError(state: SessionState, memberId: string, intent: Intent): string | null {
  const isHost = state.hostId === memberId;
  switch (intent.type) {
    case "skip":
    case "seek":
    case "move":
      return isHost ? null : `only the host can ${intent.type}`;
    case "remove": {
      const entry = isInt(intent.index) ? state.queue[intent.index] : undefined;
      if (isHost || (entry && entry.addedBy === memberId)) return null;
      return "only the host or whoever added it can remove a track";
    }
    default:
      return null;
  }
}

function startEntry(state: SessionState, index: number, now: number): void {
  state.currentIndex = index;
  state.startedAt = now + TRACK_LEAD_MS;
  state.pausedAt = null;
}

function advance(state: SessionState, now: number): void {
  if (state.currentIndex + 1 < state.queue.length) {
    startEntry(state, state.currentIndex + 1, now);
  } else {
    // End of the queue. Adding a track will start it.
    state.currentIndex = -1;
    state.startedAt = 0;
    state.pausedAt = null;
  }
}

// Mutates `state` in place. The caller bumps `version` and broadcasts when
// the result says it changed.
export function applyIntent(state: SessionState, memberId: string, intent: Intent, now: number): ApplyResult {
  const denied = permissionError(state, memberId, intent);
  if (denied) return fail(denied);

  switch (intent.type) {
    case "join": {
      const name = String(intent.name ?? "").trim().slice(0, MAX_NAME) || "Guest";
      const existing = state.members.find((m) => m.id === memberId);
      if (existing) {
        // Back within the grace period: same member, same place.
        if (existing.name === name && existing.awayUntil === null) return unchanged;
        existing.name = name;
        existing.awayUntil = null;
      } else {
        state.members.push({ id: memberId, name, awayUntil: null });
      }
      if (state.hostId === null) state.hostId = memberId;
      return changed;
    }

    case "add": {
      if (typeof intent.videoId !== "string" || !VIDEO_ID.test(intent.videoId)) return fail("invalid videoId");
      if (state.queue.length >= MAX_QUEUE) return fail("queue is full");
      state.queue.push({
        videoId: intent.videoId,
        addedBy: memberId,
        title: typeof intent.title === "string" && intent.title ? intent.title.slice(0, 200) : null,
        durationSec:
          typeof intent.durationSec === "number" && intent.durationSec > 0 && intent.durationSec < 86400
            ? intent.durationSec
            : null,
        // Deterministic, so the server can fill it in without any API call.
        thumbnailUrl: `https://i.ytimg.com/vi/${intent.videoId}/mqdefault.jpg`,
      });
      if (state.currentIndex === -1) startEntry(state, state.queue.length - 1, now);
      return changed;
    }

    case "play": {
      if (state.currentIndex === -1) {
        if (state.queue.length === 0) return unchanged;
        startEntry(state, 0, now);
        return changed;
      }
      if (state.pausedAt === null) return unchanged; // already playing
      // Resume: keep the paused position, but at a moment slightly in the
      // future. (Spec: startedAt += now - pausedAt, plus LEAD.)
      state.startedAt += now + LEAD_MS - state.pausedAt;
      state.pausedAt = null;
      return changed;
    }

    case "pause": {
      if (state.currentIndex === -1 || state.pausedAt !== null) return unchanged;
      // Pausing during the start lead (now < startedAt) pauses at 0, not at
      // a negative position.
      state.pausedAt = Math.max(now, state.startedAt);
      return changed;
    }

    case "seek": {
      if (state.currentIndex === -1) return unchanged;
      if (typeof intent.positionSec !== "number" || !Number.isFinite(intent.positionSec)) return fail("invalid positionSec");
      let t = Math.max(0, intent.positionSec);
      const duration = state.queue[state.currentIndex].durationSec;
      if (duration) t = Math.min(t, duration);
      const tMs = Math.round(t * 1000);
      if (state.pausedAt === null) state.startedAt = now - tMs;
      else state.startedAt = state.pausedAt - tMs;
      return changed;
    }

    case "skip": {
      if (state.currentIndex === -1) return unchanged;
      advance(state, now);
      return changed;
    }

    case "unplayable": {
      if (!isInt(intent.code) || !UNPLAYABLE_CODES.has(intent.code)) return unchanged;
      if (!isInt(intent.index) || intent.index !== state.currentIndex) return unchanged;
      if (state.queue[intent.index].videoId !== intent.videoId) return unchanged; // stale report
      advance(state, now);
      return changed;
    }

    case "ended": {
      // Idempotent: every client reports the end; only the first counts.
      if (!isInt(intent.index) || intent.index !== state.currentIndex) return unchanged;
      // Ignore reports while paused (a client whose player hiccuped).
      if (state.pausedAt !== null) return unchanged;
      advance(state, now);
      return changed;
    }

    case "remove": {
      const i = intent.index;
      if (!isInt(i) || i < 0 || i >= state.queue.length) return fail("invalid index");
      state.queue.splice(i, 1);
      if (i < state.currentIndex) {
        state.currentIndex--;
      } else if (i === state.currentIndex) {
        // Removing what's playing behaves like skip: the next entry has slid
        // into this index, so step back one and advance onto it.
        state.currentIndex = i - 1;
        advance(state, now);
      }
      return changed;
    }

    case "move": {
      const { from, to } = intent;
      const n = state.queue.length;
      if (!isInt(from) || !isInt(to) || from < 0 || from >= n || to < 0 || to >= n) return fail("invalid index");
      if (from === to) return unchanged;
      const [entry] = state.queue.splice(from, 1);
      state.queue.splice(to, 0, entry);
      // Keep currentIndex pointing at the same entry.
      const cur = state.currentIndex;
      if (cur === from) state.currentIndex = to;
      else if (from < cur && to >= cur) state.currentIndex--;
      else if (from > cur && to <= cur) state.currentIndex++;
      return changed;
    }

    case "meta": {
      const entry = isInt(intent.index) ? state.queue[intent.index] : undefined;
      if (!entry || entry.videoId !== intent.videoId) return unchanged; // queue changed meanwhile
      let didChange = false;
      if (entry.title === null && typeof intent.title === "string" && intent.title) {
        entry.title = intent.title.slice(0, 200);
        didChange = true;
      }
      if (entry.durationSec === null && typeof intent.durationSec === "number" && intent.durationSec > 0) {
        entry.durationSec = intent.durationSec;
        didChange = true;
      }
      return didChange ? changed : unchanged;
    }

    case "leave":
      return removeMember(state, memberId) ? changed : unchanged;

    case "timeSync":
      return unchanged; // handled before state is touched

    default:
      return fail("unknown intent type");
  }
}

// Removes a member for good. The host role passes to the longest-present
// member who is still connected (or, failing that, anyone left).
export function removeMember(state: SessionState, memberId: string): boolean {
  const before = state.members.length;
  state.members = state.members.filter((m) => m.id !== memberId);
  if (state.members.length === before) return false;
  if (state.hostId === memberId) {
    const next = state.members.find((m) => m.awayUntil === null) ?? state.members[0];
    state.hostId = next?.id ?? null;
  }
  return true;
}

// A member's connection dropped: keep them (and their host role) until
// `until`, unless they come back. Returns whether anything changed.
export function markAway(state: SessionState, memberId: string, until: number): boolean {
  const m = state.members.find((x) => x.id === memberId);
  if (!m || m.awayUntil !== null) return false;
  m.awayUntil = until;
  return true;
}

// Removes members whose grace period has run out.
export function expireAway(state: SessionState, now: number): boolean {
  let didChange = false;
  for (const m of [...state.members]) {
    if (m.awayUntil !== null && m.awayUntil <= now) didChange = removeMember(state, m.id) || didChange;
  }
  return didChange;
}

// When the next away member is due for removal, if any.
export function nextAwayDeadline(state: SessionState): number | null {
  let next: number | null = null;
  for (const m of state.members) if (m.awayUntil !== null && (next === null || m.awayUntil < next)) next = m.awayUntil;
  return next;
}
