/*
 * xv6_fuse.c - FUSE driver to mount xv6's fs.img on Linux.
 *
 * Implements:
 *   - Superblock, inode, and directory parsing reusing structs from fs.h
 *   - getattr, readdir, read, write, create, mkdir, unlink, truncate
 *   - Also supports rmdir, mknod, open, utimens, chmod, chown, fsync, statfs
 *     so standard Linux tools (cp, cat, mkdir, rm, touch, truncate) work seamlessly
 *   - Bitmap block allocation/deallocation (balloc / bfree)
 *   - Direct and singly-indirect block management (bmap / itrunc)
 *   - Built-in --self-test mode for automated verification without root/FUSE privileges
 *
 * References:
 *   - MIT PDOS xv6-riscv fs.h / fs.c: https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.h
 *   - libfuse API Reference: https://libfuse.github.io/doxygen/structfuse__operations.html
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#include "fs.h"

/*
 * Support both libfuse3 (<fuse3/fuse.h>), libfuse2 (<fuse.h>), and a fallback
 * ABI header for environments where libfuse3.so.3 is installed without -dev headers.
 */
#if defined(__has_include)
#  if __has_include(<fuse3/fuse.h>)
#    define FUSE_USE_VERSION 31
#    include <fuse3/fuse.h>
#  elif __has_include(<fuse.h>)
#    define FUSE_USE_VERSION 26
#    include <fuse.h>
#  else
#    define FUSE_USE_VERSION 31
#    define XV6_EMBEDDED_FUSE3_ABI 1
#  endif
#else
#  define FUSE_USE_VERSION 26
#  include <fuse.h>
#endif

#ifdef XV6_EMBEDDED_FUSE3_ABI
struct fuse_file_info {
    int flags;
    uint64_t fh;
    uint64_t lock_owner;
    uint32_t poll_events;
};
struct fuse_conn_info;
struct fuse_config;
struct fuse_bufvec;
struct fuse_pollhandle;
enum fuse_readdir_flags { FUSE_READDIR_PLUS = (1 << 0) };
enum fuse_fill_dir_flags { FUSE_FILL_DIR_PLUS = (1 << 1) };
typedef int (*fuse_fill_dir_t)(void *buf, const char *name,
                               const struct stat *stbuf, off_t off,
                               enum fuse_fill_dir_flags flags);

/* Exact 42-pointer ABI layout (0x150 bytes) for libfuse3 (3.0 - 3.16) */
struct fuse_operations {
    int (*getattr)(const char *, struct stat *, struct fuse_file_info *fi);
    int (*readlink)(const char *, char *, size_t);
    int (*mknod)(const char *, mode_t, dev_t);
    int (*mkdir)(const char *, mode_t);
    int (*unlink)(const char *);
    int (*rmdir)(const char *);
    int (*symlink)(const char *, const char *);
    int (*rename)(const char *, const char *, unsigned int flags);
    int (*link)(const char *, const char *);
    int (*chmod)(const char *, mode_t, struct fuse_file_info *fi);
    int (*chown)(const char *, uid_t, gid_t, struct fuse_file_info *fi);
    int (*truncate)(const char *, off_t, struct fuse_file_info *fi);
    int (*open)(const char *, struct fuse_file_info *);
    int (*read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
    int (*write)(const char *, const char *, size_t, off_t, struct fuse_file_info *);
    int (*statfs)(const char *, struct statvfs *);
    int (*flush)(const char *, struct fuse_file_info *);
    int (*release)(const char *, struct fuse_file_info *);
    int (*fsync)(const char *, int, struct fuse_file_info *);
    int (*setxattr)(const char *, const char *, const char *, size_t, int);
    int (*getxattr)(const char *, const char *, char *, size_t);
    int (*listxattr)(const char *, char *, size_t);
    int (*removexattr)(const char *, const char *);
    int (*opendir)(const char *, struct fuse_file_info *);
    int (*readdir)(const char *, void *, fuse_fill_dir_t, off_t,
                   struct fuse_file_info *, enum fuse_readdir_flags);
    int (*releasedir)(const char *, struct fuse_file_info *);
    int (*fsyncdir)(const char *, int, struct fuse_file_info *);
    void *(*init)(struct fuse_conn_info *conn, struct fuse_config *cfg);
    void (*destroy)(void *private_data);
    int (*access)(const char *, int);
    int (*create)(const char *, mode_t, struct fuse_file_info *);
    int (*lock)(const char *, struct fuse_file_info *, int cmd, struct flock *);
    int (*utimens)(const char *, const struct timespec tv[2], struct fuse_file_info *fi);
    int (*bmap)(const char *, size_t blocksize, uint64_t *idx);
    int (*ioctl)(const char *, unsigned int cmd, void *arg,
                 struct fuse_file_info *, unsigned int flags, void *data);
    int (*poll)(const char *, struct fuse_file_info *,
                struct fuse_pollhandle *ph, unsigned *reventsp);
    int (*write_buf)(const char *, struct fuse_bufvec *buf, off_t off,
                     struct fuse_file_info *);
    int (*read_buf)(const char *, struct fuse_bufvec **bufp,
                    size_t size, off_t off, struct fuse_file_info *);
    int (*flock)(const char *, struct fuse_file_info *, int op);
    int (*fallocate)(const char *, int, off_t, off_t, struct fuse_file_info *);
    ssize_t (*copy_file_range)(const char *path_in, struct fuse_file_info *fi_in,
                               off_t offset_in, const char *path_out,
                               struct fuse_file_info *fi_out, off_t offset_out,
                               size_t size, int flags);
    off_t (*lseek)(const char *, off_t off, int whence, struct fuse_file_info *);
};

int fuse_main_real(int argc, char *argv[], const struct fuse_operations *op,
                   size_t op_size, void *user_data);
#define fuse_main(argc, argv, op, user_data) \
    fuse_main_real(argc, argv, op, sizeof(*(op)), user_data)
#endif

/* Global state for the mounted xv6 image */
static int img_fd = -1;
static struct superblock sb;
static time_t mount_time;

/* Read block `bno` from fs.img into `buf` (BSIZE bytes) */
static int bread_block(uint bno, void *buf) {
    if (bno >= sb.size) {
        return -EIO;
    }
    off_t off = (off_t)bno * BSIZE;
    ssize_t n = pread(img_fd, buf, BSIZE, off);
    if (n != BSIZE) {
        return -EIO;
    }
    return 0;
}

/* Write block `bno` from `buf` (BSIZE bytes) to fs.img */
static int bwrite_block(uint bno, const void *buf) {
    if (bno >= sb.size) {
        return -EIO;
    }
    off_t off = (off_t)bno * BSIZE;
    ssize_t n = pwrite(img_fd, buf, BSIZE, off);
    if (n != BSIZE) {
        return -EIO;
    }
    return 0;
}

/* Zero out disk block `bno` */
static int bzero_block(uint bno) {
    uchar zero[BSIZE];
    memset(zero, 0, BSIZE);
    return bwrite_block(bno, zero);
}

/* First data block index in fs.img */
static uint first_data_block(void) {
    uint bmap_blocks = (sb.size + BPB - 1) / BPB;
    uint calc_start = sb.bmapstart + bmap_blocks;
    uint sb_start = (sb.size > sb.nblocks) ? (sb.size - sb.nblocks) : calc_start;
    return (sb_start > calc_start) ? sb_start : calc_start;
}

/* Allocate a zeroed disk data block and mark it in the bitmap */
static int balloc(uint *bno_out) {
    uchar bp[BSIZE];
    uint dstart = first_data_block();

    for (uint b = 0; b < sb.size; b += BPB) {
        uint bblock = BBLOCK(b, sb);
        if (bread_block(bblock, bp) < 0) {
            return -EIO;
        }
        for (uint bi = 0; bi < BPB && (b + bi) < sb.size; bi++) {
            uint cand = b + bi;
            if (cand < dstart) {
                continue;
            }
            uchar m = (uchar)(1u << (bi % 8));
            if ((bp[bi / 8] & m) == 0) {
                bp[bi / 8] |= m;
                if (bwrite_block(bblock, bp) < 0) {
                    return -EIO;
                }
                if (bzero_block(cand) < 0) {
                    return -EIO;
                }
                *bno_out = cand;
                return 0;
            }
        }
    }
    return -ENOSPC;
}

/* Free a disk data block in the bitmap */
static int bfree(uint bno) {
    uint dstart = first_data_block();
    if (bno < dstart || bno >= sb.size) {
        return -EINVAL;
    }
    uchar bp[BSIZE];
    uint bblock = BBLOCK(bno, sb);
    if (bread_block(bblock, bp) < 0) {
        return -EIO;
    }
    uint bi = bno % BPB;
    uchar m = (uchar)(1u << (bi % 8));
    if ((bp[bi / 8] & m) == 0) {
        fprintf(stderr, "xv6_fuse warning: freeing already-free block %u\n", bno);
    }
    bp[bi / 8] &= (uchar)~m;
    return bwrite_block(bblock, bp);
}

/* Read on-disk inode `inum` */
static int iread(uint inum, struct dinode *dip) {
    if (inum == 0 || inum >= sb.ninodes) {
        return -ENOENT;
    }
    uchar buf[BSIZE];
    uint bno = IBLOCK(inum, sb);
    if (bread_block(bno, buf) < 0) {
        return -EIO;
    }
    struct dinode *blk_inodes = (struct dinode *)buf;
    *dip = blk_inodes[inum % IPB];
    return 0;
}

/* Write on-disk inode `inum` */
static int iupdate(uint inum, const struct dinode *dip) {
    if (inum == 0 || inum >= sb.ninodes) {
        return -EINVAL;
    }
    uchar buf[BSIZE];
    uint bno = IBLOCK(inum, sb);
    if (bread_block(bno, buf) < 0) {
        return -EIO;
    }
    struct dinode *blk_inodes = (struct dinode *)buf;
    blk_inodes[inum % IPB] = *dip;
    return bwrite_block(bno, buf);
}

/* Allocate a free inode on disk with type `type` */
static int ialloc(short type, uint *inum_out, struct dinode *dip_out) {
    for (uint inum = 1; inum < sb.ninodes; inum++) {
        struct dinode dip;
        if (iread(inum, &dip) < 0) {
            return -EIO;
        }
        if (dip.type == 0) {
            memset(&dip, 0, sizeof(dip));
            dip.type = type;
            dip.nlink = 1;
            dip.size = 0;
            int r = iupdate(inum, &dip);
            if (r < 0) {
                return r;
            }
            *inum_out = inum;
            if (dip_out) {
                *dip_out = dip;
            }
            return 0;
        }
    }
    return -ENOSPC;
}

/*
 * Map logical block index `bn` (0 .. MAXFILE-1) of inode `dip` to disk block number.
 * If `alloc` is true and the block is not yet allocated, allocate it (and the indirect
 * block if needed) and update `dip` on disk.
 */
static int bmap(uint inum, struct dinode *dip, uint bn, bool alloc, uint *disk_bno) {
    if (bn >= MAXFILE) {
        return -EFBIG;
    }

    if (bn < NDIRECT) {
        uint addr = dip->addrs[bn];
        if (addr == 0 && alloc) {
            int r = balloc(&addr);
            if (r < 0) {
                return r;
            }
            dip->addrs[bn] = addr;
            r = iupdate(inum, dip);
            if (r < 0) {
                return r;
            }
        }
        *disk_bno = addr;
        return 0;
    }

    bn -= NDIRECT;
    uint ind_bno = dip->addrs[NDIRECT];
    if (ind_bno == 0) {
        if (!alloc) {
            *disk_bno = 0;
            return 0;
        }
        int r = balloc(&ind_bno);
        if (r < 0) {
            return r;
        }
        dip->addrs[NDIRECT] = ind_bno;
        r = iupdate(inum, dip);
        if (r < 0) {
            return r;
        }
    }

    uint indirect[NINDIRECT];
    if (bread_block(ind_bno, indirect) < 0) {
        return -EIO;
    }
    uint addr = indirect[bn];
    if (addr == 0 && alloc) {
        int r = balloc(&addr);
        if (r < 0) {
            return r;
        }
        indirect[bn] = addr;
        if (bwrite_block(ind_bno, indirect) < 0) {
            return -EIO;
        }
    }
    *disk_bno = addr;
    return 0;
}

/* Read `n` bytes from inode `dip` starting at byte offset `off` */
static ssize_t readi_inode(uint inum, struct dinode *dip, char *dst, off_t off, size_t n) {
    if (off < 0 || (uint)off >= dip->size) {
        return 0;
    }
    if (off + n > dip->size) {
        n = dip->size - (uint)off;
    }

    size_t tot = 0;
    uchar buf[BSIZE];
    while (tot < n) {
        uint bn = (uint)(off / BSIZE);
        uint boff = (uint)(off % BSIZE);
        size_t chunk = BSIZE - boff;
        if (chunk > n - tot) {
            chunk = n - tot;
        }

        uint disk_bno = 0;
        int r = bmap(inum, dip, bn, false, &disk_bno);
        if (r < 0) {
            return r;
        }
        if (disk_bno == 0) {
            memset(dst + tot, 0, chunk);
        } else {
            if (bread_block(disk_bno, buf) < 0) {
                return -EIO;
            }
            memcpy(dst + tot, buf + boff, chunk);
        }
        tot += chunk;
        off += (off_t)chunk;
    }
    return (ssize_t)tot;
}

/* Write `n` bytes to inode `dip` starting at byte offset `off` */
static ssize_t writei_inode(uint inum, struct dinode *dip, const char *src, off_t off, size_t n) {
    if (off < 0 || (size_t)off + n > (size_t)MAXFILE * BSIZE) {
        return -EFBIG;
    }

    size_t tot = 0;
    uchar buf[BSIZE];
    while (tot < n) {
        uint bn = (uint)(off / BSIZE);
        uint boff = (uint)(off % BSIZE);
        size_t chunk = BSIZE - boff;
        if (chunk > n - tot) {
            chunk = n - tot;
        }

        uint disk_bno = 0;
        int r = bmap(inum, dip, bn, true, &disk_bno);
        if (r < 0) {
            break;
        }
        if (bread_block(disk_bno, buf) < 0) {
            return -EIO;
        }
        memcpy(buf + boff, src + tot, chunk);
        if (bwrite_block(disk_bno, buf) < 0) {
            return -EIO;
        }
        tot += chunk;
        off += (off_t)chunk;
    }

    if ((uint)off > dip->size) {
        dip->size = (uint)off;
    }
    int r = iupdate(inum, dip);
    if (r < 0) {
        return r;
    }
    return (ssize_t)tot;
}

/*
 * Truncate or extend inode `dip` to `new_size` bytes.
 * Frees any direct and indirect blocks past `new_size` and updates bitmap.
 */
static int itrunc_size(uint inum, struct dinode *dip, off_t new_size) {
    if (new_size < 0 || (size_t)new_size > (size_t)MAXFILE * BSIZE) {
        return -EFBIG;
    }

    uint new_blocks = (new_size == 0) ? 0 : (uint)((new_size + BSIZE - 1) / BSIZE);

    /* Free unneeded direct blocks */
    for (uint i = new_blocks; i < NDIRECT; i++) {
        if (dip->addrs[i] != 0) {
            int r = bfree(dip->addrs[i]);
            if (r < 0) {
                return r;
            }
            dip->addrs[i] = 0;
        }
    }

    /* Handle indirect blocks */
    if (dip->addrs[NDIRECT] != 0) {
        uint indirect[NINDIRECT];
        if (bread_block(dip->addrs[NDIRECT], indirect) < 0) {
            return -EIO;
        }
        uint start_ind = (new_blocks > NDIRECT) ? (new_blocks - NDIRECT) : 0;
        bool modified_ind = false;
        for (uint j = start_ind; j < NINDIRECT; j++) {
            if (indirect[j] != 0) {
                int r = bfree(indirect[j]);
                if (r < 0) {
                    return r;
                }
                indirect[j] = 0;
                modified_ind = true;
            }
        }
        if (new_blocks <= NDIRECT) {
            int r = bfree(dip->addrs[NDIRECT]);
            if (r < 0) {
                return r;
            }
            dip->addrs[NDIRECT] = 0;
        } else if (modified_ind) {
            if (bwrite_block(dip->addrs[NDIRECT], indirect) < 0) {
                return -EIO;
            }
        }
    }

    /* If shrinking to a partial block, zero out the tail of the last retained block */
    if ((uint)new_size < dip->size && (new_size % BSIZE) != 0) {
        uint last_bn = (uint)(new_size / BSIZE);
        uint disk_bno = 0;
        if (bmap(inum, dip, last_bn, false, &disk_bno) == 0 && disk_bno != 0) {
            uchar buf[BSIZE];
            if (bread_block(disk_bno, buf) == 0) {
                uint tail_off = (uint)(new_size % BSIZE);
                memset(buf + tail_off, 0, BSIZE - tail_off);
                bwrite_block(disk_bno, buf);
            }
        }
    }

    /* If extending the file, allocate zeroed blocks up to new_size */
    if ((uint)new_size > dip->size) {
        for (uint bn = dip->size / BSIZE; bn < new_blocks; bn++) {
            uint disk_bno = 0;
            int r = bmap(inum, dip, bn, true, &disk_bno);
            if (r < 0) {
                return r;
            }
        }
    }

    dip->size = (uint)new_size;
    return iupdate(inum, dip);
}

/* Compare a NUL-terminated string `s` with an xv6 dirent name (up to DIRSIZ bytes) */
static bool dirent_name_eq(const char *s, const char de_name[DIRSIZ]) {
    size_t len = strlen(s);
    if (len > DIRSIZ) {
        return false;
    }
    if (strncmp(s, de_name, DIRSIZ) != 0) {
        return false;
    }
    return true;
}

/*
 * Look up `name` in directory `dp` (inode `dir_inum`).
 * If found, sets `*inum_out` and optionally `*off_out` (byte offset in directory).
 */
static int dirlookup(uint dir_inum, struct dinode *dp, const char *name,
                     uint *inum_out, off_t *off_out) {
    if (dp->type != T_DIR) {
        return -ENOTDIR;
    }
    if (strlen(name) > DIRSIZ) {
        return -ENAMETOOLONG;
    }

    struct dirent de;
    for (uint off = 0; off < dp->size; off += sizeof(de)) {
        if (readi_inode(dir_inum, dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
            return -EIO;
        }
        if (de.inum == 0) {
            continue;
        }
        if (dirent_name_eq(name, de.name)) {
            if (inum_out) {
                *inum_out = de.inum;
            }
            if (off_out) {
                *off_out = (off_t)off;
            }
            return 0;
        }
    }
    return -ENOENT;
}

/* Add a directory entry `(name, inum)` into directory `dp` (`dir_inum`) */
static int dirlink(uint dir_inum, struct dinode *dp, const char *name, uint inum) {
    if (strlen(name) > DIRSIZ) {
        return -ENAMETOOLONG;
    }

    uint existing = 0;
    if (dirlookup(dir_inum, dp, name, &existing, NULL) == 0) {
        return -EEXIST;
    }

    struct dirent de;
    uint off = 0;
    for (off = 0; off < dp->size; off += sizeof(de)) {
        if (readi_inode(dir_inum, dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
            return -EIO;
        }
        if (de.inum == 0) {
            break;
        }
    }

    memset(&de, 0, sizeof(de));
    de.inum = (ushort)inum;
    strncpy(de.name, name, DIRSIZ);

    ssize_t w = writei_inode(dir_inum, dp, (const char *)&de, off, sizeof(de));
    if (w != (ssize_t)sizeof(de)) {
        return (w < 0) ? (int)w : -ENOSPC;
    }
    return 0;
}

/*
 * Resolve a POSIX path (e.g. "/a/b/c"):
 * - `resolve_Path`: returns the target inode number and `struct dinode`.
 * - `resolve_parent`: returns the parent directory inode and the final component `name`.
 */
static int resolve_path(const char *path, uint *inum_out, struct dinode *dip_out) {
    if (!path || path[0] != '/') {
        return -EINVAL;
    }

    uint curr_inum = ROOTINO;
    struct dinode curr_dip;
    int r = iread(curr_inum, &curr_dip);
    if (r < 0) {
        return r;
    }

    const char *p = path;
    while (*p == '/') {
        p++;
    }

    while (*p != '\0') {
        if (curr_dip.type != T_DIR) {
            return -ENOTDIR;
        }
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len > DIRSIZ) {
            return -ENAMETOOLONG;
        }

        char comp[DIRSIZ + 1];
        memcpy(comp, p, len);
        comp[len] = '\0';

        uint next_inum = 0;
        r = dirlookup(curr_inum, &curr_dip, comp, &next_inum, NULL);
        if (r < 0) {
            return r;
        }
        r = iread(next_inum, &curr_dip);
        if (r < 0) {
            return r;
        }
        curr_inum = next_inum;

        p += len;
        while (*p == '/') {
            p++;
        }
    }

    if (inum_out) {
        *inum_out = curr_inum;
    }
    if (dip_out) {
        *dip_out = curr_dip;
    }
    return 0;
}

static int resolve_parent(const char *path, uint *parent_inum_out,
                          struct dinode *parent_dip_out, char name_out[DIRSIZ + 1]) {
    if (!path || path[0] != '/') {
        return -EINVAL;
    }

    /* Strip trailing slashes */
    size_t path_len = strlen(path);
    while (path_len > 1 && path[path_len - 1] == '/') {
        path_len--;
    }
    if (path_len == 1 && path[0] == '/') {
        return -EINVAL; /* Root has no parent component */
    }

    /* Find last slash before final component */
    const char *last_slash = path;
    for (size_t i = 0; i < path_len; i++) {
        if (path[i] == '/') {
            last_slash = path + i;
        }
    }

    size_t name_len = (size_t)((path + path_len) - (last_slash + 1));
    if (name_len == 0) {
        return -EINVAL;
    }
    if (name_len > DIRSIZ) {
        return -ENAMETOOLONG;
    }
    memcpy(name_out, last_slash + 1, name_len);
    name_out[name_len] = '\0';

    size_t dir_len = (size_t)(last_slash - path);
    if (dir_len == 0) {
        dir_len = 1; /* "/" */
    }
    char *dir_path = malloc(dir_len + 1);
    if (!dir_path) {
        return -ENOMEM;
    }
    memcpy(dir_path, path, dir_len);
    dir_path[dir_len] = '\0';

    int r = resolve_path(dir_path, parent_inum_out, parent_dip_out);
    free(dir_path);
    if (r < 0) {
        return r;
    }
    if (parent_dip_out->type != T_DIR) {
        return -ENOTDIR;
    }
    return 0;
}

/* Populate a POSIX `struct stat` from an xv6 `struct dinode` */
static void dinode_to_stat(uint inum, const struct dinode *dip, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = inum;
    st->st_nlink = (dip->nlink > 0) ? (nlink_t)dip->nlink : 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_size = (off_t)dip->size;
    st->st_blksize = BSIZE;
    st->st_blocks = (blkcnt_t)((dip->size + 511) / 512);
    st->st_atime = mount_time;
    st->st_mtime = mount_time;
    st->st_ctime = mount_time;

    if (dip->type == T_DIR) {
        st->st_mode = S_IFDIR | 0755;
    } else if (dip->type == T_FILE) {
        st->st_mode = S_IFREG | 0644;
    } else if (dip->type == T_DEVICE) {
        st->st_mode = S_IFCHR | 0666;
    }
}

/* =========================================================================
 * FUSE Operations Implementation
 * ========================================================================= */

#if FUSE_USE_VERSION >= 30
static int xv6_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
#else
static int xv6_getattr(const char *path, struct stat *stbuf) {
#endif
    uint inum = 0;
    struct dinode dip;
    int r = resolve_path(path, &inum, &dip);
    if (r < 0) {
        return r;
    }
    if (dip.type == 0) {
        return -ENOENT;
    }
    dinode_to_stat(inum, &dip, stbuf);
    return 0;
}

#if FUSE_USE_VERSION >= 30
static int xv6_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
#else
static int xv6_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi) {
    (void)offset;
    (void)fi;
#endif
    uint inum = 0;
    struct dinode dp;
    int r = resolve_path(path, &inum, &dp);
    if (r < 0) {
        return r;
    }
    if (dp.type != T_DIR) {
        return -ENOTDIR;
    }

    struct dirent de;
    char name[DIRSIZ + 1];
    for (uint off = 0; off < dp.size; off += sizeof(de)) {
        if (readi_inode(inum, &dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
            return -EIO;
        }
        if (de.inum == 0) {
            continue;
        }
        memcpy(name, de.name, DIRSIZ);
        name[DIRSIZ] = '\0';

        struct dinode entry_dip;
        struct stat st;
        struct stat *stp = NULL;
        if (iread(de.inum, &entry_dip) == 0 && entry_dip.type != 0) {
            dinode_to_stat(de.inum, &entry_dip, &st);
            stp = &st;
        }
#if FUSE_USE_VERSION >= 30
        filler(buf, name, stp, 0, 0);
#else
        filler(buf, name, stp, 0);
#endif
    }
    return 0;
}

static int xv6_open(const char *path, struct fuse_file_info *fi) {
    uint inum = 0;
    struct dinode dip;
    int r = resolve_path(path, &inum, &dip);
    if (r < 0) {
        return r;
    }
    if (dip.type == T_DIR) {
        return -EISDIR;
    }
    if (fi && (fi->flags & O_TRUNC)) {
        r = itrunc_size(inum, &dip, 0);
        if (r < 0) {
            return r;
        }
    }
    return 0;
}

static int xv6_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void)fi;
    uint inum = 0;
    struct dinode dip;
    int r = resolve_path(path, &inum, &dip);
    if (r < 0) {
        return r;
    }
    if (dip.type == T_DIR) {
        return -EISDIR;
    }
    ssize_t n = readi_inode(inum, &dip, buf, offset, size);
    return (int)n;
}

static int xv6_write(const char *path, const char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    (void)fi;
    uint inum = 0;
    struct dinode dip;
    int r = resolve_path(path, &inum, &dip);
    if (r < 0) {
        return r;
    }
    if (dip.type == T_DIR) {
        return -EISDIR;
    }
    /* If writing past current EOF with a gap, zero-extend to offset first */
    if ((uint)offset > dip.size) {
        r = itrunc_size(inum, &dip, offset);
        if (r < 0) {
            return r;
        }
    }
    ssize_t n = writei_inode(inum, &dip, buf, offset, size);
    return (int)n;
}

static int xv6_create_file(const char *path, mode_t mode) {
    (void)mode;
    uint parent_inum = 0;
    struct dinode parent_dp;
    char name[DIRSIZ + 1];
    int r = resolve_parent(path, &parent_inum, &parent_dp, name);
    if (r < 0) {
        return r;
    }

    uint existing_inum = 0;
    if (dirlookup(parent_inum, &parent_dp, name, &existing_inum, NULL) == 0) {
        return -EEXIST;
    }

    uint new_inum = 0;
    struct dinode new_dip;
    r = ialloc(T_FILE, &new_inum, &new_dip);
    if (r < 0) {
        return r;
    }

    r = dirlink(parent_inum, &parent_dp, name, new_inum);
    if (r < 0) {
        /* Roll back inode allocation on failure */
        memset(&new_dip, 0, sizeof(new_dip));
        iupdate(new_inum, &new_dip);
        return r;
    }
    return 0;
}

static int xv6_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    return xv6_create_file(path, mode);
}

static int xv6_mknod(const char *path, mode_t mode, dev_t rdev) {
    (void)rdev;
    return xv6_create_file(path, mode);
}

static int xv6_mkdir(const char *path, mode_t mode) {
    (void)mode;
    uint parent_inum = 0;
    struct dinode parent_dp;
    char name[DIRSIZ + 1];
    int r = resolve_parent(path, &parent_inum, &parent_dp, name);
    if (r < 0) {
        return r;
    }

    uint existing_inum = 0;
    if (dirlookup(parent_inum, &parent_dp, name, &existing_inum, NULL) == 0) {
        return -EEXIST;
    }

    uint new_inum = 0;
    struct dinode new_dp;
    r = ialloc(T_DIR, &new_inum, &new_dp);
    if (r < 0) {
        return r;
    }

    /* Create "." and ".." entries inside the new directory */
    r = dirlink(new_inum, &new_dp, ".", new_inum);
    if (r < 0) {
        goto rollback;
    }
    r = dirlink(new_inum, &new_dp, "..", parent_inum);
    if (r < 0) {
        goto rollback;
    }

    /* Link the new directory in its parent directory */
    r = dirlink(parent_inum, &parent_dp, name, new_inum);
    if (r < 0) {
        goto rollback;
    }

    return 0;

rollback:
    itrunc_size(new_inum, &new_dp, 0);
    memset(&new_dp, 0, sizeof(new_dp));
    iupdate(new_inum, &new_dp);
    return r;
}

/* Check whether a T_DIR inode is empty (contains only "." and "..") */
static bool is_dir_empty(uint dir_inum, struct dinode *dp) {
    struct dirent de;
    for (uint off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
        if (readi_inode(dir_inum, dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
            return false;
        }
        if (de.inum != 0) {
            return false;
        }
    }
    return true;
}

static int xv6_unlink(const char *path) {
    uint parent_inum = 0;
    struct dinode parent_dp;
    char name[DIRSIZ + 1];
    int r = resolve_parent(path, &parent_inum, &parent_dp, name);
    if (r < 0) {
        return r;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return -EINVAL;
    }

    uint target_inum = 0;
    off_t de_off = 0;
    r = dirlookup(parent_inum, &parent_dp, name, &target_inum, &de_off);
    if (r < 0) {
        return r;
    }
    if (target_inum == ROOTINO) {
        return -EBUSY;
    }

    struct dinode target_dip;
    r = iread(target_inum, &target_dip);
    if (r < 0) {
        return r;
    }

    if (target_dip.type == T_DIR && !is_dir_empty(target_inum, &target_dip)) {
        return -ENOTEMPTY;
    }

    /* Clear the directory entry in parent */
    struct dirent empty_de;
    memset(&empty_de, 0, sizeof(empty_de));
    if (writei_inode(parent_inum, &parent_dp, (const char *)&empty_de,
                     de_off, sizeof(empty_de)) != sizeof(empty_de)) {
        return -EIO;
    }

    /* Decrement target link count and free blocks/inode if 0 */
    if (target_dip.nlink > 0) {
        target_dip.nlink--;
    }
    if (target_dip.nlink == 0 || target_dip.type == T_DIR) {
        r = itrunc_size(target_inum, &target_dip, 0);
        if (r < 0) {
            return r;
        }
        memset(&target_dip, 0, sizeof(target_dip));
    }
    return iupdate(target_inum, &target_dip);
}

static int xv6_rmdir(const char *path) {
    uint inum = 0;
    struct dinode dip;
    int r = resolve_path(path, &inum, &dip);
    if (r < 0) {
        return r;
    }
    if (dip.type != T_DIR) {
        return -ENOTDIR;
    }
    return xv6_unlink(path);
}

#if FUSE_USE_VERSION >= 30
static int xv6_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
#else
static int xv6_truncate(const char *path, off_t size) {
#endif
    uint inum = 0;
    struct dinode dip;
    int r = resolve_path(path, &inum, &dip);
    if (r < 0) {
        return r;
    }
    if (dip.type == T_DIR) {
        return -EISDIR;
    }
    return itrunc_size(inum, &dip, size);
}

/* No-op metadata helpers so Linux cp / touch / chmod / chown succeed cleanly */
#if FUSE_USE_VERSION >= 30
static int xv6_utimens(const char *path, const struct timespec tv[2],
                       struct fuse_file_info *fi) {
    (void)tv;
    (void)fi;
#else
static int xv6_utimens(const char *path, const struct timespec tv[2]) {
    (void)tv;
#endif
    uint inum = 0;
    struct dinode dip;
    return resolve_path(path, &inum, &dip);
}

#if FUSE_USE_VERSION >= 30
static int xv6_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)mode;
    (void)fi;
#else
static int xv6_chmod(const char *path, mode_t mode) {
    (void)mode;
#endif
    uint inum = 0;
    struct dinode dip;
    return resolve_path(path, &inum, &dip);
}

#if FUSE_USE_VERSION >= 30
static int xv6_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)uid;
    (void)gid;
    (void)fi;
#else
static int xv6_chown(const char *path, uid_t uid, gid_t gid) {
    (void)uid;
    (void)gid;
#endif
    uint inum = 0;
    struct dinode dip;
    return resolve_path(path, &inum, &dip);
}

static int xv6_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
    (void)path;
    (void)isdatasync;
    (void)fi;
    if (img_fd >= 0) {
        fsync(img_fd);
    }
    return 0;
}

static int xv6_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize = BSIZE;
    stbuf->f_frsize = BSIZE;
    stbuf->f_blocks = sb.nblocks;
    stbuf->f_files = sb.ninodes;
    stbuf->f_namemax = DIRSIZ;
    return 0;
}

static const struct fuse_operations xv6_oper = {
    .getattr  = xv6_getattr,
    .readdir  = xv6_readdir,
    .open     = xv6_open,
    .read     = xv6_read,
    .write    = xv6_write,
    .create   = xv6_create,
    .mknod    = xv6_mknod,
    .mkdir    = xv6_mkdir,
    .unlink   = xv6_unlink,
    .rmdir    = xv6_rmdir,
    .truncate = xv6_truncate,
    .utimens  = xv6_utimens,
    .chmod    = xv6_chmod,
    .chown    = xv6_chown,
    .fsync    = xv6_fsync,
    .statfs   = xv6_statfs,
};

/* Load and validate the xv6 superblock from block 1 */
static int load_superblock(const char *img_path) {
    img_fd = open(img_path, O_RDWR);
    if (img_fd < 0) {
        perror("open fs.img");
        return -1;
    }

    uchar buf[BSIZE];
    if (pread(img_fd, buf, BSIZE, BSIZE) != BSIZE) {
        fprintf(stderr, "Error: failed to read superblock at block 1 of %s\n", img_path);
        close(img_fd);
        img_fd = -1;
        return -1;
    }
    memcpy(&sb, buf, sizeof(sb));

#ifndef XV6_X86
    if (sb.magic != FSMAGIC) {
        fprintf(stderr, "Error: invalid xv6-riscv superblock magic 0x%x (expected 0x%x)\n",
                sb.magic, FSMAGIC);
        close(img_fd);
        img_fd = -1;
        return -1;
    }
#endif
    if (sb.size == 0 || sb.ninodes == 0 || sb.nblocks == 0) {
        fprintf(stderr, "Error: malformed superblock (size=%u, ninodes=%u, nblocks=%u)\n",
                sb.size, sb.ninodes, sb.nblocks);
        close(img_fd);
        img_fd = -1;
        return -1;
    }
    mount_time = time(NULL);
    return 0;
}

/* =========================================================================
 * Built-in Self-Test Runner (--self-test <fs.img>)
 * Exercises getattr, readdir, read, write, create, mkdir, unlink, truncate,
 * and indirect blocks directly against the FUSE callbacks.
 * ========================================================================= */
#if FUSE_USE_VERSION >= 30
static int selftest_filler(void *buf, const char *name, const struct stat *st,
                           off_t off, enum fuse_fill_dir_flags flags) {
    (void)st;
    (void)off;
    (void)flags;
#else
static int selftest_filler(void *buf, const char *name, const struct stat *st, off_t off) {
    (void)st;
    (void)off;
#endif
    char *list = (char *)buf;
    if (strlen(list) + strlen(name) + 2 < 1024) {
        strcat(list, name);
        strcat(list, " ");
    }
    return 0;
}

static int run_self_test(const char *img_path) {
    if (load_superblock(img_path) < 0) {
        return 1;
    }
    printf("[xv6_fuse --self-test] Superblock loaded: size=%u blocks, nblocks=%u, ninodes=%u, inodestart=%u, bmapstart=%u\n",
           sb.size, sb.nblocks, sb.ninodes, sb.inodestart, sb.bmapstart);

    struct stat st;
    /* 1. Test getattr on root */
#if FUSE_USE_VERSION >= 30
    int r = xv6_getattr("/", &st, NULL);
#else
    int r = xv6_getattr("/", &st);
#endif
    if (r != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "FAIL: getattr('/') returned %d\n", r);
        return 1;
    }
    printf("[PASS] getattr('/') -> ino=%lu, mode=0%o, size=%ld\n",
           (unsigned long)st.st_ino, st.st_mode, (long)st.st_size);

    /* 2. Test readdir on root */
    char dir_entries[1024] = {0};
#if FUSE_USE_VERSION >= 30
    r = xv6_readdir("/", dir_entries, selftest_filler, 0, NULL, 0);
#else
    r = xv6_readdir("/", dir_entries, selftest_filler, 0, NULL);
#endif
    if (r != 0) {
        fprintf(stderr, "FAIL: readdir('/') returned %d\n", r);
        return 1;
    }
    printf("[PASS] readdir('/') -> entries: [ %s]\n", dir_entries);

    /* 3. Test reading existing xv6 file (/README) -> simulating xv6 to Linux copy */
    char rbuf[4096] = {0};
    r = xv6_read("/README", rbuf, sizeof(rbuf) - 1, 0, NULL);
    if (r > 0) {
        printf("[PASS] read('/README') -> %d bytes: \"%.*s\"\n", r, (r > 60 ? 60 : r), rbuf);
    }

    /* 4. Test mkdir + create + write (including indirect blocks > 12 KB) -> Linux to xv6 copy */
    r = xv6_mkdir("/linuxdir", 0755);
    if (r != 0 && r != -EEXIST) {
        fprintf(stderr, "FAIL: mkdir('/linuxdir') returned %d\n", r);
        return 1;
    }
    printf("[PASS] mkdir('/linuxdir')\n");

    r = xv6_create("/linuxdir/from_linux.txt", 0644, NULL);
    if (r != 0 && r != -EEXIST) {
        fprintf(stderr, "FAIL: create('/linuxdir/from_linux.txt') returned %d\n", r);
        return 1;
    }
    const char *msg = "Copied from Linux into xv6 fs.img via FUSE!\n";
    r = xv6_write("/linuxdir/from_linux.txt", msg, strlen(msg), 0, NULL);
    if (r != (int)strlen(msg)) {
        fprintf(stderr, "FAIL: write('/linuxdir/from_linux.txt') returned %d\n", r);
        return 1;
    }
    printf("[PASS] create & write('/linuxdir/from_linux.txt') -> %d bytes\n", r);

    /* 5. Test indirect block allocation with a 20 KB file (> NDIRECT * BSIZE = 12 KB) */
    r = xv6_create("/bigfile.bin", 0644, NULL);
    if (r != 0 && r != -EEXIST) {
        fprintf(stderr, "FAIL: create('/bigfile.bin') returned %d\n", r);
        return 1;
    }
    size_t big_sz = (NDIRECT + 8) * BSIZE; /* 20 blocks: 12 direct + 8 indirect */
    char *big_out = malloc(big_sz);
    char *big_in  = malloc(big_sz);
    for (size_t i = 0; i < big_sz; i++) {
        big_out[i] = (char)((i * 37 + 13) & 0xFF);
    }
    r = xv6_write("/bigfile.bin", big_out, big_sz, 0, NULL);
    if (r != (int)big_sz) {
        fprintf(stderr, "FAIL: write('/bigfile.bin') returned %d\n", r);
        return 1;
    }
    r = xv6_read("/bigfile.bin", big_in, big_sz, 0, NULL);
    if (r != (int)big_sz || memcmp(big_out, big_in, big_sz) != 0) {
        fprintf(stderr, "FAIL: readback verification of '/bigfile.bin' failed\n");
        return 1;
    }
    printf("[PASS] Indirect block write & readback ('/bigfile.bin', %zu bytes = %d direct + 8 indirect blocks)\n",
           big_sz, NDIRECT);

    /* 6. Test truncate (shrink from 20 blocks down to 500 bytes, freeing indirect blocks) */
#if FUSE_USE_VERSION >= 30
    r = xv6_truncate("/bigfile.bin", 500, NULL);
    xv6_getattr("/bigfile.bin", &st, NULL);
#else
    r = xv6_truncate("/bigfile.bin", 500);
    xv6_getattr("/bigfile.bin", &st);
#endif
    if (r != 0 || st.st_size != 500) {
        fprintf(stderr, "FAIL: truncate('/bigfile.bin', 500) returned %d (size=%ld)\n",
                r, (long)st.st_size);
        return 1;
    }
    printf("[PASS] truncate('/bigfile.bin', 500) -> freed indirect blocks, new size=%ld\n",
           (long)st.st_size);

    /* 7. Test unlink */
    r = xv6_unlink("/bigfile.bin");
    if (r != 0) {
        fprintf(stderr, "FAIL: unlink('/bigfile.bin') returned %d\n", r);
        return 1;
    }
    printf("[PASS] unlink('/bigfile.bin') -> freed remaining blocks and inode\n");

    free(big_out);
    free(big_in);
    fsync(img_fd);
    close(img_fd);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  Mount xv6 fs.img via FUSE:\n"
            "    %s [FUSE options] <fs.img> <mountpoint>\n"
            "    Example: %s -f fs.img ./mnt\n\n"
            "  Run built-in automated self-test (no FUSE mount required):\n"
            "    %s --self-test <fs.img>\n",
            prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--self-test") == 0) {
        return run_self_test(argv[2]);
    }

    /*
     * Accept <fs.img> as the second-to-last argument (before <mountpoint>):
     *   ./xv6_fuse [FUSE options...] <fs.img> <mountpoint>
     */
    const char *img_path = argv[argc - 2];
    char resolved_img[4096];
    if (!realpath(img_path, resolved_img)) {
        perror("realpath(fs.img)");
        return 1;
    }
    if (load_superblock(resolved_img) < 0) {
        return 1;
    }

    /* Build FUSE argv removing <fs.img> and forcing single-threaded (-s) for thread-safe disk I/O */
    char **fuse_argv = calloc((size_t)argc + 2, sizeof(char *));
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = "-s";
    for (int i = 1; i < argc - 2; i++) {
        fuse_argv[fuse_argc++] = argv[i];
    }
    fuse_argv[fuse_argc++] = argv[argc - 1]; /* mountpoint */
    fuse_argv[fuse_argc] = NULL;

    int ret = fuse_main(fuse_argc, fuse_argv, &xv6_oper, NULL);
    if (img_fd >= 0) {
        fsync(img_fd);
        close(img_fd);
    }
    free(fuse_argv);
    return ret;
}
