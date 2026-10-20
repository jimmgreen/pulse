"""Small synthetic, uncompressed Bayer DNG: no camera files or user preferences."""
import pathlib, struct, subprocess, sys, time
exe, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
work.mkdir(parents=True, exist_ok=True)
def entry(tag, kind, values):
    codes = {1:'B', 3:'H', 4:'I', 5:'I', 10:'i'}
    data = struct.pack('<'+codes[kind]*len(values), *values)
    count = len(values)//2 if kind in (5, 10) else len(values)
    return tag, kind, count, data
w=h=64
entries = [entry(256,4,[w]),entry(257,4,[h]),entry(258,3,[16]),entry(259,3,[1]),entry(262,3,[32803]),
    entry(273,4,[0]),entry(277,3,[1]),entry(278,4,[h]),entry(279,4,[w*h*2]),entry(284,3,[1]),
    entry(33421,3,[2,2]),entry(33422,1,[0,1,1,2]),entry(50706,1,[1,4,0,0]),entry(50707,1,[1,1,0,0]),
    (50708,2,15,b'Pulse Test DNG\0'),entry(50710,1,[0,1,2]),entry(50711,3,[1]),entry(50714,3,[0]),
    entry(50717,4,[65535]),entry(50721,10,[1,1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,1,1]),
    entry(50728,5,[1,1,1,1,1,1]),entry(50778,3,[21])]
entries.sort()
offset = 8+2+12*len(entries)+4
extra = bytearray(); rows = []
for tag, kind, count, data in entries:
    if len(data)>4:
        field = struct.pack('<I',offset+len(extra)); extra.extend(data)
        if len(extra)%2: extra.append(0)
    else: field=data.ljust(4,b'\0')
    rows.append((tag,struct.pack('<HHI',tag,kind,count)+field))
pixels_at=offset+len(extra)
rows=[struct.pack('<HHII',273,4,1,pixels_at) if tag==273 else row for tag,row in rows]
path=work/'synthetic-no-preview.dng'
path.write_bytes(b'II*\0'+struct.pack('<I',8)+struct.pack('<H',len(entries))+b''.join(rows)+b'\0'*4+extra+struct.pack('<H',20000)*(w*h))
def run(*args):
    started=time.perf_counter()
    result=subprocess.run([str(exe),*map(str,args)],capture_output=True,timeout=25)
    print(f"[TIME] {args[0]} {pathlib.Path(args[1]).name!a}: {(time.perf_counter()-started)*1000:.1f} ms")
    return result
def check(ok,label):
    if not ok: raise AssertionError(label)
    print('[PASS]',label)
r=run('probe',path); check(r.returncode==0 and b'width=64' in r.stdout,'synthetic DNG metadata')
r=run('decode',path,32); check(r.returncode!=0 and not r.stdout,'embedded-only request never demosaics')
r=run('decode-full',path,32); check(r.returncode==0, 'explicit full decode succeeds: '+str(r.stderr))
header=struct.unpack('<8I',r.stdout[:32]); check(header[:2]==(0x474D4950,1) and header[2:5]==(32,32,128) and len(r.stdout)==32+32*32*4,'bounded BGRA frame')
check(all(x==255 for x in r.stdout[35::4]),'opaque output alpha')
check(header[7]==0,'full decode not labelled embedded')
r=run('decode-full',path,8193); check(r.returncode==1,'oversized cap rejected')
# Add a reduced RGB IFD to the same RAW, exercising LibRaw's embedded preview path.
base=bytearray(path.read_bytes())
thumb_at=len(base)
struct.pack_into('<I',base,8+2+12*len(entries),thumb_at)
thumb_entries=[entry(254,4,[1]),entry(256,4,[16]),entry(257,4,[8]),entry(258,3,[8,8,8]),
    entry(259,3,[1]),entry(262,3,[2]),entry(273,4,[0]),entry(277,3,[3]),entry(278,4,[8]),
    entry(279,4,[16*8*3]),entry(284,3,[1])]
thumb_extra=bytearray(); thumb_rows=[]
thumb_offset=thumb_at+2+12*len(thumb_entries)+4
for tag,kind,count,data in thumb_entries:
    if len(data)>4:
        field=struct.pack('<I',thumb_offset+len(thumb_extra));thumb_extra.extend(data)
    else:field=data.ljust(4,b'\0')
    thumb_rows.append((tag,struct.pack('<HHI',tag,kind,count)+field))
thumb_pixels=thumb_offset+len(thumb_extra)
thumb_rows=[struct.pack('<HHII',273,4,1,thumb_pixels) if tag==273 else row for tag,row in thumb_rows]
embedded=work/'synthetic-embedded.dng'
embedded.write_bytes(base+struct.pack('<H',len(thumb_entries))+b''.join(thumb_rows)+b'\0'*4+thumb_extra+bytes([160,80,40])*(16*8))
r=run('decode',embedded,32)
check(r.returncode==0,'embedded RGB preview decoded: '+str(r.stderr))
header=struct.unpack('<8I',r.stdout[:32])
check(header[7]&4 and header[2:4]==(16,8), 'embedded preview is flagged and never enlarged')
check(r.stdout[32:36]==bytes([40,80,160,255]),'embedded RGB converted to BGRA correctly')
unicode_path=work/'相机 RAW 引号 空格.dng'
unicode_path.write_bytes(path.read_bytes())
r=run('probe',unicode_path)
check(r.returncode==0 and b'width=64' in r.stdout,'Unicode path with spaces')


