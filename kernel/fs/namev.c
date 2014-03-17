#include "kernel.h"
#include "globals.h"
#include "types.h"
#include "errno.h"

#include "util/string.h"
#include "util/printf.h"
#include "util/debug.h"

#include "fs/dirent.h"
#include "fs/fcntl.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"

/* This takes a base 'dir', a 'name', its 'len', and a result vnode.
 * Most of the work should be done by the vnode's implementation
 * specific lookup() function, but you may want to special case
 * "." and/or ".." here depnding on your implementation.
 *
 * If dir has no lookup(), return -ENOTDIR.
 *
 * Note: returns with the vnode refcount on *result incremented.
 */
int
lookup(vnode_t *dir, const char *name, size_t len, vnode_t **result)
{
    if(dir == NULL || name == NULL || len == 0)
        return -ENOENT;
    vnode_ops* ops = dir->vn_ops;
    KASSERT(ops);
    if(ops->lookup == NULL)
        return -ENOTDIR;
    else
    {
        /* Do not process the .. because it can be found in lookup */
        int ret = 0;
        vnode* node;
        if((ret = ops->lookup(dir, name, len, node)) < 0)
        {
            /* lookup doesn't need to vref, it's already done*/
            return ret;
        }
        else
        {
            return 0;
        }
    }

}


/* When successful this function returns data in the following "out"-arguments:
 *  o res_vnode: the vnode of the parent directory of "name"
 *  o name: the `basename' (the element of the pathname)
 *  o namelen: the length of the basename
 *
 * For example: dir_namev("/s5fs/bin/ls", &namelen, &name, NULL,
 * &res_vnode) would put 2 in namelen, "ls" in name, and a pointer to the
 * vnode corresponding to "/s5fs/bin" in res_vnode.
 *
 * The "base" argument defines where we start resolving the path from:
 * A base value of NULL means to use the process's current working directory,
 * curproc->p_cwd.  If pathname[0] == '/', ignore base and start with
 * vfs_root_vn.  dir_namev() should call lookup() to take care of resolving each
 * piece of the pathname.
 *
 * Note: A successful call to this causes vnode refcount on *res_vnode to
 * be incremented.
 */
 /*
    ENOENT for path not exist
    ENAMETOOLONG if a component of path is too long
  */
int
dir_namev(const char *pathname, size_t *namelen, const char **name,
          vnode_t *base, vnode_t **res_vnode)
{
        //TODO: check
        if(pathname == NULL)
            return -ENOENT;
        int path_len = strlen(pathname);
        if(path_len == 0)
            return -ENOENT;
        else if(path_len >= MAXPATHLEN)
            return -ENAMETOOLONG;

        /* initialize the output arguments */
        *res_vnode = NULL;

        vnode_t* parent = NULL;
        if(base == NULL)
        {
            parent = curproc->p_cwd;
        }else 
        {
            if(pathname[0] == '/')
            {
                parent = vfs_root_vn;
            }else
            {
                parent = base;
            }
        }
        vref(parent);
        int i = 0;
        while( i < path_len )
        {
            if(pathname[i] == '/')
            {
                i++;
                continue;
            }else
            {
                /* find the next one, using greedy approach */
                int start_index = i;
                int end_index = i+1;
                int should_terminate = 0;
                while(end_index < path_len && pathname[end_index] != '/')
                {
                    end_index++;
                }
                if(end_index - start_index > NAME_LEN)
                {
                    vput(parent);
                    return -ENAMETOOLONG;
                }
                if(end_index == path_len)
                    should_terminate = 1;

                char tmp_name[NAME_LEN+1];
                const char* tmp_name_ptr = tmp_name;
                memset(tmp_name,0,sizeof(tmp_name));
                size_t tmp_name_len = end_index-start_index;
                strncpy(tmp_name, pathname+start_index,tmp_name_len);
                tmp_name[tmp_name_len] = '\0';
                if(should_terminate == 1)
                {
                    if(!S_ISDIR(parent->vn_mode))
                    {
                        vput(parent);
                        return -ENOTDIR;
                    }
                    *namelen = tmp_name_len;
                    strncpy(*name, tmp_name_ptr, *namelen);
                    *((*name)+namelen) = '\0';
                    *res_vnode = parent;
                    return 0;
                }
                int ret = 0;
                vnote_t* next = NULL;
                if((ret = lookup(parent, tmp_name, &next)) < 0)
                {
                    return ret;
                }else
                {
                    parent = next;
                }
            }
        }

        /* If the program gets to here, means the string is 
         * like: /s5fs/bin/ls/, /s5fs/bin//
         */
        *res_vnode = parent;
        *namelen = 0;
        return 0;
}

/* This returns in res_vnode the vnode requested by the other parameters.
 * It makes use of dir_namev and lookup to find the specified vnode (if it
 * exists).  flag is right out of the parameters to open(2); see
 * <weenix/fnctl.h>.  If the O_CREAT flag is specified, and the file does
 * not exist call create() in the parent directory vnode.
 *
 * Note: Increments vnode refcount on *res_vnode.
 */
int
open_namev(const char *pathname, int flag, vnode_t **res_vnode, vnode_t *base)
{
    vnote_t* dir = NULL;
    size_t name_len = 0;
    char name[NAME_LEN+1];
    const char* name = name;
    int ret = 0;
    if((ret = dir_namev(pathname, &name_len, &name, NULL, &dir)) < 0)
    {
        if(dir)
        {
            vput(dir);
        }
        return ret;
    }
    KASSERT(dir);
    vnode_t* target = NULL;
    ret = lookup(dir, name, name_len, &target);
    if(ret == 0) /* already there*/
    {   
        vput(dir);
        return target;
    }else if(ret < 0)
    {
        if(ret == -ENOENT)
        {
            if((flag & O_CREAT) != 0)
            {
                vnode_ops* ops = dir->vn_ops;
                KASSERT(ops);
                KASSERT(ops->create != NULL);
                vnode_t* result = NULL;
                if((ret = ops->create(dir, name, name_len, &result)) < 0)
                {
                    if(result)
                    {
                        vput(result);
                    }
                    vput(dir);
                    return ret;
                }
            }else
            {
                vput(dir);
                return ret;
            }
        }else
        {
            return ret;
        }
    }
}

#ifdef __GETCWD__
/* Finds the name of 'entry' in the directory 'dir'. The name is writen
 * to the given buffer. On success 0 is returned. If 'dir' does not
 * contain 'entry' then -ENOENT is returned. If the given buffer cannot
 * hold the result then it is filled with as many characters as possible
 * and a null terminator, -ERANGE is returned.
 *
 * Files can be uniquely identified within a file system by their
 * inode numbers. */
int
lookup_name(vnode_t *dir, vnode_t *entry, char *buf, size_t size)
{
        NOT_YET_IMPLEMENTED("GETCWD: lookup_name");
        return -ENOENT;
}


/* Used to find the absolute path of the directory 'dir'. Since
 * directories cannot have more than one link there is always
 * a unique solution. The path is writen to the given buffer.
 * On success 0 is returned. On error this function returns a
 * negative error code. See the man page for getcwd(3) for
 * possible errors. Even if an error code is returned the buffer
 * will be filled with a valid string which has some partial
 * information about the wanted path. */
ssize_t
lookup_dirpath(vnode_t *dir, char *buf, size_t osize)
{
        NOT_YET_IMPLEMENTED("GETCWD: lookup_dirpath");

        return -ENOENT;
}
#endif /* __GETCWD__ */
