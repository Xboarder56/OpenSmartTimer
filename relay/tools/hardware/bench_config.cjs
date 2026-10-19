// Bench settings from the environment (see bench_config.py for the list).
'use strict';
const path = require('node:path');
const fs = require('node:fs');

function need(name) {
  const value = (process.env[name] || '').trim().replace(/\/$/, '');
  if (!value) { console.error(`Set ${name} (see relay/tools/hardware/README.md).`); process.exit(2); }
  return value;
}

const playwright = require(require.resolve('playwright', {
  paths: [path.resolve(__dirname, '../../tests')],
}));

const OUT = process.env.OST_BENCH_OUT || path.join(__dirname, 'out');
fs.mkdirSync(path.join(OUT, 'shots'), { recursive: true });

function adminToken() {
  if (process.env.OST_ADMIN_TOKEN) return process.env.OST_ADMIN_TOKEN;
  const env = fs.readFileSync(need('OST_RELAY_ENV'), 'utf8');
  const line = env.split('\n').find((l) => l.startsWith('OSTRELAY_ADMIN_TOKEN='));
  return line ? line.slice('OSTRELAY_ADMIN_TOKEN='.length).trim() : '';
}

module.exports = {
  playwright, OUT,
  get T() { return need('OST_TIMER'); },
  get ADMIN() { return (process.env.OST_RELAY_ADMIN || 'http://127.0.0.1:8080').replace(/\/$/, ''); },
  adminToken,
  shot: (name) => path.join(OUT, 'shots', name),
};
