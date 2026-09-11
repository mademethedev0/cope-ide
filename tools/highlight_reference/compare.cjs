'use strict';

// CI-only compatibility measurement. Differences are reported, not accepted
// as correctness: the first run establishes evidence for a reviewed baseline.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawnSync } = require('node:child_process');
const tm = require('vscode-textmate');
const onig = require('vscode-oniguruma');

function invariant(condition, message) {
  if (!condition) throw new Error(message);
}

function byteBoundaries(text) {
  const offsets = new Map([[0, 0]]);
  let utf16 = 0;
  let bytes = 0;
  for (const character of text) {
    utf16 += character.length;
    bytes += Buffer.byteLength(character, 'utf8');
    offsets.set(utf16, bytes);
  }
  return offsets;
}

function parseActual(output, lines, rootScope) {
  const result = lines.map(() => []);
  for (const record of output.split('\n')) {
    if (!record) continue;
    const match = /^(\d+):(\d+)-(\d+) (.*)$/.exec(record);
    invariant(match, 'Malformed CLI scope record');
    const [, lineText, beginText, endText, scopes] = match;
    const line = Number(lineText);
    const begin = Number(beginText);
    const end = Number(endText);
    invariant(line < lines.length, 'CLI returned an unknown line');
    const spans = result[line];
    const previousEnd = spans.length ? spans[spans.length - 1].end : 0;
    invariant(begin === previousEnd && end > begin && end <= Buffer.byteLength(lines[line]),
      'CLI scopes do not tile the content line');
    invariant(scopes.split(' ')[0] === rootScope, 'CLI used the wrong root grammar');
    spans.push({ begin, end, scopes });
  }
  for (let line = 0; line < lines.length; ++line) {
    const spans = result[line];
    const end = spans.length ? spans[spans.length - 1].end : 0;
    invariant(end === Buffer.byteLength(lines[line]), 'CLI omitted source bytes');
  }
  return result;
}

function differentBytes(actual, expected, length) {
  let a = 0;
  let e = 0;
  let cursor = 0;
  let different = 0;
  while (cursor < length) {
    invariant(a < actual.length && e < expected.length, 'Missing comparison span');
    const end = Math.min(actual[a].end, expected[e].end);
    invariant(end > cursor, 'Non-progressing comparison');
    if (actual[a].scopes !== expected[e].scopes) different += end - cursor;
    cursor = end;
    if (cursor === actual[a].end) ++a;
    if (cursor === expected[e].end) ++e;
  }
  return different;
}

async function main() {
  invariant(process.argv.length === 5, 'Usage: compare.cjs <cope_cli> <repo> <report.json>');
  const binary = path.resolve(process.argv[2]);
  const root = path.resolve(process.argv[3]);
  const reportPath = path.resolve(process.argv[4]);
  const grammarDir = path.join(root, 'textmate', 'grammars');
  const manifest = JSON.parse(fs.readFileSync(path.join(root, 'tests', 'fixtures', 'highlight', 'corpus.json'), 'utf8'));
  invariant(Array.isArray(manifest.cases) && manifest.cases.length > 0, 'Empty corpus');
  const sources = new Map();
  for (const file of fs.readdirSync(grammarDir).filter(file => file.endsWith('.json')).sort()) {
    const raw = fs.readFileSync(path.join(grammarDir, file), 'utf8');
    const scope = JSON.parse(raw).scopeName;
    if (!sources.has(scope)) sources.set(scope, { raw, file });
  }
  const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm'));
  await onig.loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));
  const missing = new Set();
  const registry = new tm.Registry({
    onigLib: Promise.resolve({
      createOnigScanner: patterns => new onig.OnigScanner(patterns),
      createOnigString: text => new onig.OnigString(text),
    }),
    loadGrammar: async scope => {
      const source = sources.get(scope);
      if (!source) { missing.add(scope); return null; }
      return tm.parseRawGrammar(source.raw, source.file);
    },
  });
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'cope-reference-'));
  const report = {
    mode: 'measurement-not-regression-gate',
    versions: { textmate: require('vscode-textmate/package.json').version,
      oniguruma: require('vscode-oniguruma/package.json').version },
    backend: 'pcre2',
    offsets: 'UTF-8 bytes; LF/CRLF terminators excluded; synthetic LF supplied to both tokenizers',
    cases: [],
  };
  try {
    for (const item of manifest.cases) {
      invariant(/^[a-z0-9-]+$/.test(item.name), 'Unsafe fixture name');
      invariant(/^[a-z0-9]+$/.test(item.extension), 'Unsafe fixture extension');
      const grammar = await registry.loadGrammar(item.scope);
      invariant(grammar, `Reference grammar missing for ${item.name}`);
      const filename = path.join(temp, `${item.name}.${item.extension}`);
      fs.writeFileSync(filename, item.source);
      const lines = item.source.split('\n').map(line => line.endsWith('\r') ? line.slice(0, -1) : line);
      const child = spawnSync(binary, ['syntax', filename, '--scope', item.scope, '--engine', 'pcre2'], {
        encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 * 1024,
        env: { ...process.env, NO_COLOR: '1', COPE_GRAMMARS_DIR: grammarDir },
      });
      invariant(!child.error && child.status === 0,
        `CLI failed for ${item.name}: ${child.error ? child.error.message : child.stderr.slice(0, 300)}`);
      const actual = parseActual(child.stdout, lines, item.scope);
      let state = tm.INITIAL;
      const summary = { name: item.name, scope: item.scope, bytes: 0, differentBytes: 0, differentLines: 0, examples: [] };
      for (let line = 0; line < lines.length; ++line) {
        const text = lines[line];
        const boundaries = byteBoundaries(text);
        const result = grammar.tokenizeLine(text, state, 1000);
        invariant(!result.stoppedEarly, `Reference budget exceeded: ${item.name}:${line}`);
        state = result.ruleStack;
        const expected = [];
        let covered = 0;
        for (const token of result.tokens) {
          const start = Math.min(token.startIndex, text.length);
          const stop = Math.min(token.endIndex, text.length);
          if (start >= stop) continue;
          invariant(boundaries.has(start) && boundaries.has(stop), 'Reference split a Unicode codepoint');
          const begin = boundaries.get(start);
          const end = boundaries.get(stop);
          invariant(begin === covered && end > begin, 'Reference scopes do not tile the line');
          expected.push({ begin, end, scopes: token.scopes.join(' ') });
          covered = end;
        }
        const length = Buffer.byteLength(text);
        invariant(covered === length, 'Reference omitted source bytes');
        const different = differentBytes(actual[line], expected, length);
        summary.bytes += length;
        summary.differentBytes += different;
        if (different) {
          ++summary.differentLines;
          if (summary.examples.length < 3) summary.examples.push({ line, actual: actual[line], expected });
        }
      }
      report.cases.push(summary);
      console.log(`${item.name}: ${summary.differentBytes}/${summary.bytes} differing bytes; ${summary.differentLines} lines`);
    }
    report.missingScopes = [...missing].sort();
    fs.mkdirSync(path.dirname(reportPath), { recursive: true });
    fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n');
    console.log(`Reference measurement complete: ${report.cases.length} cases; ${missing.size} unavailable embedded scopes. Differences are not gated yet.`);
  } finally {
    registry.dispose();
    fs.rmSync(temp, { recursive: true, force: true });
  }
}

main().catch(error => { console.error(error.message); process.exitCode = 1; });
