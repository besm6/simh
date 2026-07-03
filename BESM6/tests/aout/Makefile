CC      = b6cc
LD      = b6ld
DISASM  = b6disasm
NM      = b6nm

all:    test1

clean:
	rm -f *.o *.dis *.nm test1

test1:  test1.o
	$(LD) $< -o $@
	$(DISASM) -c $@ > $@.dis
	$(NM) $@ > $@.nm
