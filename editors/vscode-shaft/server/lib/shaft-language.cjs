'use strict';

const KEYWORDS = [
  'align', 'as', 'async', 'await', 'break', 'case', 'cdec', 'cdef', 'char', 'continue',
  'dec', 'default', 'def', 'else', 'enum', 'export', 'f32', 'f64', 'for', 'foreach', 'i8',
  'i16', 'i32', 'i64', 'if', 'import', 'index', 'init', 'inline', 'match', 'move', 'mut',
  'impl', 'namespace', 'raw', 'ref', 'reserve', 'return', 'self', 'sizeof', 'start', 'State', 'struct',
  'Thread', 'trait', 'tunnel', 'u8', 'u16', 'u32', 'u64', 'usize', 'using', 'valid', 'while', 'bool',
];

const STANDARD_COMPLETIONS = [
  { label: 'std::string::String', kind: 'struct', detail: 'Owned UTF-8 string' },
  { label: 'std::string::view::View', kind: 'struct', detail: 'Borrowed string view' },
  { label: 'std::vector::Vector', kind: 'struct', detail: 'Growable generic vector' },
  { label: 'std::io::print', kind: 'function', detail: 'Write a string view to standard output' },
  { label: 'std::io::println', kind: 'function', detail: 'Write a string view and newline to standard output' },
  { label: 'std::vector::init', kind: 'function', detail: 'Initialize a Vector<T>' },
  { label: 'std::vector::push', kind: 'function', detail: 'Append an element to a Vector<T>' },
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
  let pendingStruct = null;
  let pendingDocumentation = [];

  for (let line = 0; line < sanitized.length; line += 1) {
    const source = sanitized[line];
    const documentation = lines[line].match(/^\s*\/\/\/\s?(.*)$/);
    if (documentation) {
      pendingDocumentation.push(documentation[1]);
      continue;
    }
    const namespaceMatch = source.match(/^\s*namespace\s+([A-Za-z_]\w*)\b/);
    if (namespaceMatch) {
      pendingNamespace = namespaceMatch[1];
      const namespaceName = qualifiedName(namespaceStack, namespaceMatch[1]);
      const start = source.indexOf(namespaceMatch[1]);
      symbols.push({ name: namespaceName, shortName: namespaceMatch[1], kind: 'namespace', uri, range: range(line, start, start + namespaceMatch[1].length), selectionRange: range(line, start, start + namespaceMatch[1].length), detail: `namespace ${namespaceName}`, declaration: `namespace ${namespaceName}`, documentation: '' });
    }

    const declaration = source.match(/^\s*(?:(?:export|inline)\s+)*(?:cdef|cdec|def|dec|struct|trait|impl|enum)\s*(?:<[^>]*>)?\s*([A-Za-z_]\w*)/);
    if (declaration) {
      const prefix = source.slice(0, declaration.index + declaration[0].lastIndexOf(declaration[1]));
      const word = prefix.match(/(cdef|cdec|def|dec|struct|trait|impl|enum)\s*(?:<[^>]*>)?\s*$/)?.[1] || 'symbol';
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
      if (word === 'struct') pendingStruct = name;
      if (['def', 'cdef', 'dec', 'cdec'].includes(word)) {
        const parameters = source.slice(start + declaration[1].length).match(/^\s*\(([^)]*)\)/)?.[1] || '';
        for (const parameter of parameters.split(',')) {
          const match = parameter.trim().match(/(?:\*\s*)?[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:\s*\[[^\]]*\])?\s+([A-Za-z_]\w*)\s*$/);
          if (match) {
            const parameterStart = source.indexOf(match[1], start + declaration[1].length);
            const parameterType = parameter.trim().slice(0, parameter.trim().lastIndexOf(match[1])).trim();
            symbols.push({ name: match[1], shortName: match[1], kind: 'parameter', type: parameterType, uri, range: range(line, parameterStart, parameterStart + match[1].length), selectionRange: range(line, parameterStart, parameterStart + match[1].length), detail: `parameter ${parameter.trim()}`, declaration: parameter.trim(), documentation: '' });
          }
        }
      }
      pendingDocumentation = [];
    } else if (source.trim()) {
      pendingDocumentation = [];
    }

    const variable = source.match(/^\s*(?:(?:reserve|mut|ref)\s+)*(?:\*\s*)?[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:\s*\[[^\]]*\])?\s+([A-Za-z_]\w*)\s*(?:=[^;]*)?;/);
    if (variable) {
      const name = variable[1];
      const start = source.indexOf(name, variable.index || 0);
      const owner = [...scopeStack].reverse().find((scope) => scope.kind === 'struct');
      const declaredType = source.slice(0, start).trim().replace(/^(?:(?:reserve|mut|ref)\s+)+/, '').trim();
      symbols.push({ name, shortName: name, kind: owner ? 'field' : 'variable', owner: owner?.name || '', type: declaredType, uri, range: range(line, start, start + name.length), selectionRange: range(line, start, start + name.length), detail: `${declaredType} ${name}${owner ? ` (field of ${owner.name})` : ''}`, declaration: `${declaredType} ${name};`, documentation: '' });
    }

    for (const ch of source) {
      if (ch === '{') {
        if (pendingNamespace) {
          const namespaceName = qualifiedName(namespaceStack, pendingNamespace);
          namespaceStack.push(pendingNamespace);
          scopeStack.push({ namespace: true, kind: 'namespace', name: namespaceName, line });
          pendingNamespace = null;
        } else if (pendingStruct) {
          scopeStack.push({ namespace: false, kind: 'struct', name: pendingStruct, line });
          pendingStruct = null;
        } else scopeStack.push({ namespace: false, kind: 'block', line });
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

function completions(analysis, pos, additionalSymbols = []) {
  const seen = new Set();
  const items = [];
  const line = analysis.lines[pos?.line] || '';
  const beforeCursor = line.slice(0, pos?.character ?? line.length);
  const memberAccess = beforeCursor.match(/([A-Za-z_]\w*)\s*\.\s*([A-Za-z_]\w*)?$/);
  const namespaceAccess = beforeCursor.match(/([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)::([A-Za-z_]\w*)?$/);
  const prefix = memberAccess?.[2] || namespaceAccess?.[2] || beforeCursor.match(/[A-Za-z_]\w*$/)?.[0] || '';
  let memberOwner = '';
  if (memberAccess) {
    const receiver = memberAccess[1];
    const variable = [...analysis.symbols, ...additionalSymbols].find((symbol) => symbol.shortName === receiver && ['variable', 'parameter'].includes(symbol.kind));
    memberOwner = variable?.type?.replace(/^\*\s*/, '').split('::').at(-1) || '';
  }
  const symbols = [...analysis.symbols, ...additionalSymbols.filter((symbol) => !['variable', 'parameter'].includes(symbol.kind))];
  let candidates;
  if (memberAccess) {
    candidates = symbols.filter((symbol) => symbol.kind === 'field' && (!memberOwner || symbol.owner.split('::').at(-1) === memberOwner));
  } else if (namespaceAccess) {
    const namespace = namespaceAccess[1];
    candidates = [...STANDARD_COMPLETIONS, ...symbols].filter((item) => (item.name || item.label).startsWith(`${namespace}::`)).map((item) => ({ ...item, label: (item.name || item.label).slice(namespace.length + 2), name: undefined }));
  } else {
    candidates = [...STANDARD_COMPLETIONS, ...KEYWORDS.map((label) => ({ label, kind: 'keyword', detail: 'Shaft keyword' })), ...symbols];
  }
  for (const item of candidates) {
    const label = item.name || item.label || item.shortName;
    if (!label || (prefix && !label.toLowerCase().startsWith(prefix.toLowerCase()))) continue;
    const key = `${item.kind}:${label}`;
    if (seen.has(key)) continue;
    seen.add(key);
    items.push({ label, kind: item.kind, detail: item.detail || item.declaration, insertText: item.shortName || label });
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
  const lines = output.replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, '').split(/\r?\n/);
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    const bracketed = line.match(/^(Error|Warning)\s*\[(\d+):(\d+)\]:\s*(.+)$/);
    const textual = line.match(/^(Error|Warning)\s+at\s+line\s+(\d+):(\d+)\s+-\s+(.+)$/);
    const lexer = line.match(/^(.+?)\s+at\s+line\s+(\d+):(\d+)$/);
    const newStyle = line.match(/^(?:error|warning):\s*(.+)$/i);
    const match = bracketed || textual || lexer || newStyle;
    if (!match) continue;

    let severity = 1;
    let lineNumber = 0;
    let columnNumber = 0;
    let message = '';

    if (bracketed) {
      severity = match[1] === 'Warning' ? 2 : 1;
      lineNumber = Number(match[2]);
      columnNumber = Number(match[3]);
      message = match[4];
    } else if (textual) {
      severity = match[1] === 'Warning' ? 2 : 1;
      lineNumber = Number(match[2]);
      columnNumber = Number(match[3]);
      message = match[4];
    } else if (lexer) {
      severity = 1;
      lineNumber = Number(match[2]);
      columnNumber = Number(match[3]);
      message = match[1];
    } else {
      severity = /^warning:/i.test(line) ? 2 : 1;
      message = match[1];
      let end = index + 1;
      while (end < lines.length && !/^(?:error|warning):\s*/i.test(lines[end])) end += 1;
      let location = null;
      let help = '';
      for (let detail = index + 1; detail < end; detail += 1) {
        location ||= lines[detail].match(/^-->\s*.*:(\d+):(\d+)\s*$/);
        const suggestion = lines[detail].match(/^help:\s*(.+)$/);
        if (suggestion) help = `help: ${suggestion[1]}`;
      }
      if (location) {
        lineNumber = Number(location[1]);
        columnNumber = Number(location[2]);
      } else {
        lineNumber = 1;
        columnNumber = 1;
      }
      if (help) message = `${message}\n${help}`;
      index = end - 1;
    }

    diagnostics.push({
      severity,
      message,
      range: range(lineNumber - 1, Math.max(0, columnNumber - 1), Math.max(1, columnNumber)),
      source: 'shaftc',
    });
  }
  return diagnostics;
}

module.exports = { KEYWORDS, analyzeDocument, completions, findDefinition, formatDocument, parseCompilerDiagnostics, wordAt };
