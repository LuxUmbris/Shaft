'use strict';

const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const manifest = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
const output = path.join(root, `${manifest.name}-${manifest.version}.vsix`);
const files = [
  'package.json', 'README.md', 'extension.cjs', 'language-configuration.json',
  'server/shaft-lsp.cjs', 'server/lib/shaft-language.cjs', 'server/lib/compiler-discovery.cjs', 'server/lib/extension-settings.cjs',
  'syntaxes/shaft.tmLanguage.json', 'snippets/shaft.code-snippets',
];
const staging = fs.mkdtempSync(path.join(os.tmpdir(), 'shaft-vsix-'));
try {
  for (const file of files) {
    const source = path.join(root, file);
    if (!fs.statSync(source).isFile()) throw new Error(`Missing extension payload: ${file}`);
    const target = path.join(staging, 'extension', file);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.copyFileSync(source, target);
  }
  fs.rmSync(output, { force: true });
  const result = childProcess.spawnSync('zip', ['-q', '-r', output, 'extension'], { cwd: staging, encoding: 'utf8' });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(result.stderr || 'zip failed');
  console.log(`Created ${output}`);
} finally {
  fs.rmSync(staging, { recursive: true, force: true });
}
