const test = require('node:test');
const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function launch(options = {}) {
  const server = childProcess.spawn(process.execPath, [path.join(__dirname, '..', 'server', 'shaft-lsp.cjs')], { stdio: ['pipe', 'pipe', 'pipe'], env: { ...process.env, ...(options.env || {}) } });
  let buffer = Buffer.alloc(0);
  let nextId = 0;
  const pending = new Map();
  const notifications = [];
  const waiters = [];
  function publishNotification(message) {
    notifications.push(message);
    for (let index = waiters.length - 1; index >= 0; index -= 1) {
      if (waiters[index].predicate(message)) {
        const waiter = waiters.splice(index, 1)[0];
        clearTimeout(waiter.timer);
        waiter.resolve(message);
      }
    }
  }
  server.stdout.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    while (true) {
      const separator = buffer.indexOf('\r\n\r\n');
      if (separator < 0) return;
      const length = Number(buffer.slice(0, separator).toString('ascii').match(/Content-Length:\s*(\d+)/i)?.[1]);
      const bodyStart = separator + 4;
      if (!Number.isFinite(length) || buffer.length < bodyStart + length) return;
      const message = JSON.parse(buffer.slice(bodyStart, bodyStart + length).toString('utf8'));
      buffer = buffer.slice(bodyStart + length);
      if (message.id !== undefined && pending.has(message.id)) {
        const { resolve, reject } = pending.get(message.id);
        pending.delete(message.id);
        message.error ? reject(new Error(message.error.message)) : resolve(message.result);
      } else if (message.method) publishNotification(message);
    }
  });
  const send = (message) => {
    const body = Buffer.from(JSON.stringify(message));
    server.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    server.stdin.write(body);
  };
  return {
    notify(method, params) { send({ jsonrpc: '2.0', method, params }); },
    request(method, params) {
      const id = ++nextId;
      send({ jsonrpc: '2.0', id, method, params });
      return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
    },
    nextNotification(predicate, timeout = 10000) {
      const existing = notifications.find(predicate);
      if (existing) return Promise.resolve(existing);
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('timed out waiting for LSP notification')), timeout);
        waiters.push({ predicate, resolve, reject, timer });
      });
    },
    async close() { try { await this.request('shutdown', {}); this.notify('exit', {}); } catch { server.kill(); } },
  };
}

test('the stdio server exposes LSP analysis, completion, definition, and formatting', async (t) => {
  const client = launch();
  t.after(() => client.close());
  const initialized = await client.request('initialize', { processId: process.pid, rootUri: null, capabilities: {}, initializationOptions: { diagnostics: 'off' } });
  assert.equal(initialized.serverInfo.name, 'shaft-lsp');
  assert.equal(initialized.capabilities.definitionProvider, true);
  client.notify('initialized', {});

  const uri = 'file:///workspace/example.shaft';
  client.notify('textDocument/didOpen', { textDocument: { uri, version: 1, languageId: 'shaft', text: 'namespace Core\n{\n/// Returns the canonical answer.\ndef answer() -> u64 result\n{\ntunnel 42 -> u64 result;\n}\n}\nCore::answer();\n' } });
  const definition = await client.request('textDocument/definition', { textDocument: { uri }, position: { line: 8, character: 7 } });
  assert.equal(definition[0].range.start.line, 3);
  const hover = await client.request('textDocument/hover', { textDocument: { uri }, position: { line: 8, character: 7 } });
  assert.match(hover.contents.value, /def Core::answer\(\) -> u64 result/);
  assert.match(hover.contents.value, /Returns the canonical answer/);
  const completion = await client.request('textDocument/completion', { textDocument: { uri }, position: { line: 8, character: 0 } });
  assert.ok(completion.items.some((item) => item.label === 'Core::answer'));
  const formatting = await client.request('textDocument/formatting', { textDocument: { uri }, options: { insertSpaces: true, tabSize: 2 } });
  assert.match(formatting[0].newText, /^namespace Core\n\{/);
  assert.match(formatting[0].newText, /  def answer/);
  const tokens = await client.request('textDocument/semanticTokens/full', { textDocument: { uri } });
  assert.ok(tokens.data.length > 0);
});

test('the server marks lexer errors such as invalid tokens immediately', async (t) => {
  const client = launch();
  t.after(() => client.close());
  const repository = path.resolve(__dirname, '..', '..', '..');
  await client.request('initialize', {
    processId: process.pid, rootUri: `file://${repository}`, capabilities: {},
    initializationOptions: { compilerPath: path.join(repository, 'build', 'shaftc'), diagnostics: 'onChange' },
  });
  client.notify('initialized', {});
  const uri = 'file:///workspace/invalid-token.shaft';
  const text = 'cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return @;\n}\n';
  const published = client.nextNotification((message) => message.method === 'textDocument/publishDiagnostics' && message.params.uri === uri && message.params.diagnostics.some((diagnostic) => /Unknown token '@'/.test(diagnostic.message)));
  client.notify('textDocument/didOpen', { textDocument: { uri, version: 1, languageId: 'shaft', text } });
  const message = await published;
  const diagnostic = message.params.diagnostics.find((item) => /Unknown token '@'/.test(item.message));
  assert.equal(diagnostic.severity, 1);
  assert.equal(diagnostic.source, 'shaftc');
  assert.equal(diagnostic.range.start.line, 2);
  assert.equal(diagnostic.range.start.character, 11);
});

test('the server marks parser errors such as missing semicolons immediately', async (t) => {
  const client = launch();
  t.after(() => client.close());
  const repository = path.resolve(__dirname, '..', '..', '..');
  await client.request('initialize', {
    processId: process.pid, rootUri: `file://${repository}`, capabilities: {},
    initializationOptions: { compilerPath: path.join(repository, 'build', 'shaftc'), diagnostics: 'onChange' },
  });
  client.notify('initialized', {});
  const uri = 'file:///workspace/missing-semicolon.shaft';
  const text = 'cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 0\n}\n';
  const published = client.nextNotification((message) => message.method === 'textDocument/publishDiagnostics' && message.params.uri === uri && message.params.diagnostics.some((diagnostic) => /Expected ';' after return statement/.test(diagnostic.message)));
  client.notify('textDocument/didOpen', { textDocument: { uri, version: 1, languageId: 'shaft', text } });
  const message = await published;
  const diagnostic = message.params.diagnostics.find((item) => /Expected ';' after return statement/.test(item.message));
  assert.equal(diagnostic.severity, 1);
  assert.equal(diagnostic.source, 'shaftc');
  assert.equal(diagnostic.range.start.line, 3);
});

test('the server marks checker errors such as undeclared identifiers immediately', async (t) => {
  const client = launch();
  t.after(() => client.close());
  const repository = path.resolve(__dirname, '..', '..', '..');
  await client.request('initialize', {
    processId: process.pid, rootUri: `file://${repository}`, capabilities: {},
    initializationOptions: { compilerPath: path.join(repository, 'build', 'shaftc'), diagnostics: 'onChange' },
  });
  client.notify('initialized', {});
  const uri = 'file:///workspace/undeclared-identifier.shaft';
  const text = 'cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return unknownValue;\n}\n';
  const published = client.nextNotification((message) => message.method === 'textDocument/publishDiagnostics' && message.params.uri === uri && message.params.diagnostics.some((diagnostic) => /Use of undeclared identifier/.test(diagnostic.message)));
  client.notify('textDocument/didOpen', { textDocument: { uri, version: 1, languageId: 'shaft', text } });
  const message = await published;
  const diagnostic = message.params.diagnostics.find((item) => /Use of undeclared identifier/.test(item.message));
  assert.equal(diagnostic.severity, 1);
  assert.equal(diagnostic.source, 'shaftc');
  assert.equal(diagnostic.range.start.line, 2);
  assert.equal(diagnostic.range.start.character, 11);
});

test('the server marks compiler errors immediately when diagnostics are onChange', async (t) => {
  const client = launch();
  t.after(() => client.close());
  const repository = path.resolve(__dirname, '..', '..', '..');
  await client.request('initialize', {
    processId: process.pid, rootUri: `file://${repository}`, capabilities: {},
    initializationOptions: { compilerPath: path.join(repository, 'build', 'shaftc'), diagnostics: 'onChange' },
  });
  client.notify('initialized', {});
  const uri = 'file:///workspace/on-change-error.shaft';
  client.notify('textDocument/didOpen', { textDocument: { uri, version: 1, languageId: 'shaft', text: 'cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 0;\n}\n' } });
  const published = client.nextNotification((message) => message.method === 'textDocument/publishDiagnostics' && message.params.uri === uri && message.params.diagnostics.some((diagnostic) => /Unknown function 'missing'/.test(diagnostic.message)));
  client.notify('textDocument/didChange', { textDocument: { uri, version: 2 }, contentChanges: [{ text: 'cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return missing(42);\n}\n' }] });
  const message = await published;
  const diagnostic = message.params.diagnostics.find((item) => /Unknown function 'missing'/.test(item.message));
  assert.equal(diagnostic.severity, 1);
  assert.equal(diagnostic.range.start.line, 2);
});

test('the server discovers an installer-registered compiler outside PATH', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'shaft-lsp-registration-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const repository = path.resolve(__dirname, '..', '..', '..');
  const prefix = path.join(directory, 'prefix');
  const compiler = path.join(prefix, 'bin', 'shaftc');
  const standardLibrary = path.join(prefix, 'share', 'shaft', 'std', 'std.shaft');
  fs.mkdirSync(path.dirname(compiler), { recursive: true });
  fs.mkdirSync(path.dirname(standardLibrary), { recursive: true });
  fs.copyFileSync(path.join(repository, 'build', 'shaftc'), compiler);
  fs.copyFileSync(path.join(repository, 'std', 'std.shaft'), standardLibrary);
  for (const runtime of ['linux.c', 'darwin.c', 'windows.c']) {
    const target = path.join(prefix, 'share', 'shaft', 'std', 'runtime', runtime);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.copyFileSync(path.join(repository, 'std', 'runtime', runtime), target);
  }
  const configHome = path.join(directory, 'config');
  const registration = path.join(configHome, 'shaft', 'compiler.json');
  fs.mkdirSync(path.dirname(registration), { recursive: true });
  fs.writeFileSync(registration, JSON.stringify({ compilerPath: compiler, stdlibPath: standardLibrary, resourcePath: path.join(prefix, 'share', 'shaft') }));

  const client = launch({ env: { XDG_CONFIG_HOME: configHome, PATH: '' } });
  t.after(() => client.close());
  const initialized = await client.request('initialize', { processId: process.pid, rootUri: null, capabilities: {}, initializationOptions: { compilerPath: '', diagnostics: 'onSave' } });
  assert.equal(initialized.serverInfo.name, 'shaft-lsp');
  client.notify('initialized', {});
  const uri = 'file:///workspace/registered-compiler.shaft';
  client.notify('textDocument/didOpen', { textDocument: { uri, version: 1, languageId: 'shaft', text: 'cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return missing(42);\n}\n' } });
  const published = client.nextNotification((message) => message.method === 'textDocument/publishDiagnostics' && message.params.uri === uri && message.params.diagnostics.some((diagnostic) => /Unknown function/.test(diagnostic.message)));
  client.notify('textDocument/didSave', { textDocument: { uri } });
  const message = await published;
  assert.equal(message.params.diagnostics.find((item) => /Unknown function/.test(item.message)).source, 'shaftc');
});

test('the server publishes live shaftc diagnostics with source-line correction', async (t) => {
  const client = launch();
  t.after(() => client.close());
  const repository = path.resolve(__dirname, '..', '..', '..');
  await client.request('initialize', {
    processId: process.pid, rootUri: `file://${repository}`, capabilities: {},
    initializationOptions: { compilerPath: path.join(repository, 'build', 'shaftc'), diagnostics: 'onSave' },
  });
  client.notify('initialized', {});
  const uri = 'file:///workspace/unknown-call.shaft';
  client.notify('textDocument/didOpen', { textDocument: { uri, version: 1, languageId: 'shaft', text: 'cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return missing(42);\n}\n' } });
  const published = client.nextNotification((message) => message.method === 'textDocument/publishDiagnostics' && message.params.uri === uri && message.params.diagnostics.some((diagnostic) => /Unknown function/.test(diagnostic.message)));
  client.notify('textDocument/didSave', { textDocument: { uri } });
  const message = await published;
  const diagnostic = message.params.diagnostics.find((item) => /Unknown function/.test(item.message));
  assert.equal(diagnostic.range.start.line, 2);
  assert.equal(diagnostic.source, 'shaftc');
});
