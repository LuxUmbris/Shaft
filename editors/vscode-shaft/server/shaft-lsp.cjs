#!/usr/bin/env node
'use strict';

const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { fileURLToPath, pathToFileURL } = require('node:url');
const language = require('./lib/shaft-language.cjs');
const compilerDiscovery = require('./lib/compiler-discovery.cjs');

const documents = new Map();
let buffer = Buffer.alloc(0);
let shutdownRequested = false;
let rootUri = null;
let settings = { compilerPath: '', diagnostics: 'onChange', maxProblems: 100 };

const completionKinds = { class: 7, struct: 22, enum: 13, function: 3, keyword: 14, symbol: 13 };
const symbolKinds = { class: 5, struct: 23, enum: 10, function: 12 };
const tokenTypes = ['keyword', 'type', 'function', 'number', 'string', 'comment', 'operator'];
const keywordSet = new Set(language.KEYWORDS);

function send(message) {
  const body = Buffer.from(JSON.stringify(message), 'utf8');
  process.stdout.write(`Content-Length: ${body.length}\r\n\r\n`);
  process.stdout.write(body);
}
function respond(id, result) { send({ jsonrpc: '2.0', id, result }); }
function fail(id, code, message) { send({ jsonrpc: '2.0', id, error: { code, message } }); }
function notify(method, params) { send({ jsonrpc: '2.0', method, params }); }
function log(message, type = 3) { notify('window/logMessage', { type, message: `[Shaft] ${message}` }); }

function uriToPath(uri) {
  try { return fileURLToPath(uri); } catch { return uri; }
}
function workspacePath() { return rootUri ? uriToPath(rootUri) : process.cwd(); }
function documentFor(uri) { return documents.get(uri); }
function analyze(uri, text) {
  const analysis = language.analyzeDocument(uri, text);
  documents.set(uri, analysis);
  return analysis;
}
function publish(analysis, extra = []) {
  const diagnostics = [...analysis.diagnostics, ...extra].slice(0, settings.maxProblems || 100);
  notify('textDocument/publishDiagnostics', { uri: analysis.uri, diagnostics });
}
function positionToOffset(text, position) {
  const lines = text.split(/\r?\n/);
  let offset = 0;
  for (let line = 0; line < position.line && line < lines.length; line += 1) offset += lines[line].length + 1;
  return offset + Math.min(position.character, (lines[position.line] || '').length);
}
function applyChanges(text, changes) {
  let current = text;
  for (const change of changes) {
    if (!change.range) { current = change.text; continue; }
    const start = positionToOffset(current, change.range.start);
    const end = positionToOffset(current, change.range.end);
    current = current.slice(0, start) + change.text + current.slice(end);
  }
  return current;
}
function allSymbols() {
  const symbols = [];
  for (const analysis of documents.values()) symbols.push(...analysis.symbols);
  return symbols;
}
function indexWorkspace() {
  if (!rootUri || !rootUri.startsWith('file:')) return;
  const root = workspacePath();
  const ignored = new Set(['.git', 'build', 'node_modules', '.vscode-test']);
  let indexed = 0;
  const visit = (directory) => {
    if (indexed >= 1000) return;
    let entries;
    try { entries = fs.readdirSync(directory, { withFileTypes: true }); } catch { return; }
    for (const entry of entries) {
      if (indexed >= 1000) return;
      const absolute = path.join(directory, entry.name);
      if (entry.isDirectory()) {
        if (!ignored.has(entry.name)) visit(absolute);
      } else if (entry.isFile() && entry.name.endsWith('.shaft')) {
        try {
          const uri = pathToFileURL(absolute).toString();
          analyze(uri, fs.readFileSync(absolute, 'utf8'));
          indexed += 1;
        } catch { /* A transient editor/workspace file must not stop indexing. */ }
      }
    }
  };
  visit(root);
  log(`Indexed ${indexed} Shaft source file${indexed === 1 ? '' : 's'}.`);
}
function standardLibraryPath() {
  const configured = settings.stdlibPath;
  if (configured && fs.existsSync(configured)) return configured;
  const projectLibrary = path.join(workspacePath(), 'std', 'std.shaft');
  return fs.existsSync(projectLibrary) ? projectLibrary : null;
}
function applyCompilerDiscovery() {
  const resolved = compilerDiscovery.resolveCompilerSettings(settings);
  settings = { ...settings, compilerPath: resolved.compilerPath, stdlibPath: resolved.stdlibPath, resourcePath: resolved.resourcePath };
  log(`Compiler: ${settings.compilerPath} (${resolved.source}).`);
}
function compilerDiagnostics(uri, text) {
  if (settings.diagnostics === 'off') return [];
  const compiler = settings.compilerPath;
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'shaft-lsp-'));
  const sourcePath = path.join(directory, 'document.shaft');
  try {
    fs.writeFileSync(sourcePath, text, 'utf8');
    const args = ['--emit', 'llvm', '-o', path.join(directory, 'document.ll')];
    if (settings.stdlibPath) args.push('--std', settings.stdlibPath);
    else {
      const stdlib = standardLibraryPath();
      if (stdlib) args.push('--std', stdlib);
    }
    if (settings.resourcePath) args.push('--resources', settings.resourcePath);
    args.push(sourcePath);
    const result = childProcess.spawnSync(compiler, args, { cwd: workspacePath(), encoding: 'utf8', timeout: 10000, windowsHide: true });
    if (result.error) return [{ severity: 2, source: 'shaft-lsp', message: `Compiler unavailable: ${result.error.message}`, range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } } }];
    return language.parseCompilerDiagnostics(`${result.stdout || ''}\n${result.stderr || ''}`);
  } finally { fs.rmSync(directory, { recursive: true, force: true }); }
}
function fullDocumentRange(analysis) {
  const lastLine = Math.max(0, analysis.lines.length - 1);
  return { start: { line: 0, character: 0 }, end: { line: lastLine, character: analysis.lines[lastLine].length } };
}
function semanticTokens(analysis) {
  const tokens = [];
  for (let line = 0; line < analysis.lines.length; line += 1) {
    const raw = analysis.lines[line];
    const commentAt = raw.indexOf('//');
    const code = commentAt < 0 ? raw : raw.slice(0, commentAt);
    if (commentAt >= 0) tokens.push({ line, start: commentAt, length: raw.length - commentAt, type: 5 });
    const matcher = /"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\b\d+(?:\.\d+)?\b|\b[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*\b|::|->|<-|==|!=|<=|>=|&&|\|\||[+\-*/%=<>!&|^]/g;
    for (const match of code.matchAll(matcher)) {
      const text = match[0];
      let type = 6;
      if (text.startsWith('"') || text.startsWith("'")) type = 4;
      else if (/^\d/.test(text)) type = 3;
      else if (keywordSet.has(text)) type = 0;
      else if (/^[A-Z]/.test(text) || text.includes('::')) type = 1;
      else if (/^[A-Za-z_]/.test(text) && /\(/.test(code.slice(match.index + text.length))) type = 2;
      tokens.push({ line, start: match.index, length: text.length, type });
    }
  }
  tokens.sort((a, b) => a.line - b.line || a.start - b.start);
  const data = [];
  let previousLine = 0;
  let previousStart = 0;
  for (const token of tokens) {
    data.push(token.line - previousLine, token.line === previousLine ? token.start - previousStart : token.start, token.length, token.type, 0);
    previousLine = token.line;
    previousStart = token.start;
  }
  return { data };
}
function hover(analysis, pos) {
  const definition = language.findDefinition(analysis, pos) || allSymbols().find((symbol) => symbol.name === language.wordAt(analysis, pos));
  if (definition) {
    const documentation = definition.documentation ? `\n\n${definition.documentation}` : '';
    return { contents: { kind: 'markdown', value: `\`\`\`shaft\n${definition.declaration || definition.detail}\n\`\`\`${documentation}\n\nDefined in ${definition.uri}` }, range: definition.range };
  }
  const word = language.wordAt(analysis, pos);
  const docs = {
    'Collections::HashMap': 'Open-addressing hash map layout. The bootstrap operational API currently supports `u64` keys and values.',
    'Collections::HashSet': 'Open-addressing hash set layout. The bootstrap operational API currently supports `u64` elements.',
    tunnel: 'Writes a value to a named result slot. A slot may execute only once in a function.',
    namespace: 'Introduces a declaration scope. Qualified names preserve every `::` segment.',
  };
  return docs[word] ? { contents: { kind: 'markdown', value: docs[word] } } : null;
}
function definitionLocation(definition) { return definition ? [{ uri: definition.uri, range: definition.selectionRange }] : null; }
function foldingRanges(analysis) {
  const ranges = [];
  const stack = [];
  for (let line = 0; line < analysis.sanitized.length; line += 1) {
    for (const ch of analysis.sanitized[line]) {
      if (ch === '{') stack.push(line);
      else if (ch === '}' && stack.length) { const startLine = stack.pop(); if (line > startLine) ranges.push({ startLine, endLine: line, kind: 'region' }); }
    }
  }
  return ranges;
}
function documentSymbols(analysis) {
  return analysis.symbols.map((symbol) => ({ name: symbol.shortName, detail: symbol.name, kind: symbolKinds[symbol.kind] || 13, range: symbol.range, selectionRange: symbol.selectionRange }));
}
function handle(message) {
  const { id, method, params = {} } = message;
  try {
    switch (method) {
      case 'initialize': {
        rootUri = params.rootUri || params.workspaceFolders?.[0]?.uri || null;
        settings = { ...settings, ...(params.initializationOptions || {}) };
        applyCompilerDiscovery();
        return respond(id, { capabilities: {
          positionEncoding: 'utf-16', textDocumentSync: { openClose: true, change: 2, save: { includeText: true } },
          completionProvider: { triggerCharacters: ['.', ':'], resolveProvider: false }, definitionProvider: true,
          hoverProvider: true, documentFormattingProvider: true, documentSymbolProvider: true,
          workspaceSymbolProvider: true, foldingRangeProvider: true,
          semanticTokensProvider: { legend: { tokenTypes, tokenModifiers: [] }, full: true },
        }, serverInfo: { name: 'shaft-lsp', version: '0.1.0' } });
      }
      case 'initialized': indexWorkspace(); return;
      case 'shutdown': shutdownRequested = true; return respond(id, null);
      case 'exit': return process.exit(shutdownRequested ? 0 : 1);
      case 'workspace/didChangeConfiguration': {
        settings = { ...settings, ...(params.settings?.shaft?.languageServer || params.settings || {}) };
        applyCompilerDiscovery();
        return;
      }
      case 'textDocument/didOpen': {
        const analysis = analyze(params.textDocument.uri, params.textDocument.text);
        publish(analysis, settings.diagnostics === 'onChange' ? compilerDiagnostics(analysis.uri, analysis.text) : []); return;
      }
      case 'textDocument/didChange': {
        const previous = documentFor(params.textDocument.uri);
        const text = applyChanges(previous?.text || '', params.contentChanges || []);
        const analysis = analyze(params.textDocument.uri, text);
        publish(analysis, settings.diagnostics === 'onChange' ? compilerDiagnostics(analysis.uri, text) : []); return;
      }
      case 'textDocument/didSave': {
        const analysis = documentFor(params.textDocument.uri) || analyze(params.textDocument.uri, params.text || '');
        publish(analysis, compilerDiagnostics(analysis.uri, analysis.text)); return;
      }
      case 'textDocument/didClose': documents.delete(params.textDocument.uri); notify('textDocument/publishDiagnostics', { uri: params.textDocument.uri, diagnostics: [] }); return;
      case 'textDocument/completion': {
        const analysis = documentFor(params.textDocument.uri); const items = analysis ? language.completions(analysis, params.position) : [];
        return respond(id, { isIncomplete: false, items: items.map((item) => ({ ...item, kind: completionKinds[item.kind] || 1 })) });
      }
      case 'textDocument/definition': { const analysis = documentFor(params.textDocument.uri); return respond(id, definitionLocation(analysis && (language.findDefinition(analysis, params.position) || allSymbols().find((symbol) => symbol.name === language.wordAt(analysis, params.position))))); }
      case 'textDocument/hover': { const analysis = documentFor(params.textDocument.uri); return respond(id, analysis ? hover(analysis, params.position) : null); }
      case 'textDocument/documentSymbol': { const analysis = documentFor(params.textDocument.uri); return respond(id, analysis ? documentSymbols(analysis) : []); }
      case 'workspace/symbol': { const query = (params.query || '').toLowerCase(); return respond(id, allSymbols().filter((symbol) => symbol.name.toLowerCase().includes(query)).map((symbol) => ({ name: symbol.name, kind: symbolKinds[symbol.kind] || 13, location: { uri: symbol.uri, range: symbol.range } }))); }
      case 'textDocument/formatting': { const analysis = documentFor(params.textDocument.uri); if (!analysis) return respond(id, []); return respond(id, [{ range: fullDocumentRange(analysis), newText: language.formatDocument(analysis.text, { ...params.options, insertFinalNewline: settings.insertFinalNewline }) }]); }
      case 'textDocument/semanticTokens/full': { const analysis = documentFor(params.textDocument.uri); return respond(id, analysis ? semanticTokens(analysis) : { data: [] }); }
      case 'textDocument/foldingRange': { const analysis = documentFor(params.textDocument.uri); return respond(id, analysis ? foldingRanges(analysis) : []); }
      default: if (id !== undefined) return fail(id, -32601, `Method not found: ${method}`);
    }
  } catch (error) {
    if (id !== undefined) fail(id, -32603, error.message || String(error));
    else log(error.stack || String(error), 1);
  }
}
function processBuffer() {
  while (true) {
    const separator = buffer.indexOf('\r\n\r\n');
    if (separator < 0) return;
    const header = buffer.slice(0, separator).toString('ascii');
    const length = Number(header.match(/Content-Length:\s*(\d+)/i)?.[1]);
    if (!Number.isFinite(length)) { buffer = buffer.slice(separator + 4); continue; }
    const bodyStart = separator + 4;
    if (buffer.length < bodyStart + length) return;
    const body = buffer.slice(bodyStart, bodyStart + length).toString('utf8');
    buffer = buffer.slice(bodyStart + length);
    try { handle(JSON.parse(body)); } catch (error) { log(`Invalid JSON-RPC payload: ${error.message}`, 1); }
  }
}
process.stdin.on('data', (chunk) => { buffer = Buffer.concat([buffer, chunk]); processBuffer(); });
process.stdin.on('error', (error) => log(`stdin error: ${error.message}`, 1));
