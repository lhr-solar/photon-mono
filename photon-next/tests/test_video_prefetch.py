"""A slow video range must not block other WebSocket control traffic."""
import argparse, base64, json, os, socket, struct, subprocess, tempfile, threading, time
from pathlib import Path
from test_ui_reconnect import read_exact, receive

def send(conn, message):
    data=json.dumps(message).encode(); mask=b'abcd'
    header=bytes([0x81,0x80|len(data)]) if len(data)<126 else b'\x81\xfe'+struct.pack('!H',len(data))
    conn.sendall(header+mask+bytes(x^mask[i%4] for i,x in enumerate(data)))

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('backend',type=Path); args=parser.parse_args()
    video=socket.socket();video.bind(('127.0.0.1',0));video.listen();video.settimeout(.1)
    ws_port=socket.socket();ws_port.bind(('127.0.0.1',0));port=ws_port.getsockname()[1];ws_port.close()
    slow=threading.Event();stop=threading.Event();errors=[]
    gop=b'\0\0\0\1\x67\x42\xe0\x1e\0\0\0\1\x68\x80\0\0\0\1\x65\x80\0\0\0\1\x41\x80'
    sps=gop[:8];pps=gop[8:14];base=1000000
    reply=struct.pack('>BqBqIQQ',0,0,1,0,5,base,base+4990)
    reply+=struct.pack('>H',len(sps))+sps+struct.pack('>H',len(pps))+pps+struct.pack('>H',5)
    for i in range(5):reply+=struct.pack('>QQQIIIBBH',base+i*1000,base+i*1000+990,i*len(gop),len(gop),1,4,0,1,0)
    reply+=struct.pack('>HIQQ',1,1,base,base+4990)
    def peer(conn):
        try:
            with conn:
                conn.settimeout(3)
                while not stop.is_set():
                    mode=read_exact(conn,1)[0]
                    if mode==3:read_exact(conn,11);body=reply
                    elif mode==4:
                        _,_,offset,length=struct.unpack('>QIQI',read_exact(conn,24))
                        if offset:slow.set();time.sleep(.15)
                        body=gop[:length]
                    else:raise AssertionError(mode)
                    conn.sendall(struct.pack('>I',len(body))+body)
        except (EOFError,ConnectionError,OSError):pass
        except Exception as e:errors.append(str(e))
    def serve():
        while not stop.is_set():
            try:conn,_=video.accept()
            except socket.timeout:continue
            except OSError:return
            threading.Thread(target=peer,args=(conn,),daemon=True).start()
    threading.Thread(target=serve,daemon=True).start()
    with tempfile.TemporaryDirectory() as directory:
        env=dict(os.environ);env.pop('PHOTON_PASSWORD',None)
        process=subprocess.Popen([str(args.backend.resolve()),'--source','daq_tcp','--no-live-tail',
            '--tsdb-dir',directory,'--log-path',directory,'--ws-port',str(port),'--video-host','127.0.0.1',
            '--video-port',str(video.getsockname()[1]),'--history-remote','off'],
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env)
        try:
            for _ in range(200):
                if process.poll() is not None:raise AssertionError(process.stdout.read().decode())
                try:conn=socket.create_connection(('127.0.0.1',port),.1);break
                except OSError:time.sleep(.02)
            else:raise AssertionError('backend did not start')
            with conn:
                conn.settimeout(3)
                key=base64.b64encode(b'video-test-12345!').decode()
                conn.sendall(('GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: '+key+'\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
                headers=b''
                while b'\r\n\r\n' not in headers:headers+=read_exact(conn,1)
                assert b'101' in headers
                send(conn,{'type':'hello','client':'prefetch-test','protocol_version':1})
                send(conn,{'type':'video_locate','at_ms':base,'prefetch_gops':4,'query_id':71})
                assert slow.wait(2),'prefetch did not start'
                started=time.perf_counter();send(conn,{'type':'track_sim_list'})
                frames=0;acks={}
                def record(op,data):
                    nonlocal frames
                    if op==2 and len(data)>=24 and data[3]==8 and data[15]==4:frames+=1
                    if op==1:
                        message=json.loads(data)
                        if message['type']=='video_status' and message.get('query_id')==71:
                            acks[message.get('reason')]=frames
                        return message['type']
                while True:
                    op,data=receive(conn)
                    if record(op,data)=='track_sim_list':break
                elapsed=(time.perf_counter()-started)*1000
                assert elapsed<100, f'control blocked on prefetch: {elapsed:.1f}ms'
                while 'prefetch_complete' not in acks:
                    record(*receive(conn))
                assert acks=={'entry_complete':4,'prefetch_complete':20},acks
                assert not errors,errors
                print(f'150ms video reads kept off hub; control response {elapsed:.1f}ms; entry/batch acknowledgements after 4/20 NALs')
        finally:
            process.terminate();process.wait(timeout=5);stop.set();video.close()

if __name__=='__main__':main()
