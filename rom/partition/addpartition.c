/*
    Copyright � 1995-2017, The AROS Development Team. All rights reserved.
    $Id$

*/
#include <utility/tagitem.h>
#include "partition_intern.h"
#include "partition_support.h"
#include "platform.h"
#include "debug.h"

/*****************************************************************************

    NAME */
#include <libraries/partition.h>

        AROS_LH2(struct PartitionHandle *, AddPartition,

/*  SYNOPSIS */
        AROS_LHA(struct PartitionHandle *, root,       A1),
        AROS_LHA(const struct TagItem *  , taglist,    A2),

/*  LOCATION */
        struct Library *, PartitionBase, 11, Partition)

/*  FUNCTION
        Add a new partition.

    INPUTS
        root - PartitionHandle, where to add the new partition
        taglist - tags that specify more information about the partition
                  unknown tags are ignored
            PT_DOSENVEC - ptr to a DosEnvec
                de_LowCyl and de_HighCyl specify start and end of cylinder
                de_Reserved, de_Bootblocks, ...
                de_Surfaces, de_BlocksPerTrack, ... are inherited from "root"
            PT_TYPE     - partition type (depends on PartitionTable type)
            PT_POSITION - position number within the partition table (MBR->PC)
            PT_ACTIVE   - set this partition active (MBR->PC)
            PT_NAME     - set partition name (HD0, HD1, ...)

    RESULT
        PartitionHandle of the new partition; 0 for an error

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    D(bug("[PART] AddPartition\n"));

    if (root->table)
    {
    struct PTFunctionTable *handler = root->table->handler;

        if (handler->addPartition)
            return handler->addPartition(PartitionBase, root, taglist); 
    }
    return 0;
    AROS_LIBFUNC_EXIT
}
