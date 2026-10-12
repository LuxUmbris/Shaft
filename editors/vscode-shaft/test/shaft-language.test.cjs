const test = require('node:test');
const assert = require('node:assert/strict');
const language = require('../server/lib/shaft-language.cjs');

test('collects qualified declarations and resolves qualified definitions', () => {
  const source = [
    'namespace Collections',
    '{',
    '    struct HashMap',
    '    {',
    '        u64 length;',
    '    }',
    '}',
    'Collections::HashMap map;',
  ].join('\n');

  const analysis = language.analyzeDocument('file:///project/example.shaft', source);
  const declaration = analysis.symbols.find((symbol) => symbol.name === 'Collections::HashMap');
  assert.ok(declaration);
  assert.equal(declaration.kind, 'struct');
  assert.equal(declaration.declaration, 'struct Collections::HashMap');

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
  const completions = language.completions(analysis, { line: 0, character: 0 });
  assert.ok(completions.some((item) => item.label === 'std::vector::Vector'));
  assert.ok(completions.some((item) => item.label === 'Core::answer'));
  assert.ok(completions.some((item) => item.label === 'namespace'));
  const filtered = language.completions(analysis, { line: 3, character: 2 });
  assert.ok(filtered.some((item) => item.label === 'Core'));
  assert.ok(!filtered.some((item) => item.label === 'namespace'));
});

test('offers local variables, parameters, struct fields, and namespace members', () => {
  const source = [
    'namespace Core {',
    '  struct User {',
    '    u64 id;',
    '  }',
    '  def load(User user, u64 count) {',
    '    u64 localValue = count;',
    '    user.',
    '  }',
    '}',
    'Core::U',
  ].join('\n');
  const analysis = language.analyzeDocument('file:///project/completions.shaft', source);
  assert.ok(analysis.symbols.some((symbol) => symbol.kind === 'variable' && symbol.shortName === 'localValue'));
  assert.ok(analysis.symbols.some((symbol) => symbol.kind === 'parameter' && symbol.shortName === 'user'));
  assert.ok(analysis.symbols.some((symbol) => symbol.kind === 'field' && symbol.owner === 'Core::User' && symbol.shortName === 'id'));
  const fields = language.completions(analysis, { line: 6, character: 10 });
  assert.ok(fields.some((item) => item.label === 'id'));
  const namespace = language.completions(analysis, { line: 9, character: 7 });
  assert.ok(namespace.some((item) => item.label === 'User'));
});

test('keeps async keyword metadata synchronized with shaftc', () => {
  const grammar = require('../syntaxes/shaft.tmLanguage.json');
  assert.ok(language.KEYWORDS.includes('async'));
  assert.ok(!language.KEYWORDS.includes('asyc'));
  assert.match(grammar.repository.keywords.match, /async/);
  assert.doesNotMatch(grammar.repository.keywords.match, /asyc/);
  const functionDeclaration = grammar.repository.declarations.patterns.find((pattern) => pattern.match.includes('(def|'));
  assert.ok(functionDeclaration);
  assert.match(functionDeclaration.match, /async\|naked/);
});

test('keeps casts and trait declarations synchronized while excluding removed classes', () => {
  const grammar = require('../syntaxes/shaft.tmLanguage.json');
  const analysis = language.analyzeDocument('file:///project/traits.shaft', [
    'trait Printable {',
    '    def print(&self);',
    '}',
    'struct Item { u64 id; }',
    'impl Printable for Item {',
    '    def print(&self) {}',
    '}',
    'u8 narrow = wide as u8;',
  ].join('\n'));
  assert.ok(language.KEYWORDS.includes('as'));
  assert.ok(language.KEYWORDS.includes('trait'));
  assert.ok(language.KEYWORDS.includes('impl'));
  assert.ok(!language.KEYWORDS.includes('class'));
  assert.equal(analysis.symbols.find((symbol) => symbol.shortName === 'Printable').kind, 'trait');
  assert.equal(analysis.symbols.find((symbol) => symbol.shortName === 'Item').kind, 'struct');
  assert.equal(analysis.symbols.find((symbol) => symbol.shortName === 'Printable' && symbol.kind === 'impl').kind, 'impl');
  assert.match(grammar.repository.keywords.match, /as/);
  assert.match(grammar.repository.declarations.patterns[0].match, /trait\|impl/);
  assert.doesNotMatch(grammar.repository.declarations.patterns[0].match, /class/);
});

test('grammar scopes custom types, macros, global, and boolean literals', () => {
  const grammar = require('../syntaxes/shaft.tmLanguage.json');
  assert.match(grammar.repository.declarations.patterns[0].captures['3'].name, /entity\.name\.type/);
  assert.match(grammar.repository.customTypes.patterns[0].match, /reserve/);
  const functionMacros = grammar.repository.macros.patterns.find((pattern) => pattern.match?.includes('!'));
  assert.match(functionMacros.match, /!/);
  assert.match(grammar.repository.keywords.match, /global/);
  assert.match(grammar.repository.booleans.match, /true\|false/);
});

test('grammar scopes imports and config/error metaprogramming directives', () => {
  const grammar = require('../syntaxes/shaft.tmLanguage.json');
  assert.match(grammar.repository.keywords.match, /import/);
  const directives = grammar.repository.macros.patterns.find((pattern) => pattern.name?.includes('preprocessor') && pattern.match?.includes('config'));
  const conditions = grammar.repository.macros.patterns.find((pattern) => pattern.match?.includes('in|matches'));
  const error = grammar.repository.macros.patterns.find((pattern) => pattern.match?.includes('@error'));
  const unsafe = grammar.repository.macros.patterns.find((pattern) => pattern.match?.includes('unsafe'));
  assert.ok(directives);
  assert.equal(directives.name, 'keyword.control.preprocessor.shaft');
  assert.match(directives.match, /!?config/);
  assert.ok(conditions);
  assert.equal(conditions.captures['1'].name, 'keyword.control.preprocessor.shaft');
  assert.ok(error);
  assert.match(error.name, /invalid/);
  assert.ok(unsafe);
  assert.equal(unsafe.name, 'keyword.control.preprocessor.shaft');
});

test('grammar scopes naked functions and raw assembly bodies', () => {
  const grammar = require('../syntaxes/shaft.tmLanguage.json');
  assert.match(grammar.repository.keywords.match, /naked/);
  assert.ok(grammar.repository.assembly);
  assert.match(grammar.repository.assembly.begin, /@asm/);
  assert.match(grammar.repository.assembly.end, /@end/);
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

test('parses colored compiler diagnostics from terminal output', () => {
  const parsed = language.parseCompilerDiagnostics('\u001b[1;31merror: \u001b[0mUnknown token \'@\'.\n\u001b[1;34m--> \u001b[0m/tmp/main.shaft:3:12\n');
  assert.equal(parsed.length, 1);
  assert.equal(parsed[0].range.start.line, 2);
  assert.equal(parsed[0].range.start.character, 11);
  assert.match(parsed[0].message, /Unknown token/);
});

test('preserves compiler help in source-aware LSP diagnostics', () => {
  const parsed = language.parseCompilerDiagnostics(
    "error: Cannot assign type 'u16' to 'u8'.\n--> /tmp/project/main.shaft:3:5\n3 | u8 value = wide;\n  |    ^\nhelp: Consider an explicit `as u8` conversion.\n",
  );
  assert.equal(parsed.length, 1);
  assert.equal(parsed[0].range.start.line, 2);
  assert.equal(parsed[0].range.start.character, 4);
  assert.match(parsed[0].message, /Cannot assign type 'u16' to 'u8'/);
  assert.match(parsed[0].message, /help: Consider an explicit `as u8` conversion/);
});
