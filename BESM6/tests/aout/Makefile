AS      = b6as
LD      = b6ld
DISASM  = b6disasm
NM      = b6nm

all:    hello

clean:
	rm -f *.o *.dis *.nm hello

hello:  hello.o
	$(LD) $< -o $@
	$(DISASM) -c $@ > $@.dis
	$(NM) $@ > $@.nm
