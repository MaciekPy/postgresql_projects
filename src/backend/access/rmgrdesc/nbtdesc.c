/*-------------------------------------------------------------------------
 *
 * nbtdesc.c
 *	  rmgr descriptor routines for access/nbtree/nbtxlog.c
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/nbtdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"

static void
out_target(StringInfo buf, xl_btreetid *target)
{
	appendStringInfo(buf, "rel %u/%u/%u; tid %u/%u",
			 target->node.spcNode, target->node.dbNode, target->node.relNode,
					 ItemPointerGetBlockNumber(&(target->tid)),
					 ItemPointerGetOffsetNumber(&(target->tid)));
}

void
btree_desc(StringInfo buf, XLogRecord *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
		case XLOG_BTREE_INSERT_UPPER:
		case XLOG_BTREE_INSERT_META:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_SPLIT_L:
		case XLOG_BTREE_SPLIT_R:
		case XLOG_BTREE_SPLIT_L_ROOT:
		case XLOG_BTREE_SPLIT_R_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
								 xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_VACUUM:
			{
				xl_btree_vacuum *xlrec = (xl_btree_vacuum *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; blk %u, lastBlockVacuumed %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode, xlrec->block,
								 xlrec->lastBlockVacuumed);
				break;
			}
		case XLOG_BTREE_DELETE:
			{
				xl_btree_delete *xlrec = (xl_btree_delete *) rec;

				appendStringInfo(buf, "index %u/%u/%u; iblk %u, heap %u/%u/%u;",
								 xlrec->node.spcNode, xlrec->node.dbNode, xlrec->node.relNode,
								 xlrec->block,
								 xlrec->hnode.spcNode, xlrec->hnode.dbNode, xlrec->hnode.relNode);
				break;
			}
		case XLOG_BTREE_MARK_PAGE_HALFDEAD:
			{
				xl_btree_mark_page_halfdead *xlrec = (xl_btree_mark_page_halfdead *) rec;

				out_target(buf, &(xlrec->target));
				appendStringInfo(buf, "; topparent %u; leaf %u; left %u; right %u",
								 xlrec->topparent, xlrec->leafblk, xlrec->leftblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_UNLINK_PAGE_META:
		case XLOG_BTREE_UNLINK_PAGE:
			{
				xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; ",
								 xlrec->node.spcNode, xlrec->node.dbNode, xlrec->node.relNode);
				appendStringInfo(buf, "dead %u; left %u; right %u; btpo_xact %u; ",
								 xlrec->deadblk, xlrec->leftsib, xlrec->rightsib, xlrec->btpo_xact);
				appendStringInfo(buf, "leaf %u; leafleft %u; leafright %u; topparent %u",
								 xlrec->leafblk, xlrec->leafleftsib, xlrec->leafrightsib, xlrec->topparent);
				break;
			}
		case XLOG_BTREE_NEWROOT:
			{
				xl_btree_newroot *xlrec = (xl_btree_newroot *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; root %u lev %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode,
								 xlrec->rootblk, xlrec->level);
				break;
			}
		case XLOG_BTREE_REUSE_PAGE:
			{
				xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; latestRemovedXid %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode, xlrec->latestRemovedXid);
				break;
			}
	}
}

const char *
btree_identify(uint8 info)
{
	const char *id = NULL;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			id = "INSERT_LEAF";
			break;
		case XLOG_BTREE_INSERT_UPPER:
			id = "INSERT_UPPER";
			break;
		case XLOG_BTREE_INSERT_META:
			id = "INSERT_META";
			break;
		case XLOG_BTREE_SPLIT_L:
			id = "SPLIT_L";
			break;
		case XLOG_BTREE_SPLIT_R:
			id = "SPLIT_R";
			break;
		case XLOG_BTREE_SPLIT_L_ROOT:
			id = "SPLIT_L_ROOT";
			break;
		case XLOG_BTREE_SPLIT_R_ROOT:
			id = "SPLIT_R_ROOT";
			break;
		case XLOG_BTREE_VACUUM:
			id = "VACUUM";
			break;
		case XLOG_BTREE_DELETE:
			id = "DELETE";
			break;
		case XLOG_BTREE_MARK_PAGE_HALFDEAD:
			id = "MARK_PAGE_HALFDEAD";
			break;
		case XLOG_BTREE_UNLINK_PAGE:
			id = "UNLINK_PAGE";
			break;
		case XLOG_BTREE_UNLINK_PAGE_META:
			id = "UNLINK_PAGE_META";
			break;
		case XLOG_BTREE_NEWROOT:
			id = "NEWROOT";
			break;
		case XLOG_BTREE_REUSE_PAGE:
			id = "REUSE_PAGE";
			break;
	}

	return id;
}
