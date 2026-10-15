# shared.mk
#
# Environment variables for the Linux build host.

# Make sure we know a few things about the architecture 
UNAME := $(shell uname)
ARCH := $(shell uname -m)
ifneq (,$(filter $(ARCH), x86_64 amd64))
  X86-64 := 1
  X86_64 := 1
  AMD64 := 1
  ARCHFAMILY := x86_64
else
  ARCHFAMILY := $(ARCH)
endif

# configure some variables dependent upon what type of system this is

# Linux
# Reject unsupported hosts before selecting tools or creating build output.
ifneq ($(UNAME), Linux)
  $(error Unsupported build host $(UNAME); droniNG supports Linux only)
endif
OSFAMILY := linux
export LINUX := 1
