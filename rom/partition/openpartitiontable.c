/*
    Copyright � 1995-2017, The AROS Development Team. All rights reserved.
    $Id$
*/

#include <exec/memory.h>
#include <proto/exec.h>

#include "partition_intern.h"
#include "partition_support.h"
#include "platform.h"
#include "debug.h"

/*****************************************************************************

    NAME */
#include <libraries/partition.h>

        AROS_LH1(LONG, OpenPartitionTable,

/*  SYNOPSIS */
        AROS_LHA(struct PartitionHandle *, root, A1),

/*  LOCATION */
        struct Library *, PartitionBase, 7, Partition)

/*  FUNCTION
        Open a partition table. On success root->list will be filled with a
        list of PartitionHandles. If one partition contains more
        subpartitions, the caller should call OpenPartitionTable() on the
        PartitionHandle recursively.

    INPUTS
        root - root partition

    RESULT
        0 for success; an error code otherwise.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    D(bug("[PART:ROOT] OpenPartitionTable\n"));
    
    const struct PTFunctionTable * const *pst;

    pst = PartitionSupport;
    while (pst[0])
    {
        if (pst[0]->checkPartitionTable(PartitionBase, root))
        {
            root->table = AllocMem(sizeof(struct PartitionTableHandler), MEMF_PUBLIC | MEMF_CLEAR);

            if (root->table)
            {
            	LONG retval;

	            NEWLIST(&root->table->list);

                root->table->type    = pst[0]->type;
                root->table->handler = (void *)pst[0];

                D(bug("[PART:ROOT] OpenPartitionTable: Type = %u | Handler = %u\n", root->table->type, root->table->handler ));

                retval = pst[0]->openPartitionTable(PartitionBase, root);
                if (retval)
                {
                    FreeMem(root->table, sizeof(struct PartitionTableHandler));

                    root->table = NULL;
                }
                return retval;
            }
        }
        pst++;
    }
    return 1;

    AROS_LIBFUNC_EXIT
}
