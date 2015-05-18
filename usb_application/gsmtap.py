#!/usr/bin/env python

import socket
import array

ip="127.0.0.1"
port=4729
sp=58621
gsmtap_hdr="\x02\x04\x04"+"\x00"*13

# FIXME: Is ATR something special?

def gsmtap_send_apdu(data):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((ip, port))
    s.send(gsmtap_hdr+data.tostring())
    s.close()

if __name__ == '__main__':
    cmds = ("\xa0\xa4\x00\x00\x02\x6f\x7e\x9f\x0f",
            "\xa0\xd6\x00\x00\x0b\xff\xff\xff\xff\x09\xf1\x07\xff\xfe\x00\x03\x90\x00",
            )
    for cmd in cmds:
        gsmtap_send_apdu(array.array('B', cmd))