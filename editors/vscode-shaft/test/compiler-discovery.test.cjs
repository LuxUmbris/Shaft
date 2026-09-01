const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const discovery = require('../server/lib/compiler-discovery.cjs');

test('uses the installer registration when no explicit compiler is configured', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'shaft-discovery-'));
  try {
    const compiler = path.join(directory, 'prefix', 'bin', 'shaftc');
    const standardLibrary = path.join(directory, 'prefix', 'share', 'shaft', 'std', 'std.shaft');
    fs.mkdirSync(path.dirname(compiler), { recursive: true });
    fs.mkdirSync(path.dirname(standardLibrary), { recursive: true });
    fs.writeFileSync(compiler, 'compiler');
    fs.writeFileSync(standardLibrary, '// std\n');
    const registration = path.join(directory, 'config', 'shaft', 'compiler.json');
    fs.mkdirSync(path.dirname(registration), { recursive: true });
    fs.writeFileSync(registration, JSON.stringify({ compilerPath: compiler, stdlibPath: standardLibrary, resourcePath: path.dirname(path.dirname(standardLibrary)) }));

    const resolved = discovery.resolveCompilerSettings({ compilerPath: '', stdlibPath: '', resourcePath: '' }, { registrationPath: registration, path: '' });
    assert.equal(resolved.compilerPath, compiler);
    assert.equal(resolved.stdlibPath, standardLibrary);
    assert.equal(resolved.source, 'installer registration');
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test('keeps an explicit VS Code compiler setting ahead of installer registration', () => {
  const resolved = discovery.resolveCompilerSettings(
    { compilerPath: '/custom/shaftc', stdlibPath: '', resourcePath: '' },
    { registrationPath: '/does/not/exist', path: '' },
  );
  assert.equal(resolved.compilerPath, '/custom/shaftc');
  assert.equal(resolved.source, 'VS Code setting');
});

test('computes config paths for supported operating systems', () => {
  assert.equal(discovery.registrationPath({ platform: 'linux', env: { XDG_CONFIG_HOME: '/config' }, home: '/home/test' }), '/config/shaft/compiler.json');
  assert.equal(discovery.registrationPath({ platform: 'darwin', env: {}, home: '/Users/test' }), '/Users/test/Library/Application Support/Shaft/compiler.json');
  assert.equal(discovery.registrationPath({ platform: 'win32', env: { APPDATA: 'C:\\Users\\test\\AppData\\Roaming' }, home: 'C:\\Users\\test' }), 'C:\\Users\\test\\AppData\\Roaming/Shaft/compiler.json');
});
