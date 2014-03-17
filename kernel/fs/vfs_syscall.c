/*
 *  FILE: vfs_syscall.c
 *  AUTH: mcc | jal
 *  DESC:
 *  DATE: Wed Apr  8 02:46:19 1998
 *  $Id: vfs_syscall.c,v 1.9.2.2 2006/06/04 01:02:32 afenn Exp $
 */

#include "kernel.h"
#include "errno.h"
#include "globals.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/vnode.h"
#include "fs/vfs_syscall.h"
#include "fs/open.h"
#include "fs/fcntl.h"
#include "fs/lseek.h"
#include "mm/kmalloc.h"
#include "util/string.h"
#include "util/printf.h"
#include "fs/stat.h"
#include "util/debug.h"

/* To read a file:
 *      o fget(fd)
 *      o call its virtual read f_op
 *      o update f_pos
 *      o fput() it
 *      o return the number of bytes read, or an error
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not a valid file descriptor or is not open for reading.
 *      o EISDIR
 *        fd refers to a directory.
 *
 * In all cases, be sure you do not leak file refcounts by returning before
 * you fput() a file that you fget()'ed.
 */
int
do_read(int fd, void *buf, size_t nbytes)
{
        if( !(fd >= 0 && fd < NFILES) )
                return -EBADF;
        
        file_t* file = NULL;
        if((file = fget(fd)) == NULL || !FMODE_ISREAD(file->f_mode))
        {
            if(file)
                fput(file);

            return -EBADF;
        }
        vnode_t* vnode = file->f_vnode;
        KASSERT(vnode);
        /* The fd is for directory */
        if(S_ISDIR(vnode->vn_mode))
        {
            fput(file);
            return -EISDIR;
        }

        vnode_ops_t* ops = vnode->vn_ops;

        KASSERT(ops);
        KASSERT(ops->read != NULL);
        int byte_count = ops->read(vnode,file->f_pos, buf, nbytes);
        file->f_pos += byte_count;
        /* release */
        fput(file);

        return byte_count;
}

/* Very similar to do_read.  Check f_mode to be sure the file is writable.  If
 * f_mode & FMODE_APPEND, do_lseek() to the end of the file, call the write
 * f_op, and fput the file.  As always, be mindful of refcount leaks.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not a valid file descriptor or is not open for writing.
 */
int
do_write(int fd, const void *buf, size_t nbytes)
{
        if(!(fd >= 0 && fd < NFILES))
                return -EBADF;
        
        file_t* file = NULL;

        /* Only read? */
        if(((file = fget(fd)) == NULL) || FMODE_ISREAD(file->f_mode))
        {
            if(file)
                fput(file);
            return -EBADF;
        }
        
        /* Append mode, do lseek to the end firstly*/
        if(FMODE_ISAPPPEND(file->f_mode))
        {
            int ret = 0;
            if((ret = do_lseek(fd, 0, SEEK_END)) < 0)
            {
                fput(file);
                return ret;
            }
        }

        vnode_t* vnode = file->f_vnode;
        KASSERT(vnode);

        vnode_ops_t* ops = vnode->vn_ops;
        KASSERT(ops);
        KASSERT(ops->write != NULL);

        int bytes_count = ops->write(file, file->f_pos, buf, nbytes);
        file->f_pos += bytes_count;
        fput(file);

        return bytes_count;
}

/*
 * Zero curproc->p_files[fd], and fput() the file. Return 0 on success
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd isn't a valid open file descriptor.
 */
int
do_close(int fd)
{
        if(!(fd >= 0 && fd < NFILES))
                return -EBADF;
        
        file_t* file = NULL;

        if((file = fget(fd))== NULL)
        {
            return -EBADF;
        }

        KASSERT(file == curproc->p_files[fd]);
        /* close it*/
        fput(file);
        
        /* zero it*/
        curproc->p_files[fd] = NULL;
        
        /* return zero for success*/
        return 0;
}

/* To dup a file:
 *      o fget(fd) to up fd's refcount
 *      o get_empty_fd()
 *      o point the new fd to the same file_t* as the given fd
 *      o return the new file descriptor
 *
 * Don't fput() the fd unless something goes wrong.  Since we are creating
 * another reference to the file_t*, we want to up the refcount.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd isn't an open file descriptor.
 *      o EMFILE
 *        The process already has the maximum number of file descriptors open
 *        and tried to open a new one.
 */
int
do_dup(int fd)
{
        if(!(fd >= 0 && fd < NFILES))
                return -EBADF;
        
        file_t* file = NULL;

        if((file  = fget(fd)) == NULL)
        {
            return -EBADF;
        }
        KASSERT(file == curproc->p_files[fd]);
        int id = -1;
        if((id = get_empty_fd(curproc))< 0)
        {
            fput(file);
            return id; /* Implicit EMFILE*/
        }
        else
            curproc->p_files[id] = curproc->p_files[fd];
        
        return id;
}

/* Same as do_dup, but insted of using get_empty_fd() to get the new fd,
 * they give it to us in 'nfd'.  If nfd is in use (and not the same as ofd)
 * do_close() it first.  Then return the new file descriptor.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        ofd isn't an open file descriptor, or nfd is out of the allowed
 *        range for file descriptors.
 */
int
do_dup2(int ofd, int nfd)
{
    /* check range*/
        if(!(ofd >= 0 && ofd < NFILES) || !(nfd >= 0 && nfd < NFILES))
            return -EBADF;
        
        file_t* file = NULL;
        if((file = fget(ofd)) == NULL)
        {
            return -EBADF;
        }
        KASSERT(file == curproc->p_files[ofd]);
        /* In use */
        if(curproc->p_files[nfd] != NULL)
        {
            if(nfd != ofd)
            {
                 do_close(nfd);
            }
            else
            {
                /* dup to itself??? No */
                fput(file);
                return ofd;
            }
        }
        /* new one puts to old one*/
        curproc->p_files[nfd] = curproc->p_files[ofd];
        return nfd;
        
}

/*
 * This routine creates a special file of the type specified by 'mode' at
 * the location specified by 'path'. 'mode' should be one of S_IFCHR or
 * S_IFBLK (you might note that mknod(2) normally allows one to create
 * regular files as well-- for simplicity this is not the case in Weenix).
 * 'devid', as you might expect, is the device identifier of the device
 * that the new special file should represent.
 *
 * You might use a combination of dir_namev, lookup, and the fs-specific
 * mknod (that is, the containing directory's 'mknod' vnode operation).
 * Return the result of the fs-specific mknod, or an error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        mode requested creation of something other than a device special
 *        file.
 *      o EEXIST
 *        path already exists.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_mknod(const char *path, int mode, unsigned devid)
{
        if(!S_IFCHR(mode) && !S_IFBLK(mode))
            return -EINVAL;
        size_t name_len = 0;
        char name[NAME_LEN+1];
        const char* nameptr = name;
        int ret = 0;
        vnode_t* dir = NULL;
        if((ret = dir_namev(path, &name_len, &nameptr, NULL, dir)) < 0)
        {
            if(dir)
            {
                vput(dir);
            }
            return ret; /* Implicit errors includes ENAMETOOLONG, ENOTDIR */
        }
        vnode_t* target = NULL;
        /* lookup */
        ret = lookup(dir, name, name_len, &target);
        if(ret == 0)
        {
            KASSERT(target);
            /* already exist */
            vput(target);
            vput(dir);
            return -EEXIST;
        }else if(ret != -ENOENT)
        {
            /* Bad thing happens in lookup */
            vput(dir);
            return ret;
        }else
        {
            /* ret is -ENOENT*/
            ret = target->
        }
}

/* Use dir_namev() to find the vnode of the dir we want to make the new
 * directory in.  Then use lookup() to make sure it doesn't already exist.
 * Finally call the dir's mkdir vn_ops. Return what it returns.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EEXIST
 *        path already exists.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_mkdir(const char *path)
{
    if(path == NULL || strlen(path) == 0)
        return -ENOENT;
    size_t name_len;
    char name[NAME_LEN+1];
    const char* nameptr = name;
    vnote_t* dir = NULL;
    int ret = 0;
    if((ret = dir_namev(path, &name_len, nameptr, NULL, &dir)) < 0)
    {
        if(node)
        {
            vput(node);
        }
        return ret;
    }

    vnode_t* node_file;
    ret = lookup(dir, name, name_len, &node_file);
    // TODO: dif
    // TODO: need verification
    if(ret == 0)
    {
        // not exist
        vnode_ops_t* ops = node_file->vn_ops;
        KASSERT(ops);
        ret = ops->mkdir(dir, name, name_len);
        if(dir)
            vput(dir);
        if(node_file)
            vput(node_file);
    }else if(ret < 0)
    {
        return ret;
    }else
    {
        return -EEXIST;
    }
}

/* Use dir_namev() to find the vnode of the directory containing the dir to be
 * removed. Then call the containing dir's rmdir v_op.  The rmdir v_op will
 * return an error if the dir to be removed does not exist or is not empty, so
 * you don't need to worry about that here. Return the value of the v_op,
 * or an error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        path has "." as its final component.
 *      o ENOTEMPTY
 *        path has ".." as its final component.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_rmdir(const char *path)
{
    if(path == NULL || strlen(path) == 0)
    {
        return -ENOENT;
    }
    size_t name_len = 0;
    vnode_t* node = NULL;
    char name[NAME_LEN+1];
    const char* nameptr = name;
    /* TODO: base not sure*/
    int ret = dir_namev(path,&name_len, nameptr, NULL, &node)；
    if(ret < 0)
    {
        if(node)
            vput(node);
        return ret;
    }

    if(node == NULL)
    {
        return -ENOENT;
    }
    else if(!S_ISDIR(node->vn_mode))
    {
        if(node)
        {
            vput(node);
        }
        return -ENOTDIR;
    }else if(strcmp(name,'.') == 0)
    {
        if(node)
        {
            vput(node);
        }
        return -EINVAL;
    }else if(strcmp(name, '..') == 0)
    {
        if(node)
        {
            vput(node);
        }
        return -ENOTEMPTY;
    }

    
    vnode_ops_t* ops = node->vn_ops;
    KASSERT(ops != NULL);

    ret = ops->rmdir(node, name, name_len);
    vput(node);
    return node;
}

/*
 * Same as do_rmdir, but for files.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EISDIR
 *        path refers to a directory.
 *      o ENOENT
 *        A component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_unlink(const char *path)
{

    if(path == NULL || strlen(path) == 0)
    {
        return -ENOENT;
    }
    size_t name_len = 0;
    vnode_t* node = NULL;
    char name[NAME_LEN+1];
    int ret = dir_namev(path,&name_len, &name, NULL, &node)；
    if(ret < 0)
    {
        if(node)
            vput(node);
        return ret;
    }

    /*TODO: need to lookup? */
    if(node == NULL)
    {
        return -ENOENT;
    }
    else if(S_ISDIR(node->vn_mode))
    {
        if(node)
        {
            vput(node);
        }
        return -EISDIR;
    }

    
    vnode_ops_t* ops = node->vn_ops;
    KASSERT(ops != NULL);

    ret = ops->unlink(node, name, name_len);
    vput(node);
    return node;
}

/* To link:
 *      o open_namev(from)
 *      o dir_namev(to)
 *      o call the destination dir's (to) link vn_ops.
 *      o return the result of link, or an error
 *
 * Remember to vput the vnodes returned from open_namev and dir_namev.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EEXIST
 *        to already exists.
 *      o ENOENT
 *        A directory component in from or to does not exist.
 *      o ENOTDIR
 *        A component used as a directory in from or to is not, in fact, a
 *        directory.
 *      o ENAMETOOLONG
 *        A component of from or to was too long.
 */
int
do_link(const char *from, const char *to)
{
    /* TODO: need verify*/
    if( from == NULL || strlen(from) == 0 || to == NULL || strlen(to) == 0)
        return -ENOENT;

    vnote_t* from_node = NULL;
    int ret = 0;
    if((ret = open_namev(from, O_RDONLY, &from_node, NULL)) < 0)
    {
        if(from_node)
        {
            vput(from_node);
        }
        return ret;
    }

    if(!S_ISDIR(from_node->vn_mode))
    {
        vput(from_node);
        return -ENOTDIR;
    }

    vnode_t* to_node = NULL;
    size_t name_len;
    char name[NAME_LEN+1];
    const char* nameptr = name;
    if((ret = dir_namev(to, &name_len, nameptr, NULL, &to_node)) < 0)
    {
        if(from_node)
        {
            vput(from_node);
        }
        if(to_node)
        {
            vput(to_node);
        }
        return ret;
    }

    vnode_t* tmp_node = NULL;
    if((ret = lookup(to_node, nameptr, name_len, &tmp_node)) < 0)
    {
        if(from_node)
            vput(from_node);
        if(to_node)
            vput(to_node);
        if(tmp_node)
            vput(tmp_node);
        return ret;
    }
    else if(ret == 0)
    {
        return -EEXIST;
    }
    /* ops */

    vnode_ops_t* ops = from_node->vn_ops;
    KASSERT(ops != NULL);
    ret = ops->link(from_node, to_node, name, name_len);
    vput(from_node);
    vput(to_node);
    /* do not vput(tmp node) because it's not there*/
    return ret;
}

/*      o link newname to oldname
 *      o unlink oldname
 *      o return the value of unlink, or an error
 *
 * Note that this does not provide the same behavior as the
 * Linux system call (if unlink fails then two links to the
 * file could exist).
 */
int
do_rename(const char *oldname, const char *newname)
{
        int ret = 0;
        /* link the new name to old name*/
        if((ret = do_link(newname, oldname)) < 0)
        {
            return ret;
        }

        /* unlink the old name*/
        ret = do_unlink(oldname);
        
        return ret;
}

/* Make the named directory the current process's cwd (current working
 * directory).  Don't forget to down the refcount to the old cwd (vput()) and
 * up the refcount to the new cwd (open_namev() or vget()). Return 0 on
 * success.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o ENOENT
 *        path does not exist.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 *      o ENOTDIR
 *        A component of path is not a directory.
 */
int
do_chdir(const char *path)
{
    if(path == NULL || strlen(path) == 0)
        return -ENOENT;
    int ret = 0;
    vnote_t* node = NULL;
    if((ret = open_namev(path, O_RDONLY, &node, NULL)) < 0)
    {
        if(node)
        {
            vput(node);
        }
        return ret;
    }
    if(S_ISDIR(node->vn_mode))
    {
        vput(node);
        return -ENOTDIR;
    }
    vnote_t* old = curproc->p_cwd;
    curproc->p_cwd = node;
    vput(old);
    return 0;
}

/* Call the readdir f_op on the given fd, filling in the given dirent_t*.
 * If the readdir f_op is successful, it will return a positive value which
 * is the number of bytes copied to the dirent_t.  You need to increment the
 * file_t's f_pos by this amount.  As always, be aware of refcounts, check
 * the return value of the fget and the virtual function, and be sure the
 * virtual function exists (is not null) before calling it.
 *
 * Return either 0 or sizeof(dirent_t), or -errno.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        Invalid file descriptor fd.
 *      o ENOTDIR
 *        File descriptor does not refer to a directory.
 */
int
do_getdent(int fd, struct dirent *dirp)
{

        if(!(fd >= 0 && fd < NFILES))
            return -EBADF;
        file_t* file = NULL;
        if((file == fget(fd)) == NULL)
        {
            return -EBADF;
        }

        vnode_t* node = file->f_vnode;
        KASSERT(node);
        if(!S_ISDIR(node->vn_mode))
        {
            fput(file);
            return -ENOTDIR;
        }

        vnode_ops_t* ops = node->vn_ops;
        KASSERT(ops);

        off_t offset;
        int ret = ops->readdir(node, offset, dirp);
        file->f_pos += ret;
        fput(file);

        return ret == 0?0:sizeof(dirent);
        /*return -1;*/
}

/*
 * Modify f_pos according to offset and whence.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not an open file descriptor.
 *      o EINVAL
 *        whence is not one of SEEK_SET, SEEK_CUR, SEEK_END; or the resulting
 *        file offset would be negative.
 */
int
do_lseek(int fd, int offset, int whence)
{
        if(!(fd >= 0 && fd < NFILES))
            return -EBADF;
        file_t* file = NULL;
        if((file == fget(fd)) == NULL)
        {
            return -EBADF;
        }
        vnode_t* vnode = file->f_vnode;
        KASSERT(vnode);
        switch(whence)
        {
                case SEEK_SET:
                {
                        if(offset < 0)
                        {
                                fput(file);
                                return -EINVAL;
                        }
                        else
                                file->f_pos = offset;
                        break;
                }
                case SEEK_CUR:
                {
                        if(file->f_pos + offset < 0)
                        {
                                fput(file);
                                return -EINVAL;
                        }
                        else
                                file->f_pos += offset;
                        break;
                }
                case SEEK_END:
                {
                        if(offset > vnode->vn_len)
                        {
                                fput(file);
                                return -EINVAL;
                        }
                        else
                                file->f_pos = vnode->vn_len - offset;
                        break;
                }
                default:
                {
                        fput(file);
                        return -EINVAL;
                }
        }
        int off = file->f_pos;
        fput(file);
        return off;
}

/*
 * Find the vnode associated with the path, and call the stat() vnode operation.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o ENOENT
 *        A component of path does not exist.
 *      o ENOTDIR
 *        A component of the path prefix of path is not a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_stat(const char *path, struct stat *buf)
{
        if(path == NULL || strlen(path) == 0)
            return -ENOENT;
        KASSERT(buf != NULL);
        int ret = 0;
        vnote_t* result = NULL;
        /* TODO: flag*/
        if((ret = open_namev(path, O_RDWR, &result, NULL) < 0)
        {
            if(result)
            {
                vput(result);
            }
            return ret;
        }

        KASSERT(result);
        vnode_ops_t* ops = result->vn_ops;
        KASSERT(ops);
        ret = ops->stat(result, buf);
        vput(result);
        return ret;
        
}

#ifdef __MOUNTING__
/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutely sure your Weenix is perfect.
 *
 * This is the syscall entry point into vfs for mounting. You will need to
 * create the fs_t struct and populate its fs_dev and fs_type fields before
 * calling vfs's mountfunc(). mountfunc() will use the fields you populated
 * in order to determine which underlying filesystem's mount function should
 * be run, then it will finish setting up the fs_t struct. At this point you
 * have a fully functioning file system, however it is not mounted on the
 * virtual file system, you will need to call vfs_mount to do this.
 *
 * There are lots of things which can go wrong here. Make sure you have good
 * error handling. Remember the fs_dev and fs_type buffers have limited size
 * so you should not write arbitrary length strings to them.
 */
int
do_mount(const char *source, const char *target, const char *type)
{
        NOT_YET_IMPLEMENTED("MOUNTING: do_mount");
        return -EINVAL;
}

/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutley sure your Weenix is perfect.
 *
 * This function delegates all of the real work to vfs_umount. You should not worry
 * about freeing the fs_t struct here, that is done in vfs_umount. All this function
 * does is figure out which file system to pass to vfs_umount and do good error
 * checking.
 */
int
do_umount(const char *target)
{
        NOT_YET_IMPLEMENTED("MOUNTING: do_umount");
        return -EINVAL;
}
#endif
