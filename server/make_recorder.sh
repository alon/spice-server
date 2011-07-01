#!/bin/bash
if [ ! -e red_parse_qxl.c ]; then
    echo run in server subdir
    exit 1
fi
if [ -e red_record_qxl.c ]; then
    echo not overwriting red_record_qxl.c
    exit 1
fi
cp red_parse_qxl.c red_record_qxl.c.base
sed -e 's/\(static.*\)red_get\([a-z_]*(\)/\1red_record\2int fd, /' -e 's/^void red_get_\(.*\)(/void red_record_\1(int fd, /' < red_record_qxl.c.base > red_record_qxl.1.c
sed -e 's/red_get\(.*\)(/red_record\1(fd, /' < red_record_qxl.1.c > red_record_qxl.c
diff -u red_parse_qxl.c red_record_qxl.c
