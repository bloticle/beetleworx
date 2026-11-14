import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

let now = 0;
const source = fs.readFileSync(new URL('../services/relay/worker.mjs', import.meta.url), 'utf8');
const body = source.slice(source.indexOf('export class Admission'), source.indexOf('export class RelayRoom')).replace('export class', 'class');
const context = vm.createContext({
  DurableObject: class {}, Response, Date: { now: () => now }, Math,
  reply: (value, status = 200) => Response.json(value, { status }),
});
vm.runInContext(body + ';globalThis.Admission=Admission', context);
let saved = { creates: 6, connects: 120, hour: 0, minute: 0 };
const service = new context.Admission();
service.ctx = {
  blockConcurrencyWhile: fn => fn(),
  storage: {
    get: async () => saved,
    put: async (_, value) => { saved = structuredClone(value); },
    setAlarm: async () => {},
  },
};
const attempt = kind => service.fetch({ text: async () => kind });
for (let i = 0; i < 12; ++i) assert.equal((await attempt('create')).status, 200);
const blocked = await attempt('create');
assert.equal(blocked.status, 429);
assert.equal(blocked.headers.get('Retry-After'), '5');
now = 5000;
assert.equal((await attempt('create')).status, 200);
for (let i = 0; i < 119; ++i) assert.equal((await attempt('connect')).status, 200);
assert.equal((await attempt('connect')).status, 429);
now += 1000;
assert.equal((await attempt('connect')).status, 200);
console.log('Relay admission passed: old lockout migration, bounded bursts, independent budgets, refill and Retry-After.');
