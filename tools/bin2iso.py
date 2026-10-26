import sys
src,dst=sys.argv[1],sys.argv[2]
with open(src,'rb') as f, open(dst,'wb') as o:
    while True:
        s=f.read(2352)
        if len(s)<2352: break
        o.write(s[16:2064])
