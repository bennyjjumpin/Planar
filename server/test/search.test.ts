import { test } from "node:test";
import assert from "node:assert/strict";

import { decodeEntities, handleSearch, normalizeQuery, parseIsoDuration } from "../src/search.ts";

test("ISO 8601 durations", () => {
  assert.equal(parseIsoDuration("PT4M13S"), 253);
  assert.equal(parseIsoDuration("PT1H2M"), 3720);
  assert.equal(parseIsoDuration("PT45S"), 45);
  assert.equal(parseIsoDuration("P1DT2H"), 93600);
  assert.equal(parseIsoDuration("P0D"), 0);
  assert.equal(parseIsoDuration("PT"), null);
  assert.equal(parseIsoDuration("garbage"), null);
});

test("HTML entities in titles", () => {
  assert.equal(decodeEntities("Rock &amp; Roll"), "Rock & Roll");
  assert.equal(decodeEntities("Don&#39;t Stop"), "Don't Stop");
  assert.equal(decodeEntities("&quot;Live&quot; &lt;3"), '"Live" <3');
  assert.equal(decodeEntities("caf&#xE9;"), "café");
  assert.equal(decodeEntities("&bogus; & stays"), "&bogus; & stays");
});

test("queries are trimmed, collapsed and capped", () => {
  assert.equal(normalizeQuery("  daft   punk \n"), "daft punk");
  assert.equal(normalizeQuery("x".repeat(500)).length, 100);
});

// A fake YouTube API plus an in-memory KV.
function fakeApi() {
  const calls: string[] = [];
  const fetcher = (async (input: RequestInfo | URL) => {
    const url = String(input);
    calls.push(url);
    if (url.includes("/search?")) {
      return Response.json({
        items: [
          { id: { videoId: "AAAAAAAAAAA" }, snippet: { title: "Song &amp; Dance", channelTitle: "Band", liveBroadcastContent: "none" } },
          { id: { videoId: "BBBBBBBBBBB" }, snippet: { title: "Live now", channelTitle: "Radio", liveBroadcastContent: "live" } },
        ],
      });
    }
    return Response.json({ items: [{ id: "AAAAAAAAAAA", contentDetails: { duration: "PT3M5S" } }] });
  }) as typeof fetch;
  const store = new Map<string, string>();
  const kv = {
    get: async (k: string) => store.get(k) ?? null,
    put: async (k: string, v: string) => void store.set(k, v),
  } as unknown as KVNamespace;
  return { calls, fetcher, kv };
}

test("search returns embeddable non-live results with durations, then serves from cache", async () => {
  const { calls, fetcher, kv } = fakeApi();
  const env = { YOUTUBE_API_KEY: "k", SEARCH_CACHE: kv };
  const req = () => new Request("https://relay/search?q=Song");

  const first = await handleSearch(req(), env, fetcher);
  assert.equal(first.status, 200);
  assert.equal(first.headers.get("x-cache"), "miss");
  const body = (await first.json()) as { results: Array<Record<string, unknown>> };
  assert.deepEqual(body.results, [
    {
      videoId: "AAAAAAAAAAA",
      title: "Song & Dance",
      channel: "Band",
      durationSec: 185,
      thumbnailUrl: "https://i.ytimg.com/vi/AAAAAAAAAAA/mqdefault.jpg",
    },
  ]);
  assert.ok(calls[0].includes("videoEmbeddable=true") && calls[0].includes("type=video"));
  assert.equal(calls.length, 2);

  // Same query, different case/spacing: cache hit, no API calls.
  const second = await handleSearch(new Request("https://relay/search?q=%20song%20"), env, fetcher);
  assert.equal(second.headers.get("x-cache"), "hit");
  assert.equal(calls.length, 2);
});

test("search errors are friendly", async () => {
  const noKey = await handleSearch(new Request("https://relay/search?q=x"), {});
  assert.equal(noKey.status, 503);

  const empty = await handleSearch(new Request("https://relay/search?q=%20"), { YOUTUBE_API_KEY: "k" });
  assert.equal(empty.status, 400);

  const quota = (async () =>
    Response.json({ error: { errors: [{ reason: "quotaExceeded" }] } }, { status: 403 })) as unknown as typeof fetch;
  const res = await handleSearch(new Request("https://relay/search?q=x"), { YOUTUBE_API_KEY: "k" }, quota);
  assert.equal(res.status, 503);
  assert.match(((await res.json()) as { error: string }).error, /used up for today/);
});
