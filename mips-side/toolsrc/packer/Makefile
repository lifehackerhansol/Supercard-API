# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2023-2024

# Tools
# -----

STRIP		:= -s
BINMODE		:= 755

HOSTCC		?= gcc
CP		:= cp
RM		:= rm -rf
INSTALL		:= install

# Verbose flag
# ------------

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

# Targets
# -------

.PHONY: all clean install

all: packer

packer: main.c
	$(V)$(HOSTCC) -Wall -Wextra -std=gnu11 -O3 -lz -o $@ $<

clean:
	rm -rf packer

INSTALLDIR	?= /opt/scds2_toolchain/tools/packer
INSTALLDIR_ABS	:= $(abspath $(INSTALLDIR))

install: all
	@echo "  INSTALL $(INSTALLDIR_ABS)"
	@test $(INSTALLDIR_ABS)
	$(V)$(RM) $(INSTALLDIR_ABS)
	$(V)$(INSTALL) -d $(INSTALLDIR_ABS)
	$(V)$(INSTALL) $(STRIP) -m $(BINMODE) packer $(INSTALLDIR_ABS)
	$(V)$(CP) COPYING $(INSTALLDIR_ABS)
