/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001  Miklos Szeredi (mszeredi@inf.bme.hu)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include "fuse_i.h"
#include <linux/fuse.h>


#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>


static inline struct node *get_node(fino_t ino)
{
    return (struct node *) ((ino << 3) + 0x8000000);
}

static inline fino_t get_ino(struct node *node)
{
    return (((fino_t) node) - 0x8000000) >> 3;
}

static guint name_hash(const struct node *node)
{
    return g_str_hash(node->name) ^ node->parent;
}

static gint name_compare(const struct node *node1, const struct node *node2)
{
    return
        node1->parent == node2->parent &&
        strcmp(node1->name, node2->name) == 0;
}

static int free_node(struct node *node)
{
    g_free(node->name);
    g_free(node);
    return 1;
}

static struct node *lookup_node(struct fuse *f, fino_t parent,
                                const char *name)
{
    struct node tmp;

    tmp.name = (char *) name;
    tmp.parent = parent;

    return g_hash_table_lookup(f->nametab, &tmp);
}

static void hash_node(struct fuse *f, struct node *node, fino_t parent,
                      const char *name)
{
    node->name = g_strdup(name);
    node->parent = parent;
    g_hash_table_insert(f->nametab, node, node);
}

static void unhash_node(struct fuse *f, struct node *node)
{
    if(node->name != NULL) {
        g_hash_table_remove(f->nametab, node);
        g_free(node->name);
        node->parent = 0;
        node->name = NULL;
    }
}

static fino_t find_node(struct fuse *f, fino_t parent, char *name,
                        struct fuse_attr *attr, int version)
{
    struct node *node;
    int mode = attr->mode & S_IFMT;
    int rdev = 0;
    
    if(S_ISCHR(mode) || S_ISBLK(mode))
        rdev = attr->rdev;

    pthread_mutex_lock(&f->lock);
    node = lookup_node(f, parent, name);
    if(node != NULL) {
        if(node->mode == mode && node->rdev == rdev)
            goto out;
        
        unhash_node(f, node);
    }

    node = g_new0(struct node, 1);
    node->mode = mode;
    node->rdev = rdev;    
    hash_node(f, node, parent, name);

  out:
    node->version = version;
    pthread_mutex_unlock(&f->lock);
    return get_ino(node);
}

static fino_t find_node_dir(struct fuse *f, fino_t parent, char *name)
{
    struct node *node;

    pthread_mutex_lock(&f->lock);
    node = lookup_node(f, parent, name);
    pthread_mutex_unlock(&f->lock);

    if(node != NULL)
        return get_ino(node);
    else
        return (fino_t) -1;
}

static char *get_path(struct fuse *f, fino_t ino)
{
    GString *s;
    char *ss;

    s = g_string_new("");
    if(ino == FUSE_ROOT_INO)
        g_string_prepend_c(s, '/');
    else {
        struct node *node;
        pthread_mutex_lock(&f->lock);
        for(; ino != FUSE_ROOT_INO; ino = node->parent) {
            node = get_node(ino);
            if(node->name == NULL) {
                pthread_mutex_unlock(&f->lock);
                g_string_free(s, TRUE);
                return NULL;
            }
            g_string_prepend(s, node->name);
            g_string_prepend_c(s, '/');
        }
        pthread_mutex_unlock(&f->lock);
    }

    ss = s->str;
    g_string_free(s, FALSE);
    
    return ss;
}

static char *get_path_name(struct fuse *f, fino_t ino, const char *name)
{
    char *path2;
    char *path;
    
    if(ino == FUSE_ROOT_INO)
        return g_strconcat("/", name, NULL);

    path = get_path(f, ino);
    if(path == NULL)
        return NULL;
    
    path2 = g_strconcat(path, "/", name, NULL);
    g_free(path);

    return path2;
}

static void destroy_node(struct fuse *f, fino_t ino, int version)
{
    struct node *node;

    pthread_mutex_lock(&f->lock);
    node = get_node(ino);
    if(node->version == version) {
        unhash_node(f, node);
        free_node(node);
    }
    pthread_mutex_unlock(&f->lock);

}

static void remove_node(struct fuse *f, fino_t dir, const char *name)
{
    struct node *node;

    pthread_mutex_lock(&f->lock);
    node = lookup_node(f, dir, name);
    assert(node != NULL);
    unhash_node(f, node);
    pthread_mutex_unlock(&f->lock);
}

static void rename_node(struct fuse *f, fino_t olddir, const char *oldname,
                        fino_t newdir, const char *newname)
{
    struct node *node;
    struct node *newnode;
    
    pthread_mutex_lock(&f->lock);
    node  = lookup_node(f, olddir, oldname);
    newnode  = lookup_node(f, newdir, newname);
    assert(node != NULL);

    if(newnode != NULL)
        unhash_node(f, newnode);
        
    unhash_node(f, node);
    hash_node(f, node, newdir, newname);
    pthread_mutex_unlock(&f->lock);
}


static void convert_stat(struct stat *stbuf, struct fuse_attr *attr)
{
    attr->mode    = stbuf->st_mode;
    attr->nlink   = stbuf->st_nlink;
    attr->uid     = stbuf->st_uid;
    attr->gid     = stbuf->st_gid;
    attr->rdev    = stbuf->st_rdev;
    attr->size    = stbuf->st_size;
    attr->blksize = stbuf->st_blksize;
    attr->blocks  = stbuf->st_blocks;
    attr->atime   = stbuf->st_atime;
    attr->mtime   = stbuf->st_mtime;
    attr->ctime   = stbuf->st_ctime;
}

static int fill_dir(struct fuse_dirhandle *dh, char *name, int type)
{
    struct fuse_dirent dirent;
    size_t reclen;
    size_t res;

    dirent.ino = find_node_dir(dh->fuse, dh->dir, name);
    dirent.namelen = strlen(name);
    strncpy(dirent.name, name, sizeof(dirent.name));
    dirent.type = type;
    reclen = FUSE_DIRENT_SIZE(&dirent);
    res = fwrite(&dirent, reclen, 1, dh->fp);
    if(res == 0) {
        perror("writing directory file");
        return -EIO;
    }
    return 0;
}

static void send_reply(struct fuse *f, struct fuse_in_header *in, int error,
                       void *arg, size_t argsize)
{
    int res;
    char *outbuf;
    size_t outsize;
    struct fuse_out_header *out;

    if(error > 0) {
        fprintf(stderr, "positive error code: %i\n",  error);
        error = -ERANGE;
    }

    if(error)
        argsize = 0;

    outsize = sizeof(struct fuse_out_header) + argsize;
    outbuf = (char *) g_malloc(outsize);
    out = (struct fuse_out_header *) outbuf;
    out->unique = in->unique;
    out->error = error;
    if(argsize != 0)
        memcpy(outbuf + sizeof(struct fuse_out_header), arg, argsize);

    printf("   unique: %i, error: %i (%s), outsize: %i\n", out->unique,
           out->error, strerror(-out->error), outsize);
    fflush(stdout);
                
    res = write(f->fd, outbuf, outsize);
    if(res == -1)
        perror("writing fuse device");

    g_free(outbuf);
}

static void fill_cred(struct fuse_in_header *in, struct fuse_cred *cred)
{
    cred->uid = in->uid;
    cred->gid = in->gid;
}

static void do_lookup(struct fuse *f, struct fuse_in_header *in, char *name)
{
    int res;
    char *path;
    struct stat buf;
    struct fuse_lookup_out arg;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.getattr)
            res = f->op.getattr(&cred, path, &buf);
        g_free(path);
    }
    if(res == 0) {
        convert_stat(&buf, &arg.attr);
        arg.ino = find_node(f, in->ino, name, &arg.attr, in->unique);
    }
    send_reply(f, in, res, &arg, sizeof(arg));
}

static void do_forget(struct fuse *f, struct fuse_in_header *in,
                      struct fuse_forget_in *arg)
{
    destroy_node(f, in->ino, arg->version);
}

static void do_getattr(struct fuse *f, struct fuse_in_header *in)
{
    int res;
    char *path;
    struct stat buf;
    struct fuse_getattr_out arg;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path(f, in->ino);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.getattr)
            res = f->op.getattr(&cred, path, &buf);
        g_free(path);
    }
    if(res == 0) 
        convert_stat(&buf, &arg.attr);

    send_reply(f, in, res, &arg, sizeof(arg));
}

static void do_setattr(struct fuse *f, struct fuse_in_header *in,
                       struct fuse_setattr_in *arg)
{
    int res;
    char *path;
    int valid = arg->valid;
    struct fuse_attr *attr = &arg->attr;
    struct fuse_setattr_out outarg;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path(f, in->ino);
    if(path != NULL) {
        res = 0;
        if(!res && (valid & FATTR_MODE)) {
            res = -ENOSYS;
            if(f->op.chmod)
                res = f->op.chmod(&cred, path, attr->mode);
        }        
        if(!res && (valid & (FATTR_UID | FATTR_GID))) {
            uid_t uid = (valid & FATTR_UID) ? attr->uid : (uid_t) -1;
            gid_t gid = (valid & FATTR_GID) ? attr->gid : (gid_t) -1;
            
            res = -ENOSYS;
            if(f->op.chown)
                res = f->op.chown(&cred, path, uid, gid);
        }
        if(!res && (valid & FATTR_SIZE)) {
            res = -ENOSYS;
            if(f->op.truncate && f->op.getattr) {
                res = f->op.truncate(&cred, path, attr->size);
                if(!res) {
                    struct stat buf;
                    res = f->op.getattr(&cred, path, &buf);
                    outarg.newsize = buf.st_size;
                }
            }
        }
        if(!res && (valid & FATTR_UTIME)) {
            struct utimbuf buf;
            buf.actime = attr->atime;
            buf.modtime = attr->mtime;
            res = -ENOSYS;
            if(f->op.utime)
                res = f->op.utime(&cred, path, &buf);
        }
        g_free(path);
    }
    send_reply(f, in, res, &outarg, sizeof(outarg));
}

static void do_readlink(struct fuse *f, struct fuse_in_header *in)
{
    int res;
    char link[PATH_MAX + 1];
    char *path;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path(f, in->ino);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.readlink)
            res = f->op.readlink(&cred, path, link, sizeof(link));
        g_free(path);
    }
    link[PATH_MAX] = '\0';
    send_reply(f, in, res, link, !res ? strlen(link) : 0);
}

static void do_getdir(struct fuse *f, struct fuse_in_header *in)
{
    int res;
    struct fuse_getdir_out arg;
    struct fuse_dirhandle dh;
    char *path;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    dh.fuse = f;
    dh.fp = tmpfile();
    dh.dir = in->ino;
    res = -ENOENT;
    path = get_path(f, in->ino);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.getdir)
            res = f->op.getdir(&cred, path, &dh, (fuse_dirfil_t) fill_dir);
        g_free(path);
    }
    fflush(dh.fp);
    arg.fd = fileno(dh.fp);
    send_reply(f, in, res, &arg, sizeof(arg));
    fclose(dh.fp);
}

static void do_mknod(struct fuse *f, struct fuse_in_header *in,
                     struct fuse_mknod_in *inarg)
{
    int res;
    char *path;
    struct fuse_mknod_out outarg;
    struct stat buf;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path_name(f, in->ino, inarg->name);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.mknod && f->op.getattr) {
            res = f->op.mknod(&cred, path, inarg->mode, inarg->rdev);
            if(res == 0)
                res = f->op.getattr(&cred, path, &buf);
        }
        g_free(path);
    }
    if(res == 0) {
        convert_stat(&buf, &outarg.attr);
        outarg.ino = find_node(f, in->ino, inarg->name, &outarg.attr,
                               in->unique);
    }

    send_reply(f, in, res, &outarg, sizeof(outarg));
}

static void do_mkdir(struct fuse *f, struct fuse_in_header *in,
                     struct fuse_mkdir_in *inarg)
{
    int res;
    char *path;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path_name(f, in->ino, inarg->name);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.mkdir)
            res = f->op.mkdir(&cred, path, inarg->mode);
        g_free(path);
    }
    send_reply(f, in, res, NULL, 0);
}

static void do_remove(struct fuse *f, struct fuse_in_header *in, char *name)
{
    int res;
    char *path;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if(path != NULL) {
        res = -ENOSYS;
        if(in->opcode == FUSE_UNLINK) {
            if(f->op.unlink)
                res = f->op.unlink(&cred, path);
        }
        else {
            if(f->op.rmdir)
                res = f->op.rmdir(&cred, path);
        }
        g_free(path);
    }
    if(res == 0)
        remove_node(f, in->ino, name);
    send_reply(f, in, res, NULL, 0);
}

static void do_symlink(struct fuse *f, struct fuse_in_header *in, char *name,
                       char *link)
{
    int res;
    char *path;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.symlink)
            res = f->op.symlink(&cred, link, path);
        g_free(path);
    }
    send_reply(f, in, res, NULL, 0);
}

static void do_rename(struct fuse *f, struct fuse_in_header *in,
                      struct fuse_rename_in *inarg)
{
    int res;
    fino_t olddir = in->ino;
    fino_t newdir = inarg->newdir;
    char *oldname = inarg->names;
    char *newname = inarg->names + strlen(oldname) + 1;
    char *oldpath;
    char *newpath;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    oldpath = get_path_name(f, olddir, oldname);
    if(oldpath != NULL) {
        newpath = get_path_name(f, newdir, newname);
        if(newpath != NULL) {
            res = -ENOSYS;
            if(f->op.rename)
                res = f->op.rename(&cred, oldpath, newpath);
            if(res == 0)
                rename_node(f, olddir, oldname, newdir, newname);
            g_free(newpath);
        }
        g_free(oldpath);
    }
    send_reply(f, in, res, NULL, 0);   
}

static void do_link(struct fuse *f, struct fuse_in_header *in,
                    struct fuse_link_in *arg)
{
    int res;
    char *oldpath;
    char *newpath;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    oldpath = get_path(f, in->ino);
    if(oldpath != NULL) {
        newpath =  get_path_name(f, arg->newdir, arg->name);
        if(newpath != NULL) {
            res = -ENOSYS;
            if(f->op.link)
                res = f->op.link(&cred, oldpath, newpath);
            g_free(newpath);
        }
        g_free(oldpath);
    }
    send_reply(f, in, res, NULL, 0);   
}

static void do_open(struct fuse *f, struct fuse_in_header *in,
                    struct fuse_open_in *arg)
{
    int res;
    char *path;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    res = -ENOENT;
    path = get_path(f, in->ino);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.open)
            res = f->op.open(&cred, path, arg->flags);
        g_free(path);
    }
    send_reply(f, in, res, NULL, 0);
}

static void do_read(struct fuse *f, struct fuse_in_header *in,
                    struct fuse_read_in *arg)
{
    int res;
    char *path;
    char *buf = g_malloc(arg->size);
    size_t size;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    path = get_path(f, in->ino);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.read)
            res = f->op.read(&cred, path, buf, arg->size, arg->offset);
        g_free(path);
    }
    
    size = 0;
    if(res > 0) {
        size = res;
        res = 0;
    }

    send_reply(f, in, res, buf, size);
    g_free(buf);
}

static void do_write(struct fuse *f, struct fuse_in_header *in,
                     struct fuse_write_in *arg)
{
    int res;
    char *path;
    struct fuse_cred cred;

    fill_cred(in, &cred);
    path = get_path(f, in->ino);
    if(path != NULL) {
        res = -ENOSYS;
        if(f->op.write)
            res = f->op.write(&cred, path, arg->buf, arg->size, arg->offset);
        g_free(path);
    }
    
    if(res > 0) {
        if((size_t) res != arg->size) {
            fprintf(stderr, "short write: %u (should be %u)\n", res,
                    arg->size);
            res = -EIO;
        }
        else 
            res = 0;
    }

    send_reply(f, in, res, NULL, 0);
}

struct cmd {
    struct fuse *f;
    char *buf;
    size_t buflen;
};

static void *do_command(void *data)
{
    struct cmd *cmd = (struct cmd *) data;
    struct fuse_in_header *in = (struct fuse_in_header *) cmd->buf;
    void *inarg = cmd->buf + sizeof(struct fuse_in_header);
    size_t argsize;
    struct fuse *f = cmd->f;

    printf("unique: %i, opcode: %i, ino: %li, insize: %i\n", in->unique,
           in->opcode, in->ino, cmd->buflen);
    fflush(stdout);
    
    argsize = cmd->buflen - sizeof(struct fuse_in_header);
        
    switch(in->opcode) {
    case FUSE_LOOKUP:
        do_lookup(f, in, (char *) inarg);
        break;

    case FUSE_FORGET:
        do_forget(f, in, (struct fuse_forget_in *) inarg);
        break;

    case FUSE_GETATTR:
        do_getattr(f, in);
        break;

    case FUSE_SETATTR:
        do_setattr(f, in, (struct fuse_setattr_in *) inarg);
        break;

    case FUSE_READLINK:
        do_readlink(f, in);
        break;

    case FUSE_GETDIR:
        do_getdir(f, in);
        break;

    case FUSE_MKNOD:
        do_mknod(f, in, (struct fuse_mknod_in *) inarg);
        break;
            
    case FUSE_MKDIR:
        do_mkdir(f, in, (struct fuse_mkdir_in *) inarg);
        break;
            
    case FUSE_UNLINK:
    case FUSE_RMDIR:
        do_remove(f, in, (char *) inarg);
        break;

    case FUSE_SYMLINK:
        do_symlink(f, in, (char *) inarg, 
                   ((char *) inarg) + strlen((char *) inarg) + 1);
        break;

    case FUSE_RENAME:
        do_rename(f, in, (struct fuse_rename_in *) inarg);
        break;
            
    case FUSE_LINK:
        do_link(f, in, (struct fuse_link_in *) inarg);
        break;

    case FUSE_OPEN:
        do_open(f, in, (struct fuse_open_in *) inarg);
        break;

    case FUSE_READ:
        do_read(f, in, (struct fuse_read_in *) inarg);
        break;

    case FUSE_WRITE:
        do_write(f, in, (struct fuse_write_in *) inarg);
        break;

    default:
        fprintf(stderr, "Operation %i not implemented\n", in->opcode);
        /* No need to send reply to async requests */
        if(in->unique != 0)
            send_reply(f, in, -ENOSYS, NULL, 0);
    }

    g_free(cmd->buf);
    g_free(cmd);

    return NULL;
}

/* This hack makes it possible to link FUSE with or without the
   pthread library */
__attribute__((weak))
int pthread_create(pthread_t *thrid           __attribute__((unused)), 
                   const pthread_attr_t *attr __attribute__((unused)), 
                   void *(*func)(void *)      __attribute__((unused)),
                   void *arg                  __attribute__((unused)))
{
    return ENOSYS;
}

void fuse_loop(struct fuse *f)
{
    int res;
    char inbuf[FUSE_MAX_IN];
    pthread_attr_t attr;
    pthread_t thrid;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
    while(1) {
        struct cmd *cmd;

        res = read(f->fd, inbuf, sizeof(inbuf));
        if(res == -1) {
            perror("reading fuse device");
            continue;
        }
        if((size_t) res < sizeof(struct fuse_in_header)) {
            fprintf(stderr, "short read on fuse device\n");
            continue;
        }

        cmd = g_new0(struct cmd, 1);
        cmd->f = f;
        cmd->buflen = res;
        cmd->buf = g_malloc(cmd->buflen);
        memcpy(cmd->buf, inbuf, cmd->buflen);
        
        if(f->flags & FUSE_MULTITHREAD) {
            res = pthread_create(&thrid, &attr, do_command, cmd);
            if(res == 0)
                continue;
            
            fprintf(stderr, "Error creating thread: %s\n", strerror(res));
            fprintf(stderr, "Will run in single thread mode\n");
            f->flags &= ~FUSE_MULTITHREAD;
        }

        do_command(cmd);
    }
}

struct fuse *fuse_new(int flags, mode_t root)
{
    struct fuse *f = g_new0(struct fuse, 1);

    if(!root)
        root = S_IFDIR;

    if(!S_ISDIR(root) && !S_ISREG(root)) {
        fprintf(stderr, "Invalid mode for root: 0%o\n", root);
        root = S_IFDIR;
    }
    root &= S_IFMT;

    f->flags = flags;
    f->rootmode = root;
    f->fd = -1;
    f->mnt = NULL;
    f->nametab = g_hash_table_new((GHashFunc) name_hash,
                                  (GCompareFunc) name_compare);
    pthread_mutex_init(&f->lock, NULL);

    return f;
}

void fuse_set_operations(struct fuse *f, const struct fuse_operations *op)
{
    f->op = *op;
}

void fuse_destroy(struct fuse *f)
{
    close(f->fd);
    g_hash_table_foreach_remove(f->nametab, (GHRFunc) free_node, NULL);
    g_hash_table_destroy(f->nametab);
    pthread_mutex_destroy(&f->lock);
    g_free(f);
}