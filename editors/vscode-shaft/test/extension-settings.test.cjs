const test = require('node:test');
const assert = require('node:assert/strict');
const settings = require('../server/lib/extension-settings.cjs');

test('uses only explicitly configured values rather than extension defaults', () => {
  assert.equal(settings.explicitValue({ defaultValue: 'shaftc' }), '');
  assert.equal(settings.explicitValue({ defaultValue: '', globalValue: '/opt/shaft/bin/shaftc' }), '/opt/shaft/bin/shaftc');
});

test('prefers folder settings over workspace and global settings', () => {
  assert.equal(settings.explicitValue({ globalValue: '/global/shaftc', workspaceValue: '/workspace/shaftc', workspaceFolderValue: '/folder/shaftc' }), '/folder/shaftc');
  assert.equal(settings.explicitValue({ globalValue: '/global/shaftc', workspaceValue: '/workspace/shaftc' }), '/workspace/shaftc');
});
