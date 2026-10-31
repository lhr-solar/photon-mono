// Exercise the actual EM_JS decoder glue with WebCodecs' key-after-flush rule.
// Run: node tests/test_video_webcodecs.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/ui/video_decoder.cpp'), 'utf8');
function glue(name, args) {
    const start = source.indexOf(', ' + name + ',');
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
// These unit fixtures declare a whole AU at once; exercise its boundary
// submission directly. Actual paint polling is tested with delayed slices below.
const completeAu = () => context.Module._photonVideo.flushAU(false);
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
completeAu();
completeAu();
run.feed(false, 1033);
completeAu();
run.feed(false, 1066);
completeAu();
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
completeAu();
assert.equal(run.state.error, '');
assert.deepEqual(run.decoded, ['key']);
run.feed(true, 2066);
completeAu();
run.feed(false, 2099);
completeAu();
assert.equal(run.state.error, '');
assert.deepEqual(run.decoded, ['key', 'key', 'delta']);
// Pause may drain a delta picture too. Continuing requires replaying the
// keyframe, rather than sending a delta into a post-flush decoder.
run.feed(false,2132); endAu(1);
assert.equal(run.flushes(),2);
assert.equal(run.state.needKey,true);
// Leftover delta AU after configure()/flush() must be dropped, not decoded.
run = setup();
run.state.needKey = true;
run.feed(false, 3000);
completeAu();
assert.equal(run.state.error, '');
assert.deepEqual(run.decoded, []);
run.feed(true, 3033);
completeAu();
assert.deepEqual(run.decoded, ['key']);
// A reset can leave callbacks queued from the closed codec. Those callbacks
// must neither close the replacement decoder nor enqueue old pictures.
const reset = vm.runInContext(glue('photon_video_reset', 'preservePresentation'), context);
const feedNal = vm.runInContext(glue('photon_video_feed', 'ptr,len,tsMs,flags'), context);
const decoders = [];
context.HEAPU8 = new Uint8Array(1024);
context.VideoDecoder = class {
    constructor(callbacks) { this.callbacks=callbacks; this.state='unconfigured'; decoders.push(this); }
    configure(config) { this.config=config; this.state='configured'; }
    decode(chunk) { (this.chunks ||= []).push(chunk); }
    close() { this.state='closed'; }
};
function nal(bytes, timestamp) {
    context.HEAPU8.set(bytes, 0); feedNal(0,bytes.length,timestamp,0);
}
function gop(timestamp) {
    nal([0,0,1,0x67,0x42,0xe0,0x1e,0x80],timestamp);
    nal([0,0,1,0x68,0x80],timestamp);
    nal([0,0,1,0x65,0x80],timestamp); completeAu();
}
reset(); gop(4000); const stale=decoders.at(-1);
reset(); gop(5000); const current=decoders.at(-1);
assert.notEqual(current,stale);
let closed=false;
stale.callbacks.output({ close() { closed=true; } });
stale.callbacks.error(new Error('late old decoder error'));
assert.equal(closed,true);
assert.equal(context.Module._photonVideo.pending.length,0);
assert.equal(context.Module._photonVideo.decoder,current);
assert.equal(context.Module._photonVideo.error,'');
current.callbacks.output({timestamp:5000000,close(){}});
assert.equal(context.Module._photonVideo.pending.length,1);
// A failed codec may recover from a resent IDR at the same timestamp.
current.callbacks.error(new Error('decode failed'));
gop(5000);
assert.equal(context.Module._photonVideo.configured,true);
assert.equal(context.Module._photonVideo.error,'');
console.log('WebCodecs pause, still, stale callback, and IDR recovery tests passed');
// Small reference slices must reach the decoder; byte length cannot tell us
// whether a skip picture is valid. Same-profile SPS/PPS changes need a reset.
reset(); gop(6000);
const smallSliceDecoder = decoders.at(-1);
nal([0,0,1,0x41,0x80,0x01],6033); completeAu();
assert.equal(smallSliceDecoder.chunks.at(-1).type, 'delta');
const repeatedDecoder = decoders.at(-1);
nal([0,0,1,0x67,0x42,0xe0,0x1e,0x80],6066);
nal([0,0,1,0x68,0x80],6066);
assert.equal(context.Module._photonVideo.decoder,repeatedDecoder);
let oldPictureClosed=false;
repeatedDecoder.callbacks.output({timestamp:6033000,close(){oldPictureClosed=true;}});
nal([0,0,1,0x67,0x42,0xe0,0x1e,0x81],6066);
assert.equal(repeatedDecoder.state,'closed');
assert.equal(oldPictureClosed,true);
assert.equal(context.Module._photonVideo.pending.length,0);
nal([0,0,1,0x65,0x80],6066); completeAu();
const changedSpsDecoder=decoders.at(-1);
assert.notEqual(changedSpsDecoder,repeatedDecoder);
nal([0,0,1,0x68,0x81],6099);
assert.equal(changedSpsDecoder.state,'closed');
nal([0,0,1,0x65,0x80],6099); completeAu();
const changedPpsDecoder=decoders.at(-1);
context.Module._photonVideo.lastPresentedTs=6099000;
let latePictureClosed=false;
changedPpsDecoder.callbacks.output({timestamp:6066000,close(){latePictureClosed=true;}});
assert.equal(latePictureClosed,true);
assert.equal(context.Module._photonVideo.pending.length,0);
console.log('Short reference slices, full SPS/PPS changes, and late-picture rejection passed');
// Equal-length slices at one IDR timestamp are parts of one picture, not
// duplicate keyframes. Dropping the second slice produces corruption.
reset();
nal([0,0,1,0x67,0x42,0xe0,0x1e,0x80],7000);
nal([0,0,1,0x68,0x80],7000);
nal([0,0,1,0x65,0x80],7000);
nal([0,0,1,0x65,0x40],7000);
completeAu();
assert.equal(decoders.at(-1).chunks.length,1);
assert.equal(decoders.at(-1).chunks[0].data.length,8+5+5+5);
console.log('Multi-slice keyframe preserved: both equal-length slices reached one access unit.');
// A UI paint between slices must not submit a partial picture. Submission
// happens when the next first_mb=0 picture arrives, with every slice intact.
reset();
nal([0,0,1,0x67,0x42,0xe0,0x1e,0x80],7100);
nal([0,0,1,0x68,0x80],7100);
nal([0,0,1,0x65,0x80],7100);
endAu(0);
assert.equal((decoders.at(-1).chunks || []).length,0);
nal([0,0,1,0x65,0x40],7100);
endAu(0);
assert.equal((decoders.at(-1).chunks || []).length,0);
nal([0,0,1,0x41,0x80],7133);
assert.equal(decoders.at(-1).chunks.length,1);
assert.equal(decoders.at(-1).chunks[0].data.length,8+5+5+5);
console.log('Delayed slices across UI paints remain one complete picture.');
// Decoded pictures can arrive in bursts with 100 ms gaps. The actual upload
// path should pace them at camera speed instead of draining at UI speed.
const upload = vm.runInContext(glue('photon_video_upload', 'glTex,cursorMs'), context);
reset();
const presented = [];
let now = 0;
context.performance.now = () => now;
context.GL = {textures: {1: {}}};
context.GLctx = {
    bindTexture() {}, texParameteri() {},
    texImage2D(...args) { presented.push({wall: now, ts: args.at(-1).timestamp}); },
};
const batches = Array.from({length: 20}, (_, group) => ({
    arrival: group * 165 + (group % 3 === 1 ? 100 : 0),
    frames: Array.from({length: 5}, (_, i) => ({
        timestamp: (1000 + (group * 5 + i) * 33) * 1000,
        displayWidth: 640, displayHeight: 480, close() {},
    })),
}));
let batch = 0;
for (now = 0; now <= 3500; now += 10) {
    while (batch < batches.length && batches[batch].arrival <= now) {
        context.Module._photonVideo.pending.push(...batches[batch++].frames);
    }
    upload(1, 0);
}
assert.equal(presented.length, 100);
assert.equal(presented[0].wall, 150);
for (let i = 1; i < presented.length; ++i) {
    assert.ok(presented[i].wall - presented[i - 1].wall <= 40, 'presentation froze on decoder jitter');
    assert.equal(presented[i].ts - presented[i - 1].ts, 33000);
}
reset();
assert.equal(context.Module._photonVideo.livePresentWall, null);
console.log('Live presentation passed: 100 ms decoder gaps smoothed with 150 ms reserve.');
// A paused/cursor-held picture uploads once, including across 60 UI paints.
// Future decoded pictures must remain queued until their presentation time.
reset(); presented.length = 0;
context.Module._photonVideo.pending = [1000,1033,1066].map(ms => ({
    timestamp: ms * 1000, displayWidth: 640, displayHeight: 480, close() {},
}));
for (let paint = 0; paint < 60; ++paint) upload(1, 1000);
assert.equal(presented.length, 1);
assert.equal(presented[0].ts, 1000000);
upload(1, 1033);
assert.equal(presented.length, 2);
assert.equal(presented[1].ts, 1033000);
assert.equal(context.Module._photonVideo.pending.at(-1).timestamp, 1066000);
console.log('Replay presentation: no repeated uploads and no future-frame presentation.');
// Forward recovery keeps the display watermark; old keyframe output cannot
// flash while newer reference pictures are being reconstructed.
reset();gop(8000);
context.Module._photonVideo.lastPresentedTs=8500000;
reset(1);gop(8000);
const recoveredDecoder=decoders.at(-1);
let recoveryOldClosed=false;
recoveredDecoder.callbacks.output({timestamp:8000000,close(){recoveryOldClosed=true;}});
assert.equal(recoveryOldClosed,true);
assert.equal(context.Module._photonVideo.pending.length,0);
assert.equal(context.Module._photonVideo.lastPresentedTs,8500000);
recoveredDecoder.callbacks.output({timestamp:8533000,displayWidth:640,displayHeight:480,close(){}});
assert.equal(upload(1,8533),1);
// An intentional backwards seek is allowed to display an older picture.
reset(0);gop(8000);
decoders.at(-1).callbacks.output({timestamp:8000000,displayWidth:640,displayHeight:480,close(){}});
assert.equal(upload(1,8000),1);
console.log('Forward codec recovery rejects older pictures; explicit backwards seek still works.');
// Timing diagnostics are same-origin, asynchronous, and rate-limited.
reset();
const reports=[];
context.location={protocol:'https:'};context.document={visibilityState:'visible'};
context.fetch=(url,options)=>{reports.push({url,options});return Promise.resolve({ok:true});};
now=100;
context.Module._photonVideo.pending=[9000,9033,9066].map(ms=>({timestamp:ms*1000,displayWidth:640,displayHeight:480,close(){}}));
upload(1,9000);now=300;upload(1,9033);now=500;upload(1,9066);
assert.equal(reports.length,1);
assert.equal(reports[0].url,'/crash');
const report=JSON.parse(reports[0].options.body);
assert.equal(report.kind,'video_timing');assert.equal(report.frames.at(-1).gap,200);
assert.equal(report.frames.at(-1).lag,0);
console.log('Frame timing anomalies traced asynchronously with a five-second rate limit.');
// Software frame threading increased buffering on the user's browser.
const setReplay=vm.runInContext(glue('photon_video_set_replay','replay'),context);
reset();setReplay(1);gop(10000);
assert.equal(decoders.at(-1).config.hardwareAcceleration,'no-preference');
assert.equal(decoders.at(-1).config.optimizeForLatency,true);
decoders.at(-1).callbacks.error(new Error('software codec unavailable'));
gop(10033);
assert.equal(decoders.at(-1).config.hardwareAcceleration,'no-preference');
reset();setReplay(0);gop(11000);
assert.equal(decoders.at(-1).config.hardwareAcceleration,'no-preference');
console.log('Replay and live retain the browser decoder preference.');
// Exercise a real presentation clock against both immediate output and a
// decoder holding nine pictures, releasing them together at each keyframe.
// A 600 ms cached decode lead must preserve the cursor's picture even when
// a whole future reserve arrives in one burst (the old cap of 12 lost it).
for (const heldPictures of [0, 9]) {
    reset();setReplay(1);gop(12000);presented.length=0;
    const output=decoders.at(-1).callbacks.output;
    const held=[];let fed=0;
    for (let tick=0;tick<200;tick++) {
        now=tick*33;
        const cursor=12000+now;
        while (fed<200 && 12000+fed*33<=cursor+600) {
            held.push({timestamp:(12000+fed*33)*1000,displayWidth:640,displayHeight:480,close(){}});
            const key=fed%30===0;
            fed++;
            while (held.length>(key?0:heldPictures)) output(held.shift());
        }
        upload(1,cursor);
    }
    // The final delayed pictures have no following input; exclude that tail.
    assert.equal(presented.length,200-heldPictures);
    for (let i=0;i<presented.length;i++) {
        assert.equal(presented[i].ts,(12000+i*33)*1000,'buffered replay skipped a picture');
        if(i) assert.equal(presented[i].wall-presented[i-1].wall,33,'buffered replay stuttered');
    }
}
console.log('Replay clock passed immediate and nine-picture buffered decoder output with keyframe bursts.');
