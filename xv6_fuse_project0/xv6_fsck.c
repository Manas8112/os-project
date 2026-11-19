/*
 * xv6_fsck.c - File System Consistency Checker (fsck) for xv6's fs.img.
 *
 * Reuses the on-disk structs from fs.h (struct superblock, struct dinode, struct dirent)
 * and validates 12 core file system consistency invariants:
 *   1. Superblock magic & geometry consistency
 *   2. Valid inode types (T_FREE, T_DIR, T_FILE, T_DEVICE)
 *   3. Inode size bounds (<= MAXFILE * BSIZE)
 *   4. Valid direct block addresses ([data_start, sb.size))
 *   5. Valid indirect block address & entries ([data_start, sb.size))
 *   6. Root directory (ROOTINO = 1) exists, is T_DIR, and "." / ".." point to ROOTINO
 *   7. Directory "." points to self and ".." points to actual parent T_DIR
 *   8. Every block in use by an inode is marked allocated (1) in the bitmap
 *   9. Every data block marked allocated (1) in the bitmap is used by an inode
 *  10. No data block is referenced more than once (direct or indirect)
 *  11. Every in-use inode is referenced in at least one directory, and all dirents point to valid inodes
 *  12. File link counts (nlink) match directory reference counts; directories have no extra hard links/loops
 *
 * Reference: https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.h
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "fs.h"

static int img_fd = -1;
static struct superblock sb;

static int read_block(uint bno, void *buf) {
    off_t off = (off_t)bno * BSIZE;
    if (pread(img_fd, buf, BSIZE, off) != BSIZE) {
        return -1;
    }
    return 0;
}

static int read_inode(uint inum, struct dinode *dip) {
    uchar buf[BSIZE];
    if (read_block(IBLOCK(inum, sb), buf) < 0) {
        return -1;
    }
    struct dinode *blk = (struct dinode *)buf;
    *dip = blk[inum % IPB];
    return 0;
}

static bool bitmap_is_set(uint bno) {
    uchar buf[BSIZE];
    if (read_block(BBLOCK(bno, sb), buf) < 0) {
        return false;
    }
    uint bi = bno % BPB;
    return (buf[bi / 8] & (1u << (bi % 8))) != 0;
}

/* Read `n` bytes from inode `dip` at offset `off` for directory scanning */
static ssize_t fsck_readi(const struct dinode *dip, char *dst, uint off, uint n) {
    if (off >= dip->size) {
        return 0;
    }
    if (off + n > dip->size) {
        n = dip->size - off;
    }
    uint tot = 0;
    uchar buf[BSIZE];
    uint indirect[NINDIRECT];
    bool ind_loaded = false;

    while (tot < n) {
        uint bn = off / BSIZE;
        uint boff = off % BSIZE;
        uint chunk = BSIZE - boff;
        if (chunk > n - tot) {
            chunk = n - tot;
        }
        uint bno = 0;
        if (bn < NDIRECT) {
            bno = dip->addrs[bn];
        } else if (bn - NDIRECT < NINDIRECT && dip->addrs[NDIRECT] != 0) {
            if (!ind_loaded) {
                if (read_block(dip->addrs[NDIRECT], indirect) < 0) {
                    return -1;
                }
                ind_loaded = true;
            }
            bno = indirect[bn - NDIRECT];
        }
        if (bno == 0) {
            memset(dst + tot, 0, chunk);
        } else {
            if (read_block(bno, buf) < 0) {
                return -1;
            }
            memcpy(dst + tot, buf + boff, chunk);
        }
        tot += chunk;
        off += chunk;
    }
    return (ssize_t)tot;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <fs.img>\n", argv[0]);
        return 2;
    }

    img_fd = open(argv[1], O_RDONLY);
    if (img_fd < 0) {
        perror("open fs.img");
        return 2;
    }

    uchar sbbuf[BSIZE];
    if (read_block(1, sbbuf) < 0) {
        fprintf(stderr, "ERROR: cannot read superblock at block 1\n");
        close(img_fd);
        return 1;
    }
    memcpy(&sb, sbbuf, sizeof(sb));

    int errors = 0;

    /* 1. Superblock validation */
#ifndef XV6_X86
    if (sb.magic != FSMAGIC) {
        fprintf(stderr, "ERROR: bad superblock magic 0x%x (expected 0x%x)\n", sb.magic, FSMAGIC);
        errors++;
    }
#endif
    uint bmap_blocks = (sb.size + BPB - 1) / BPB;
    uint calc_dstart = sb.bmapstart + bmap_blocks;
    uint data_start  = (sb.size > sb.nblocks) ? (sb.size - sb.nblocks) : calc_dstart;

    printf("=== xv6 fsck: Inspecting '%s' ===\n", argv[1]);
    printf("  Superblock: size=%u blocks (%u bytes/block), nblocks=%u, ninodes=%u, nlog=%u\n",
           sb.size, BSIZE, sb.nblocks, sb.ninodes, sb.nlog);
    printf("  Layout:     logstart=%u, inodestart=%u, bmapstart=%u, datastart=%u\n",
           sb.logstart, sb.inodestart, sb.bmapstart, data_start);

    if (sb.size == 0 || sb.nblocks >= sb.size || data_start >= sb.size) {
        fprintf(stderr, "ERROR: invalid superblock layout geometry\n");
        close(img_fd);
        return 1;
    }

    uint *block_usage = calloc(sb.size, sizeof(uint));
    uint *block_owner = calloc(sb.size, sizeof(uint));
    short *inode_type = calloc(sb.ninodes, sizeof(short));
    short *inode_nlink = calloc(sb.ninodes, sizeof(short));
    uint *dir_refs    = calloc(sb.ninodes, sizeof(uint));
    uint *parent_of   = calloc(sb.ninodes, sizeof(uint));

    uint alloc_inodes = 0;
    uint alloc_dirs = 0;
    uint alloc_files = 0;

    /* 2-5. Scan all inodes, validate types, sizes, and direct/indirect blocks */
    for (uint inum = 0; inum < sb.ninodes; inum++) {
        struct dinode dip;
        if (read_inode(inum, &dip) < 0) {
            fprintf(stderr, "ERROR: failed to read inode %u\n", inum);
            errors++;
            continue;
        }
        inode_type[inum] = dip.type;
        inode_nlink[inum] = dip.nlink;

        if (dip.type == 0) {
            continue;
        }
        if (inum == 0) {
            fprintf(stderr, "ERROR: inode 0 is marked allocated (type=%d)\n", dip.type);
            errors++;
        }
        if (dip.type != T_DIR && dip.type != T_FILE && dip.type != T_DEVICE) {
            fprintf(stderr, "ERROR: bad inode %u type %d\n", inum, dip.type);
            errors++;
            continue;
        }

        alloc_inodes++;
        if (dip.type == T_DIR) alloc_dirs++;
        if (dip.type == T_FILE) alloc_files++;

        if (dip.size > MAXFILE * BSIZE) {
            fprintf(stderr, "ERROR: inode %u size %u exceeds MAXFILE*BSIZE (%zu)\n",
                    inum, dip.size, (size_t)MAXFILE * BSIZE);
            errors++;
        }

        /* Check direct blocks */
        for (uint i = 0; i < NDIRECT; i++) {
            uint bno = dip.addrs[i];
            if (bno == 0) {
                continue;
            }
            if (bno < data_start || bno >= sb.size) {
                fprintf(stderr, "ERROR: bad direct address %u in inode %u (valid range [%u, %u))\n",
                        bno, inum, data_start, sb.size);
                errors++;
            } else {
                block_usage[bno]++;
                if (block_usage[bno] > 1) {
                    fprintf(stderr, "ERROR: direct address %u used more than once (inode %u and inode %u)\n",
                            bno, block_owner[bno], inum);
                    errors++;
                }
                block_owner[bno] = inum;
                if (!bitmap_is_set(bno)) {
                    fprintf(stderr, "ERROR: address %u used by inode %u but marked free in bitmap\n",
                            bno, inum);
                    errors++;
                }
            }
        }

        /* Check singly-indirect block */
        uint ind_bno = dip.addrs[NDIRECT];
        if (ind_bno != 0) {
            if (ind_bno < data_start || ind_bno >= sb.size) {
                fprintf(stderr, "ERROR: bad indirect block address %u in inode %u\n", ind_bno, inum);
                errors++;
            } else {
                block_usage[ind_bno]++;
                if (block_usage[ind_bno] > 1) {
                    fprintf(stderr, "ERROR: indirect block %u used more than once (inode %u and inode %u)\n",
                            ind_bno, block_owner[ind_bno], inum);
                    errors++;
                }
                block_owner[ind_bno] = inum;
                if (!bitmap_is_set(ind_bno)) {
                    fprintf(stderr, "ERROR: indirect block %u used by inode %u but marked free in bitmap\n",
                            ind_bno, inum);
                    errors++;
                }

                uint indirect[NINDIRECT];
                if (read_block(ind_bno, indirect) == 0) {
                    for (uint j = 0; j < NINDIRECT; j++) {
                        uint bno = indirect[j];
                        if (bno == 0) {
                            continue;
                        }
                        if (bno < data_start || bno >= sb.size) {
                            fprintf(stderr, "ERROR: bad indirect data address %u in inode %u\n",
                                    bno, inum);
                            errors++;
                        } else {
                            block_usage[bno]++;
                            if (block_usage[bno] > 1) {
                                fprintf(stderr, "ERROR: indirect data address %u used more than once (inode %u and inode %u)\n",
                                        bno, block_owner[bno], inum);
                                errors++;
                            }
                            block_owner[bno] = inum;
                            if (!bitmap_is_set(bno)) {
                                fprintf(stderr, "ERROR: indirect data address %u used by inode %u but marked free in bitmap\n",
                                        bno, inum);
                                errors++;
                            }
                        }
                    }
                }
            }
        }
    }

    /* 6. Root directory check */
    if (inode_type[ROOTINO] != T_DIR) {
        fprintf(stderr, "ERROR: root directory (inode %d) does not exist or is not T_DIR\n", ROOTINO);
        errors++;
    }

    /* 7, 11, 12. Scan all directories for '.', '..', parent links, and reference counts */
    for (uint inum = 1; inum < sb.ninodes; inum++) {
        if (inode_type[inum] != T_DIR) {
            continue;
        }
        struct dinode dp;
        read_inode(inum, &dp);

        bool seen_dot = false;
        bool seen_dotdot = false;
        struct dirent de;

        for (uint off = 0; off < dp.size; off += sizeof(de)) {
            if (fsck_readi(&dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
                fprintf(stderr, "ERROR: cannot read directory entry at offset %u in dir inode %u\n",
                        off, inum);
                errors++;
                break;
            }
            if (de.inum == 0) {
                continue;
            }
            if (de.inum >= sb.ninodes) {
                fprintf(stderr, "ERROR: directory %u contains out-of-range inum %u\n", inum, de.inum);
                errors++;
                continue;
            }

            char name[DIRSIZ + 1];
            memcpy(name, de.name, DIRSIZ);
            name[DIRSIZ] = '\0';

            if (strcmp(name, ".") == 0) {
                seen_dot = true;
                if (de.inum != inum) {
                    fprintf(stderr, "ERROR: directory %u '.' points to %u instead of %u\n",
                            inum, de.inum, inum);
                    errors++;
                }
            } else if (strcmp(name, "..") == 0) {
                seen_dotdot = true;
                if (inum == ROOTINO && de.inum != ROOTINO) {
                    fprintf(stderr, "ERROR: root directory '..' points to %u instead of %d\n",
                            de.inum, ROOTINO);
                    errors++;
                }
            } else {
                dir_refs[de.inum]++;
                if (inode_type[de.inum] == T_DIR) {
                    if (parent_of[de.inum] != 0) {
                        fprintf(stderr, "ERROR: directory inode %u referenced in multiple parents (%u and %u)\n",
                                de.inum, parent_of[de.inum], inum);
                        errors++;
                    }
                    parent_of[de.inum] = inum;
                }
            }
        }

        if (!seen_dot || !seen_dotdot) {
            fprintf(stderr, "ERROR: directory %u missing '.' or '..' entry\n", inum);
            errors++;
        }
    }

    /* Verify '..' parent consistency for non-root directories */
    for (uint inum = 2; inum < sb.ninodes; inum++) {
        if (inode_type[inum] != T_DIR) {
            continue;
        }
        struct dinode dp;
        read_inode(inum, &dp);
        struct dirent de;
        for (uint off = 0; off < dp.size; off += sizeof(de)) {
            if (fsck_readi(&dp, (char *)&de, off, sizeof(de)) == sizeof(de) && de.inum != 0) {
                char name[DIRSIZ + 1];
                memcpy(name, de.name, DIRSIZ);
                name[DIRSIZ] = '\0';
                if (strcmp(name, "..") == 0 && parent_of[inum] != 0 && de.inum != parent_of[inum]) {
                    fprintf(stderr, "ERROR: directory %u '..' points to %u, but actual parent is %u\n",
                            inum, de.inum, parent_of[inum]);
                    errors++;
                }
            }
        }
    }

    /* 8-9. Verify bitmap consistency for metadata and data blocks */
    for (uint b = 0; b < data_start; b++) {
        if (!bitmap_is_set(b)) {
            fprintf(stderr, "ERROR: metadata block %u is marked free in bitmap\n", b);
            errors++;
        }
    }
    uint used_data_blocks = 0;
    for (uint b = data_start; b < sb.size; b++) {
        bool in_bmap = bitmap_is_set(b);
        if (in_bmap) {
            used_data_blocks++;
        }
        if (in_bmap && block_usage[b] == 0) {
            fprintf(stderr, "ERROR: bitmap marks data block %u in use, but it is not referenced by any inode\n", b);
            errors++;
        }
    }

    /* 11-12. Verify inode reference and link counts */
    dir_refs[ROOTINO]++; /* Root directory is self-anchored */
    for (uint inum = 1; inum < sb.ninodes; inum++) {
        if (inode_type[inum] != 0 && dir_refs[inum] == 0) {
            fprintf(stderr, "ERROR: inode %u (type=%d) marked in use but not found in any directory\n",
                    inum, inode_type[inum]);
            errors++;
        }
        if (inode_type[inum] == 0 && dir_refs[inum] > 0) {
            fprintf(stderr, "ERROR: inode %u referred to in a directory but marked free on disk\n", inum);
            errors++;
        }
        if (inode_type[inum] == T_FILE && (uint)inode_nlink[inum] != dir_refs[inum]) {
            fprintf(stderr, "ERROR: bad reference count for file inode %u (nlink=%d, directory refs=%u)\n",
                    inum, inode_nlink[inum], dir_refs[inum]);
            errors++;
        }
        if (inode_type[inum] == T_DIR && inum != ROOTINO && dir_refs[inum] != 1) {
            fprintf(stderr, "ERROR: directory inode %u appears %u times in directory tree (expected 1)\n",
                    inum, dir_refs[inum]);
            errors++;
        }
    }

    printf("  Summary:    %u allocated inodes (%u dirs, %u files), %u/%u data blocks used\n",
           alloc_inodes, alloc_dirs, alloc_files, used_data_blocks, sb.nblocks);

    free(block_usage);
    free(block_owner);
    free(inode_type);
    free(inode_nlink);
    free(dir_refs);
    free(parent_of);
    close(img_fd);

    if (errors == 0) {
        printf("  Result:     [PASS] File system is 100%% consistent (0 errors).\n");
        return 0;
    } else {
        fprintf(stderr, "  Result:     [FAIL] Found %d consistency error(s).\n", errors);
        return 1;
    }
}
