# test1.s -- print a UTF-8 greeting to the operator console (Consul).
#
# All BESM-6 peripheral I/O goes through opcode 033 (ext / увв): the effective
# address selects the device, the accumulator (A) carries the datum.  Selector
# 0174 drives the operator console typewriter (Consul #1) -- the simplest output
# device, since it takes a whole character per instruction.  b6as has no `ext`
# mnemonic for the undedicated 033 slot, so it is written raw as `$33`.
#
# The message is KOI-7; with the console line in UNICODE mode (set ttyN unicode)
# the KOI-7 codes are rendered as UTF-8.  See aout.ini.
#
# In b6as `#` starts a comment only at the beginning of a line; a mid-line `#`
# is the constant-pool operator, so comments here are kept on their own lines.

        .data
msg:    .word   0160,0162,0151,0167,0145,0164,012
len     = 7

        .text
main:
# M2 = -7 (loop counter); walk msg[0..6] as M2 runs -7..-1.
        vtm     -len, 2
loop:   xta     msg+len, 2
# ext 0174: send the character in A to Consul #1.
        $33     0174
# step M2 toward 0, branch back while nonzero.
        vlm     loop, 2
        stop
