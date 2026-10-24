import os
import re
import sys
import os

# usage: ./genMappings.py [path to csv] [game version in quotes (i.e "1.2.72")]

file = open(sys.argv[1])
version = sys.argv[2]
outfile = open('../mappings/%s_0.0.0.txt' % version, 'w')

id = 0
for line in file.readlines():
    matches = re.match('\"([^"]*)\",\"(14[^"]*)\",\"([^"]*)\",\"([^"]*)\",\"([^"]*)\",\"([^"]*)\",\"([^"]*)\"', line)
    if matches:
        vr_address = int(matches.group(2), 16)
        outfile.write("%16X\t%16X\n" % (vr_address, id))
        id += 1
