// Exercise actual browser cache callbacks during A -> B -> A transitions.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../src/ui/idb_tile_store.cpp'), 'utf8');
function glue(name, args) {
    const start = source.indexOf(', ' + name + ',');
    const body = source.indexOf('{', start);
    return '(' + args + ') => ' + source.slice(body, source.indexOf('\n});', body) + 2);
}
const reads = [];
const state = {scope:'', inFlight:0, done:[], failed:false, bytes:1234};
state.db = {transaction(store, mode) {
    assert.equal(mode, 'readonly');
    return {objectStore() {return {openCursor(range) {
        const request = {range}; reads.push(request); return request;
    }};}};
}};
const context = vm.createContext({Module:{_photonTiles:state}, UTF8ToString:s=>s,
    IDBKeyRange:{bound:(lower,upper)=>({lower,upper})}});
const scope = vm.runInContext(glue('photon_tiles_set_scope','scope'),context);
const request = vm.runInContext(glue('photon_tiles_request','signalId,level,indexFrom,indexTo'),context);
scope('v1|a|dbc');
assert.equal(request(7,0,0,1),1);
scope('v1|b|dbc');
assert.equal(request(7,0,0,1),1);
scope('v1|a|dbc');
assert.equal(request(7,0,0,1),1);
// Both stale completions have identical signal/range coordinates to the new
// request. Neither may release it, including the first A generation.
reads[0].onsuccess({target:{result:null}});
reads[1].onsuccess({target:{result:null}});
assert.equal(state.done.length,0);
assert.equal(state.inFlight,1);
reads[2].onsuccess({target:{result:null}});
assert.equal(state.done.length,1);
assert.equal(state.inFlight,0);
assert.equal(state.bytes,1234);
assert.equal(reads[2].range.lower[0],'v1|a|dbc');
console.log('Browser cache passed: scoped reads survive rapid A -> B -> A switches without stale completions or cache deletion.');
