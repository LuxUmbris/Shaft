'use strict';

const KEYWORDS = [
  'align', 'async', 'await', 'break', 'case', 'cdec', 'cdef', 'char', 'class', 'continue',
  'dec', 'default', 'def', 'else', 'enum', 'export', 'f32', 'f64', 'for', 'foreach', 'i8',
  'i16', 'i32', 'i64', 'if', 'import', 'index', 'init', 'inline', 'match', 'move', 'mut',
  'namespace', 'raw', 'ref', 'reserve', 'return', 'self', 'start', 'State', 'struct',
  'Thread', 'tunnel', 'u8', 'u16', 'u32', 'u64', 'usize', 'using', 'valid', 'while', 'bool',
];

const STANDARD_COMPLETIONS = [
  { label: 'Collections::HashMap', kind: 'class', detail: 'Shaft standard collection (u64 operations)' },
  { label: 'Collections::HashSet', kind: 'class', detail: 'Shaft standard collection (u64 operations)' },
  { label: 'Collections::hash_map_u64_init', kind: 'function', detail: 'Initialize a u64 hash map' },
  { label: 'Collections::hash_map_u64_insert', kind: 'function', detail: 'Insert or replace a u64 mapping' },
  { label: 'Collections::hash_map_u64_contains', kind: 'function', detail: 'Check map membership' },
  { label: 'Collections::hash_map_u64_get', kind: 'function', detail: 'Get value or zero when missing' },
  { label: 'Collections::hash_map_u64_len', kind: 'function', detail: 'Number of distinct map keys' },
  { label: 'Collections::hash_set_u64_init', kind: 'function', detail: 'Initialize a u64 hash set' },
  { label: 'Collections::hash_set_u64_insert', kind: 'function', detail: 'Insert a distinct u64 value' },
  { label: 'Collections::hash_set_u64_contains', kind: 'function', detail: 'Check set membership' },
  { label: 'Collections::hash_set_u64_len', kind: 'function', detail: 'Number of distinct set values' },
];

function position(line, character) { return { line, character }; }
function range(line, start, end) { return { start: position(line, start), end: position(line, end) }; }

function stripCommentsAndStrings(text) {
  let out = '';
  let quote = null;
  let escaped = false;
  for (let i = 0; i < text.length; i += 1) {
    const ch = text[i];
    const next = text[i + 1];
    if (quote) {
      out += ' ';
      if (escaped) escaped = false;
      else if (ch === '\\') escaped = true;
      else if (ch === quote) quote = null;
      continue;
    }
    if (ch === '/' && next === '/') {
      out += ' '.repeat(text.length - i);
      break;
    }
    if (ch === '"' || ch === "'") {
      quote = ch;
      out += ' ';
      continue;
    }
    out += ch;
  }
  return out;
}

function qualifiedName(namespaceStack, name) {
  return namespaceStack.length ? `${namespaceStack.join('::')}::${name}` : name;
}

function analyzeDocument(uri, text) {
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  const symbols = [];
  const diagnostics = [];
  const sanitized = lines.map(stripCommentsAndStrings);
  const namespaceStack = [];
  const scopeStack = [];
  let pendingNamespace = null;
  let pendingDocumentation = [];

  for (let line = 0; line < sanitized.length; line += 1) {
    const source = sanitized[line];
    const documentation = lines[line].match(/^\s*\/\/\/\s?(.*)$/);
    if (documentation) {
      pendingDocumentation.push(documentation[1]);
      continue;
    }
    const namespaceMatch = source.match(/^\s*namespace\s+([A-Za-z_]\w*)\b/);
    if (namespaceMatch) pendingNamespace = namespaceMatch[1];

    const declaration = source.match(/^\s*(?:(?:export|inline)\s+)*(?:cdef|cdec|def|dec|class|struct|enum)\s*(?:<[^>]*>)?\s*([A-Za-z_]\w*)/);
    if (declaration) {
      const prefix = source.slice(0, declaration.index + declaration[0].lastIndexOf(declaration[1]));
      const word = prefix.match(/(cdef|cdec|def|dec|class|struct|enum)\s*(?:<[^>]*>)?\s*$/)?.[1] || 'symbol';
      const start = source.indexOf(declaration[1], declaration.index);
      const name = qualifiedName(namespaceStack, declaration[1]);
      const declarationSource = source.slice(0, source.indexOf('{') >= 0 ? source.indexOf('{') : source.length);
      const declarationText = `${declarationSource.slice(0, start)}${name}${declarationSource.slice(start + declaration[1].length)}`.trim();
      symbols.push({
        name,
        shortName: declaration[1],
        kind: word === 'def' || word === 'cdef' || word === 'dec' || word === 'cdec' ? 'function' : word,
        uri,
        range: range(line, start, start + declaration[1].length),
        selectionRange: range(line, start, start + declaration[1].length),
        detail: declarationText,
        declaration: declarationText,
        documentation: pendingDocumentation.join('\n').trim(),
      });
      pendingDocumentation = [];
    } else if (source.trim()) {
      pendingDocumentation = [];
    }

    for (const ch of source) {
      if (ch === '{') {
        if (pendingNamespace) {
          namespaceStack.push(pendingNamespace);
          scopeStack.push({ namespace: true, line });
          pendingNamespace = null;
        } else scopeStack.push({ namespace: false, line });
      } else if (ch === '}') {
        const scope = scopeStack.pop();
        if (!scope) diagnostics.push({ severity: 1, message: "Unmatched '}'", range: range(line, source.indexOf('}'), source.indexOf('}') + 1), source: 'shaft-lsp' });
        else if (scope.namespace) namespaceStack.pop();
      }
    }
  }

  for (const scope of scopeStack) {
    diagnostics.push({ severity: 1, message: "Unclosed '{'", range: range(scope.line, 0, 1), source: 'shaft-lsp' });
  }
  return { uri, text, lines, sanitized, symbols, diagnostics };
}

function wordAt(analysis, pos) {
  const line = analysis.lines[pos.line] || '';
  const left = line.slice(0, pos.character).match(/[A-Za-z_][\w:]*(?=$|[^\w:])/)?.[0] || '';
  const right = line.slice(pos.character).match(/^[\w:]*/)?.[0] || '';
  return `${left}${right}`.replace(/^:+|:+$/g, '');
}

function findDefinition(analysis, pos) {
  const word = wordAt(analysis, pos);
  if (!word) return null;
  const exact = analysis.symbols.find((symbol) => symbol.name === word);
  return exact || analysis.symbols.find((symbol) => symbol.shortName === word) || null;
}

function completions(analysis, _position) {
  const seen = new Set();
  const items = [];
  for (const item of [...STANDARD_COMPLETIONS, ...KEYWORDS.map((label) => ({ label, kind: 'keyword', detail: 'Shaft keyword' })), ...analysis.symbols]) {
    if (seen.has(item.name || item.label)) continue;
    seen.add(item.name || item.label);
    items.push({ label: item.name || item.label, kind: item.kind, detail: item.detail });
  }
  return items.sort((a, b) => a.label.localeCompare(b.label));
}

function formatDocument(text, options = {}) {
  const indentText = options.insertSpaces === false ? '\t' : ' '.repeat(options.tabSize || 4);
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  let depth = 0;
  const result = lines.map((line) => {
    const trimmed = line.trim();
    if (!trimmed) return '';
    const code = stripCommentsAndStrings(trimmed);
    const closes = (code.match(/}/g) || []).length;
    const opens = (code.match(/{/g) || []).length;
    const lineDepth = /^}/.test(trimmed) ? Math.max(0, depth - 1) : depth;
    const formatted = `${indentText.repeat(lineDepth)}${trimmed}`;
    depth = Math.max(0, depth + opens - closes);
    return formatted;
  });
  const formatted = result.join('\n');
  return options.insertFinalNewline && formatted && !formatted.endsWith('\n') ? `${formatted}\n` : formatted;
}

function parseCompilerDiagnostics(output) {
  const diagnostics = [];
  for (const line of output.split(/\r?\n/)) {
    const bracketed = line.match(/^(Error|Warning)\s*\[(\d+):(\d+)\]:\s*(.+)$/);
    const textual = line.match(/^(Error|Warning)\s+at\s+line\s+(\d+):(\d+)\s+-\s+(.+)$/);
    const lexer = line.match(/^(.+?)\s+at\s+line\s+(\d+):(\d+)$/);
    const match = bracketed || textual || lexer;
    if (!match) continue;
    const severity = lexer ? 1 : match[1] === 'Warning' ? 2 : 1;
    const lineNumber = Number(lexer ? match[2] : match[2]);
    const columnNumber = Number(lexer ? match[3] : match[3]);
    const message = lexer ? match[1] : match[4];
    diagnostics.push({ severity, message, range: range(lineNumber - 1, Math.max(0, columnNumber - 1), Math.max(1, columnNumber)), source: 'shaftc' });
  }
  return diagnostics;
}

module.exports = { KEYWORDS, analyzeDocument, completions, findDefinition, formatDocument, parseCompilerDiagnostics, wordAt };
