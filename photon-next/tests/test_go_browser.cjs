// Run with node test_go_browser.cjs SERVER_EXE WASM_DIRECTORY.
// Requires Playwright (or PHOTON_PLAYWRIGHT_MODULE pointing to its package).
const { chromium } = require(process.env.PHOTON_PLAYWRIGHT_MODULE || 'playwright');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const net = require('net');
const { spawn } = require('child_process');
const assert = require('assert');
const { createHash } = require('crypto');

async function freePort() {
  const server = net.createServer();
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'photon-go-browser-'));
  let browser, processHandle;
  const fake = http.createServer((req, res) => {
    req.resume(); req.on('end', () => {
      if (req.url.startsWith('/api/v3/write_lp')) { res.writeHead(204); res.end(); }
      else { res.setHeader('Content-Type', 'application/json'); res.end('[]'); }
    });
  });
  await new Promise(resolve => fake.listen(0, '127.0.0.1', resolve));
  try {
    const port = await freePort(), ingress = await freePort();
    const dbc = 'BO_ 1 T: 1 X\n SG_ V : 0|8@1+ (1,0) [0|255] "V" X\n';
    const dbcHash = createHash('sha256').update(dbc).digest('hex');
    fs.mkdirSync(path.join(root, 'dbcs'));
    fs.writeFileSync(path.join(root, 'dbcs', dbcHash + '.dbc'), dbc);
    const args = ['-host', '127.0.0.1', '-http-port', String(port), '-ingress-port', String(ingress),
      '-client-port', String(await freePort()), '-observer-port', String(await freePort()),
      '-video-ingress-port', String(await freePort()), '-video-client-port', String(await freePort()),
      '-video-udp-port', '0', '-password', 'browser-test', '-dbc-hash', dbcHash, '-data-dir', root,
      '-ui', path.resolve(process.argv[3]), '-influx-url', `http://127.0.0.1:${fake.address().port}`,
      '-influx-db', 'photon_dev'];
    processHandle = spawn(path.resolve(process.argv[2]), args, { windowsHide: true, stdio: ['ignore','pipe','pipe'] });
    let logs = ''; processHandle.stderr.on('data', data => { logs += data; });
    for (let i = 0; !logs.includes('Go browser service'); i++) {
      if (i >= 100 || processHandle.exitCode !== null) throw new Error(logs);
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    const socket = net.connect(ingress, '127.0.0.1');
    await new Promise(resolve => socket.once('connect', resolve));
    const packet = (at, value) => {
      const b = Buffer.alloc(49); b.writeUInt32BE(0x43414e31); b.writeUInt16BE(3,4);
      b.writeUInt16BE(1,6); b.writeBigUInt64BE(BigInt(at),12); b.writeUInt32BE(1,20); b[24]=1; b[25]=value; return b;
    };
    const raw = Buffer.concat([packet(1000,10), packet(2000,20)]);
    socket.write(raw.subarray(0,7)); socket.end(raw.subarray(7));
    browser = await chromium.launch({headless:true, channel:process.env.PHOTON_BROWSER_CHANNEL || undefined, args:['--enable-webgl','--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader']});
    const contexts = [];
    for (let i=0;i<2;i++) {
      const context = await browser.newContext({httpCredentials:{username:'photon',password:'browser-test'}});
      contexts.push(context);
      await context.addInitScript(() => {
        const Native = window.WebSocket;
        window.photonTest = {json:[],can:[],socket:null};
        class Capture extends Native {
          constructor(...args) {
            super(...args); window.photonTest.socket=this;
            this.addEventListener('message', async event => {
              if (typeof event.data==='string') { window.photonTest.json.push(JSON.parse(event.data));return; }
              const bytes = event.data instanceof Blob ? await event.data.arrayBuffer() : event.data;
              const v=new DataView(bytes);
              if (v.byteLength>=36 && v.getUint8(3)===2) window.photonTest.can.push(Number(v.getBigUint64(20,true)));
            });
          }
        }
        window.WebSocket=Capture;
      });
      const page = await context.newPage();
      const errors=[];page.on('pageerror',error=>errors.push(error.message));
      await page.goto(`http://127.0.0.1:${port}/photon.html`);
      await page.waitForFunction(()=>window.photonTest.json.some(m=>m.type==='hello_ack'),null,{timeout:60000});
      assert(await page.evaluate(()=>window.photonTest.json.find(m=>m.type==='hello_ack').features.includes('server_replay')));
      const catalog = await context.request.get(`http://127.0.0.1:${port}/api/recordings`);
      const recordings=await catalog.json();assert.strictEqual(recordings.length,1);
      await page.evaluate(({id,at})=>{
        const send=q=>window.photonTest.socket.send(JSON.stringify(q));
        send({type:'subscribe_can'});send({type:'recording_select',session_id:id});
        send({type:'timeline_control',mode:'pause',cursor_ms:at});
      },{id:recordings[0].id,at:i===0?1000:2000});
      await page.waitForFunction(at=>window.photonTest.can.includes(at),i===0?1000:2000);
      assert.strictEqual(errors.length,0,errors.join('\n'));
      assert(await page.evaluate(()=>!!document.querySelector('canvas')));
    }
    const upload=await contexts[1].request.post(`http://127.0.0.1:${port}/api/dbcs?name=revised.dbc`,{data:'BO_ 1 T: 1 X\n SG_ V : 0|8@1+ (2,0) [0|255] "V" X\n'});
    assert.strictEqual(upload.status(),200);assert.match((await upload.json()).hash,/^[a-f0-9]{64}$/);
    console.log('PASS: WASM client, custom Go HTTP port, two fresh browsers, independent raw replay, authenticated DBC upload');
  } finally {
    if(browser) await browser.close();
    if(processHandle && processHandle.exitCode===null) { processHandle.kill();await new Promise(resolve=>processHandle.once('exit',resolve)); }
    await new Promise(resolve=>fake.close(resolve));
    assert(path.resolve(root).startsWith(path.resolve(os.tmpdir())+path.sep));
    fs.rmSync(root,{recursive:true,force:true});
  }
}
main().catch(error=>{console.error(error);process.exitCode=1;});
