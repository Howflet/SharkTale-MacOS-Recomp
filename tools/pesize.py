import sys,struct
def rva2off(secs,r):
    for va,vs,ro,rs in secs:
        if va<=r<va+max(vs,rs): return r-va+ro
for p in sys.argv[1:]:
    d=open(p,'rb').read(); o=struct.unpack_from('<I',d,0x3c)[0]
    n=struct.unpack_from('<H',d,o+6)[0]; so=struct.unpack_from('<H',d,o+20)[0]; opt=o+24
    secs=[];text=0
    for i in range(n):
        b=opt+so+i*40; nm=d[b:b+8].rstrip(b'\0'); vs,va,rs,ro=struct.unpack_from('<IIII',d,b+8)
        secs.append((va,vs,ro,rs))
        if nm==b'.text': text=vs
    er,_=struct.unpack_from('<II',d,opt+96); ir,_=struct.unpack_from('<II',d,opt+104)
    ne=0
    if er:
        eo=rva2off(secs,er); ne=struct.unpack_from('<I',d,eo+24)[0]
    dlls=[]
    if ir:
        io=rva2off(secs,ir)
        while True:
            f=struct.unpack_from('<5I',d,io)
            if f[3]==0: break
            no=rva2off(secs,f[3]); dlls.append(d[no:d.index(b'\0',no)].decode())
            io+=20
    print(f"{p.split('/')[-1]:22} text={text//1024:5}KB exports={ne:5} imports={','.join(dlls)}")
