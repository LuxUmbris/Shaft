'use strict';

const childProcess = require('node:child_process');
const path = require('node:path');
const vscode = require('vscode');
const extensionSettings = require('./server/lib/extension-settings.cjs');

let server;
let requestId = 0;
const pending = new Map();
let readBuffer = Buffer.alloc(0);

function send(message) {
  if (!server?.stdin?.writable) return;
  const body = Buffer.from(JSON.stringify(message), 'utf8');
  server.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  server.stdin.write(body);
}
function request(method, params) {
  const id = ++requestId;
  send({ jsonrpc: '2.0', id, method, params });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`Shaft LSP timed out: ${method}`)); }, 10000);
    pending.set(id, { resolve, reject, timer });
  });
}
function notify(method, params) { send({ jsonrpc: '2.0', method, params }); }
function asUri(uri) { return vscode.Uri.parse(uri); }
function asPosition(position) { return new vscode.Position(position.line, position.character); }
function asRange(range) { return new vscode.Range(asPosition(range.start), asPosition(range.end)); }
function diagnosticsFor(items) {
  return items.map((item) => {
    const severity = item.severity === 2 ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Error;
    const diagnostic = new vscode.Diagnostic(asRange(item.range), item.message, severity);
    diagnostic.source = item.source || 'shaft';
    return diagnostic;
  });
}
function handle(message) {
  if (Object.prototype.hasOwnProperty.call(message, 'id') && (message.result !== undefined || message.error)) {
    const pendingRequest = pending.get(message.id);
    if (!pendingRequest) return;
    clearTimeout(pendingRequest.timer);
    pending.delete(message.id);
    if (message.error) pendingRequest.reject(new Error(message.error.message));
    else pendingRequest.resolve(message.result);
    return;
  }
  if (message.method === 'textDocument/publishDiagnostics') {
    diagnosticCollection.set(asUri(message.params.uri), diagnosticsFor(message.params.diagnostics));
  } else if (message.method === 'window/logMessage' && message.params.type <= 2) {
    output.appendLine(message.params.message);
  }
}
function processMessages() {
  while (true) {
    const separator = readBuffer.indexOf('\r\n\r\n');
    if (separator < 0) return;
    const header = readBuffer.slice(0, separator).toString('ascii');
    const length = Number(header.match(/Content-Length:\s*(\d+)/i)?.[1]);
    if (!Number.isFinite(length)) { readBuffer = readBuffer.slice(separator + 4); continue; }
    const bodyStart = separator + 4;
    if (readBuffer.length < bodyStart + length) return;
    const body = readBuffer.slice(bodyStart, bodyStart + length).toString('utf8');
    readBuffer = readBuffer.slice(bodyStart + length);
    try { handle(JSON.parse(body)); } catch (error) { output.appendLine(`Invalid server message: ${error.message}`); }
  }
}
function config() {
  const values = vscode.workspace.getConfiguration('shaft.languageServer');
  const formatting = vscode.workspace.getConfiguration('shaft.format');
  return {
    compilerPath: extensionSettings.explicitValue(values.inspect('compilerPath')), diagnostics: values.get('diagnostics'),
    stdlibPath: values.get('stdlibPath'), resourcePath: values.get('resourcePath'), maxProblems: values.get('maxProblems'),
    insertFinalNewline: formatting.get('insertFinalNewline'),
  };
}
async function startServer(context) {
  if (server) return;
  const executable = process.execPath;
  const script = context.asAbsolutePath('server/shaft-lsp.cjs');
  server = childProcess.spawn(executable, [script], { stdio: ['pipe', 'pipe', 'pipe'] });
  server.stdout.on('data', (chunk) => { readBuffer = Buffer.concat([readBuffer, chunk]); processMessages(); });
  server.stderr.on('data', (chunk) => output.append(chunk.toString()));
  server.on('exit', (code) => { output.appendLine(`Shaft language server stopped (${code ?? 'signal'}).`); server = undefined; });
  const workspaceFolders = vscode.workspace.workspaceFolders || [];
  await request('initialize', {
    processId: process.pid, rootUri: workspaceFolders[0]?.uri.toString() || null,
    workspaceFolders: workspaceFolders.map((folder) => ({ uri: folder.uri.toString(), name: folder.name })),
    capabilities: {}, initializationOptions: config(),
  });
  notify('initialized', {});
}
async function stopServer() {
  if (!server) return;
  try { await request('shutdown', {}); notify('exit', {}); } catch { server.kill(); }
  server = undefined;
}
function textDocument(document) { return { uri: document.uri.toString(), languageId: document.languageId, version: document.version, text: document.getText() }; }
function isShaft(document) { return document.languageId === 'shaft'; }

const output = vscode.window.createOutputChannel('Shaft Language Server');
const diagnosticCollection = vscode.languages.createDiagnosticCollection('shaft');

function activate(context) {
  context.subscriptions.push(output, diagnosticCollection);
  startServer(context).catch((error) => vscode.window.showErrorMessage(`Could not start Shaft language server: ${error.message}`));
  context.subscriptions.push(vscode.workspace.onDidOpenTextDocument((document) => { if (isShaft(document)) notify('textDocument/didOpen', { textDocument: textDocument(document) }); }));
  context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((event) => {
    if (!isShaft(event.document)) return;
    notify('textDocument/didChange', { textDocument: { uri: event.document.uri.toString(), version: event.document.version }, contentChanges: event.contentChanges.map((change) => ({ range: { start: { line: change.range.start.line, character: change.range.start.character }, end: { line: change.range.end.line, character: change.range.end.character } }, text: change.text })) });
  }));
  context.subscriptions.push(vscode.workspace.onDidSaveTextDocument((document) => { if (isShaft(document)) notify('textDocument/didSave', { textDocument: { uri: document.uri.toString() } }); }));
  context.subscriptions.push(vscode.workspace.onDidCloseTextDocument((document) => { if (isShaft(document)) notify('textDocument/didClose', { textDocument: { uri: document.uri.toString() } }); }));
  for (const document of vscode.workspace.textDocuments) if (isShaft(document)) notify('textDocument/didOpen', { textDocument: textDocument(document) });

  context.subscriptions.push(vscode.languages.registerCompletionItemProvider({ language: 'shaft' }, {
    provideCompletionItems(document, position) { return request('textDocument/completion', { textDocument: { uri: document.uri.toString() }, position }).then((result) => result.items.map((item) => Object.assign(new vscode.CompletionItem(item.label, item.kind || vscode.CompletionItemKind.Text), { detail: item.detail }))); },
  }, '.', ':'));
  context.subscriptions.push(vscode.languages.registerDefinitionProvider({ language: 'shaft' }, {
    provideDefinition(document, position) { return request('textDocument/definition', { textDocument: { uri: document.uri.toString() }, position }).then((locations) => locations?.map((location) => new vscode.Location(asUri(location.uri), asRange(location.range))) || []); },
  }));
  context.subscriptions.push(vscode.languages.registerHoverProvider({ language: 'shaft' }, {
    provideHover(document, position) { return request('textDocument/hover', { textDocument: { uri: document.uri.toString() }, position }).then((result) => result ? new vscode.Hover(new vscode.MarkdownString(result.contents.value), result.range && asRange(result.range)) : undefined); },
  }));
  context.subscriptions.push(vscode.languages.registerDocumentFormattingEditProvider({ language: 'shaft' }, {
    provideDocumentFormattingEdits(document, options) { return request('textDocument/formatting', { textDocument: { uri: document.uri.toString() }, options }).then((edits) => edits.map((edit) => new vscode.TextEdit(asRange(edit.range), edit.newText))); },
  }));
  context.subscriptions.push(vscode.languages.registerDocumentSymbolProvider({ language: 'shaft' }, {
    provideDocumentSymbols(document) { return request('textDocument/documentSymbol', { textDocument: { uri: document.uri.toString() } }).then((symbols) => symbols.map((symbol) => new vscode.DocumentSymbol(symbol.name, symbol.detail, symbol.kind, asRange(symbol.range), asRange(symbol.selectionRange)))); },
  }));
  context.subscriptions.push(vscode.languages.registerWorkspaceSymbolProvider({ provideWorkspaceSymbols(query) { return request('workspace/symbol', { query }).then((symbols) => symbols.map((symbol) => new vscode.SymbolInformation(symbol.name, symbol.kind, '', new vscode.Location(asUri(symbol.location.uri), asRange(symbol.location.range))))); } }));
  context.subscriptions.push(vscode.languages.registerFoldingRangeProvider({ language: 'shaft' }, { provideFoldingRanges(document) { return request('textDocument/foldingRange', { textDocument: { uri: document.uri.toString() } }).then((ranges) => ranges.map((range) => new vscode.FoldingRange(range.startLine, range.endLine, vscode.FoldingRangeKind.Region))); } }));
  const semanticLegend = new vscode.SemanticTokensLegend(['keyword', 'type', 'function', 'number', 'string', 'comment', 'operator'], []);
  context.subscriptions.push(vscode.languages.registerDocumentSemanticTokensProvider({ language: 'shaft' }, {
    provideDocumentSemanticTokens(document) {
      return request('textDocument/semanticTokens/full', { textDocument: { uri: document.uri.toString() } }).then((result) => new vscode.SemanticTokens(new Uint32Array(result.data)));
    },
  }, semanticLegend));

  context.subscriptions.push(vscode.commands.registerCommand('shaft.restartLanguageServer', async () => { await stopServer(); await startServer(context); for (const document of vscode.workspace.textDocuments) if (isShaft(document)) notify('textDocument/didOpen', { textDocument: textDocument(document) }); vscode.window.showInformationMessage('Shaft language server restarted.'); }));
  context.subscriptions.push(vscode.commands.registerCommand('shaft.checkFile', (document = vscode.window.activeTextEditor?.document) => { if (!document || !isShaft(document)) return vscode.window.showWarningMessage('Open a Shaft file first.'); notify('textDocument/didSave', { textDocument: { uri: document.uri.toString() }, text: document.getText() }); }));
  context.subscriptions.push(vscode.commands.registerCommand('shaft.openSyntaxSpecification', () => vscode.commands.executeCommand('vscode.open', vscode.Uri.file(path.join(vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || '', 'syntax.md')))));
  context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => { if (event.affectsConfiguration('shaft.languageServer')) notify('workspace/didChangeConfiguration', { settings: { shaft: { languageServer: config() } } }); }));
}
function deactivate() { return stopServer(); }
module.exports = { activate, deactivate };
