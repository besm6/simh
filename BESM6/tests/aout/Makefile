CC      = b6cc
LD      = b6ld
DISASM  = b6disasm
NM      = b6nm

all:    test1

clean:
	rm -f *.o *.dis *.nm test1

# Assemble with the BESM-6 cross toolchain (not the host assembler).
test1.o: test1.s
	$(CC) -c -o $@ $<

test1:  test1.o
	$(LD) $< -o $@
	$(DISASM) -c $@ > $@.dis
	$(NM) $@ > $@.nm
