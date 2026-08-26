#!/usr/bin/env node

// A custom DuckDB-Wasm core can contain indirect calls whose signatures are
// discovered only at runtime (DuckLake's JavaScript procedure path is one
// example). The browser worker must therefore contain a dynCall_* declaration
// for every generated invoke wrapper that references one.

import { readFileSync } from 'node:fs';

const files = process.argv.slice(2);
if (files.length === 0) {
  console.error('usage: verify-dyncall-bundle.mjs <worker.js>...');
  process.exit(2);
}

let failed = false;
for (const file of files) {
  const source = readFileSync(file, 'utf8');
  const used = new Set();
  const defined = new Set();

  for (const match of source.matchAll(/(?<![A-Za-z0-9_.$'])dynCall_([a-z0-9]+)\b/g)) {
    used.add(match[1]);
  }
  for (const match of source.matchAll(/function dynCall_([a-z0-9]+)\s*\(/g)) {
    defined.add(match[1]);
  }

  const missing = [...used].filter((signature) => !defined.has(signature)).sort();
  const missingSetThrew = source.includes('_setThrew(') &&
    !/\b(?:function\s+_setThrew|var\s+_setThrew|_setThrew\s*=|Module\["_setThrew"\])/.test(source);
  if (missing.length > 0) {
    console.error(`${file}: missing dynCall definitions: ${missing.join(', ')}`);
    failed = true;
  }
  if (missingSetThrew) {
    console.error(`${file}: missing Emscripten _setThrew runtime binding`);
    failed = true;
  }
  if (missing.length === 0 && !missingSetThrew) {
    console.log(`${file}: dynCall definitions verified (${defined.size})`);
  }
}

if (failed) {
  process.exit(1);
}
