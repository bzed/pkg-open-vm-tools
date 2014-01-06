################################################################################
### Copyright 2007 VMware, Inc.  All rights reserved.
###
### Top-level Makefile for building the VMware OSS Tools.
###
###
### This program is free software; you can redistribute it and/or modify
### it under the terms of version 2 of the GNU General Public License as
### published by the Free Software Foundation.
###
### This program is distributed in the hope that it will be useful,
### but WITHOUT ANY WARRANTY; without even the implied warranty of
### MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
### GNU General Public License for more details.
###
### You should have received a copy of the GNU General Public License
### along with this program; if not, write to the Free Software
### Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
################################################################################

SUBDIRS = 
SUBDIRS += lib
SUBDIRS += guestd
SUBDIRS += vmware-user
SUBDIRS += toolbox
SUBDIRS += hgfsclient
# hgfsmounter is only needed on Linux.
if LINUX
   SUBDIRS += hgfsmounter
endif
SUBDIRS += checkvm
SUBDIRS += libguestlib

MODULES =
MODULES += vmblock
MODULES += vmhgfs
MODULES += vmmemctl
MODULES += vmxnet

.PHONY: modules $(MODULES)
modules: $(MODULES)

$(MODULES):
if LINUX
	(cd modules/linux/$@ && \
	make)
else
	@echo 'Only Linux modules are currently available.'
	exit 1
endif

## Automake will supplement its own "distclean" and "clean" targets with these.
distclean-local:
clean-local:
if LINUX
	(cd modules/linux && \
	for MOD in $(MODULES); do \
		(cd "$$MOD" && make clean); \
	done; \
	rm -f *.o)
endif