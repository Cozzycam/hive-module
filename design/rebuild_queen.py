import json, re
F='F81F'
rows = []
for _ in range(5): rows.append([F]*44)
rows.append([F]*21+['0000']+[F]*22)
rows.append([F]*20+['0000','F6AC','0000']+[F]*21)
rows.append([F]*14+['0000','0000']+[F]*2+['0000','F6AC','FF75','F6AC','0000']+[F]*2+['0000','0000']+[F]*17)
rows.append([F]*14+['0000','DD6A','FF75','0000']+[F]+['0000','F6AC','FF75','F6AC','0000']+[F]+['0000','FF75','DD6A','0000']+[F]*15)
rows.append([F]*14+['0000','30E3','DD6A','F6AC','0000','0000','F6AC','F6AC','F6AC','0000','0000','F6AC','DD6A','30E3','0000']+[F]*15)
rows.append([F]*9+['0000','0000']+[F]*2+['0000','EE13','EE13','30E3','DD6A','F6AC','F6AC','DD6A','F6AC','DD6A','F6AC','F6AC','DD6A','30E3','EE13','EE13','0000']+[F]*2+['0000','0000']+[F]*10)
rows.append([F]*8+['0000','EE13','EE13','0000']+[F]+['0000','EE13','CBCD','CBCD','20C2','DD6A','DD6A','20C2','20C2','20C2','DD6A','DD6A','20C2','CBCD','CBCD','EE13','0000']+[F]+['0000','EE13','EE13','0000']+[F]*9)
rows.append([F]*8+['0000','EE13','CBCD','EE13','0000','CBCD','CBCD','CBCD','20C2','8B87','20C2','20C2','51C3','FF75','51C3','20C2','20C2','8B87','20C2','CBCD','CBCD','CBCD','0000','EE13','CBCD','EE13','0000']+[F]*9)
rows.append([F]*7+['0000','EE13','CBCD','CBCD','CBCD','EE13','30E3','CBCD','20C2','8B87','51C3','51C3','51C3','F6AC','F6AC','F6AC','51C3','51C3','51C3','8B87','20C2','CBCD','30E3','EE13','CBCD','CBCD','CBCD','EE13','0000']+[F]*8)
rows.append([F]*7+['0000','EE13','CBCD','30E3','CBCD','CBCD','CBCD','20C2','8B87','51C3','51C3','20C2','20C2','20C2','20C2','20C2','20C2','20C2','51C3','51C3','8B87','20C2','CBCD','CBCD','CBCD','30E3','CBCD','EE13','0000']+[F]*8)
rows.append([F]*7+['0000','EE13','CBCD','30E3','CBCD','CBCD','20C2','8B87','51C3','51C3','20C2','20C2','20C2','51C3','51C3','51C3','20C2','20C2','20C2','51C3','51C3','8B87','20C2','CBCD','CBCD','30E3','CBCD','EE13','0000']+[F]*8)
rows.append([F]*7+['0000','EE13','CBCD','30E3','CBCD','CBCD','20C2','8B87','51C3','51C3','20C2','20C2','51C3','51C3','51C3','51C3','51C3','20C2','20C2','51C3','51C3','8B87','20C2','CBCD','CBCD','30E3','CBCD','EE13','0000']+[F]*8)
rows.append([F]*4+['0000','0000']+[F]+['0000','CBCD','CBCD','CBCD','30E3','CBCD','20C2','51C3','51C3','20C2','51C3','51C3','51C3','51C3','51C3','51C3','51C3','51C3','51C3','20C2','51C3','51C3','20C2','CBCD','30E3','CBCD','CBCD','CBCD','0000']+[F]+['0000','0000']+[F]*5)
rows.append([F]*3+['0000','EE13','EE13','0000','0000','CBCD','CBCD','CBCD','CBCD','20C2','8B87']+['51C3']*15+['8B87','20C2','CBCD','CBCD','CBCD','CBCD','0000','0000','EE13','EE13','0000']+[F]*4)
rows.append([F]*3+['0000','EE13','CBCD','CBCD','CBCD','30E3','30E3','CBCD','CBCD','20C2','8B87','51C3','20C2','20C2','DF59','DF59','51C3','51C3','51C3','51C3','51C3','DF59','DF59','20C2','20C2','51C3','8B87','20C2','CBCD','CBCD','30E3','30E3','CBCD','CBCD','CBCD','EE13','0000']+[F]*4)
rows.append([F]*3+['0000','CBCD','CBCD','CBCD','30E3','EE13','EE13','30E3','30E3','20C2','51C3','51C3','20C2','20C2','CF35','CF35','51C3','51C3','51C3','51C3','51C3','CF35','CF35','20C2','20C2','51C3','51C3','20C2','30E3','30E3','EE13','EE13','30E3','CBCD','CBCD','CBCD','0000']+[F]*4)
rows.append([F]*3+['0000','CBCD','CBCD','CBCD','30E3','EE13','CBCD','EE13','EE13','30E3','20C2','51C3','51C3','51C3','CF35','CF35','51C3','51C3','51C3','51C3','51C3','CF35','CF35','51C3','51C3','51C3','20C2','CBCD','EE13','EE13','CBCD','EE13','30E3','CBCD','CBCD','CBCD','0000']+[F]*4)
rows.append([F]*4+['0000','CBCD','CBCD','30E3','EE13','30E3','CBCD','CBCD','CBCD','30E3','20C2']+['51C3']*13+['20C2','CBCD','CBCD','CBCD','CBCD','30E3','EE13','30E3','CBCD','CBCD','0000']+[F]*5)
rows.append([F]*5+['0000','30E3','EE13','CBCD','CBCD','30E3','CBCD','CBCD','30E3','20C2','20C2']+['51C3']*11+['20C2','20C2','30E3','CBCD','CBCD','30E3','CBCD','CBCD','EE13','30E3','0000']+[F]*6)
rows.append([F]*3+['0000','0000','0000','30E3','CBCD','CBCD','CBCD','CBCD','30E3','30E3','EE13','EE13','20C2','20C2','20C2','20C2','51C3','51C3','51C3','51C3','51C3','20C2','20C2','20C2','20C2','EE13','EE13','30E3','30E3','CBCD','CBCD','CBCD','CBCD','30E3','0000','0000','0000']+[F]*4)
rows.append([F]*2+['0000','9CA8','9CA8','9CA8','7B86','30E3','CBCD','CBCD','CBCD','CBCD','30E3','EE13','CBCD','EE13','EE13']+['20C2']*9+['EE13','EE13','CBCD','EE13','30E3','CBCD','CBCD','CBCD','CBCD','30E3','7B86','9CA8','9CA8','9CA8','0000']+[F]*3)
rows.append([F]*3+['0000','9CA8','39A3','7B86','30E3','CBCD','CBCD','CBCD','CBCD','30E3','EE13','CBCD','CBCD','CBCD','CBCD','20C2','20C2','51C3','51C3','51C3','20C2','20C2','CBCD','CBCD','CBCD','CBCD','EE13','30E3','CBCD','CBCD','CBCD','CBCD','30E3','7B86','39A3','9CA8','0000']+[F]*4)
rows.append([F]*3+['0000','7B86','7B86','39A3','30E3','CBCD','CBCD','CBCD','30E3','EE13','CBCD','CBCD','30E3','CBCD','CBCD','CBCD','20C2','51C3','51C3','51C3','20C2','CBCD','CBCD','CBCD','30E3','CBCD','CBCD','EE13','30E3','CBCD','CBCD','CBCD','30E3','39A3','7B86','7B86','0000']+[F]*4)
rows.append([F]*4+['0000','7B86','39A3','7B86','30E3','CBCD','CBCD','30E3','CBCD','CBCD','CBCD','CBCD','30E3','CBCD','CBCD','20C2','20C2','51C3','20C2','20C2','CBCD','CBCD','30E3','CBCD','CBCD','CBCD','CBCD','30E3','CBCD','CBCD','30E3','7B86','39A3','7B86','0000']+[F]*5)
rows.append([F]*5+['0000','7B86','39A3','7B86','30E3','30E3','30E3','CBCD','CBCD','CBCD','CBCD','CBCD','30E3','CBCD','CBCD','20C2','20C2','20C2','CBCD','CBCD','30E3','CBCD','CBCD','CBCD','CBCD','CBCD','30E3','30E3','30E3','7B86','39A3','7B86','0000']+[F]*6)
rows.append([F]*4+['0000','9CA8','39A3','7B86','39A3','39A3','7B86','30E3','30E3','CBCD','CBCD','CBCD','CBCD','30E3','30E3','CBCD','CBCD','30E3','CBCD','CBCD','30E3','30E3','CBCD','CBCD','CBCD','CBCD','30E3','30E3','7B86','39A3','39A3','7B86','39A3','9CA8','0000']+[F]*5)
rows.append([F]*3+['0000','9CA8','7B86','7B86','39A3','39A3','9CA8','7B86','39A3','7B86','30E3','30E3','CBCD','CBCD','CBCD','30E3','30E3','30E3','39A3','30E3','30E3','30E3','CBCD','CBCD','CBCD','30E3','30E3','7B86','39A3','7B86','9CA8','39A3','39A3','7B86','7B86','9CA8','0000']+[F]*4)
rows.append([F]*2+['0000','9CA8','7B86','7B86','39A3','39A3','9CA8','7B86','39A3','7B86','7B86','39A3','39A3','30E3','30E3','30E3','30E3','30E3','39A3','39A3','39A3','30E3','30E3','30E3','30E3','30E3','39A3','39A3','7B86','7B86','39A3','7B86','9CA8','39A3','39A3','7B86','7B86','9CA8','0000']+[F]*3)
rows.append([F]*3+['0000','0000','0000','39A3','9CA8','9CA8','39A3','7B86','7B86','39A3','39A3','7B86','7B86','39A3','7B86','39A3','7B86','39A3','39A3','39A3','7B86','39A3','7B86','39A3','7B86','7B86','39A3','39A3','7B86','7B86','39A3','9CA8','9CA8','39A3','0000','0000','0000']+[F]*4)
rows.append([F]*6+['0000','9CA8','7B86','7B86','7B86','39A3','39A3','7B86','7B86','39A3','7B86','39A3','39A3','7B86','7B86','39A3','7B86','7B86','39A3','39A3','7B86','39A3','7B86','7B86','39A3','39A3','7B86','7B86','7B86','9CA8','0000']+[F]*7)
rows.append([F]*7+['0000','0000','0000','0000','0000','9CA8','9CA8','39A3','7B86','7B86','39A3','7B86','7B86','9CA8','39A3','9CA8','7B86','7B86','39A3','7B86','7B86','39A3','9CA8','9CA8','0000','0000','0000','0000','0000']+[F]*8)
rows.append([F]*10+['0000','9CA8','39A3','39A3','9CA8','7B86','7B86','0000','0000','9CA8','9CA8','9CA8','9CA8','9CA8','0000','0000','7B86','7B86','9CA8','39A3','39A3','9CA8','0000']+[F]*11)
rows.append([F]*9+['0000','9CA8','9CA8','9CA8','9CA8','7B86','0000','0000','0000']+[F]+['0000','9CA8','9CA8','9CA8','0000']+[F]+['0000','0000','0000','7B86','9CA8','9CA8','9CA8','9CA8','0000']+[F]*10)
rows.append([F]*10+['0000','0000','0000','0000','0000','0000']+[F]*4+['0000','0000','0000']+[F]*4+['0000','0000','0000','0000','0000','0000']+[F]*11)
for _ in range(5): rows.append([F]*44)

pixels = []
for i,r in enumerate(rows):
    assert len(r)==44, f'Row {i} has {len(r)} pixels'
    pixels.extend(r)
assert len(pixels)==1936, f'Total {len(pixels)}, expected 1936'

data = {'name':'QUEEN','width':44,'height':44,'pixels':pixels}
with open('design/sprites/QUEEN.json','w',newline='\n') as f:
    json.dump(data, f, indent=2)

def to_c(name,w,h,px):
    lines=[f'#define {name}_W {w}',f'#define {name}_H {h}',
           f'static const uint16_t {name}[{w*h}] PROGMEM = {{']
    for y in range(h):
        row=px[y*w:(y+1)*w]
        lines.append('    '+', '.join(f'0x{p}' for p in row)+(',' if y<h-1 else ''))
    lines.append('};')
    return '\n'.join(lines)

with open('firmware/include/sprites.h') as f:
    content=f.read()
content=re.sub(r'#define QUEEN_W.*?^};',to_c('QUEEN',44,44,pixels),content,count=1,flags=re.DOTALL|re.MULTILINE)
with open('firmware/include/sprites.h','w',newline='\n') as f:
    f.write(content)

print('Restored your painted queen (44x44)')
print(f'Non-transparent pixels: {sum(1 for p in pixels if p != "F81F")}')
