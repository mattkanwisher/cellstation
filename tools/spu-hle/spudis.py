#!/usr/bin/env python3
"""Minimal SPU disassembler — enough to trace job entry/exit ABI (DMA, channels, control flow)."""
import struct, sys, pathlib

# (mask, match, name, format) — checked longest-opcode-first.
# formats: RR (op[0:11]), RI7 (op[0:11]), RI10 (op[0:8]), RI16 (op[0:9]), RI18 (op[0:7]), RRR (op[0:4])
OPS = [
    # RRR (4-bit)
    (0xE0000000,0x80000000,"selb","RRR"),(0xE0000000,0xC0000000,"shufb","RRR"),
    (0xE0000000,0xE0000000,"fma","RRR"),(0xE0000000,0xA0000000,"mpya","RRR"),
    (0xE0000000,0x60000000,"fnms","RRR"),(0xE0000000,0x40000000,"fms","RRR"),
    # RI16 (9-bit)
    (0xFF800000,0x40800000,"il","RI16"),(0xFF800000,0x41000000,"ilhu","RI16"),
    (0xFF800000,0x41800000,"iohl","RI16"),(0xFF800000,0x40000000,"ilh","RI16"),
    (0xFF800000,0x32000000,"br","BR16"),(0xFF800000,0x32800000,"fsmbi","RI16"),
    (0xFF800000,0x30000000,"bra","BR16"),(0xFF800000,0x33000000,"brsl","BRSL"),
    (0xFF800000,0x31000000,"brasl","BRSL"),(0xFF800000,0x21000000,"brz","BR16"),
    (0xFF800000,0x21800000,"brnz","BR16"),(0xFF800000,0x22000000,"brhz","BR16"),
    (0xFF800000,0x22800000,"brhnz","BR16"),
    # RI18 (7-bit)
    (0xFE000000,0x42000000,"ila","RI18"),
    # RI10 (8-bit)
    (0xFF000000,0x34000000,"lqd","RI10"),(0xFF000000,0x24000000,"stqd","RI10"),
    (0xFF000000,0x1C000000,"ai","RI10"),(0xFF000000,0x0C000000,"sfi","RI10"),
    (0xFF000000,0x14000000,"andi","RI10"),(0xFF000000,0x16000000,"andbi","RI10"),
    (0xFF000000,0x04000000,"ori","RI10"),(0xFF000000,0x06000000,"orbi","RI10"),
    (0xFF000000,0x44000000,"orbi2","RI10"),
    (0xFF000000,0x7C000000,"ceqi","RI10"),(0xFF000000,0x7E000000,"ceqbi","RI10"),
    (0xFF000000,0x4C000000,"cgti","RI10"),(0xFF000000,0x5C000000,"clgti","RI10"),
    (0xFF000000,0x74000000,"mpyi","RI10"),(0xFF000000,0x75000000,"mpyui","RI10"),
    # RI7 (11-bit)
    (0xFFE00000,0x3B000000,"shufb7","RI7"),(0xFFE00000,0x0F600000,"shli","RI7"),
    (0xFFE00000,0x0F400000,"shlqbii","RI7"),(0xFFE00000,0x1F600000,"shlqbyi","RI7"),
    (0xFFE00000,0x0F600000,"roti","RI7"),(0xFFE00000,0x0F800000,"rotqbyi","RI7"),
    (0xFFE00000,0x0FC00000,"rotqbii","RI7"),(0xFFE00000,0x0EC00000,"rotmi","RI7"),
    (0xFFE00000,0x0F000000,"rotmai","RI7"),(0xFFE00000,0x3F800000,"rotqbyi2","RI7"),
    # RR (11-bit) — arithmetic/logic/mem-indexed/channel/branch-indirect
    (0xFFE00000,0x18000000,"a","RR"),(0xFFE00000,0x08000000,"sf","RR"),
    (0xFFE00000,0x40200000,"nop","RR"),(0xFFE00000,0x00200000,"lnop","RR"),
    (0xFFE00000,0x00000000,"stop","STOP"),(0xFFE00000,0x00400000,"sync","RR"),
    (0xFFE00000,0x00600000,"dsync","RR"),
    (0xFFE00000,0x35000000,"bi","BI"),(0xFFE00000,0x35200000,"bisl","BI"),
    (0xFFE00000,0x35400000,"iret","BI"),(0xFFE00000,0x25000000,"biz","BI"),
    (0xFFE00000,0x25200000,"binz","BI"),(0xFFE00000,0x25400000,"bihz","BI"),
    (0xFFE00000,0x21A00000,"wrch","CH"),(0xFFE00000,0x01A00000,"rdch","CH"),
    (0xFFE00000,0x01E00000,"rchcnt","CH"),
    (0xFFE00000,0x38800000,"lqx","RR"),(0xFFE00000,0x28800000,"stqx","RR"),
    (0xFFE00000,0x04000000,"or","RR"),(0xFFE00000,0x08000000,"sf2","RR"),
    (0xFFE00000,0x18000000,"a2","RR"),(0xFFE00000,0x48000000,"and","RR"),
    (0xFFE00000,0x58000000,"andc","RR"),(0xFFE00000,0x49000000,"orc","RR"),
    (0xFFE00000,0x0A000000,"orx","RR"),(0xFFE00000,0x41000000,"nor","RR"),
    (0xFFE00000,0x3E800000,"cwd","RI7b"),(0xFFE00000,0x3EC00000,"cdd","RI7b"),
    (0xFFE00000,0x3E000000,"cbd","RI7b"),(0xFFE00000,0x3E400000,"chd","RI7b"),
    (0xFFE00000,0x78000000,"ceq","RR"),(0xFFE00000,0x7A000000,"ceqb","RR"),
    (0xFFE00000,0x04800000,"mr","RR"),
]

CH = {0:"SPU_RdEventStat",1:"SPU_WrEventMask",2:"SPU_WrEventAck",3:"SPU_RdSigNotify1",
      4:"SPU_RdSigNotify2",7:"SPU_WrDec",8:"SPU_RdDec",9:"MFC_WrMSSyncReq",11:"SPU_RdEventMask",
      12:"MFC_RdTagMask",13:"SPU_RdMachStat",14:"SPU_WrSRR0",15:"SPU_RdSRR0",
      16:"MFC_LSA",17:"MFC_EAH",18:"MFC_EAL",19:"MFC_Size",20:"MFC_TagID",21:"MFC_Cmd",
      22:"MFC_WrTagMask",23:"MFC_WrTagUpdate",24:"MFC_RdTagStat",25:"MFC_RdListStallStat",
      26:"MFC_WrListStallAck",27:"MFC_RdAtomicStat",28:"SPU_WrOutMbox",29:"SPU_RdInMbox",
      30:"SPU_WrOutIntrMbox"}

def dis(code, base):
    out=[]
    for off in range(0,len(code)-3,4):
        w = struct.unpack(">I", code[off:off+4])[0]
        a=base+off
        rt=w&0x7f; ra=(w>>7)&0x7f; rb=(w>>14)&0x7f; rc=rt
        name="?"; args=f"0x{w:08x}"
        for mask,match,nm,fmt in OPS:
            if (w&mask)==match:
                name=nm
                if fmt=="RR": args=f"r{rt},r{ra},r{rb}"
                elif fmt=="RRR": rt4=(w>>21)&0x7f; args=f"r{rt4},r{ra},r{rb},r{rc}"
                elif fmt=="RI7": i7=(w>>14)&0x7f; args=f"r{rt},r{ra},{i7}"
                elif fmt=="RI7b": i7=(w>>14)&0x7f; args=f"r{rt},r{ra},{i7}"
                elif fmt=="RI10": i10=(w>>14)&0x3ff; i10=i10-0x400 if i10&0x200 else i10; args=f"r{rt},r{ra},{i10}"
                elif fmt=="RI16": i16=(w>>7)&0xffff; args=f"r{rt},0x{i16:x}"
                elif fmt=="RI18": i18=(w>>7)&0x3ffff; args=f"r{rt},0x{i18:x}"
                elif fmt=="BR16": i16=(w>>7)&0xffff; tgt=(a+((i16-0x10000 if i16&0x8000 else i16)*4))&0x3fffc; args=f"r{rt}->0x{tgt:x}" if nm.startswith('br') and nm not in('br','bra') else f"0x{tgt:x}"
                elif fmt=="BRSL": i16=(w>>7)&0xffff; tgt=(a+((i16-0x10000 if i16&0x8000 else i16)*4))&0x3fffc; args=f"r{rt},0x{tgt:x}"
                elif fmt=="BI": args=f"r{ra}"
                elif fmt=="CH": args=f"{CH.get(ra,f'ch{ra}')},r{rt}" if nm=='wrch' else f"r{rt},{CH.get(ra,f'ch{ra}')}"
                elif fmt=="STOP": args=f"0x{w&0x3fff:x}"
                break
        out.append(f"  {a:05x}: {w:08x}  {name:8s} {args}")
    return "\n".join(out)

if __name__=="__main__":
    f=pathlib.Path(sys.argv[1]); d=f.read_bytes()
    e_phoff=struct.unpack(">I",d[0x1c:0x20])[0]
    ph=d[e_phoff:e_phoff+32]
    _,off,vaddr,_,filesz,_,_,_=struct.unpack(">IIIIIIII",ph)
    code=d[off:off+filesz]
    print(dis(code, vaddr))
