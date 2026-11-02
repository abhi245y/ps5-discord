#!/usr/bin/env python3
"""Validate source-built shared worker against this loader's supported ELF subset."""
import json
from pathlib import Path
import struct
import sys
p=Path(sys.argv[1]);data=p.read_bytes()
def region(at,n):
    if at<0 or n<0 or at>len(data) or n>len(data)-at:raise ValueError('ELF region outside file')
    return data[at:at+n]
def unpack(fmt,at):return struct.unpack(fmt,region(at,struct.calcsize(fmt)))
h=unpack('<16sHHIQQQIHHHHHH',0)
ident,kind,machine,_,entry,phoff,shoff,_,ehsize,phsize,phnum,shsize,shnum,shstr=h
assert ident[:6]==b'\x7fELF\x02\x01' and kind==3 and machine==62 and ehsize==64
assert phsize==56 and shsize==64 and phnum and shnum and shstr<shnum
loads=[];needed=[]
for i in range(phnum):
    t,flags,offset,va,_,filesz,memsz,_=unpack('<IIQQQQQQ',phoff+i*phsize)
    if t==1:
        region(offset,filesz);assert filesz<=memsz and va+memsz<=16*1024*1024
        loads.append((va,va+memsz,flags))
    if t==2:
        # The loader walks whole Elf64_Dyn entries until DT_NULL; lld may group
        # neighbouring sections into this segment, so its size need not be a
        # multiple of 16, but the terminator must fall inside the entry count.
        entries=filesz//16
        assert entries,'dynamic segment smaller than one entry'
        saw_null=False
        for j in range(entries):
            tag,value=unpack('<qQ',offset+j*16)
            if tag==0:
                saw_null=True
                break
            if tag==0x6ffffffb:assert not value&0x08000000,'PIE worker unsupported'
        assert saw_null,'dynamic table missing DT_NULL terminator'
assert loads and min(a for a,_,_ in loads)==0
assert any(a<=entry<b and flags&1 for a,b,flags in loads),'entry not executable'
sections=[unpack('<IIQQQQIIQQ',shoff+i*shsize) for i in range(shnum)]
relocs={};imports=set()
for sec in sections:
    _,t,_,_,offset,size,link,_,_,entsize=sec
    if t not in (4,9):continue
    assert t==4 and entsize==24 and size%24==0 and link<shnum,'unsupported relocation section'
    symsec=sections[link];assert symsec[1] in (2,11) and symsec[9]==24 and symsec[6]<shnum
    strings=sections[symsec[6]];string_data=region(strings[4],strings[5])
    for j in range(size//24):
        target,info,addend=unpack('<QQq',offset+j*24);typ=info&0xffffffff;index=info>>32
        assert typ in (0,1,6,8),f'unsupported relocation {typ}'
        assert any(a<=target and target+8<=b for a,b,_ in loads),'relocation outside mapped segments'
        relocs[typ]=relocs.get(typ,0)+1
        if typ in (1,6):
            assert index<symsec[5]//24
            name,_,_,defined,_,_=unpack('<IBBHQQ',symsec[4]+index*24)
            assert name<len(string_data)
            end=string_data.find(b'\0',name);assert end>=0
            if not defined:imports.add(string_data[name:end].decode('ascii'))
assert not any(x in imports for x in ('__cxa_atexit','__cxa_guard_acquire','_Unwind_Resume')),'worker requires C++ runtime initialization'
print(json.dumps({'worker':str(p),'bytes':len(data),'entry':hex(entry),'relocations':relocs,'imports':sorted(imports),
                  'result':'ELF layout compatible; runtime export availability remains untested'},indent=2))
