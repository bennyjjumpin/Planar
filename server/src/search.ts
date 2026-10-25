// Song search, backed by the YouTube Data API v3.
//
// The API key lives only here, as a Worker secret (YOUTUBE_API_KEY), so it
// never ships inside the app. Searches are expensive in quota (search.list
// costs 100 units of the free 10,000/day), so results are cached in KV for a
// day: repeated searches by anyone are free.

export interface SearchResult {
  videoId: string;
  title: string;
  channel: string;
  durationSec: number | null;
  thumbnailUrl: string;
}

export interface SearchEnv {
  YOUTUBE_API_KEY?: string;
  // Testing only: point at a mock instead of https://www.googleapis.com.
  YOUTUBE_API_BASE?: string;
  SEARCH_CACHE?: KVNamespace;
}

export const MAX_QUERY = 100;
export const MAX_RESULTS = 15;
const CACHE_TTL_S = 24 * 60 * 60;

// ISO 8601 durations as used by the API: "PT4M13S", "PT1H2M", "P1DT2H".
// Returns null for anything unparseable. Live streams report "P0D".
export function parseIsoDuration(iso: string): number | null {
  const m = /^P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+(?:\.\d+)?)S)?)?$/.exec(iso);
  if (!m || iso === "P" || iso === "PT") return null;
  const [, d, h, min, s] = m;
  return Number(d ?? 0) * 86400 + Number(h ?? 0) * 3600 + Number(min ?? 0) * 60 + Number(s ?? 0);
}

// search.list returns titles HTML-escaped ("Rock &amp; Roll", "Don&#39;t").
export function decodeEntities(s: string): string {
  const named: Record<string, string> = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " " };
  return s.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);/gi, (whole, code: string) => {
    if (code[0] === "#") {
      const n = code[1].toLowerCase() === "x" ? parseInt(code.slice(2), 16) : parseInt(code.slice(1), 10);
      return Number.isFinite(n) && n > 0 && n <= 0x10ffff ? String.fromCodePoint(n) : whole;
    }
    return named[code.toLowerCase()] ?? whole;
  });
}

export function normalizeQuery(q: string): string {
  return q.trim().replace(/\s+/g, " ").slice(0, MAX_QUERY);
}

function json(body: unknown, status = 200, extra: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", ...extra },
  });
}

// GET /search?q=...  ->  {"results": SearchResult[]}  or  {"error": "..."}
export async function handleSearch(request: Request, env: SearchEnv, fetcher: typeof fetch = fetch): Promise<Response> {
  if (request.method !== "GET") return json({ error: "method not allowed" }, 405);
  const q = normalizeQuery(new URL(request.url).searchParams.get("q") ?? "");
  if (!q) return json({ error: "empty query" }, 400);
  if (!env.YOUTUBE_API_KEY) return json({ error: "Search isn't set up on this relay (no YouTube API key)." }, 503);

  const cacheKey = `search:v1:${q.toLowerCase()}`;
  const cached = await env.SEARCH_CACHE?.get(cacheKey);
  if (cached) return json(JSON.parse(cached), 200, { "x-cache": "hit" });

  const base = env.YOUTUBE_API_BASE ?? "https://www.googleapis.com";
  const key = encodeURIComponent(env.YOUTUBE_API_KEY);

  // 1. Find videos. videoEmbeddable=true skips most videos we couldn't play
  //    anyway (the rest get auto-skipped as "unplayable").
  const searchUrl =
    `${base}/youtube/v3/search?part=snippet&type=video&videoEmbeddable=true` +
    `&maxResults=${MAX_RESULTS}&q=${encodeURIComponent(q)}&key=${key}`;
  const searchRes = await fetcher(searchUrl);
  if (!searchRes.ok) return apiError(searchRes);
  const searchBody = (await searchRes.json()) as {
    items?: Array<{
      id?: { videoId?: string };
      snippet?: {
        title?: string;
        channelTitle?: string;
        liveBroadcastContent?: string;
        thumbnails?: { medium?: { url?: string }; default?: { url?: string } };
      };
    }>;
  };

  const items = (searchBody.items ?? []).filter(
    // Live streams have no fixed timeline to sync to.
    (it) => it.id?.videoId && (it.snippet?.liveBroadcastContent ?? "none") === "none",
  );

  // 2. Durations (1 quota unit for all of them).
  const durations = new Map<string, number | null>();
  if (items.length > 0) {
    const ids = items.map((it) => it.id!.videoId!).join(",");
    const videosRes = await fetcher(`${base}/youtube/v3/videos?part=contentDetails&id=${ids}&key=${key}`);
    if (videosRes.ok) {
      const videosBody = (await videosRes.json()) as {
        items?: Array<{ id?: string; contentDetails?: { duration?: string } }>;
      };
      for (const v of videosBody.items ?? []) {
        if (v.id) durations.set(v.id, parseIsoDuration(v.contentDetails?.duration ?? ""));
      }
    }
    // Durations are nice-to-have: on failure, results just lack them.
  }

  const results: SearchResult[] = items.map((it) => {
    const id = it.id!.videoId!;
    return {
      videoId: id,
      title: decodeEntities(it.snippet?.title ?? ""),
      channel: decodeEntities(it.snippet?.channelTitle ?? ""),
      durationSec: durations.get(id) ?? null,
      thumbnailUrl: it.snippet?.thumbnails?.medium?.url ?? `https://i.ytimg.com/vi/${id}/mqdefault.jpg`,
    };
  });

  const body = { results };
  await env.SEARCH_CACHE?.put(cacheKey, JSON.stringify(body), { expirationTtl: CACHE_TTL_S });
  return json(body, 200, { "x-cache": "miss" });
}

async function apiError(res: Response): Promise<Response> {
  let reason = "";
  let message = "";
  try {
    const body = (await res.json()) as { error?: { message?: string; errors?: Array<{ reason?: string }> } };
    reason = body.error?.errors?.[0]?.reason ?? "";
    message = body.error?.message ?? ""; // Google's text; never contains the key
  } catch {
    // not JSON
  }
  if (reason === "quotaExceeded" || reason === "dailyLimitExceeded") {
    return json({ error: "Search is used up for today (YouTube API quota). It resets at midnight Pacific time." }, 503);
  }
  console.error(`youtube api error ${res.status} ${reason}: ${message}`);
  if (/API key not valid|API_KEY_INVALID/i.test(message) || reason === "keyInvalid") {
    return json({ error: "Search isn't working: the relay's YouTube API key is invalid." }, 502);
  }
  return json(
    { error: `Search failed (YouTube API ${res.status}${reason ? `: ${reason}` : ""}${message ? ` - ${message}` : ""}).` },
    502,
  );
}
