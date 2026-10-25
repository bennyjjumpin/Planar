// Run with: npm test   (Node >= 22.18 runs TypeScript files directly)
import { test } from "node:test";
import assert from "node:assert/strict";

import { emptyState, type Intent, type SessionState } from "../src/protocol.ts";
import {
  applyIntent,
  expireAway,
  LEAD_MS,
  markAway,
  nextAwayDeadline,
  removeMember,
  TRACK_LEAD_MS,
} from "../src/session.ts";

const A = "member-a";
const B = "member-b";
const V1 = "M7lc1UVf-VE";
const V2 = "dQw4w9WgXcQ";
const V3 = "jNQXAC9IVRw";

function apply(state: SessionState, intent: Intent, now: number, member = A) {
  const r = applyIntent(state, member, intent, now);
  assert.ok(r.ok, `intent ${intent.type} failed: ${!r.ok && r.error}`);
  return r.changed;
}

// Derived position in ms, exactly as clients compute it.
function position(s: SessionState, now: number): number {
  return (s.pausedAt ?? now) - s.startedAt;
}

// A session where A is the host (first to join) and B is a guest.
function withQueue(...ids: string[]): SessionState {
  const s = emptyState("t");
  apply(s, { type: "join", name: "Ann" }, 0, A);
  apply(s, { type: "join", name: "Bob" }, 0, B);
  ids.forEach((id) => apply(s, { type: "add", videoId: id }, 1000));
  return s;
}

test("join adds members and the first one becomes host", () => {
  const s = emptyState("t");
  apply(s, { type: "join", name: "Ann" }, 0, A);
  apply(s, { type: "join", name: "Bob" }, 0, B);
  assert.deepEqual(s.members.map((m) => m.name), ["Ann", "Bob"]);
  assert.equal(s.hostId, A);
  assert.equal(apply(s, { type: "join", name: "Ann" }, 0, A), false, "same name is a no-op");
});

test("host passes on when the host leaves", () => {
  const s = emptyState("t");
  apply(s, { type: "join", name: "Ann" }, 0, A);
  apply(s, { type: "join", name: "Bob" }, 0, B);
  assert.ok(removeMember(s, A));
  assert.equal(s.hostId, B);
  assert.ok(removeMember(s, B));
  assert.equal(s.hostId, null);
});

test("adding to an empty queue starts it after the track lead", () => {
  const s = emptyState("t");
  apply(s, { type: "add", videoId: V1 }, 1000);
  assert.equal(s.currentIndex, 0);
  assert.equal(s.startedAt, 1000 + TRACK_LEAD_MS);
  assert.equal(s.pausedAt, null);
  assert.equal(s.queue[0].thumbnailUrl, `https://i.ytimg.com/vi/${V1}/mqdefault.jpg`);
  // A second add only queues.
  apply(s, { type: "add", videoId: V2 }, 2000);
  assert.equal(s.currentIndex, 0);
  assert.equal(s.queue.length, 2);
});

test("add accepts title/duration hints and ignores bad ones", () => {
  const s = emptyState("t");
  apply(s, { type: "add", videoId: V1, title: "From search", durationSec: 213 }, 0);
  apply(s, { type: "add", videoId: V2, title: "", durationSec: -5 }, 0);
  assert.equal(s.queue[0].title, "From search");
  assert.equal(s.queue[0].durationSec, 213);
  assert.equal(s.queue[1].title, null);
  assert.equal(s.queue[1].durationSec, null);
});

test("invalid video ids are rejected", () => {
  const s = emptyState("t");
  const r = applyIntent(s, A, { type: "add", videoId: "not-an-id" }, 0);
  assert.equal(r.ok, false);
});

test("pause then resume keeps the position, resuming after LEAD", () => {
  const s = withQueue(V1); // startedAt = 2500
  apply(s, { type: "pause" }, 12_500); // 10 s in
  assert.equal(position(s, 99_999), 10_000);

  apply(s, { type: "play" }, 20_000);
  assert.equal(s.pausedAt, null);
  // At the scheduled resume moment the position is still exactly 10 s...
  assert.equal(position(s, 20_000 + LEAD_MS), 10_000);
  // ...and it advances from there.
  assert.equal(position(s, 21_000 + LEAD_MS), 11_000);
});

test("pausing during the start lead pauses at 0", () => {
  const s = withQueue(V1); // starts at 2500
  apply(s, { type: "pause" }, 1500);
  assert.equal(position(s, 0), 0);
});

test("seek while playing and while paused", () => {
  const s = withQueue(V1);
  apply(s, { type: "seek", positionSec: 60 }, 10_000);
  assert.equal(position(s, 10_000), 60_000);
  assert.equal(position(s, 11_000), 61_000);

  apply(s, { type: "pause" }, 15_000);
  apply(s, { type: "seek", positionSec: 30 }, 20_000);
  assert.equal(position(s, 50_000), 30_000);
  assert.notEqual(s.pausedAt, null, "still paused");
});

test("seek clamps to [0, duration] once duration is known", () => {
  const s = withQueue(V1);
  apply(s, { type: "meta", index: 0, videoId: V1, title: "T", durationSec: 100 }, 0);
  apply(s, { type: "seek", positionSec: 500 }, 10_000);
  assert.equal(position(s, 10_000), 100_000);
  apply(s, { type: "seek", positionSec: -5 }, 10_000);
  assert.equal(position(s, 10_000), 0);
});

test("skip advances, and stops past the end", () => {
  const s = withQueue(V1, V2);
  apply(s, { type: "skip" }, 5000);
  assert.equal(s.currentIndex, 1);
  assert.equal(s.startedAt, 5000 + TRACK_LEAD_MS);
  apply(s, { type: "skip" }, 6000);
  assert.equal(s.currentIndex, -1);
  // Adding now starts the new entry.
  apply(s, { type: "add", videoId: V3 }, 7000);
  assert.equal(s.currentIndex, 2);
});

test("ended is idempotent across clients", () => {
  const s = withQueue(V1, V2, V3);
  assert.equal(apply(s, { type: "ended", index: 0 }, 5000, A), true);
  assert.equal(s.currentIndex, 1);
  // B's report for the same track arrives late: ignored.
  assert.equal(apply(s, { type: "ended", index: 0 }, 5010, B), false);
  assert.equal(s.currentIndex, 1);
});

test("ended is ignored while paused", () => {
  const s = withQueue(V1, V2);
  apply(s, { type: "pause" }, 5000);
  assert.equal(apply(s, { type: "ended", index: 0 }, 6000), false);
});

test("remove keeps currentIndex on the same entry", () => {
  const s = withQueue(V1, V2, V3);
  apply(s, { type: "skip" }, 5000); // playing V2 at index 1
  apply(s, { type: "remove", index: 0 }, 6000);
  assert.equal(s.queue[s.currentIndex].videoId, V2);
  assert.equal(s.startedAt, 5000 + TRACK_LEAD_MS, "playback unaffected");
});

test("removing the current entry plays the next one", () => {
  const s = withQueue(V1, V2, V3);
  apply(s, { type: "remove", index: 0 }, 6000);
  assert.equal(s.currentIndex, 0);
  assert.equal(s.queue[0].videoId, V2);
  assert.equal(s.startedAt, 6000 + TRACK_LEAD_MS);
});

test("removing the last, current entry stops", () => {
  const s = withQueue(V1);
  apply(s, { type: "remove", index: 0 }, 6000);
  assert.equal(s.currentIndex, -1);
  assert.equal(s.queue.length, 0);
});

test("move keeps currentIndex on the same entry", () => {
  for (const [from, to] of [[0, 2], [2, 0], [1, 2], [2, 1], [1, 0], [0, 1]]) {
    const s = withQueue(V1, V2, V3);
    apply(s, { type: "skip" }, 5000); // current = V2 at index 1
    apply(s, { type: "move", from, to }, 6000);
    assert.equal(s.queue[s.currentIndex].videoId, V2, `move ${from}->${to}`);
  }
});

test("meta fills empty fields only and checks the videoId", () => {
  const s = withQueue(V1, V2);
  assert.equal(apply(s, { type: "meta", index: 0, videoId: V2, title: "wrong" }, 0), false);
  assert.equal(apply(s, { type: "meta", index: 0, videoId: V1, title: "Song", durationSec: 200 }, 0), true);
  assert.equal(apply(s, { type: "meta", index: 0, videoId: V1, title: "Other" }, 0), false);
  assert.equal(s.queue[0].title, "Song");
  assert.equal(s.queue[0].durationSec, 200);
});

test("guests can add and play/pause, but not skip, seek or move", () => {
  const s = withQueue(V1, V2);
  apply(s, { type: "add", videoId: V3 }, 2000, B);
  apply(s, { type: "pause" }, 3000, B);
  apply(s, { type: "play" }, 4000, B);
  for (const intent of [
    { type: "skip" },
    { type: "seek", positionSec: 10 },
    { type: "move", from: 0, to: 1 },
  ] as Intent[]) {
    const r = applyIntent(s, B, intent, 5000);
    assert.equal(r.ok, false, intent.type);
  }
  assert.equal(s.currentIndex, 0, "nothing skipped");
});

test("guests can remove only what they added; the host can remove anything", () => {
  const s = withQueue(V1); // added by A
  apply(s, { type: "add", videoId: V2 }, 2000, B);
  assert.equal(applyIntent(s, B, { type: "remove", index: 0 }, 3000).ok, false);
  apply(s, { type: "remove", index: 1 }, 3000, B); // B's own
  apply(s, { type: "add", videoId: V3 }, 4000, B);
  apply(s, { type: "remove", index: 1 }, 5000, A); // host removes B's
  assert.deepEqual(s.queue.map((e) => e.videoId), [V1]);
});

test("the next member becomes host and gains host controls", () => {
  const s = withQueue(V1, V2);
  removeMember(s, A);
  apply(s, { type: "skip" }, 5000, B);
  assert.equal(s.currentIndex, 1);
});

test("unplayable skips only for universal error codes and the current entry", () => {
  const s = withQueue(V1, V2, V3);
  assert.equal(apply(s, { type: "unplayable", index: 0, videoId: V1, code: 5 }, 1000, B), false, "local HTML5 error");
  assert.equal(apply(s, { type: "unplayable", index: 1, videoId: V2, code: 150 }, 1000, B), false, "not current");
  assert.equal(apply(s, { type: "unplayable", index: 0, videoId: V1, code: 150 }, 1000, B), true);
  assert.equal(s.currentIndex, 1);
  assert.equal(apply(s, { type: "unplayable", index: 0, videoId: V1, code: 150 }, 1010, A), false, "duplicate");
  // The race seen in testing: a late error for V1 from a client that has
  // already moved on, mis-attributed to the new current index.
  assert.equal(apply(s, { type: "unplayable", index: 1, videoId: V1, code: 150 }, 1020, A), false, "stale");
  assert.equal(s.currentIndex, 1);
});

test("bad indices are errors, not crashes", () => {
  const s = withQueue(V1);
  for (const intent of [
    { type: "remove", index: 5 },
    { type: "remove", index: -1 },
    { type: "move", from: 0, to: 9 },
    { type: "seek", positionSec: Number.NaN },
  ] as Intent[]) {
    assert.equal(applyIntent(s, A, intent, 0).ok, false, JSON.stringify(intent));
  }
});

test("leave removes the member at once and passes the host role", () => {
  const s = withQueue(V1);
  assert.equal(apply(s, { type: "leave" }, 1000, A), true);
  assert.deepEqual(s.members.map((m) => m.name), ["Bob"]);
  assert.equal(s.hostId, B);
  assert.equal(apply(s, { type: "leave" }, 1000, A), false, "already gone");
});

test("an away member keeps their place and host role until the deadline", () => {
  const s = withQueue(V1); // A is host
  assert.ok(markAway(s, A, 40_000));
  assert.equal(markAway(s, A, 50_000), false, "already away: deadline not extended");
  assert.equal(nextAwayDeadline(s), 40_000);
  assert.equal(expireAway(s, 39_999), false);
  assert.equal(s.hostId, A, "still host while away");
  // Back in time: same member, no longer away, still host.
  assert.equal(apply(s, { type: "join", name: "Ann" }, 39_000, A), true);
  assert.equal(s.members.find((m) => m.id === A)?.awayUntil, null);
  assert.equal(nextAwayDeadline(s), null);
  assert.equal(s.hostId, A);
});

test("an away member is removed after the deadline and the host role passes on", () => {
  const s = withQueue(V1);
  markAway(s, A, 40_000);
  assert.equal(expireAway(s, 40_000), true);
  assert.deepEqual(s.members.map((m) => m.name), ["Bob"]);
  assert.equal(s.hostId, B);
});

test("the host role skips members who are themselves away", () => {
  const C = "member-c";
  const s = withQueue(V1); // A host, B guest
  apply(s, { type: "join", name: "Cat" }, 0, C);
  markAway(s, B, 60_000); // B is away too
  markAway(s, A, 40_000);
  expireAway(s, 40_000);
  assert.equal(s.hostId, C, "connected Cat, not away Bob");
});
