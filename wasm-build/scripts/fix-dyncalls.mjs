#!/usr/bin/env node

// Emscripten emits invoke_* helpers at the module scope, but the custom
// DuckLake build can discover their dynCall signatures only through runtime
// indirect calls. Add the missing wrappers at that same scope. Keeping them
// outside receiveInstance is essential: declarations inside receiveInstance
// are not visible to invoke_* after the WASM instance is initialized.

import { readFileSync, writeFileSync } from 'node:fs';

const files = process.argv.slice(2);
if (files.length === 0) {
  console.error('usage: fix-dyncalls.mjs <js-file>...');
  process.exit(2);
}

const injectedBlock = /\n?\/\* DuckLake dynCall wrappers begin \*\/[\s\S]*?\/\* DuckLake dynCall wrappers end \*\/\n?/g;
const tableUpdateBlock = /\n?\/\* DuckLake dynCall table update begin \*\/[\s\S]*?\/\* DuckLake dynCall table update end \*\/\n?/g;
const tableDeclaration = /(\bvar wasmTable\s*;)/;

for (const file of files) {
  let source = readFileSync(file, 'utf8');

  // Make the transform idempotent for local rebuilds.
  source = source.replace(injectedBlock, '\n');
  source = source.replace(tableUpdateBlock, '\n');

  const used = new Set();
  const defined = new Set();
  for (const match of source.matchAll(/(?<![A-Za-z0-9_.$'])dynCall_([a-z0-9]+)\b/g)) {
    used.add(match[1]);
  }
  for (const match of source.matchAll(/function dynCall_([a-z0-9]+)\s*\(/g)) {
    defined.add(match[1]);
  }
  const missing = [...used].filter((signature) => !defined.has(signature)).sort();
  if (missing.length === 0) {
    console.log(`fix-dyncalls: ${file} - nothing to inject`);
    writeFileSync(file, source);
    continue;
  }

  if (!tableDeclaration.test(source)) {
    throw new Error(`fix-dyncalls: ${file} has no module-scope wasmTable declaration`);
  }

  const helper = [
    '/* DuckLake dynCall wrappers begin */',
    'var __ducklake_dynCall_wrappers = Object.create(null);',
  ];
  for (const signature of missing) {
    // Emscripten signatures are return-type followed by parameter types.
    // `iiijj` therefore returns i and accepts iijj.
    const parameters = signature.slice(1);
    const args = Array.from({ length: parameters.length }, (_, index) => `a${index}`);
    helper.push(`function dynCall_${signature}(ptr${args.length ? `, ${args.join(', ')}` : ''}) {`);
    helper.push(`  var f = __ducklake_dynCall_wrappers["${signature}"];`);
    helper.push(`  if (!f) f = __ducklake_dynCall_wrappers["${signature}"] = createDyncallWrapper("${signature}");`);
    helper.push(`  return f(ptr${args.length ? `, ${args.join(', ')}` : ''});`);
    helper.push('}');
  }
  helper.push('/* DuckLake dynCall wrappers end */');

  source = source.replace(tableDeclaration, (match) => `${match}\n${helper.join('\n')}\n`);
  writeFileSync(file, source);
  console.log(`fix-dyncalls: ${file} - injected ${missing.length} dynCall definitions at module scope`);
}
