/*
 * fat-handler - FAT12/16/32 filesystem handler
 *
 * Copyright � 2006 Marek Szyprowski
 * Copyright � 2007-2015 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 *
 * $Id$
 */

#include <aros/macros.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <string.h>

#include "fat_fs.h"
#include "fat_protos.h"

#define DEBUG DEBUG_LOCK
#include "debug.h"

#if DEBUG == 0
#define DumpLocks(sb, glob)
#else
static void DumpLocks(struct FSSuper *sb, struct Globals *glob)
{
    struct GlobalLock *gl;
    ULONG count;

    ListLength(&sb->info->root_lock.locks, count);

    D(bug("[FAT] [%s] Global Locks Root: %ld references:\n",__FUNCTION__, count));

    ForeachNode(&sb->info->locks, gl)
    {
        ListLength(&gl->locks, count);
        D(bug("\t(%ld/%ld) ", gl->dir_cluster, gl->dir_entry));
        RawPutChars(&(gl->name[1]), gl->name[0]);
        D(bug(": %ld references\n", count));
    }
}
#endif

LONG TestLock(struct ExtFileLock *fl, struct Globals *glob)
{
    if (fl == 0 && glob->sb == NULL)
    {
        if (!glob->disk_inserted)
            return ERROR_NO_DISK;
        else
            return ERROR_NOT_A_DOS_DISK;
    }

    if (glob->sb == NULL || glob->disk_inhibited != 0
        || (fl && fl->fl_Volume != MKBADDR(glob->sb->doslist)))
        return ERROR_DEVICE_NOT_MOUNTED;

    if (fl && fl->magic != ID_FAT_DISK)
        return ERROR_OBJECT_WRONG_TYPE;

    return 0;
}

LONG LockFileByName(struct ExtFileLock *fl, UBYTE *name, LONG namelen,
    LONG access, struct ExtFileLock **lock, struct Globals *glob)
{
    LONG err = ERROR_OBJECT_NOT_FOUND;
    struct DirHandle dh;
    struct DirEntry de;
    ULONG dir_cluster;

    /* If the name is empty, just duplicate the base lock */
    if (namelen == 0)
        return CopyLock(fl, lock, glob);

    /* If the base lock is a file, the name must either be empty (handled
     * above) or start with '/' (handled here) */
    if (fl != NULL && !(fl->gl->attr & ATTR_DIRECTORY))
    {
        if (name[0] == '/')
        {
            if (namelen == 1)
                return OpLockParent(fl, lock, glob);
            else
            {
                name++;
                namelen--;
            }
        }
        else
            return ERROR_OBJECT_WRONG_TYPE;
    }

    /* The . and .. entries are invisible to the user */
    if (name[0] == '.' && (namelen == 1 || (name[1] == '.' && namelen == 2)))
    {
        D(bug("[FAT] [%s] not allowing access to '.' or '..' entries\n",__FUNCTION__));
        return ERROR_OBJECT_NOT_FOUND;
    }

    /* Get the first cluster of the directory to look for the file in */
    if (fl == NULL)
        dir_cluster = 0;
    else if (fl->gl->attr & ATTR_DIRECTORY)
        dir_cluster = fl->ioh.first_cluster;
    else
        dir_cluster = fl->gl->dir_cluster;

    D(
        bug("[FAT] [%s] trying to obtain lock on '",__FUNCTION__);
        RawPutChars(name, namelen);
        bug("' in dir at cluster %ld\n", dir_cluster);
    )

    /* Open the dir */
    InitDirHandle(glob->sb, dir_cluster, &dh, FALSE, glob);

    /* Look for the entry */
    if ((err = GetDirEntryByPath(&dh, name, namelen, &de, glob)) != 0)
    {
        ReleaseDirHandle(&dh, glob);
        D(bug("[FAT] [%s] couldn't get lock\n",__FUNCTION__));
        return err;
    }

    /* Found it, do the locking proper */
    if (de.e.entry.attr & ATTR_DIRECTORY && FIRST_FILE_CLUSTER(&de)
        <= glob->sb->rootdir_cluster)
        err = LockRoot(access, lock, glob);
    else
        err = LockFile(dh.ioh.first_cluster, de.index, access, lock, glob);

    ReleaseDirHandle(&dh, glob);

    return err;
}

LONG LockFile(ULONG dir_cluster, ULONG dir_entry, LONG access,
    struct ExtFileLock **lock, struct Globals *glob)
{
    struct GlobalLock *node, *gl;
    struct ExtFileLock *fl;
    struct DirHandle dh;
    struct DirEntry de;
    ULONG len;

    D(bug("[FAT] [%s] locking file (%ld/%ld) (%s)\n",__FUNCTION__, dir_cluster, dir_entry, access == SHARED_LOCK ? "shared" : "exclusive"));

    /* First see if we already have a global lock for this file */
    gl = NULL;
    ForeachNode(&glob->sb->info->locks, node)
    {
        if (node->dir_cluster == dir_cluster
            && node->dir_entry == dir_entry)
        {
            gl = node;
            break;
        }
    }

    /* If we do and we're trying for an exclusive lock, then bail out */
    if (gl != NULL && access == EXCLUSIVE_LOCK)
    {
        D(bug("[FAT] [%s] can't obtain exclusive lock on already-locked file\n",__FUNCTION__));
        return ERROR_OBJECT_IN_USE;
    }

    /* Allocate space for the lock. We do this first so that we don't go to
     * all the effort of setting up the global lock only to have to discard it
     * if the filelock allocation fails */
    if ((fl = AllocVecPooled(glob->sb->info->mem_pool,
        sizeof(struct ExtFileLock))) == NULL)
        return ERROR_NO_FREE_STORE;

    /* If we don't have a global lock we need to build one */
    if (gl == NULL)
    {
        if ((gl = AllocVecPooled(glob->sb->info->mem_pool,
            sizeof(struct GlobalLock))) == NULL)
        {
            FreeVecPooled(glob->sb->info->mem_pool, fl);
            return ERROR_NO_FREE_STORE;
        }

        gl->dir_cluster = dir_cluster;
        gl->dir_entry = dir_entry;
        gl->access = access;

        /* Gotta fish some stuff out of the dir entry too */
        InitDirHandle(glob->sb, dir_cluster, &dh, FALSE, glob);
        GetDirEntry(&dh, dir_entry, &de, glob);

        gl->first_cluster = FIRST_FILE_CLUSTER(&de);
        if (gl->first_cluster == 0)
            gl->first_cluster = 0xffffffff;

        gl->attr = de.e.entry.attr;
        gl->size = AROS_LE2LONG(de.e.entry.file_size);

        GetDirEntryShortName(&de, &(gl->name[1]), &len, glob);
        gl->name[0] = (UBYTE) len;
        GetDirEntryLongName(&de, &(gl->name[1]), &len);
        gl->name[0] = (UBYTE) len;
#if DEBUG_NAMES
        GetDirEntryShortName(&de, &(gl->shortname[1]), &len, glob);
        gl->shortname[0] = (UBYTE) len;
#endif

        ReleaseDirHandle(&dh, glob);

        NEWLIST(&gl->locks);

        ADDTAIL(&glob->sb->info->locks, gl);

        D(bug("[FAT] [%s] created new global lock\n",__FUNCTION__));

        /* Look through the notify list. If there's any in there that aren't
         * currently attached to a global lock, expand them and if they are
         * for this file, fill them in */
        {
            struct NotifyNode *nn;

            ForeachNode(&glob->sb->info->notifies, nn)
            {
                if (nn->gl == NULL)
                {
                    D(bug("[FAT] [%s] searching for notify name '%s'\n",__FUNCTION__, nn->nr->nr_FullName));

                    if (InitDirHandle(glob->sb, 0, &dh, TRUE, glob) != 0)
                        continue;

                    if (GetDirEntryByPath(&dh, nn->nr->nr_FullName,
                        strlen(nn->nr->nr_FullName), &de, glob) != 0)
                        continue;

                    if (gl->dir_cluster == de.cluster
                        && gl->dir_entry == de.index)
                    {
                        D(bug("[FAT] [%s] found and matched to the global lock (%ld/%ld)\n",__FUNCTION__, gl->dir_cluster, gl->dir_entry));
                        nn->gl = gl;
                    }
                }
            }
        }
    }

    /* Now set up the file lock */
    fl->fl_Link = BNULL;
    fl->fl_Key = 0;
    fl->fl_Access = access;
    fl->fl_Task = glob->ourport;
    fl->fl_Volume = MKBADDR(glob->sb->doslist);

    fl->magic = ID_FAT_DISK;

    fl->ioh.sb = glob->sb;
    fl->ioh.first_cluster = gl->first_cluster;
    fl->ioh.block = NULL;
    RESET_HANDLE(&(fl->ioh));

    fl->pos = 0;

    fl->do_notify = FALSE;

    fl->gl = gl;
    fl->sb = glob->sb;
    ADDTAIL(&gl->locks, &fl->node);

    D(bug("[FAT] [%s] created file lock 0x%08x\n",__FUNCTION__, fl));

    DumpLocks(glob->sb, glob);

    *lock = fl;
    return 0;
}

LONG LockRoot(LONG access, struct ExtFileLock **lock, struct Globals *glob)
{
    struct ExtFileLock *fl;

    D(bug("[FAT] [%s] locking root\n",__FUNCTION__));

    if (access == EXCLUSIVE_LOCK)
    {
        D(bug("[FAT] [%s] can't obtain exclusive lock on the fs root\n",__FUNCTION__));
        return ERROR_OBJECT_IN_USE;
    }

    if ((fl = AllocVecPooled(glob->sb->info->mem_pool,
        sizeof(struct ExtFileLock))) == NULL)
        return ERROR_NO_FREE_STORE;

    fl->fl_Link = BNULL;
    fl->fl_Key = 0;
    fl->fl_Access = SHARED_LOCK;
    fl->fl_Task = glob->ourport;
    fl->fl_Volume = MKBADDR(glob->sb->doslist);

    fl->magic = ID_FAT_DISK;

    fl->ioh.sb = glob->sb;
    fl->ioh.first_cluster = 0;
    fl->ioh.block = NULL;
    RESET_HANDLE(&(fl->ioh));

    fl->pos = 0;

    fl->do_notify = FALSE;

    if (IsListEmpty(&glob->sb->info->root_lock.locks))
        ADDTAIL(&glob->sb->info->locks, &glob->sb->info->root_lock);
    fl->gl = &glob->sb->info->root_lock;
    fl->sb = glob->sb;
    ADDTAIL(&glob->sb->info->root_lock.locks, &fl->node);

    D(bug("[FAT] [%s] created root lock 0x%08x\n",__FUNCTION__, fl));

    DumpLocks(glob->sb, glob);

    *lock = fl;
    return 0;
}

LONG CopyLock(struct ExtFileLock *fl, struct ExtFileLock **lock,
    struct Globals *glob)
{
    D(bug("[FAT] [%s] copying lock\n",__FUNCTION__));

    if (fl == NULL || fl->gl == &glob->sb->info->root_lock)
        return LockRoot(SHARED_LOCK, lock, glob);

    if (fl->fl_Access == EXCLUSIVE_LOCK)
    {
        D(bug("[FAT] [%s] can't copy exclusive lock\n",__FUNCTION__));
        return ERROR_OBJECT_IN_USE;
    }

    return LockFile(fl->gl->dir_cluster, fl->gl->dir_entry, SHARED_LOCK,
        lock, glob);
}

void FreeLock(struct ExtFileLock *fl, struct Globals *glob)
{
    struct NotifyNode *nn;

    if (fl == NULL)
        return;

    D(bug("[FAT] [%s] freeing lock 0x%08x\n",__FUNCTION__, fl));

    if (fl->do_notify)
        SendNotifyByLock(fl->ioh.sb, fl->gl);

    REMOVE(&fl->node);

    if (IsListEmpty(&fl->gl->locks))
    {
        REMOVE(fl->gl);

        ForeachNode(&fl->sb->info->notifies, nn)
            if (nn->gl == fl->gl)
                nn->gl = NULL;

        if (fl->gl != &fl->sb->info->root_lock)
            FreeVecPooled(glob->sb->info->mem_pool, fl->gl);

        D(bug("[FAT] [%s] freed associated global lock\n",__FUNCTION__));
    }

    DumpLocks(fl->sb, glob);
    if (fl->ioh.block != NULL)
        Cache_FreeBlock(fl->sb->cache, fl->ioh.block);

    if (fl->sb != glob->sb)
        AttemptDestroyVolume(fl->sb);

    D(bug("[FAT] [%s] Lock is Freed, cleaning up glob->sb->info->mem_pool\n",__FUNCTION__));

    //FreeVecPooled(glob->sb->info->mem_pool, fl);
    FreeVecPooled(fl->sb->info->mem_pool, fl);
    
    D(bug("[FAT] [%s] Lock Cleaned\n",__FUNCTION__));
}
