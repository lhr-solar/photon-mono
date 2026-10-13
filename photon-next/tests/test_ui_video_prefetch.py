"""An entry acknowledgement must not replace the query of an arriving batch."""
import argparse, base64, hashlib, json, os, socket, struct, subprocess, threading, time
from pathlib import Path
from test_ui_reconnect import read_exact, receive, send_text
from test_ui_video_switch import units, kind, first_mb, tagged

def main():
    parser=argparse.ArgumentParser();parser.add_argument('ui',type=Path);parser.add_argument('sample',type=Path)
    args=parser.parse_args();nals=units(args.sample.read_bytes());base=1000000
    sps=next(n for n in nals if kind(n)==7);pps=next(n for n in nals if kind(n)==8)
    pictures=sum(kind(n) in (1,5) and first_mb(n)==0 for n in nals);span=(pictures-1)*33
    reply=struct.pack('>BqBqIQQ',0,0,1,0,5,base,base+5000)
    reply+=struct.pack('>H',len(sps))+sps+struct.pack('>H',len(pps))+pps+struct.pack('>H',5)
    for i in range(5):reply+=struct.pack('>QQQIIIBBH',base+i*1000,base+i*1000+span,i*4096,4096,1,len(nals),0,0,0)
    reply+=struct.pack('>HIQQ',1,1,base,base+5000)
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(3)
    active=threading.Event();done=threading.Event();stop=threading.Event();errors=[];requests=[]
    def serve():
        try:
            conn,_=listener.accept()
            with conn:
                conn.settimeout(.1);headers=b''
                while b'\r\n\r\n' not in headers:headers+=read_exact(conn,1)
                key=next(line.split(':',1)[1].strip() for line in headers.decode().split('\r\n') if line.lower().startswith('sec-websocket-key:'))
                accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                conn.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n').encode())
                def deliver(query):
                    try:
                        tagged(conn,7,query,reply)
                        def gop(index):
                            picture=-1
                            for nal in nals:
                                t=kind(nal)
                                if t in (1,5) and first_mb(nal)==0:picture+=1
                                tagged(conn,4,query,struct.pack('<QB3x',base+index*1000+max(0,picture)*33,2 if t in (7,8) else 1 if t==5 else 0)+nal)
                        gop(0)
                        send_text(conn,{'type':'video_status','query_id':query,'reason':'entry_complete','connected':True})
                        time.sleep(.35)
                        send_text(conn,{'type':'video_status','query_id':query+999,'reason':'prefetch_complete','connected':True})
                        for index in range(1,5):gop(index);time.sleep(.05)
                        send_text(conn,{'type':'video_status','query_id':query,'reason':'prefetch_complete','connected':True})
                        active.clear();done.set()
                    except Exception as e:errors.append(str(e))
                while not stop.is_set():
                    try:op,data=receive(conn)
                    except socket.timeout:continue
                    if op==8:break
                    if op!=1:continue
                    msg=json.loads(data)
                    if msg['type']=='hello':
                        send_text(conn,{'type':'hello_ack','server':'batch-race-test','active_source':{'kind':'daq_tcp','host':'127.0.0.1','port':6500}})
                        send_text(conn,{'type':'catalog_result','sessions':[{'id':'batch-fixture','min_ms':base,'max_ms':base+6000,'dbc_hash':'fixture','has_video':True}]})
                    if msg['type']=='video_locate':
                        requests.append({'query_id':msg['query_id'],'at_ms':msg['at_ms']})
                        if active.is_set():errors.append('New locate interrupted the active prefetch batch')
                        elif not done.is_set():
                            active.set();threading.Thread(target=deliver,args=(msg['query_id'],),daemon=True).start()
        except (EOFError,ConnectionResetError,BrokenPipeError):pass
        except Exception as e:errors.append(str(e))
    threading.Thread(target=serve,daemon=True).start()
    env=dict(os.environ,PHOTON_GL3D='0',PHOTON_PIN_SESSION='batch-fixture')
    try:
        result=subprocess.run([str(args.ui.resolve()),'--private','--screen','live','--ws','ws://127.0.0.1:'+str(listener.getsockname()[1]),
            '--capture',str(args.ui.resolve().parent/'video-prefetch-race.bmp'),'--width','1200','--height','800','--frames','180'],env=env,capture_output=True,text=True,timeout=20)
        assert result.returncode==0,result.stdout+result.stderr
        assert done.is_set(),'prefetch never completed'
        assert not errors,errors
        assert requests and all(item['at_ms']>=base+4000 for item in requests[1:]),f'Redundant locates: {requests}'
        print('Entry ack, 350ms prefetch delay and stale completion: arriving batch retained; later requests only extend its window.')
    finally:stop.set();listener.close()

if __name__=='__main__':main()
