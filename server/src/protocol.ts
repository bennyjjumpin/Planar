// Wire types. Keep in sync with protocol/PROTOCOL.md and client/src/SessionState.h.

export interface Member {
  id: string;
  name: string;
  // Set while the member is disconnected but within the grace period (server
  // ms when they'll be removed); null while connected. A member who comes
  // back in time keeps their place, including the host role.
  awayUntil: number | null;
}

export interface QueueEntry {
  videoId: string;
  addedBy: string; // Member.id
  title: string | null;
  durationSec: number | null;
  thumbnailUrl: string | null;
}

export interface SessionState {
  sessionId: string;
  members: Member[];
  queue: QueueEntry[];
  currentIndex: number; // -1 = nothing playing
  startedAt: number; // server ms (Date.now())
  pausedAt: number | null; // server ms; null while playing
  version: number;
  hostId: string | null; // may skip/seek/move/remove (see permissionError)
}

// Client -> server
export type Intent =
  // clientId: a secret, persistent ID the client generates once. The same
  // clientId always maps to the same member, so reconnecting keeps your
  // identity. Optional (test scripts omit it and get a throwaway identity).
  | { type: "join"; name: string; clientId?: string }
  // Leaving on purpose (the Leave button, closing the app): removed at once,
  // without the grace period meant for accidental disconnects.
  | { type: "leave" }
  // title/durationSec: optional hints from the adding client (e.g. its
  // search result), shown until a player reports the real values.
  | { type: "add"; videoId: string; title?: string; durationSec?: number }
  | { type: "play" }
  | { type: "pause" }
  | { type: "skip" }
  | { type: "seek"; positionSec: number }
  | { type: "remove"; index: number }
  | { type: "move"; from: number; to: number }
  // A client's player reached the end of queue[index]. Every client sends it;
  // only the first one that still matches currentIndex advances the queue.
  | { type: "ended"; index: number }
  // Metadata a client learned from its player, for queue[index] (videoId
  // must still match). Only fills fields that are still empty.
  | { type: "meta"; index: number; videoId: string; title?: string; durationSec?: number }
  // queue[index] (which must still be videoId) can't be played anywhere
  // (YouTube error 2/100/101/150: bad ID, removed, or embedding disabled).
  // Idempotent, like "ended". videoId is required because a player error can
  // arrive after the client has already moved on to the next entry.
  | { type: "unplayable"; index: number; videoId: string; code: number }
  | { type: "timeSync"; clientSentAt: number };

// Server -> client
export type ServerMessage =
  | { type: "state"; version: number; state: SessionState }
  | { type: "timeSync"; serverTime: number; clientSentAt: number }
  // Sent to a client after it joins: its own public member ID.
  | { type: "welcome"; memberId: string }
  | { type: "error"; message: string };

export function emptyState(sessionId: string): SessionState {
  return {
    sessionId,
    members: [],
    queue: [],
    currentIndex: -1,
    startedAt: 0,
    pausedAt: null,
    version: 0,
    hostId: null,
  };
}
