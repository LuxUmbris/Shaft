const test = require('node:test');
const assert = require('node:assert/strict');
const language = require('../server/lib/shaft-language.cjs');

test('collects qualified declarations and resolves qualified definitions', () => {
  const source = [
    'namespace Collections',
    '{',
    '    class HashMap',
    '    {',
    '        u64 length;',
    '    }',
    '}',
    'Collections::HashMap map;',
  ].join('\n');

  const analysis = language.analyzeDocument('file:///project/example.shaft', source);
  const declaration = analysis.symbols.find((symbol) => symbol.name === 'Collections::HashMap');
  assert.ok(declaration);
  assert.equal(declaration.kind, 'class');
  assert.equal(declaration.declaration, 'class Collections::HashMap');

  const definition = language.findDefinition(analysis, { line: 7, character: 15 });
  assert.equal(definition.name, 'Collections::HashMap');
  assert.equal(definition.range.start.line, 2);
});

test('reports unmatched delimiters without reporting strings or comments', () => {
  const source = 'def main() {\n  // }\n  String text = "{";\n';
  const diagnostics = language.analyzeDocument('file:///project/example.shaft', source).diagnostics;
  assert.equal(diagnostics.length, 1);
  assert.match(diagnostics[0].message, /Unclosed '\{'/);
  assert.equal(diagnostics[0].range.start.line, 0);
});

test('offers context-aware language and workspace completions', () => {
  const source = 'namespace Core {\n  def answer() -> u64 result { tunnel 42 -> u64 result; }\n}\nCo';
  const analysis = language.analyzeDocument('file:///project/example.shaft', source);
  const completions = language.completions(analysis, { line: 3, character: 2 });
  assert.ok(completions.some((item) => item.label === 'Collections::HashMap'));
  assert.ok(completions.some((item) => item.label === 'Core::answer'));
  assert.ok(completions.some((item) => item.label === 'namespace'));
});

test('keeps async keyword metadata synchronized with shaftc', () => {
  const grammar = require('../syntaxes/shaft.tmLanguage.json');
  assert.ok(language.KEYWORDS.includes('async'));
  assert.ok(!language.KEYWORDS.includes('asyc'));
  assert.match(grammar.repository.keywords.match, /async/);
  assert.doesNotMatch(grammar.repository.keywords.match, /asyc/);
  const functionDeclaration = grammar.repository.declarations.patterns.find((pattern) => pattern.match.includes('(def|'));
  assert.ok(functionDeclaration);
  assert.match(functionDeclaration.match, /\(async\)/);
});

test('grammar scopes custom types, macros, global, and boolean literals', () => {
  const grammar = require('../syntaxes/shaft.tmLanguage.json');
  assert.match(grammar.repository.declarations.patterns[0].captures['3'].name, /entity\.name\.type/);
  assert.match(grammar.repository.customTypes.patterns[0].match, /reserve/);
  assert.match(grammar.repository.macros.patterns[0].match, /!/);
  assert.match(grammar.repository.keywords.match, /global/);
  assert.match(grammar.repository.booleans.match, /true\|false/);
});

test('grammar scopes qualified custom types at arbitrary namespace depth', () => {
  const grammar = require('../syntaxes/shaft.tmLanguage.json');
  const pattern = new RegExp(grammar.repository.customTypes.patterns[0].match);
  const match = pattern.exec('reserve Namespace::Nested::Widget widget;');
  assert.ok(match);
  assert.equal(match[3], 'Namespace::Nested::Widget');
});

test('formats indentation while preserving non-empty lines', () => {
  const source = 'namespace Core\n{\ndef answer()\n{\ntunnel 42 -> u64 result;\n}\n}\n';
  assert.equal(
    language.formatDocument(source, { insertSpaces: true, tabSize: 4 }),
    'namespace Core\n{\n    def answer()\n    {\n        tunnel 42 -> u64 result;\n    }\n}\n',
  );
  assert.equal(language.formatDocument('def answer() {}', { insertSpaces: true, tabSize: 2, insertFinalNewline: true }), 'def answer() {}\n');
});

test('maps compiler parser and lexer diagnostic formats to LSP ranges', () => {
  const parsed = language.parseCompilerDiagnostics("Error [12:7]: Unknown function 'missing'.\nError at line 14:1 - Expected ';' after return statement\nUnknown token '@' at line 16:9\n");
  assert.equal(parsed.length, 3);
  assert.equal(parsed[0].range.start.line, 11);
  assert.equal(parsed[0].range.start.character, 6);
  assert.match(parsed[0].message, /Unknown function/);
  assert.equal(parsed[1].range.start.line, 13);
  assert.equal(parsed[1].range.start.character, 0);
  assert.match(parsed[1].message, /Expected ';'/);
  assert.equal(parsed[2].range.start.line, 15);
  assert.equal(parsed[2].range.start.character, 8);
  assert.match(parsed[2].message, /Unknown token '@'/);
});

test('maps the new compiler error and warning format with module paths to LSP ranges', () => {
  const parsed = language.parseCompilerDiagnostics("error: Unknown function 'missing'.\n--> /tmp/project/main.shaft:12:7\nwarning: Shadowed name.\n--> /tmp/project/other.shaft:5:3\n");
  assert.equal(parsed.length, 2);
  assert.equal(parsed[0].range.start.line, 11);
  assert.equal(parsed[0].range.start.character, 6);
  assert.match(parsed[0].message, /Unknown function/);
  assert.equal(parsed[1].range.start.line, 4);
  assert.equal(parsed[1].range.start.character, 2);
  assert.match(parsed[1].message, /Shadowed name/);
});
