# Makefile for Project 0: Working with FUSE (xv6 File System Mount & Consistency Checker)
# Automatically detects libfuse3, libfuse2, or links directly to libfuse3.so.3.

CC      := gcc
CFLAGS  := -Wall -Wextra -Wno-stringop-truncation -O2 -D_FILE_OFFSET_BITS=64

# Detect FUSE flags automatically
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null || pkg-config --cflags fuse 2>/dev/null)
FUSE_LIBS   := $(shell pkg-config --libs fuse3 2>/dev/null || pkg-config --libs fuse 2>/dev/null)

ifeq ($(strip $(FUSE_LIBS)),)
  ifneq ($(wildcard /usr/lib/x86_64-linux-gnu/libfuse3.so.3),)
    FUSE_LIBS := /usr/lib/x86_64-linux-gnu/libfuse3.so.3
  else ifneq ($(wildcard /lib64/libfuse3.so.3),)
    FUSE_LIBS := /lib64/libfuse3.so.3
  else
    FUSE_LIBS := -lfuse3
  endif
endif

TARGETS := xv6_fuse xv6_fsck

all: $(TARGETS)

xv6_fuse: xv6_fuse.c fs.h
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) xv6_fuse.c $(FUSE_LIBS) -o xv6_fuse

xv6_fsck: xv6_fsck.c fs.h
	$(CC) $(CFLAGS) xv6_fsck.c -o xv6_fsck

test: all
	bash ./test_xv6_fuse.sh

clean:
	rm -f $(TARGETS) test_fs.img
	rm -rf mnt_test
