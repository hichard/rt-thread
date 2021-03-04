/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020/08/21     ShaoJinchun  first version
 */

#include <rtthread.h>
#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>

#include "dfs_cromfs.h"

#include <stdint.h>

#include "zlib.h"

/**********************************/

#define CROMFS_PATITION_HEAD_SIZE 256
#define CROMFS_DIRENT_CACHE_SIZE  8

#define CROMFS_MAGIC   "CROMFSMG"

#define CROMFS_CT_ASSERT(name, x) \
    struct assert_##name {char ary[2 * (x) - 1];}

#define CROMFS_POS_ROOT  (0x0UL)
#define CROMFS_POS_ERROR (0x1UL)

typedef struct
{
    uint8_t magic[8];        /* CROMFS_MAGIC */
    uint32_t version;
    uint32_t partition_attr; /* expand, now reserved 0 */
    uint32_t partition_size; /* with partition head */
    uint32_t root_dir_pos;   /* root dir pos */
    uint32_t root_dir_size;
} partition_head_data;

typedef struct
{
    partition_head_data head;
    uint8_t padding[CROMFS_PATITION_HEAD_SIZE - sizeof(partition_head_data)];
} partition_head;

#define CROMFS_DIRENT_ATTR_DIR  0x1UL
#define CROMFS_DIRENT_ATTR_FILE 0x0UL

typedef struct
{
    uint16_t attr;              /* dir or file add other */
    uint16_t name_size;         /* name real size */
    uint32_t file_size;         /* file data size */
    uint32_t file_origin_size;  /* file size before compress */
    uint32_t parition_pos;      /* offset of data */
    uint8_t  name[0];           /* name data */
} cromfs_dirent;

#define CROMFS_ALIGN_SIZE_BIT  4
#define CROMFS_ALIGN_SIZE      (1UL << CROMFS_ALIGN_SIZE_BIT)  /* must be same as sizeof cromfs_dirent */
#define CROMFS_ALIGN_SIZE_MASK (CROMFS_ALIGN_SIZE - 1)

CROMFS_CT_ASSERT(align_size, CROMFS_ALIGN_SIZE == sizeof(cromfs_dirent));

typedef union
{
    cromfs_dirent dirent;
    uint8_t name[CROMFS_ALIGN_SIZE];
} cromfs_dirent_item;

/**********************************/

typedef struct
{
    rt_list_t list;
    uint32_t partition_pos;
    uint32_t size;
    uint8_t *buff;
} cromfs_dirent_cache;

typedef struct st_cromfs_info
{
    rt_device_t device;
    uint32_t partition_size;
    uint32_t bytes_per_sector;
    uint32_t (*read_bytes)(struct st_cromfs_info *ci, uint32_t pos, void *buf, uint32_t size);
    partition_head_data part_info;
    struct rt_mutex lock;
    struct cromfs_avl_struct *cromfs_avl_root;
    rt_list_t cromfs_dirent_cache_head;
    int cromfs_dirent_cache_nr;
} cromfs_info;

typedef struct
{
    uint32_t ref;
    uint32_t partition_pos;
    cromfs_info *ci;
    uint32_t size;
    uint8_t *buff;
    uint32_t partition_size;
    int data_valid;
} file_info;

/**********************************/

#define avl_key_t       uint32_t
#define AVL_EMPTY       (struct cromfs_avl_struct *)0
#define avl_maxheight   32
#define heightof(tree)  ((tree) == AVL_EMPTY ? 0 : (tree)->avl_height)

struct cromfs_avl_struct
{
    struct cromfs_avl_struct *avl_left;
    struct cromfs_avl_struct *avl_right;
    int    avl_height;
    avl_key_t avl_key;
    file_info *fi;
};

static void cromfs_avl_remove(struct cromfs_avl_struct *node_to_delete, struct cromfs_avl_struct **ptree);
static void cromfs_avl_insert(struct cromfs_avl_struct *new_node, struct cromfs_avl_struct **ptree);
static struct cromfs_avl_struct* cromfs_avl_find(avl_key_t key, struct cromfs_avl_struct *ptree);

static void cromfs_avl_rebalance(struct cromfs_avl_struct ***nodeplaces_ptr, int count)
{
    for (;count > 0; count--)
    {
        struct cromfs_avl_struct **nodeplace = *--nodeplaces_ptr;
        struct cromfs_avl_struct *node = *nodeplace;
        struct cromfs_avl_struct *nodeleft = node->avl_left;
        struct cromfs_avl_struct *noderight = node->avl_right;
        int heightleft = heightof(nodeleft);
        int heightright = heightof(noderight);
        if (heightright + 1 < heightleft)
        {
            struct cromfs_avl_struct * nodeleftleft = nodeleft->avl_left;
            struct cromfs_avl_struct * nodeleftright = nodeleft->avl_right;
            int heightleftright = heightof(nodeleftright);
            if (heightof(nodeleftleft) >= heightleftright)
            {
                node->avl_left = nodeleftright;
                nodeleft->avl_right = node;
                nodeleft->avl_height = 1 + (node->avl_height = 1 + heightleftright);
                *nodeplace = nodeleft;
            }
            else
            {
                nodeleft->avl_right = nodeleftright->avl_left;
                node->avl_left = nodeleftright->avl_right;
                nodeleftright->avl_left = nodeleft;
                nodeleftright->avl_right = node;
                nodeleft->avl_height = node->avl_height = heightleftright;
                nodeleftright->avl_height = heightleft;
                *nodeplace = nodeleftright;
            }
        }
        else if (heightleft + 1 < heightright)
        {
            struct cromfs_avl_struct *noderightright = noderight->avl_right;
            struct cromfs_avl_struct *noderightleft = noderight->avl_left;
            int heightrightleft = heightof(noderightleft);
            if (heightof(noderightright) >= heightrightleft)
            {
                node->avl_right = noderightleft;
                noderight->avl_left = node;
                noderight->avl_height = 1 + (node->avl_height = 1 + heightrightleft);
                *nodeplace = noderight;
            }
            else
            {
                noderight->avl_left = noderightleft->avl_right;
                node->avl_right = noderightleft->avl_left;
                noderightleft->avl_right = noderight;
                noderightleft->avl_left = node;
                noderight->avl_height = node->avl_height = heightrightleft;
                noderightleft->avl_height = heightright;
                *nodeplace = noderightleft;
            }
        }
        else {
            int height = (heightleft<heightright ? heightright : heightleft) + 1;
            if (height == node->avl_height)
            {
                break;
            }
            node->avl_height = height;
        }
    }
}

static void cromfs_avl_remove(struct cromfs_avl_struct *node_to_delete, struct cromfs_avl_struct **ptree)
{
    avl_key_t key = node_to_delete->avl_key;
    struct cromfs_avl_struct **nodeplace = ptree;
    struct cromfs_avl_struct **stack[avl_maxheight];
    uint32_t stack_count = 0;
    struct cromfs_avl_struct ***stack_ptr = &stack[0]; /* = &stack[stackcount] */
    struct cromfs_avl_struct **nodeplace_to_delete;
    for (;;)
    {
        struct cromfs_avl_struct *node = *nodeplace;
        if (node == AVL_EMPTY)
        {
            return;
        }
        *stack_ptr++ = nodeplace;
        stack_count++;
        if (key == node->avl_key)
        {
            break;
        }
        if (key < node->avl_key)
        {
            nodeplace = &node->avl_left;
        }
        else
        {
            nodeplace = &node->avl_right;
        }
    }
    nodeplace_to_delete = nodeplace;
    if (node_to_delete->avl_left == AVL_EMPTY)
    {
        *nodeplace_to_delete = node_to_delete->avl_right;
        stack_ptr--;
        stack_count--;
    }
    else
    {
        struct cromfs_avl_struct *** stack_ptr_to_delete = stack_ptr;
        struct cromfs_avl_struct ** nodeplace = &node_to_delete->avl_left;
        struct cromfs_avl_struct * node;
        for (;;)
        {
            node = *nodeplace;
            if (node->avl_right == AVL_EMPTY)
            {
                break;
            }
            *stack_ptr++ = nodeplace;
            stack_count++;
            nodeplace = &node->avl_right;
        }
        *nodeplace = node->avl_left;
        node->avl_left = node_to_delete->avl_left;
        node->avl_right = node_to_delete->avl_right;
        node->avl_height = node_to_delete->avl_height;
        *nodeplace_to_delete = node;
        *stack_ptr_to_delete = &node->avl_left;
    }
    cromfs_avl_rebalance(stack_ptr,stack_count);
}

static void cromfs_avl_insert(struct cromfs_avl_struct *new_node, struct cromfs_avl_struct **ptree)
{
    avl_key_t key = new_node->avl_key;
    struct cromfs_avl_struct **nodeplace = ptree;
    struct cromfs_avl_struct **stack[avl_maxheight];
    int stack_count = 0;
    struct cromfs_avl_struct ***stack_ptr = &stack[0]; /* = &stack[stackcount] */
    for (;;)
    {
        struct cromfs_avl_struct * node = *nodeplace;
        if (node == AVL_EMPTY)
        {
            break;
        }
        *stack_ptr++ = nodeplace;
        stack_count++;
        if (key < node->avl_key)
        {
            nodeplace = &node->avl_left;
        }
        else
        {
            nodeplace = &node->avl_right;
        }
    }
    new_node->avl_left = AVL_EMPTY;
    new_node->avl_right = AVL_EMPTY;
    new_node->avl_height = 1;
    *nodeplace = new_node;
    cromfs_avl_rebalance(stack_ptr,stack_count);
}

static struct cromfs_avl_struct* cromfs_avl_find(avl_key_t key, struct cromfs_avl_struct* ptree)
{
    for (;;)
    {
        if (ptree == AVL_EMPTY)
        {
            return (struct cromfs_avl_struct *)0;
        }
        if (key == ptree->avl_key)
        {
            break;
        }
        if (key < ptree->avl_key)
        {
            ptree = ptree->avl_left;
        }
        else
        {
            ptree = ptree->avl_right;
        }
    }
    return ptree;
}

/**********************************/

static uint32_t cromfs_read_bytes(cromfs_info *ci, uint32_t pos, void *buf, uint32_t size)
{
    if (pos >= ci->partition_size || pos + size > ci->partition_size)
    {
        return 0;
    }
    return ci->read_bytes(ci, pos, buf, size);
}

static uint32_t cromfs_noblk_read_bytes(cromfs_info *ci, uint32_t pos, void *buf, uint32_t size)
{
    uint32_t ret = 0;

    ret = rt_device_read(ci->device, pos, buf, size);
    if (ret != size)
    {
        return 0;
    }
    else
    {
        return ret;
    }
}

static uint32_t cromfs_blk_read_bytes(cromfs_info *ci, uint32_t pos, void *buf, uint32_t size)
{
    uint32_t ret = 0;
    uint32_t size_bak = size;
    uint32_t start_blk = 0;
    uint32_t end_blk = 0;
    uint32_t off_s = 0;
    uint32_t sector_nr = 0;
    uint8_t *block_buff = NULL;
    uint32_t ret_len = 0;

    if (!size || !buf)
    {
        return 0;
    }
    block_buff = (uint8_t *)malloc(2 * ci->bytes_per_sector);
    if (!block_buff)
    {
        return 0;
    }
    start_blk = pos / ci->bytes_per_sector;
    off_s = pos % ci->bytes_per_sector;
    end_blk = (pos + size - 1) / ci->bytes_per_sector;

    sector_nr = end_blk - start_blk;
    if (sector_nr < 2)
    {
        ret_len = rt_device_read(ci->device, start_blk, block_buff, sector_nr + 1);
        if (ret_len != sector_nr + 1)
        {
            goto end;
        }
        memcpy(buf, block_buff + off_s, size);
    }
    else
    {
        ret_len = rt_device_read(ci->device, start_blk, block_buff, 1);
        if (ret_len != 1)
        {
            goto end;
        }
        memcpy(buf, block_buff + off_s, ci->bytes_per_sector - off_s);
        off_s = (ci->bytes_per_sector - off_s);
        size -= off_s;
        sector_nr--;
        start_blk++;
        if (sector_nr)
        {
            ret_len = rt_device_read(ci->device, start_blk, (char*)buf + off_s, sector_nr);
            if (ret_len != sector_nr)
            {
                goto end;
            }
            start_blk += sector_nr;
            off_s += (sector_nr * ci->bytes_per_sector);
            size -= (sector_nr * ci->bytes_per_sector);
        }
        ret_len = rt_device_read(ci->device, start_blk, block_buff, 1);
        if (ret_len != 1)
        {
            goto end;
        }
        memcpy((char*)buf + off_s, block_buff, size);
    }
    ret = size_bak;
end:
    free(block_buff);
    return ret;
}

/**********************************/

static uint8_t *cromfs_dirent_cache_get(cromfs_info *ci, uint32_t pos, uint32_t size)
{
    rt_list_t *l = NULL;
    cromfs_dirent_cache *dir = NULL;
    uint32_t len = 0;

    /* find */
    for (l = ci->cromfs_dirent_cache_head.next; l != &ci->cromfs_dirent_cache_head; l = l->next)
    {
        dir = (cromfs_dirent_cache *)l;
        if (dir->partition_pos == pos)
        {
            RT_ASSERT(dir->size == size);
            rt_list_remove(l);
            rt_list_insert_after(&ci->cromfs_dirent_cache_head, l);
            return dir->buff;
        }
    }
    /* not found */
    if (ci->cromfs_dirent_cache_nr >= CROMFS_DIRENT_CACHE_SIZE)
    {
        l = ci->cromfs_dirent_cache_head.prev;
        dir = (cromfs_dirent_cache *)l;
        rt_list_remove(l);
        free(dir->buff);
        free(dir);
        ci->cromfs_dirent_cache_nr--;
    }
    dir = (cromfs_dirent_cache *)malloc(sizeof *dir);
    if (!dir)
    {
        return NULL;
    }
    dir->buff = (uint8_t *)malloc(size);
    if (!dir->buff)
    {
        free(dir);
        return NULL;
    }
    len = cromfs_read_bytes(ci, pos, dir->buff, size);
    if (len != size)
    {
        free(dir->buff);
        free(dir);
        return NULL;
    }
    rt_list_insert_after(&ci->cromfs_dirent_cache_head, (rt_list_t *)dir);
    ci->cromfs_dirent_cache_nr++;
    dir->partition_pos = pos;
    dir->size = size;
    return dir->buff;
}

static void cromfs_dirent_cache_destroy(cromfs_info *ci)
{
    rt_list_t *l = NULL;
    cromfs_dirent_cache *dir = NULL;

    while ((l = ci->cromfs_dirent_cache_head.next) != &ci->cromfs_dirent_cache_head)
    {
        rt_list_remove(l);
        dir = (cromfs_dirent_cache *)l;
        free(dir->buff);
        free(dir);
        ci->cromfs_dirent_cache_nr--;
    }
}

/**********************************/

static int dfs_cromfs_mount(struct dfs_filesystem *fs, unsigned long rwflag, const void *data)
{
    struct rt_device_blk_geometry geometry;
    uint32_t len = 0;
    cromfs_info *ci = NULL;

    ci = (cromfs_info *)malloc(sizeof *ci);
    if (!ci)
    {
        return -ENOMEM;
    }

    memset(ci, 0, sizeof *ci);
    ci->device = fs->dev_id;
    ci->partition_size = UINT32_MAX;
    if (ci->device->type == RT_Device_Class_Block)
    {
        rt_device_control(ci->device, RT_DEVICE_CTRL_BLK_GETGEOME, &geometry);
        ci->bytes_per_sector = geometry.bytes_per_sector;
        ci->read_bytes = cromfs_blk_read_bytes;
    }
    else
    {
        ci->read_bytes = cromfs_noblk_read_bytes;
    }

    len = cromfs_read_bytes(ci, 0, &ci->part_info, sizeof ci->part_info);
    if (len != sizeof ci->part_info ||
            memcmp(ci->part_info.magic, CROMFS_MAGIC, sizeof ci->part_info.magic) != 0)
    {
        free(ci);
        return -RT_ERROR;
    }
    ci->partition_size = ci->part_info.partition_size;
    fs->data = ci;

    rt_mutex_init(&ci->lock, "crom", RT_IPC_FLAG_FIFO);
    ci->cromfs_avl_root = NULL;

    rt_list_init(&ci->cromfs_dirent_cache_head);
    ci->cromfs_dirent_cache_nr = 0;

    return RT_EOK;
}

static int dfs_cromfs_unmount(struct dfs_filesystem *fs)
{
    rt_err_t result = RT_EOK;
    cromfs_info *ci = NULL;

    ci = (cromfs_info *)fs->data;

    result =  rt_mutex_take(&ci->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return -RT_ERROR;
    }

    cromfs_dirent_cache_destroy(ci);

    while (ci->cromfs_avl_root)
    {
        struct cromfs_avl_struct *node;
        file_info *fi = NULL;

        node = ci->cromfs_avl_root;
        fi = node->fi;
        cromfs_avl_remove(node, &ci->cromfs_avl_root);
        free(node);
        if (fi->buff)
        {
            free(fi->buff);
        }
        free(fi);
    }

    rt_mutex_detach(&ci->lock);

    free(ci);

    return RT_EOK;
}

static int dfs_cromfs_ioctl(struct dfs_fd *file, int cmd, void *args)
{
    return -EIO;
}

static uint32_t cromfs_lookup(cromfs_info *ci, const char *path, int* is_dir, uint32_t *size, uint32_t *osize)
{
    uint32_t cur_size = 0, cur_pos = 0, cur_osize = 0;
    const char *subpath = NULL, *subpath_end = NULL;
    void *di_mem = NULL;
    int isdir = 0;

    if (path[0] == '\0')
    {
        return CROMFS_POS_ERROR;
    }

    cur_size = ci->part_info.root_dir_size;
    cur_osize = 0;
    cur_pos = ci->part_info.root_dir_pos;
    isdir = 1;

    subpath_end = path;
    while (1)
    {
        cromfs_dirent_item *di_iter = NULL;
        int found = 0;

        /* skip /// */
        while (*subpath_end && *subpath_end == '/')
        {
            subpath_end++;
        }
        subpath = subpath_end;
        while ((*subpath_end != '/') && *subpath_end)
        {
            subpath_end++;
        }
        if (*subpath == '\0')
        {
            break;
        }

        /* if not dir or empty dir, error */
        if (!isdir || !cur_size)
        {
            return CROMFS_POS_ERROR;
        }

        /* find subpath */
        di_mem = cromfs_dirent_cache_get(ci, cur_pos, cur_size);
        if (!di_mem)
        {
            return CROMFS_POS_ERROR;
        }

        found = 0;
        di_iter = (cromfs_dirent_item *)di_mem;
        while (1)
        {
            uint32_t name_len = subpath_end - subpath;
            uint32_t name_block = 0;

            if (di_iter->dirent.name_size == name_len)
            {
                if (memcmp(di_iter->dirent.name, subpath, name_len) == 0)
                {
                    found = 1;
                    cur_size = di_iter->dirent.file_size;
                    cur_osize = di_iter->dirent.file_origin_size;
                    cur_pos = di_iter->dirent.parition_pos;
                    if (di_iter->dirent.attr == CROMFS_DIRENT_ATTR_DIR)
                    {
                        isdir = 1;
                    }
                    else
                    {
                        isdir = 0;
                    }
                    break;
                }
            }
            name_block = (di_iter->dirent.name_size + CROMFS_ALIGN_SIZE_MASK) >> CROMFS_ALIGN_SIZE_BIT;
            di_iter += (1 + name_block);
            if ((uint32_t)di_iter - (uint32_t)di_mem >= cur_size)
            {
                break;
            }
        }
        if (!found)
        {
            return CROMFS_POS_ERROR;
        }
    }
    *size = cur_size;
    *osize = cur_osize;
    *is_dir = isdir;
    return cur_pos;
}

static uint32_t dfs_cromfs_lookup(cromfs_info *ci, const char *path, int* is_dir, uint32_t *size, uint32_t *osize)
{
    rt_err_t result = RT_EOK;
    uint32_t ret = 0;

    result =  rt_mutex_take(&ci->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return CROMFS_POS_ERROR;
    }
    ret = cromfs_lookup(ci, path, is_dir, size, osize);
    rt_mutex_release(&ci->lock);
    return ret;
}

static int fill_file_data(file_info *fi)
{
    int ret = -1;
    cromfs_info *ci = NULL;
    void *compressed_file_buff = NULL;
    uint32_t size = 0, osize = 0;

    if (!fi->data_valid)
    {
        RT_ASSERT(fi->buff != NULL);

        ci = fi->ci;
        osize = fi->size;
        size = fi->partition_size;

        compressed_file_buff = (void *)malloc(size);
        if (!compressed_file_buff)
        {
            goto end;
        }
        if (cromfs_read_bytes(ci, fi->partition_pos, compressed_file_buff, size) != size)
        {
            goto end;
        }
        if (uncompress((uint8_t *)fi->buff, (uLongf *)&osize, (uint8_t *)compressed_file_buff, size) != Z_OK)
        {
            goto end;
        }
        fi->data_valid = 1;
    }
    ret = 0;
end:
    if (compressed_file_buff)
    {
        free(compressed_file_buff);
    }
    return ret;
}

static int dfs_cromfs_read(struct dfs_fd *file, void *buf, size_t count)
{
    rt_err_t result = RT_EOK;
    struct dfs_filesystem *fs = NULL;
    file_info *fi = NULL;
    cromfs_info *ci = NULL;
    uint32_t length = 0;

    fs = (struct dfs_filesystem *)file->fnode->fs;
    ci = (cromfs_info *)fs->data;
    fi = (file_info *)file->fnode->data;

    if (count < file->fnode->size - file->pos)
    {
        length = count;
    }
    else
    {
        length = file->fnode->size - file->pos;
    }

    if (length > 0)
    {
        RT_ASSERT(fi->size != 0);

        if (fi->buff)
        {
            int fill_ret = 0;

            result =  rt_mutex_take(&ci->lock, RT_WAITING_FOREVER);
            if (result != RT_EOK)
            {
                return 0;
            }
            fill_ret = fill_file_data(fi);
            rt_mutex_release(&ci->lock);
            if (fill_ret < 0)
            {
                return 0;
            }

            memcpy(buf, fi->buff + file->pos, length);
        }
        else
        {
            void *di_mem = NULL;

            result =  rt_mutex_take(&ci->lock, RT_WAITING_FOREVER);
            if (result != RT_EOK)
            {
                return 0;
            }
            di_mem = cromfs_dirent_cache_get(ci, fi->partition_pos, fi->size);
            if (di_mem)
            {
                memcpy(buf, (char*)di_mem + file->pos, length);
            }
            rt_mutex_release(&ci->lock);
            if (!di_mem)
            {
                return 0;
            }
        }
        /* update file current position */
        file->pos += length;
    }

    return length;
}

static int dfs_cromfs_lseek(struct dfs_fd *file, off_t offset)
{
    if (offset <= file->fnode->size)
    {
        file->pos = offset;
        return file->pos;
    }
    return -EIO;
}

static file_info *get_file_info(cromfs_info *ci, uint32_t partition_pos, int inc_ref)
{
    struct cromfs_avl_struct* node = cromfs_avl_find(partition_pos, ci->cromfs_avl_root);

    if (node)
    {
        if (inc_ref)
        {
            node->fi->ref++;
        }
        return node->fi;
    }
    return NULL;
}

static file_info *inset_file_info(cromfs_info *ci, uint32_t partition_pos, int is_dir, uint32_t size, uint32_t osize)
{
    file_info *fi = NULL;
    void *file_buff = NULL;
    struct cromfs_avl_struct *node = NULL;

    fi = (file_info *)malloc(sizeof *fi);
    if (!fi)
    {
        goto err;
    }
    fi->partition_pos = partition_pos;
    fi->ci = ci;
    if (is_dir)
    {
        fi->size = size;
    }
    else
    {
        fi->size = osize;
        fi->partition_size = size;
        fi->data_valid = 0;
        if (osize)
        {
            file_buff = (void *)malloc(osize);
            if (!file_buff)
            {
                goto err;
            }
        }
    }
    fi->buff = file_buff;
    fi->ref = 1;

    node = (struct cromfs_avl_struct *)malloc(sizeof *node);
    if (!node)
    {
        goto err;
    }
    node->avl_key = partition_pos;
    node->fi = fi;
    cromfs_avl_insert(node, &ci->cromfs_avl_root);
    return fi;
err:
    if (file_buff)
    {
        free(file_buff);
    }
    if (fi)
    {
        free(fi);
    }
    return NULL;
}

static void deref_file_info(cromfs_info *ci, uint32_t partition_pos)
{
    struct cromfs_avl_struct* node = cromfs_avl_find(partition_pos, ci->cromfs_avl_root);
    file_info *fi = NULL;

    if (node)
    {
        node->fi->ref--;
        if (node->fi->ref == 0)
        {
            fi = node->fi;
            cromfs_avl_remove(node, &ci->cromfs_avl_root);
            free(node);
            if (fi->buff)
            {
                free(fi->buff);
            }
            free(fi);
        }
    }
}

static int dfs_cromfs_close(struct dfs_fd *file)
{
    file_info *fi = NULL;
    struct dfs_filesystem *fs = NULL;
    cromfs_info *ci = NULL;
    rt_err_t result = 0;

    RT_ASSERT(file->fnode->ref_count > 0);
    if (file->fnode->ref_count > 1)
    {
        return 0;
    }

    fi = (file_info *)file->fnode->data;
    fs = (struct dfs_filesystem *)file->fnode->fs;
    ci = (cromfs_info *)fs->data;

    result =  rt_mutex_take(&ci->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return -RT_ERROR;
    }
    deref_file_info(ci, fi->partition_pos);
    rt_mutex_release(&ci->lock);
    file->fnode->data = NULL;
    return RT_EOK;
}

static int dfs_cromfs_open(struct dfs_fd *file)
{
    int ret = 0;
    struct dfs_filesystem *fs = NULL;
    file_info *fi = NULL;
    cromfs_info *ci = NULL;
    uint32_t file_pos = 0;
    uint32_t size = 0, osize = 0;
    int is_dir = 0;
    rt_err_t result = RT_EOK;

    if (file->flags & (O_CREAT | O_WRONLY | O_APPEND | O_TRUNC | O_RDWR))
    {
        return -EINVAL;
    }

    RT_ASSERT(file->fnode->ref_count > 0);
    if (file->fnode->ref_count > 1)
    {
        file->pos = 0;
        return 0;
    }

    fs = file->fnode->fs;
    ci = (cromfs_info *)fs->data;

    file_pos = dfs_cromfs_lookup(ci, file->fnode->path, &is_dir, &size, &osize);
    if (file_pos == CROMFS_POS_ERROR)
    {
        ret = -ENOENT;
        goto end;
    }

    /* entry is a directory file type */
    if (is_dir)
    {
        if (!(file->flags & O_DIRECTORY))
        {
            ret = -ENOENT;
            goto end;
        }
    }
    else
    {
        /* entry is a file, but open it as a directory */
        if (file->flags & O_DIRECTORY)
        {
            ret = -ENOENT;
            goto end;
        }
    }

    result =  rt_mutex_take(&ci->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        ret = -EINTR;
        goto end;
    }

    fi = get_file_info(ci, file_pos, 1);
    if (!fi)
    {
        fi = inset_file_info(ci, file_pos, is_dir, size, osize);
    }
    rt_mutex_release(&ci->lock);
    if (!fi)
    {
        ret = -ENOENT;
        goto end;
    }

    file->fnode->data = fi;
    if (is_dir)
    {
        file->fnode->size = size;
    }
    else
    {
        file->fnode->size = osize;
    }
    file->pos = 0;

    ret = RT_EOK;
end:
    return ret;
}

static int dfs_cromfs_stat(struct dfs_filesystem *fs, const char *path, struct stat *st)
{
    uint32_t size = 0, osize = 0;
    int is_dir = 0;
    cromfs_info *ci = NULL;
    uint32_t file_pos = 0;

    ci = (cromfs_info *)fs->data;

    file_pos = dfs_cromfs_lookup(ci, path, &is_dir, &size, &osize);
    if (file_pos == CROMFS_POS_ERROR)
    {
        return -ENOENT;
    }

    st->st_dev = 0;
    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH |
        S_IWUSR | S_IWGRP | S_IWOTH;

    if (is_dir)
    {
        st->st_mode &= ~S_IFREG;
        st->st_mode |= S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH;
        st->st_size = size;
    }
    else
    {
        st->st_size = osize;
    }

    st->st_mtime = 0;

    return RT_EOK;
}

static int dfs_cromfs_getdents(struct dfs_fd *file, struct dirent *dirp, uint32_t count)
{
    uint32_t index = 0;
    uint8_t *name = NULL;
    struct dirent *d = NULL;
    file_info *fi = NULL;
    cromfs_info *ci = NULL;
    cromfs_dirent_item *dirent = NULL, *sub_dirent = NULL;
    void *di_mem = NULL;
    rt_err_t result = RT_EOK;

    fi = (file_info *)file->fnode->data;
    ci = fi->ci;

    RT_ASSERT(fi->buff == NULL);

    if (!fi->size)
    {
        return -EINVAL;
    }

    dirent = (cromfs_dirent_item *)malloc(fi->size);
    if (!dirent)
    {
        return -ENOMEM;
    }

    result =  rt_mutex_take(&ci->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        free(dirent);
        return -EINTR;
    }
    di_mem = cromfs_dirent_cache_get(ci, fi->partition_pos, fi->size);
    if (di_mem)
    {
        memcpy(dirent, di_mem, fi->size);
    }
    rt_mutex_release(&ci->lock);
    if (!di_mem)
    {
        free(dirent);
        return -ENOMEM;
    }

    /* make integer count */
    count = (count / sizeof(struct dirent));
    if (count == 0)
    {
        free(dirent);
        return -EINVAL;
    }

    for (index = 0; index < count && file->pos < file->fnode->size; index++)
    {
        uint32_t name_size = 0;

        d = dirp + index;
        sub_dirent = &dirent[file->pos >> CROMFS_ALIGN_SIZE_BIT];
        name = sub_dirent->dirent.name;

        /* fill dirent */
        if (sub_dirent->dirent.attr == CROMFS_DIRENT_ATTR_DIR)
        {
            d->d_type = DT_DIR;
        }
        else
        {
            d->d_type = DT_REG;
        }

        d->d_namlen = sub_dirent->dirent.name_size;
        d->d_reclen = (rt_uint16_t)sizeof(struct dirent);
        memcpy(d->d_name, (char *)name, sub_dirent->dirent.name_size);
        d->d_name[sub_dirent->dirent.name_size] = '\0';

        name_size = (sub_dirent->dirent.name_size + CROMFS_ALIGN_SIZE_MASK) & ~CROMFS_ALIGN_SIZE_MASK;
        /* move to next position */
        file->pos += (name_size + sizeof *sub_dirent);
    }

    free(dirent);

    return index * sizeof(struct dirent);
}

static const struct dfs_file_ops _crom_fops =
{
    dfs_cromfs_open,
    dfs_cromfs_close,
    dfs_cromfs_ioctl,
    dfs_cromfs_read,
    NULL,
    NULL,
    dfs_cromfs_lseek,
    dfs_cromfs_getdents,
};

static const struct dfs_filesystem_ops _cromfs =
{
    "crom",
    DFS_FS_FLAG_DEFAULT,
    &_crom_fops,

    dfs_cromfs_mount,
    dfs_cromfs_unmount,
    NULL,
    NULL,

    NULL,
    dfs_cromfs_stat,
    NULL,
};

int dfs_cromfs_init(void)
{
    /* register crom file system */
    dfs_register(&_cromfs);
    return 0;
}
INIT_COMPONENT_EXPORT(dfs_cromfs_init);
