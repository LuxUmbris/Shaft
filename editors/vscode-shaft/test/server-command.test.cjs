const test = require('node:test');
const assert = require('node:assert/strict');
const command = require('../server/lib/server-command.cjs');

test('uses the installed shaftls command by default', () => {
  assert.deepEqual(command.resolveShaftlsCommand(''), { executable: 'shaftls', arguments: [] });
});

test('uses an explicitly configured shaftls binary path', () => {
  assert.deepEqual(command.resolveShaftlsCommand('/opt/shaft/bin/shaftls'), { executable: '/opt/shaft/bin/shaftls', arguments: [] });
});
