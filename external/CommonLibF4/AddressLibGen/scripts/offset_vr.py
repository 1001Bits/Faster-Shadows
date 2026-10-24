import os
import re
import sys
import os

file = open("../mappings/id2offset")
outfile = open('../mappings/1.2.72_1.10.163.txt', 'w')

for line in file.readlines():
    matches = re.search('([0-9]*)\t([0-9A-F]{7})', line)
    pc_address = int(matches.group(2), 16) + 0x140000000
    vr_address = pc_address + 0x26000 # works for rtti offsets at the very least
    #if vr_address - 0x140000000 >= 0x036F1118 and vr_address - 0x140000000 <= 0x0392E0C0:
    outfile.write("%16X\t%16X\n" % (vr_address, pc_address))
