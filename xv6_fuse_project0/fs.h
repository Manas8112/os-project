/*
 * fs.h - On-disk file system format for xv6.
 * Compatible with MIT PDOS xv6-riscv (default, BSIZE=1024) and xv6-public x86 (-DXV6_X86, BSIZE=512).
 * Reference: https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.h
 *            https://github.com/mit-pdos/xv6-public/blob/master/fs.h
 */
#ifndef XV6_FS_H
#define XV6_FS_H

#include <stdint.h>

typedef uint32_t uint;
typedef uint16_t ushort;
typedef uint8_t  uchar;

#define ROOTINO  1   /* root i-number */

#ifdef XV6_X86
#define BSIZE    512  /* block size for legacy x86 xv6-public */
#else
#define BSIZE    1024 /* block size for xv6-riscv */
#endif

/*
 * Disk layout:
 * [ boot block | super block | log | inode blocks | free bit map | data blocks ]
 *
 * mkfs computes the super block and builds an initial file system.
 * The super block describes the disk layout:
 */
struct superblock {
#ifndef XV6_X86
  uint magic;        /* Must be FSMAGIC (0x10203040) in xv6-riscv */
#endif
  uint size;         /* Size of file system image (blocks) */
  uint nblocks;      /* Number of data blocks */
  uint ninodes;      /* Number of inodes */
  uint nlog;         /* Number of log blocks */
  uint logstart;     /* Block number of first log block */
  uint inodestart;   /* Block number of first inode block */
  uint bmapstart;    /* Block number of first free map block */
};

#define FSMAGIC  0x10203040

#define NDIRECT   12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE   (NDIRECT + NINDIRECT)

/* File types (from xv6 stat.h) */
#define T_FREE    0   /* Unallocated inode */
#define T_DIR     1   /* Directory */
#define T_FILE    2   /* Regular file */
#define T_DEVICE  3   /* Special device */

/* On-disk inode structure */
struct dinode {
  short type;               /* File type (0, T_DIR, T_FILE, T_DEVICE) */
  short major;              /* Major device number (T_DEVICE only) */
  short minor;              /* Minor device number (T_DEVICE only) */
  short nlink;              /* Number of links to inode in file system */
  uint  size;               /* Size of file (bytes) */
  uint  addrs[NDIRECT + 1]; /* Data block addresses (0..11 direct, 12 indirect) */
};

/* Inodes per block */
#define IPB           (BSIZE / sizeof(struct dinode))

/* Block containing inode i */
#define IBLOCK(i, sb) ((i) / IPB + (sb).inodestart)

/* Bitmap bits per block */
#define BPB           (BSIZE * 8)

/* Block of free map containing bit for block b */
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

/* Directory is a file containing a sequence of dirent structures */
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char   name[DIRSIZ];
};

#endif /* XV6_FS_H */
