/// <reference types="@cloudflare/workers-types" />

/**
 * Serves the large DuckDB-WASM runtime cores (>25 MiB) out of R2.
 *
 * Workers static assets cap individual files at 25 MiB, so the compiled
 * `duckdb-*.wasm` cores are uploaded to an R2 bucket and streamed from here,
 * while every small file stays a plain static asset. Requests that match a
 * file in `dist/` never reach this Worker (assets are served first).
 */

export interface Env {
  RUNTIME: R2Bucket;
}

const CACHE_CONTROL = 'public, max-age=86400';

function notFound(): Response {
  return new Response('Not found', { status: 404 });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (!url.pathname.startsWith('/runtime/')) {
      return notFound();
    }
    const key = decodeURIComponent(url.pathname.slice('/runtime/'.length));
    // Only flat object names are exposed - no prefixes, no traversal.
    if (key.length === 0 || key.includes('/') || key.includes('\\') || key.startsWith('.')) {
      return notFound();
    }
    const object = await env.RUNTIME.get(key);
    if (!object) {
      return notFound();
    }
    const headers = new Headers();
    object.writeHttpMetadata(headers);
    headers.set('Cache-Control', CACHE_CONTROL);
    headers.set('ETag', object.httpEtag);
    return new Response(object.body, { headers });
  },
};
