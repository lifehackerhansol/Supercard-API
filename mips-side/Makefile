export SCDS2_TOOLCHAIN			?= /opt/scds2_toolchain

export WONDERFUL_TOOLCHAIN	?= /opt/wonderful
MIPSEL_ELF_PATH	?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-mipsel-elf/bin

# Verbose flag
# ------------

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

.PHONY: all libds2 clean install

all: sys/crts/scds2_start.o libds2

sys/crts/scds2_start.o: sys/crts/scds2_start.S
	$(MIPSEL_ELF_PATH)/mipsel-elf-gcc -march=mips32 -x assembler-with-cpp -g -Iinclude -D__ASSEMBLY__ -c $< -o $@

libds2:
	$(MAKE) -f Makefile.libds2

install: all
	$(V)mkdir -p $(SCDS2_TOOLCHAIN)/sys/crts
	$(V)cp -a sys/crts/scds2_start.S sys/crts/scds2_start.o sys/crts/scds2.specs sys/crts/scds2.ld $(SCDS2_TOOLCHAIN)/sys/crts
	$(V)mkdir -p $(SCDS2_TOOLCHAIN)/tools/makeplug
	$(V)cp -a tools/makeplug $(SCDS2_TOOLCHAIN)/tools/makeplug
	$(MAKE) -C toolsrc/filter install
	$(MAKE) -C toolsrc/packer install
	$(MAKE) -f Makefile.libds2 install

clean:
	rm -rf build
	rm -f sys/crts/scds2_start.o
	$(MAKE) -C toolsrc/filter clean
	$(MAKE) -C toolsrc/packer clean
	$(MAKE) -f Makefile.libds2 clean
