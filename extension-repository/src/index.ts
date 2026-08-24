export interface Env {
	EXTENSIONS: R2Bucket;
}

const ALLOWED_METHODS = new Set(["GET", "HEAD", "OPTIONS"]);

function corsHeaders(): Headers {
	return new Headers({
		"Access-Control-Allow-Origin": "*",
		"Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
		"Access-Control-Allow-Headers": "Range, If-None-Match",
		"Access-Control-Expose-Headers": "Accept-Ranges, Content-Length, Content-Type, ETag",
	});
}

function objectKey(pathname: string): string | null {
	const key = pathname.replace(/^\/+/, "");
	if (!key || key.includes("\\") || key.split("/").some((part) => part === "..")) {
		return null;
	}
	return key;
}

function contentType(key: string): string {
	if (key.endsWith(".duckdb_extension") || key.endsWith(".duckdb_extension.gz")) {
		return "application/octet-stream";
	}
	if (key.endsWith(".json")) return "application/json; charset=utf-8";
	if (key.endsWith(".sha256") || key.endsWith(".txt")) return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		if (!ALLOWED_METHODS.has(request.method)) {
			return new Response("Method Not Allowed", {
				status: 405,
				headers: { ...Object.fromEntries(corsHeaders()), Allow: "GET, HEAD, OPTIONS" },
			});
		}

		if (request.method === "OPTIONS") {
			return new Response(null, { status: 204, headers: corsHeaders() });
		}

		const key = objectKey(new URL(request.url).pathname);
		if (!key) return new Response("Not Found", { status: 404, headers: corsHeaders() });

		const object = await env.EXTENSIONS.get(key);
		if (!object) return new Response("Not Found", { status: 404, headers: corsHeaders() });

		const headers = corsHeaders();
		headers.set("Accept-Ranges", "bytes");
		headers.set(
			"Cache-Control",
			key === "artifacts.json" ? "public, max-age=60, must-revalidate" : "public, max-age=31536000, immutable",
		);
		headers.set("Content-Type", object.httpMetadata?.contentType ?? contentType(key));
		headers.set("Content-Length", String(object.size));
		headers.set("ETag", object.etag);
		if (object.httpMetadata?.cacheControl) headers.set("Cache-Control", object.httpMetadata.cacheControl);
		if (request.headers.get("If-None-Match") === object.etag) {
			return new Response(null, { status: 304, headers });
		}

		return new Response(request.method === "HEAD" ? null : object.body, { headers });
	},
};
