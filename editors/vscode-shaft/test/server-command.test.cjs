const test = require('node:test');
const assert = require('node:assert/strict');
const command = require('../server/lib/server-command.cjs');

test('uses the bundled Node language server by default', () => {
  assert.deepEqual(command.resolveShaftlsCommand('', '/extension'), {
    executable: 'node', arguments: ['/extension/server/shaft-lsp.cjs'],
  });
});

test('uses an explicitly configured language-server executable', () => {
  assert.deepEqual(command.resolveShaftlsCommand('/opt/shaft/bin/shaftls'), { executable: '/opt/shaft/bin/shaftls', arguments: [] });
});
