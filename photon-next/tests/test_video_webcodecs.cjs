// Exercise the actual EM_JS decoder glue with WebCodecs' key-after-flush rule.
// Run: node tests/test_video_webcodecs.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../photon/ui/src/video_decoder.cpp'), 'utf8');
function glue(name, args) {
    const start = source.indexOf('EM_JS(void, ' + name + ',');
    assert.ok(start >= 0, name + ' missing');
    const body = source.indexOf('{', start);
    const end = source.indexOf('\n});', body);
    assert.ok(end >= 0, name + ' unclosed');
    return '(' + args + ') => ' + source.slice(body, end + 2);
}
const context = vm.createContext({
    Module: {},
    EncodedVideoChunk: class { constructor(init) { Object.assign(this, init); } },
    performance: { now: () => 1000 },
});
const init = vm.runInContext(glue('photon_video_init', ''), context);
const endAu = vm.runInContext(glue('photon_video_end_au', 'force'), context);
function setup() {
    init();
    const state = context.Module._photonVideo;
    let keyRequired = true;
    const decoded = [];
    let flushes = 0;
    state.configured = true;
    state.error = '';
    state.needKey = false;
    state.pending = [];
    state.decoder = {
        state: 'configured',
        decode(chunk) {
            if (keyRequired && chunk.type !== 'key') {
                throw Error('A key frame is required after configure() or flush()');
            }
            keyRequired = false;
            decoded.push(chunk.type);
        },
        flush() { keyRequired = true; ++flushes; return Promise.resolve(); },
    };
    function feed(key, timestamp) {
        state.auParts = [new Uint8Array([0, 0, 1, key ? 0x65 : 0x41, 0x80])];
        state.auKey = key;
        state.auTs = timestamp;
        state.lastVclAt = 0;
    }
    return { state, decoded, feed, flushes: () => flushes };
}
// First keyframe, then a pause longer than the old 80 ms timeout, then
// dependent pictures: a normal live poll must never invalidate references.
let run = setup();
run.feed(true, 1000);
endAu(0);
endAu(0);
run.feed(false, 1033);
endAu(0);
run.feed(false, 1066);
endAu(0);
assert.equal(run.flushes(), 0);
assert.equal(run.state.error, '');
assert.deepEqual(run.decoded, ['key', 'delta', 'delta']);
// Explicit still requests retain the ability to drain a lone keyframe.
run = setup();
run.feed(true, 2000);
endAu(1);
assert.equal(run.flushes(), 1);
assert.deepEqual(run.decoded, ['key']);
assert.equal(run.state.needKey, true);
// Catalog Play used to flush the IDR then feed P-frames, which Chrome rejects.
run.feed(false, 2033);
endAu(0);
assert.equal(run.state.error, '');
assert.deepEqual(run.decoded, ['key']);
run.feed(true, 2066);
endAu(0);
run.feed(false, 2099);
endAu(0);
assert.equal(run.state.error, '');
assert.deepEqual(run.decoded, ['key', 'key', 'delta']);
// Leftover delta AU after configure()/flush() must be dropped, not decoded.
run = setup();
run.state.needKey = true;
run.feed(false, 3000);
endAu(0);
assert.equal(run.state.error, '');
assert.deepEqual(run.decoded, []);
run.feed(true, 3033);
endAu(0);
assert.deepEqual(run.decoded, ['key']);
console.log('WebCodecs live pause and still-image regression tests passed');
