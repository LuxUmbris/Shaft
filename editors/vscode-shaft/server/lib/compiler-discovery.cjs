'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function registrationPath(options = {}) {
  const platform = options.platform || process.platform;
  const env = options.env || process.env;
  const home = options.home || os.homedir();
  if (platform === 'win32') return path.join(env.APPDATA || path.join(home, 'AppData', 'Roaming'), 'Shaft', 'compiler.json');
  if (platform === 'darwin') return path.join(home, 'Library', 'Application Support', 'Shaft', 'compiler.json');
  return path.join(env.XDG_CONFIG_HOME || path.join(home, '.config'), 'shaft', 'compiler.json');
}

function readRegistration(file) {
  try {
    const registration = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (!registration.compilerPath || !fs.existsSync(registration.compilerPath)) return null;
    return registration;
  } catch { return null; }
}

function resolveCompilerSettings(settings = {}, options = {}) {
  const configured = settings.compilerPath?.trim();
  if (configured) return { ...settings, compilerPath: configured, source: 'VS Code setting' };

  const registration = readRegistration(options.registrationPath || registrationPath(options));
  if (registration) {
    return {
      ...settings,
      compilerPath: registration.compilerPath,
      stdlibPath: settings.stdlibPath?.trim() || registration.stdlibPath || '',
      resourcePath: settings.resourcePath?.trim() || registration.resourcePath || '',
      source: 'installer registration',
    };
  }
  return { ...settings, compilerPath: 'shaftc', source: 'PATH' };
}

module.exports = { registrationPath, readRegistration, resolveCompilerSettings };
