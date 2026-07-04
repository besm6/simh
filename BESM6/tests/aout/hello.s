//
// hello.s -- print "Hello!" to the operator console.
//
// All BESM-6 peripheral I/O goes through opcode 033 (ext): the effective
// address selects the device, the accumulator carries the datum.
// Selector 0174 drives the operator console typewriter.
//
// The console line is put in RAW mode (set ttyN raw), so each code is emitted as
// a raw byte with no character-set translation; the message is plain ASCII.
// See aout.ini.
//
        .data
msg:    .word   0110,0145,0154,0154,0157,041,015,012
len     = 8

        .text
main:
        vtm     1-len, 2        // run the body for M2 = -(len-1)..0, i.e. len times
loop:   xta     msg+len-1, 2    // base the read at msg+len-1 so those map to msg[0..len-1]
        ext     0174            // send the character to console
        vlm     loop, 2         // step M2 toward 0, branch back while nonzero
        stop
