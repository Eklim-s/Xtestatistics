const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

process.env.TZ = 'America/New_York';
const root = path.resolve(__dirname, '../..');
const html = fs.readFileSync(path.join(root, 'sd-plugins/reading-stats-viewer/index.html'), 'utf8');
const code = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const nodes = new Map();
const node = id => {
  if (!nodes.has(id)) nodes.set(id, { style: {}, textContent: '', innerHTML: '' });
  return nodes.get(id);
};
const document = {
  getElementById: node,
  querySelector: () => ({ previousElementSibling: { querySelector: () => node('dow') } }),
};
const fixedNow = new Date(2026, 9, 2, 12).getTime();
class TestDate extends Date {
  constructor(...args) { super(...(args.length ? args : [fixedNow])); }
  static now() { return fixedNow; }
}
const sandbox = { document, Date: TestDate, fetch: () => Promise.reject(new Error('Offline test')) };
vm.createContext(sandbox);
vm.runInContext(code, sandbox);

const midnight = new Date(2026, 9, 2).getTime() / 1000;
const seconds = Object.create(null);
sandbox.addSession(seconds, midnight - 1, midnight + 99, 1);
assert.deepEqual(Object.keys(seconds), ['2026-10-02']);
assert.equal(seconds['2026-10-02'], 1);

const aggregate = { seconds: 60000, days: 100, record: 90, current: 20, last: 20260308 };
const springToday = new Date(2026, 2, 10);
assert.equal(sandbox.aggregateStreak(aggregate, springToday, 2), 0, 'DST must not retain an expired streak');
assert.equal(sandbox.aggregateStreak({ ...aggregate, last: 20260311 }, springToday, 2), 0);
assert.equal(sandbox.aggregateStreak({ ...aggregate, last: 20260309 }, springToday, 2), 20);
assert.equal(sandbox.aggregateStreak({ ...aggregate, today: 20260309, yesterday: 20260308 }, springToday, 2), 20,
  'Device streak follows device calendar, not the browser timezone');
for (const invalid of [null, [], { ...aggregate, seconds: null }, { ...aggregate, days: -1 },
  { ...aggregate, current: 91 }, { ...aggregate, seconds: '60000' }]) {
  assert.equal(Object.keys(sandbox.validAggregate(invalid)).length, 0);
}

sandbox.renderStats([{ s: midnight, e: midnight + 60, d: 60, b: 123, doc: '__proto__' }], aggregate);
assert.match(node('stats-row').innerHTML, /<div class="num">10<\/div><div class="lbl">Avg\/day/);
assert.match(node('stats-row').innerHTML, /<div class="num">100<\/div><div class="lbl">Days read/);
assert.doesNotMatch(node('bar-chart').innerHTML, /height:2px/);
assert.match(node('bar-chart').innerHTML, /height:90px.*title="1 min"/,
  'A session in the current week must appear in the seven-day chart');
assert.match(node('bar-chart').innerHTML, /class="bar-label">2\/F/);
assert.match(node('stats-row').innerHTML, /<div class="num">1000<\/div><div class="lbl">Total/,
  'Lifetime totals must come from the summary even when detailed sessions were compacted');
assert.equal(node('record-num').textContent, 90);
const halfOpen = Object.create(null);
sandbox.addSession(halfOpen, midnight - 600, midnight, 600);
assert.deepEqual(Object.keys(halfOpen), ['2026-10-01'],
  'An endpoint at midnight must not add an unread day');
assert.match(node('book-list').innerHTML, /Unknown/);
assert.equal((node('bar-chart').innerHTML.match(/class="bar-col"/g) || []).length, 7);

async function checkSummaryCard() {
  let callback;
  const container = { style: {}, children: [], appendChild(element) { this.children.push(element); } };
  const context = {
    document: { createElement: () => ({ style: {}, textContent: '' }) },
    CrossPoint: { registerPlugin(fn) { callback = fn; } },
    fetch: async url => ({ ok: true, text: async () => '', json: async () => ({
      seconds: 60000, days: 100, record: 90, current: 20,
      last: Number(sandbox.dayKey(new Date()).replace(/-/g, '')),
    }) }),
  };
  vm.runInNewContext(fs.readFileSync(path.join(root, 'sd-plugins/reading-stats-viewer/plugin.js'), 'utf8'), context);
  callback(container, {});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(container.children[1].textContent, 'Device streak: 20 days');
  assert.equal(container.children[3].textContent, 'Total: 1000 min (100 days)');
}
checkSummaryCard().then(() => console.log('Reading stats viewer regression checks passed')).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
