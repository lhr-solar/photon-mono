// Audit real recorded GOPs through the actual browser access-unit assembler.
// Run: node tests/test_video_encoded.cjs path/to/recording.h264 [...]
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const {spawnSync}=require('node:child_process');
const source=fs.readFileSync(path.join(__dirname,'../src/ui/video_decoder.cpp'),'utf8');
function glue(name,args){
    const start=source.indexOf(', '+name+',');const body=source.indexOf('{',start);
    return '('+args+') => '+source.slice(body,source.indexOf('\n});',body)+2);
}
const files=process.argv.slice(2);assert.ok(files.length>0,'Supply real H.264 GOP samples');
let chunks=[];
const codecs=[];
const context=vm.createContext({Module:{},HEAPU8:new Uint8Array(16*1024*1024),
    performance:{now:()=>0},EncodedVideoChunk:class{constructor(x){Object.assign(this,x);}},
    VideoDecoder:class{
        constructor(callbacks){this.callbacks=callbacks;this.state='unconfigured';this.needKey=true;codecs.push(this);}
        configure(){this.state='configured';this.needKey=true;}
        decode(chunk){assert.equal(this.state,'configured');assert.ok(!this.needKey||chunk.type==='key');this.needKey=false;chunks.push(Buffer.from(chunk.data));}
        flush(){this.needKey=true;return Promise.resolve();}
        close(){this.state='closed';}
    }});
const init=vm.runInContext(glue('photon_video_init',''),context);
const reset=vm.runInContext(glue('photon_video_reset','preservePresentation'),context);
const feed=vm.runInContext(glue('photon_video_feed','ptr,len,tsMs,flags'),context);
const endAu=vm.runInContext(glue('photon_video_end_au','force'),context);
init();
function hashes(bytes){
    const r=spawnSync('ffmpeg',['-nostdin','-v','error','-i','pipe:0','-f','framemd5','pipe:1'],{input:bytes,maxBuffer:16*1024*1024});
    assert.equal(r.status,0,r.stderr.toString());assert.equal(r.stderr.toString(),'');
    return r.stdout.toString().split('\n').filter(line=>line&&!line.startsWith('#')).map(line=>line.split(',').at(-1).trim());
}
for(const [run,file] of files.entries()){
    const data=fs.readFileSync(file);const starts=Array.from(data.toString('latin1').matchAll(/\x00\x00\x00\x01|\x00\x00\x01/g),m=>m.index);
    const nals=starts.map((a,i)=>data.subarray(a,starts[i+1]??data.length));
    const expected=hashes(data);assert.ok(expected.length>1,'Sample must contain dependent pictures');
    function send(nal,ts){context.HEAPU8.set(nal);feed(0,nal.length,ts,0);}
    function replay(limit){
        let picture=-1;
        for(const nal of nals){
            const sc=context.Module._photonVideo.startLen(nal),type=nal[sc]&31;
            if((type===1||type===5)&&context.Module._photonVideo.firstMb(nal)===0)++picture;
            if(picture>limit)break;
            send(nal,1000*run+Math.max(0,picture)*33);
        }
    }
    // Run entry, then Pause drains a delta picture. Resume re-enters the same
    // keyframe; the complete resumed reference chain must be pixel-identical.
    reset();chunks=[];replay(Math.floor(expected.length/2));endAu(1);
    assert.equal(context.Module._photonVideo.needKey,true);
    const stale=codecs.at(-1);
    reset();chunks=[];replay(Infinity);endAu(1);
    assert.deepEqual(hashes(Buffer.concat(chunks)),expected,'Access-unit assembler changed recorded pictures');
    let closed=false;stale.callbacks.output({timestamp:0,close(){closed=true;}});
    assert.equal(closed,true);assert.equal(context.Module._photonVideo.pending.length,0);
    console.log(path.basename(file)+': '+expected.length+' resumed pictures match original pixels after Pause and decoder reset.');
}
