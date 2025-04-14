/*-------------------------------------------------------------------------
 *
 * uuidShort.c
 *	  MySQL adapter uuidShort routines
 *
 * 
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 * 
 * 易景科技是Halo Database、Halo Database Management System、羲和数据
 * 库、羲和数据库管理系统（后面简称 Halo ）、openHalo软件的发明人同时也为
 * 知识产权权利人。Halo 软件的知识产权，以及与本软件相关的所有信息内容（包括
 * 但不限于文字、图片、音频、视频、图表、界面设计、版面框架、有关数据或电子文
 * 档等）均受中华人民共和国法律法规和相应的国际条约保护，易景科技享有上述知识
 * 产权，但相关权利人依照法律规定应享有的权利除外。未免疑义，本条所指的“知识
 * 产权”是指任何及所有基于 Halo 软件产生的：（a）版权、商标、商号、域名、与
 * 商标和商号相关的商誉、设计和专利；与创新、技术诀窍、商业秘密、保密技术、非
 * 技术信息相关的权利；（b）人身权、掩模作品权、署名权和发表权；以及（c）在
 * 本协议生效之前已存在或此后出现在世界任何地方的其他工业产权、专有权、与“知
 * 识产权”相关的权利，以及上述权利的所有续期和延长，无论此类权利是否已在相
 * 关法域内的相关机构注册。
 *
 * This software and related documentation are provided under a license
 * agreement containing restrictions on use and disclosure and are 
 * protected by intellectual property laws. Except as expressly permitted
 * in your license agreement or allowed by law, you may not use, copy, 
 * reproduce, translate, broadcast, modify, license, transmit, distribute,
 * exhibit, perform, publish, or display any part, in any form, or by any
 * means. Reverse engineering, disassembly, or decompilation of this 
 * software, unless required by law for interoperability, is prohibited.
 * 
 * This software is developed for general use in a variety of
 * information management applications. It is not developed or intended
 * for use in any inherently dangerous applications, including applications
 * that may create a risk of personal injury. If you use this software or
 * in dangerous applications, then you shall be responsible to take all
 * appropriate fail-safe, backup, redundancy, and other measures to ensure
 * its safe use. Halo Corporation and its affiliates disclaim any 
 * liability for any damages caused by use of this software in dangerous
 * applications.
 * 
 *
 * IDENTIFICATION
 *	  src/backend/adapter/mysql/uuidShort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "executor/tuptable.h"
#include "storage/ipc.h"
#include "storage/lockdefs.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/elog.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "adapter/mysql/uuidShort.h"
#include <ctype.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>


HTAB *globalUuidShortLock = NULL;
HTAB *globalUuidShort = NULL;

typedef struct 
{
    char lockName[NAMEDATALEN];   /* hash key must be first */
    slock_t mutex;
} GlobalUuidShortMutex;
GlobalUuidShortMutex *globalUuidShortMutex;


typedef struct 
{
    char uuidShortName[NAMEDATALEN];   /* hash key must be first */
    unsigned long long uuidShortVal;
} UuidShort;

static char *UUID_SHORT_LOCK_NAME = "UUID_SHORT_LOCK";
static char *UUID_SHORT_NAME = "UUID_SHORT";
static void lockUuidShort(void);
static void unlockUuidShort(void);


Size
UuidShortShmemSize(void)
{
	Size size = 0;

	size = sizeof(GlobalUuidShortMutex) + sizeof(UuidShort);

	return size;
}


void 
UuidShortShmemInit(void)
{
    HASHCTL hashctlLock;
    HASHCTL hashctl;
    UuidShort *uuidShort;
    bool found;

    hashctlLock.keysize = NAMEDATALEN;
    hashctlLock.entrysize = sizeof(GlobalUuidShortMutex);
    globalUuidShortLock = ShmemInitHash(UUID_SHORT_LOCK_NAME, 
                                        1,  
                                        1,  
                                        &hashctlLock,                
                                        HASH_ELEM | HASH_STRINGS);

    hashctl.keysize = NAMEDATALEN;
    hashctl.entrysize = sizeof(UuidShort);
    globalUuidShort = ShmemInitHash(UUID_SHORT_NAME, 
                                    1,  
                                    1,  
                                    &hashctl,                
                                    HASH_ELEM | HASH_STRINGS);

    uuidShort = (UuidShort *)hash_search(globalUuidShort, 
                                         UUID_SHORT_NAME, 
                                         HASH_ENTER, 
                                         &found);
    if (!found)
    {
        unsigned long long serverID = 1;
        unsigned long long svrStartupTimeSecs = 0;
        time_t curTime;
        struct sysinfo info;

        time(&curTime);
        if (sysinfo(&info)) 
        {
            elog(ERROR, "Failed to general uuid_short value");
        }
        if (curTime > info.uptime) 
        {
            svrStartupTimeSecs = (unsigned long long)(curTime - info.uptime);
        }
        else
        {
            svrStartupTimeSecs = (unsigned long long)(info.uptime - curTime);
        }
        uuidShort->uuidShortVal = ((serverID & 255) << 56) + (svrStartupTimeSecs << 24);  
    }
}


unsigned long long
getUuidShort(void)
{
    unsigned long long getRet;
    UuidShort *uuidShort;
    bool found;

    lockUuidShort();
    uuidShort = (UuidShort *)hash_search(globalUuidShort, 
                                         UUID_SHORT_NAME, 
                                         HASH_FIND, 
                                         &found);
    if (found)
    {
        uuidShort->uuidShortVal += 1;
        getRet = uuidShort->uuidShortVal;
        unlockUuidShort();
    }
    else 
    {
        unlockUuidShort();
        elog(ERROR, "Failed to get uuid_short");
    }

    return getRet;
}


static void 
lockUuidShort(void)
{
    bool found;

    globalUuidShortMutex = 
        (GlobalUuidShortMutex *)hash_search(globalUuidShortLock,                    
                                            UUID_SHORT_LOCK_NAME, 
                                            HASH_ENTER, 
                                            &found); 
    if (!found)
    {
        SpinLockInit(&globalUuidShortMutex->mutex);
    }

    SpinLockAcquire(&globalUuidShortMutex->mutex);
}


static void 
unlockUuidShort(void)
{
    SpinLockRelease(&globalUuidShortMutex->mutex);
}

