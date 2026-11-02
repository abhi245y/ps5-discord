#!/usr/bin/env python3
"""Embed only this source-built worker; never download or execute a payload."""
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_bytes()
rows=[','.join(str(v) for v in source[i:i+32]) for i in range(0,len(source),32)]
Path(sys.argv[2]).write_text('const unsigned char overlay_worker_blob[]={\n'+',\n'.join(rows)+'\n};\nconst unsigned int overlay_worker_blob_size='+str(len(source))+';\n')
