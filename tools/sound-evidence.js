// Compares PSteroid5's sound effects (ps5/sound.hpp, written by test-sound) with the browser's,
// rendered by CSSteroids-2's own public/js/core/sound.js, loaded read-only.
// usage: node tools/sound-evidence.js <ps5-samples.json>
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const reference = path.join(__dirname, '..', '..', 'CSSTeroids-2', 'public', 'js', 'core', 'sound.js');
const window = { addEventListener() {} };
vm.runInNewContext(fs.readFileSync(reference, 'utf8'), { window, localStorage: { getItem: () => null, setItem() {} }, Math, Float32Array, console });
const ps5 = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));

let worst = 0, failed = 0;
const report = {};
for (const [name, samples] of Object.entries(ps5)) {
    const browser = window.GameSound.render(name);
    let maxDiff = browser.length === samples.length ? 0 : Infinity;
    for (let i = 0; i < Math.min(browser.length, samples.length); i++) maxDiff = Math.max(maxDiff, Math.abs(browser[i] - samples[i]));
    report[name] = { samples: samples.length, browserSamples: browser.length, maxDiff };
    worst = Math.max(worst, maxDiff);
    // 1e-4 of full scale is below a 16-bit step (3e-5 of 0.8 peak is ~1 LSB): inaudible
    if (!(maxDiff < 1e-4)) failed++;
}
console.log(JSON.stringify({ reference: 'CSSteroids-2 public/js/core/sound.js', effects: Object.keys(report).length, worstDifference: worst, report }, null, 1));
if (failed) { console.error(`${failed} effect(s) differ from the browser`); process.exit(1); }
