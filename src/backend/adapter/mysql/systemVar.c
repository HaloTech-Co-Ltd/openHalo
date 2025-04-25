/*-------------------------------------------------------------------------
 *
 * systemVar.c
 *	  MySQL adapter systemVar routines
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
 *	  src/backend/adapter/mysql/systemVar.c
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
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"
#include "adapter/mysql/systemVar.h"
#include <time.h>


#define SYSTEM_VAR_NAME_MAX_LEN 128
#define SYSTEM_VAR_VALUE_MAX_LEN 512
#define SYSTEM_VAR_INT_NUM 512
#define SYSTEM_VAR_MAX_NUM 1024
#define SYSTEM_VAR_RESULT_RULE_NUM 16
#define SYSTEM_VAR_RESULT_RULE_LEN 64

uint64 mys_sqlMode = MYS_MODE_ONLY_FULL_GROUP_BY | MYS_MODE_STRICT_TRANS_TABLES | MYS_MODE_NO_ZERO_IN_DATE |
                     MYS_MODE_NO_ZERO_DATE | MYS_MODE_ERROR_FOR_DIVISION_BY_ZERO | MYS_MODE_NO_AUTO_CREATE_USER |
                     MYS_MODE_NO_ENGINE_SUBSTITUTION;
int autoCommit = 2;
int default_week_format = 0;
extern bool needCommitTrx;
extern bool needStartNewTrx;
extern bool isStrictTransTablesOn;

HTAB *globalSystemVars = NULL;
HTAB *globalSystemVarsLock = NULL;

typedef struct 
{
    char lockName[SYSTEM_VAR_NAME_MAX_LEN];   /* hash key must be first */
    slock_t mutex;
} GlobalSystemVarsMutex;
GlobalSystemVarsMutex *globalSystemVarsMutex;

typedef struct 
{
    char varName[SYSTEM_VAR_NAME_MAX_LEN];   /* hash key must be first */
    char varDefValue[SYSTEM_VAR_VALUE_MAX_LEN];
    char varConfValue[SYSTEM_VAR_VALUE_MAX_LEN];
    int sessionGlobalType;  /* 0:global & session, 1:global, 2:session, 3:sess-global */
    bool sessionDefValueFromGlobal;
    bool isReadWrite;
    int resultNum;
    char result[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
    int selectResultNum;
    char selectResult[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
    int showResultNum;
    char showResult[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
} SystemVar;

static SystemVar *baseSystemVars[SYSTEM_VAR_MAX_NUM];
static int baseSystemVarsNum = 0;
static HTAB *sessionSystemVars = NULL;

static List *readBaseVariableRecord(TupleTableSlot *slot);
static List *readBaseVariablesTable();
static bool add2BaseSystemVars(SystemVar *systemVar);
static void add2GlobalSystemVars(SystemVar *systemVar);
static void add2SessionSystemVars(SystemVar *systemVar);
static SystemVar *initBaseSystemVar(List *systemVarColumnVals);
static void initSystemVariables(void);
static void initBaseSystemVars(void);
static void initGlobalSystemVars(SystemVar *systemVars[], int systemVarsNum);
static void initSessionSystemVars(SystemVar *systemVars[], int systemVarsNum);
static SystemVar *dynAddSystemVar(char *varName);
static void lockGlobalSystemVars(void);
static void unlockGlobalSystemVars(void);
static void lowerSystemVarName(char *varName);
static bool asignSystemVar(const char *varDefValue, 
                           const char* varConfValue, 
                           int sessionGlobalType, 
                           bool sessionDefValueFromGlobal,
                           bool isReadWrite, 
                           const char* result, 
                           const char* selectResult, 
                           const char* showResult,
                           SystemVar *systemVar);
static bool asignSystemVarResult(const char *srcResult, 
                                 int *resultNum,
                                 char *result);
static void splitStr(const char *str, char separator, int itemSize, 
                     char *items, int *itemsNum);
static void copySystemVar(SystemVar *srcSystemVar, SystemVar *destSystemVar);
static void setSystemVarValueByVarName(const char *varName, 
                                       const char *value, char *varValue);
static void setSystemVarValueForOptimizerSwitch(const char *value, 
                                                char *varValue);
static void setSystemVarValue(const char* value, char *varValue);
static void rectifySystemVarValue(const char *varName, char **varValue, 
                                  bool isSessionSystemVar);
static void getDefaultValue(const char *varName, bool isSessionSystemVar, 
                            char **varValue);
static void applySystemVarValue(const char *varName, const char *varValue);
static int getSystemVariableValueImpl(char *varName, 
                                      bool isSessionSystemVar, 
                                      int selectShowType, 
                                      char **varValue);
static char *getConfValue(SystemVar *systemVar, int selectShowType);
static Node *makeStringConst(char *str, int location);
static char *getOSTimeZone(void);


Size
GlobalSystemVariablesShmemSize(void)
{
	Size size = 0;

	size = sizeof(GlobalSystemVarsMutex) + SYSTEM_VAR_MAX_NUM * sizeof(SystemVar);

	return size;
}


void 
GlobalSystemVariablesShmemInit(void)
{
    HASHCTL hashctlLock;
    HASHCTL hashctl;

    hashctlLock.keysize = SYSTEM_VAR_NAME_MAX_LEN;
    hashctlLock.entrysize = sizeof(GlobalSystemVarsMutex);
    globalSystemVarsLock = ShmemInitHash("MySQL Global System Variables lock (914)",   
                                         1,  
                                         1,  
                                         &hashctlLock,                
                                         HASH_ELEM | HASH_STRINGS);

    hashctl.keysize = SYSTEM_VAR_NAME_MAX_LEN;
    hashctl.entrysize = sizeof(SystemVar);
    globalSystemVars = ShmemInitHash("MySQL Global System Variables (914)",   
                                     SYSTEM_VAR_INT_NUM,  
                                     SYSTEM_VAR_MAX_NUM,  
                                     &hashctl,                
                                     HASH_ELEM | HASH_STRINGS);
}


void
getSystemVariableValueForSelect(char *varName, 
                                bool isSessionSystemVar, 
                                char **varValue)
{
    int getRet;

    getRet = getSystemVariableValueImpl(varName, 
                                        isSessionSystemVar, 
                                        1, 
                                        varValue);
    if (getRet == 0)
    {
        return;
    }
    else if (getRet == 1)
    {
        elog(ERROR, "Unknown system variable '%s'", varName);
    }
    else 
    {
        elog(ERROR, "Variable '%s' is a SESSION variable", varName);
    }
}


bool
getSystemVariableValueForShow(char *varName, 
                              bool isSessionSystemVar, 
                              char **varValue)
{
    int getRet;

    getRet = getSystemVariableValueImpl(varName, 
                                        isSessionSystemVar, 
                                        2, 
                                        varValue);
    if (getRet == 0)
    {
        return true;
    }
    else if (getRet == 1)
    {
        elog(ERROR, "Unknown system variable '%s'", varName);
    }
    else 
    {
        elog(ERROR, "Variable '%s' is a SESSION variable", varName);
    }
}


void
setSystemVariableValue(char *varName, char *varValue, bool isSessionSystemVar)
{
    SystemVar *systemVar;
    bool found;

    if (sessionSystemVars == NULL)
    {
        initSystemVariables();
    }

    if (varName == NULL)
    {
        elog(ERROR, "Invalid system variable name(NULL)");
        return;
    }

    lowerSystemVarName(varName);

    rectifySystemVarValue(varName, &varValue, isSessionSystemVar);

    if (isSessionSystemVar)
    {
        systemVar = (SystemVar *)hash_search(sessionSystemVars, 
                                             varName, 
                                             HASH_FIND, 
                                             &found);
        if (found || (NULL != (systemVar = dynAddSystemVar(varName))))
        {
            if ((systemVar->sessionGlobalType == 0) || 
                (systemVar->sessionGlobalType == 2) ||
                (systemVar->sessionGlobalType == 3))
            {
                if (systemVar->isReadWrite)
                {
                    setSystemVarValueByVarName(varName, 
                                               varValue, 
                                               systemVar->varConfValue);
                    applySystemVarValue(varName, varValue);
                }
                else 
                {
                    elog(ERROR, 
                         "Variable '%s' is a read only variable",
                         varName);
                }
            }
            else
            {
                elog(ERROR, 
                     "Variable '%s' is a GLOBAL variable and should be set with SET GLOBAL",
                     varName);
            }
        }
        else
        {
            elog(ERROR, "Unknown system variable '%s'", varName);
        }
    }
    else 
    {
        lockGlobalSystemVars();
        systemVar = (SystemVar *)hash_search(globalSystemVars, 
                                             varName, 
                                             HASH_FIND, 
                                             &found);
        if (found || (NULL != (systemVar = dynAddSystemVar(varName))))
        {
            if ((systemVar->sessionGlobalType == 0) || 
                (systemVar->sessionGlobalType == 1))
            {
                if (systemVar->isReadWrite)
                {
                    setSystemVarValueByVarName(varName, 
                                               varValue, 
                                               systemVar->varConfValue);
                    unlockGlobalSystemVars();

                    if ((systemVar->sessionGlobalType == 1) && 
                        systemVar->sessionDefValueFromGlobal)
                    {
                        systemVar = (SystemVar *)hash_search(sessionSystemVars, 
                                                             varName, 
                                                             HASH_FIND, 
                                                             &found);
                        if (found)
                        {
                            setSystemVarValueByVarName(varName, 
                                                       varValue, 
                                                       systemVar->varConfValue);
                        }
                        else
                        {
                            elog(ERROR, 
                                 "System session variable '%s' does not exists(set)", 
                                 varName);
                        }
                    }
                }
                else 
                {
                    unlockGlobalSystemVars();
                    elog(ERROR, 
                         "Variable '%s' is a read only variable",
                         varName);
                }
            }
            else 
            {
                unlockGlobalSystemVars();
                elog(ERROR, 
                     "Variable '%s' is a SESSION variable and can't be used with SET GLOBAL",
                     varName);
            }
        }
        else
        {
            unlockGlobalSystemVars();
            elog(ERROR, "Unknown system variable '%s'", varName);
        }
    }
}


void
setSystemVariableDatum(char *varName, Datum varConfValue, 
                       Oid varValueType, bool isNull, 
                       bool isSessionSystemVar) 
{
    char *varValue;

    if (!isNull)
    {
        Oid	typoutput;
	    bool typIsVarlena;

        getTypeOutputInfo(varValueType, &typoutput, &typIsVarlena);
        varValue = OidOutputFunctionCall(typoutput, varConfValue);
    }
    else 
    {
        varValue = NULL;
    }
    setSystemVariableValue(varName, varValue, isSessionSystemVar);
}


bool 
isSystemVariable(char *varName)
{
    bool found;

    if (sessionSystemVars == NULL)
    {
        initSystemVariables();
    }

    lowerSystemVarName(varName);

    (SystemVar *)hash_search(sessionSystemVars, 
                             varName, 
                             HASH_FIND, 
                             &found);

    return found;
}


static void 
initSystemVariables(void)
{
    MemoryContext oldContext;

    oldContext = MemoryContextSwitchTo(TopMemoryContext);

    initBaseSystemVars();

    lockGlobalSystemVars();
    initGlobalSystemVars(baseSystemVars, baseSystemVarsNum);
    initSessionSystemVars(baseSystemVars, baseSystemVarsNum);
    unlockGlobalSystemVars();

    MemoryContextSwitchTo(oldContext);
}


static bool
add2BaseSystemVars(SystemVar *systemVar)
{
    if (baseSystemVarsNum < SYSTEM_VAR_MAX_NUM)
    {
        baseSystemVars[baseSystemVarsNum] = systemVar;
        ++baseSystemVarsNum;
        return true;
    }
    else
    {
        return false;
    }
}


static SystemVar * 
initBaseSystemVar(List *systemVarDatas)
{
    SystemVar *systemVar;
    char *varName;
    char *varDefValue;
    char *varConfValue;
    int sessionGlobalType;
    bool sessionDefValueFromGlobal;
    bool isReadWrite;
    char *result;
    char *selectResult;
    char *showResult;
    ListCell *lc;
    int ind;

    systemVar = NULL;
    varName = NULL;
    varDefValue = NULL;
    varConfValue = NULL;
    sessionGlobalType = 0;
    sessionDefValueFromGlobal = true;
    isReadWrite= true;
    result = NULL;
    selectResult = NULL;
    showResult = NULL;
    ind = 0;

    foreach (lc, systemVarDatas)
    {
        Node *nd = lfirst(lc);
        if (NULL != nd)
        {
            char *val = strVal(nd);
            if (ind == 0)
            {
                varName = val;
                lowerSystemVarName(varName);
            }
            else if (ind == 1)
            {
                varDefValue = val;
            }
            else if (ind == 2)
            {
                varConfValue = val;
            }
            else if (ind == 3)
            {
                if (val[0] == '0')
                {
                    sessionGlobalType = 0;
                }
                else if (val[0] == '1')
                {
                    sessionGlobalType = 1;
                }
                else if (val[0] == '2')
                {
                    sessionGlobalType = 2;
                }
                else if (val[0] == '3')
                {
                    sessionGlobalType = 3;
                }
                else 
                {
                    return NULL;
                }
            }
            else if (ind == 4)
            {
                if (val[0] == '1')
                {
                    sessionDefValueFromGlobal = true;
                }
                else 
                {
                    sessionDefValueFromGlobal = false;
                }
            }
            else if (ind == 5)
            {
                if (val[0] == '1')
                {
                    isReadWrite = true;
                }
                else 
                {
                    isReadWrite = false;
                }
            }
            else if (ind == 6)
            {
                result = val;
            }
            else if (ind == 7)
            {
                selectResult = val;
            }
            else
            {
                showResult = val;
            }
        }
        else 
        {
            if (ind == 6)
            {
                result = NULL;
            }
            else if (ind == 7)
            {
                selectResult = NULL;
            }
            else if (ind == 8)
            {
                showResult = NULL;
            }
        }
        ++ind;
    }

    systemVar = (SystemVar *)palloc0(sizeof(SystemVar));
    snprintf(systemVar->varName, SYSTEM_VAR_NAME_MAX_LEN, "%s", varName);
    if (!asignSystemVar(varDefValue, 
                        varConfValue, 
                        sessionGlobalType, 
                        sessionDefValueFromGlobal, 
                        isReadWrite, 
                        result, 
                        selectResult, 
                        showResult, 
                        systemVar))
    {
        return NULL;
    }

    return systemVar;
}


static List * 
readBaseVariableRecord(TupleTableSlot *slot)
{
    List *columnVals;
    int columnCount;

    columnVals = NIL;
    columnCount = slot->tts_tupleDescriptor->natts;
    for (int i = 0; i < columnCount; i++)
    {
        TupleDesc tupleDesc;
        Form_pg_attribute att;
        Datum attr;
        bool is_null;
        Oid typoutput;
        bool typisvarlena;

        tupleDesc = slot->tts_tupleDescriptor;
        att = TupleDescAttr(tupleDesc, i);
        attr = slot_getattr(slot, (i + 1), &is_null);
        if (!is_null)
        {
            char *columnVal;
            getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);
            columnVal = OidOutputFunctionCall(typoutput, attr);
            columnVals = lappend(columnVals, makeString(columnVal));
        }
        else 
        {
            columnVals = lappend(columnVals, NULL);
        }
    }

    return columnVals;
}


static List * 
readBaseVariablesTable()
{
    List *systemVarsDatas;
    Relation relation;
    Snapshot snapshot;
    TableScanDesc scan;
    TupleTableSlot *slot;
    Oid schemaId;
    Oid tableId;
    char *schemaName;
    char *tableName;

    systemVarsDatas = NIL;
    schemaId = InvalidOid;
    tableId = InvalidOid;
    schemaName = "mys_informa_schema";
    tableName = "base_variables";

    schemaId = get_namespace_oid(schemaName, false);
    tableId = get_relname_relid(tableName, schemaId);
    relation = table_open(tableId, AccessShareLock);
    snapshot = RegisterSnapshot(GetLatestSnapshot());
    scan = table_beginscan(relation, snapshot, 0, NULL);
    slot = table_slot_create(relation, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
    {
        List *systemVarDatas = readBaseVariableRecord(slot);
        systemVarsDatas = lappend(systemVarsDatas, systemVarDatas);
    }
    table_endscan(scan);
    UnregisterSnapshot(snapshot);
    ExecDropSingleTupleTableSlot(slot);
    table_close(relation, AccessShareLock);

    return systemVarsDatas;
}


static void 
initBaseSystemVars(void)
{
    List *systemVarsColumnVals;
    ListCell *lc;

    baseSystemVarsNum = 0;
    systemVarsColumnVals = readBaseVariablesTable();
    foreach (lc, systemVarsColumnVals)
    {
        List *systemVarColumnVals = (List *)lfirst(lc);
        SystemVar *systemVar = initBaseSystemVar(systemVarColumnVals);
        if (NULL != systemVar)
        {
            if (!add2BaseSystemVars(systemVar))
            {
                elog(ERROR, 
                     "There are too many MySQL system variables. Connection will be disconnect.");
                proc_exit(1);
            }
        }
        else 
        {
            char *varName = strVal(linitial(systemVarColumnVals));
            elog(ERROR,
                 "Invalid data '%s' in mys_informa_schema.base_variables table. Connection will be disconnect.",
                 varName);
            proc_exit(1);
        }
    }
}


static void 
add2GlobalSystemVars(SystemVar *systemVar)
{
    SystemVar *globalSystemVar;
    bool found;

    globalSystemVar = 
        (SystemVar *)hash_search(globalSystemVars, 
                                 &(systemVar->varName[0]),
                                 HASH_ENTER, 
                                 &found);
    if (!found)
    {
        copySystemVar(systemVar, globalSystemVar);
    }
}


static void 
initGlobalSystemVars(SystemVar *systemVars[], int systemVarsNum)
{
    SystemVar *globalSystemVar;
    bool found;

    globalSystemVar = 
        (SystemVar *)hash_search(globalSystemVars, 
                                 "global_system_vars_init_flag",
                                 HASH_ENTER, 
                                 &found);

    if (!found)
    {
        if (!asignSystemVar("1", "1", false, false, false, NULL, NULL, NULL, 
                            globalSystemVar))
        {
            elog(NOTICE, 
                 "Invalid data in mys_informa_schema.base_variables table");
        }

        for (int i = 0; i < systemVarsNum; i++)
        {
            SystemVar *systemVar = systemVars[i];
            add2GlobalSystemVars(systemVar);
        }
    }
}


static void 
add2SessionSystemVars(SystemVar *systemVar)
{
    SystemVar *sessionSystemVar;
    bool found;

    sessionSystemVar = (SystemVar *)hash_search(sessionSystemVars, 
                                                &(systemVar->varName), 
                                                HASH_ENTER, 
                                                &found);
    if (!found)
    {
        copySystemVar(systemVar, sessionSystemVar);
    }

    applySystemVarValue(sessionSystemVar->varName, 
                        sessionSystemVar->varConfValue);
}


static void 
initSessionSystemVars(SystemVar *systemVars[], int systemVarsNum)
{
    HASHCTL hashctl;

    hashctl.keysize = SYSTEM_VAR_NAME_MAX_LEN;
    hashctl.entrysize = sizeof(SystemVar);
    sessionSystemVars = hash_create("Halo MySQL Session System Variables (914)", 
                                    SYSTEM_VAR_MAX_NUM, 
                                    &hashctl, 
                                    HASH_ELEM | HASH_STRINGS);

    for (int i = 0; i < systemVarsNum; i++)
    {
        SystemVar *systemVar = systemVars[i];
        add2SessionSystemVars(systemVar);
    }
}


static SystemVar * 
dynAddSystemVar(char *varName)
{
    SystemVar *systemVar;
    List *systemVarsColumnVals;
    ListCell *lc;
    List *systemVarColumnVals;
    int varNameLen;
    MemoryContext oldContext;

    oldContext = MemoryContextSwitchTo(TopMemoryContext);

    systemVar = NULL;
    systemVarsColumnVals = readBaseVariablesTable();
    systemVarColumnVals = NIL;
    varNameLen = strlen(varName);
    foreach (lc, systemVarsColumnVals)
    {
        List *curSystemVarsColumnVals = (List *)lfirst(lc);
        char *variableName = strVal(linitial(curSystemVarsColumnVals));
        if ((varNameLen == strlen(variableName)) && 
            (strncasecmp(varName, variableName, strlen(varName)) == 0))
        {
            systemVarColumnVals = curSystemVarsColumnVals;
            break;
        }
    }
    if (NIL == systemVarColumnVals)
    {
        MemoryContextSwitchTo(oldContext);
        return NULL;
    }

    systemVar = initBaseSystemVar(systemVarColumnVals);
    if (NULL == systemVar)
    {
        MemoryContextSwitchTo(oldContext);
        return NULL;
    }

    if (!add2BaseSystemVars(systemVar))
    {
        MemoryContextSwitchTo(oldContext);
        return NULL;
    }

    add2GlobalSystemVars(systemVar);
    add2SessionSystemVars(systemVar);

    MemoryContextSwitchTo(oldContext);
    return systemVar;
}


static void 
lockGlobalSystemVars(void)
{
    bool found;

    globalSystemVarsMutex = 
        (GlobalSystemVarsMutex *)hash_search(globalSystemVarsLock,                    
                                             "mysql_global_system_vars_lock", 
                                             HASH_ENTER, 
                                             &found); 
    if (!found)
    {
        SpinLockInit(&globalSystemVarsMutex->mutex);
    }

    SpinLockAcquire(&globalSystemVarsMutex->mutex);
}


static void 
unlockGlobalSystemVars(void)
{
    SpinLockRelease(&globalSystemVarsMutex->mutex);
}


static void 
lowerSystemVarName(char *varName)
{
    int varNameLen;

    if (varName == NULL)
    {
        return;
    }

    varNameLen = strlen(varName);
    for (int i = 0; i < varNameLen; i++)
    {
        if ((65 <= varName[i]) && (varName[i] <= 90))
        {
            varName[i] += 32;
        }
    }
}


static bool 
asignSystemVar(const char *varDefValue, 
               const char *varConfValue, 
               int sessionGlobalType, 
               bool sessionDefValueFromGlobal,
               bool isReadWrite, 
               const char *result, 
               const char *selectResult, 
               const char *showResult, 
               SystemVar *systemVar)
{
    setSystemVarValue(varDefValue, systemVar->varDefValue);
    setSystemVarValue(varConfValue, systemVar->varConfValue);
    systemVar->sessionGlobalType = sessionGlobalType;
    systemVar->sessionDefValueFromGlobal = sessionDefValueFromGlobal;
    systemVar->isReadWrite = isReadWrite;
    if (result != NULL)
    {
        if (!asignSystemVarResult(result,
                                  &(systemVar->resultNum), 
                                  &(systemVar->result[0][0])))
        {
            return false;
        }
    }
    else 
    {
        systemVar->resultNum = 0;
    }
    if (selectResult != NULL)
    {
        if (!asignSystemVarResult(selectResult,
                                  &(systemVar->selectResultNum), 
                                  &(systemVar->selectResult[0][0])))
        {
            return false;
        }
    }
    else 
    {
        systemVar->selectResultNum = 0;
    }
    if (showResult != NULL)
    {
        if (!asignSystemVarResult(showResult,
                                  &(systemVar->showResultNum), 
                                  &(systemVar->showResult[0][0])))
        {
            return false;
        }
    }
    else 
    {
        systemVar->showResultNum = 0;
    }

    if ((0 == systemVar->resultNum) && 
        ((0 < systemVar->selectResultNum) || 
         (0 < systemVar->showResultNum)))
    {
        return false;
    }

    if ((0 < systemVar->resultNum) && 
        (((systemVar->selectResultNum != systemVar->resultNum) && 
          (systemVar->showResultNum != systemVar->resultNum)) || 
         ((0 < systemVar->selectResultNum) && 
          (0 < systemVar->showResultNum) && 
          (systemVar->selectResultNum != systemVar->showResultNum))))
    {
        return false;
    }

    return true;
}


static bool 
asignSystemVarResult(const char *srcResult, 
                     int *resultNum,
                     char *result)
{
    char items[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
    int itemsNum;

    itemsNum = 0;
    splitStr(srcResult, '|', SYSTEM_VAR_RESULT_RULE_LEN, 
             &items[0][0], &itemsNum);
    *resultNum = itemsNum;
    for (int i = 0; i < itemsNum; i++)
    {
        if ((SYSTEM_VAR_RESULT_RULE_LEN - 1) <= (strlen(items[i])))
        {
            return false;
        }

        snprintf((result + (i * SYSTEM_VAR_RESULT_RULE_LEN)), 
                 SYSTEM_VAR_RESULT_RULE_LEN, "%s", items[i]);
    }

    return true;
}


static void 
splitStr(const char *str, char separator, 
         int itemSize, char *items, int *itemsNum)
{
    int strLen;
    int itemStartIndex;

    if (str == NULL)
    {
        return;
    }

    strLen = strlen(str);
    if (strLen == 0)
    {
        return;
    }

    *itemsNum = 0;
    itemStartIndex = -1;
    for (int i = 0; i <= strLen; i++)
    {
        if ((str[i] != separator) && (str[i] != '\0'))
        {
            if (itemStartIndex == -1)
            {
                itemStartIndex = i;
            }
        }
        else 
        {
            if (itemStartIndex != -1)
            {
                int curItemLen = i - itemStartIndex;
                char *curItemBuf = items + (*itemsNum * itemSize);
                memcpy(curItemBuf, (str + itemStartIndex), curItemLen);
                curItemBuf[curItemLen] = '\0';
                (*itemsNum)++;
                itemStartIndex = -1;
            }
        }
    }
}


static void 
copySystemVar(SystemVar *srcSystemVar, SystemVar *destSystemVar)
{
    setSystemVarValue(srcSystemVar->varDefValue, 
                      destSystemVar->varDefValue);
    setSystemVarValue(srcSystemVar->varConfValue, 
                      destSystemVar->varConfValue);
    destSystemVar->sessionGlobalType = srcSystemVar->sessionGlobalType;
    destSystemVar->sessionDefValueFromGlobal = 
        srcSystemVar->sessionDefValueFromGlobal;
    destSystemVar->isReadWrite = srcSystemVar->isReadWrite;
    destSystemVar->resultNum = srcSystemVar->resultNum;
    for (int i = 0; i < destSystemVar->resultNum; i++)
    {
        setSystemVarValue(&(srcSystemVar->result[i][0]), 
                          &(destSystemVar->result[i][0]));
    }
    destSystemVar->selectResultNum = srcSystemVar->selectResultNum;
    for (int i = 0; i < destSystemVar->selectResultNum; i++)
    {
        setSystemVarValue(&(srcSystemVar->selectResult[i][0]), 
                          &(destSystemVar->selectResult[i][0]));
    }
    destSystemVar->showResultNum = srcSystemVar->showResultNum;
    for (int i = 0; i < destSystemVar->showResultNum; i++)
    {
        setSystemVarValue(&(srcSystemVar->showResult[i][0]), 
                          &(destSystemVar->showResult[i][0]));
    }
}


static void 
setSystemVarValueByVarName(const char *varName, 
                           const char *value, char *varValue)
{
    if (strncasecmp(varName, "optimizer_switch", 16) == 0)
    {
        setSystemVarValueForOptimizerSwitch(value, varValue);
    }
    else
    {
        setSystemVarValue(value, varValue);
    }
}


static void 
setSystemVarValueForOptimizerSwitch(const char *value, char *varValue)
{
    char newVarValue[SYSTEM_VAR_VALUE_MAX_LEN];
    int valueLen;
    int setItemStartIndex;

    valueLen = strlen(value);
    setItemStartIndex = 0;
    for (int i = 0; i <= valueLen; i++)
    {
        if (value[i] == '=')
        {
            char *setItemName;
            int setItemNameLen;
            int setItemLen;

            setItemNameLen = i - setItemStartIndex;
            if (setItemNameLen == 0)
            {
                elog(ERROR, "Invalid value '%s' for 'optimizer_switch'", value);
            }
            setItemName = palloc0(setItemNameLen + 1);
            memcpy(setItemName, (value + setItemStartIndex), setItemNameLen);

            for (int j = (i + 1); j <= valueLen; j++)
            {
                if ((value[j] == ',') || (value[j] == '\0'))
                {
                    int itemIndexInVarValue;
                    int nextItemIndexInVarValue;
                    int varValueLen;

                    setItemLen = j - setItemStartIndex;
                    itemIndexInVarValue = 0;
                    nextItemIndexInVarValue = 0;
                    varValueLen = strlen(varValue);
                    for (int k = 0; k < varValueLen; k++)
                    {
                        if (strncasecmp((varValue + k), setItemName, setItemNameLen) == 0)
                        {
                            itemIndexInVarValue = k;
                            for (int l = itemIndexInVarValue; l <= varValueLen; l++)
                            {
                                if ((varValue[l] == ',') || (varValue[l] == '\0'))
                                {
                                    nextItemIndexInVarValue = l;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                    if ((itemIndexInVarValue == 0) && 
                        (nextItemIndexInVarValue == 0))
                    {
                        elog(ERROR, "Invalid value '%s' for 'optimizer_switch'", value);
                    }

                    memcpy(newVarValue, varValue, itemIndexInVarValue);
                    memcpy((newVarValue + itemIndexInVarValue), 
                           (value + setItemStartIndex), setItemLen);
                    memcpy((newVarValue + itemIndexInVarValue + setItemLen), 
                           (varValue + nextItemIndexInVarValue), 
                           (varValueLen - nextItemIndexInVarValue + 1));
                    snprintf(varValue, SYSTEM_VAR_VALUE_MAX_LEN, "%s", newVarValue);

                    i = j;
                    setItemStartIndex = i + 1;

                    break;
                }
            }
        }
    }
}


static void 
setSystemVarValue(const char* value, char *varValue)
{
    if (value != NULL)
    {
        snprintf(varValue, SYSTEM_VAR_VALUE_MAX_LEN, "%s", value);
    }
    else 
    {
        varValue[0] = '\0';
    }
}


static void 
rectifySystemVarValue(const char *varName, char **varValue, 
                      bool isSessionSystemVar)
{
    if (*varValue != NULL)
    {
        if (strncmp(*varValue, "halo_mysql_system_var_default", 29) == 0)
        {
            getDefaultValue(varName, isSessionSystemVar, varValue);
            return;
        }
        else 
        {
            if (strncasecmp(varName, "query_cache_size", 16) == 0)
            {
                char buf[64];
                int val = atoi(*varValue);
                if (val < 0)
                {
                    elog(ERROR, 
                         "Invalid value '%d' for query_cache_size", 
                         val);
                }

                val = (val / 1048576) * 1048576;
                snprintf(buf, 64, "%d", val);
                *varValue = pstrdup(buf);
                return;
            }
            else 
            {
                int varValueLen;
                SystemVar *systemVar;
                bool found;

                varValueLen = strlen(*varValue);
                systemVar = (SystemVar *)hash_search(sessionSystemVars, 
                                                     varName, 
                                                     HASH_FIND, 
                                                     &found);
                if (!found)
                {
                    elog(ERROR, "Unknown system variable '%s'", varName);
                }

                if (0 < systemVar->resultNum)
                {
                    for (int i = 0; i < systemVar->resultNum; i++)
                    {
                        int resultItemLen = strlen(systemVar->result[i]);
                        if ((varValueLen == resultItemLen) && 
                            (strncasecmp(*varValue,
                                         systemVar->result[i], 
                                         varValueLen) == 0))
                        {
                            /* do nothing; */
                            return;
                        }
                        else 
                        {
                            int showResultItemLen = strlen(systemVar->showResult[i]);
                            if ((varValueLen == showResultItemLen) && 
                                (strncasecmp(*varValue,
                                             systemVar->showResult[i], 
                                             varValueLen) == 0))
                            {
                                *varValue = pstrdup(systemVar->result[i]);
                                return;
                            }
                        }
                    }

                    elog(ERROR, 
                         "Variable '%s' can't be set to the value of '%s'",
                         varName,
                         *varValue);
                }
                return;
            }
        }
    }
}


static void
getDefaultValue(const char *varName, bool isSessionSystemVar, 
                char **varValue)
{
    bool sessionDefValueFromGlobal;

    if (strncasecmp(varName, "last_insert_id", 14) == 0)
    {
        elog(ERROR, 
             "Variable '%s' doesn't have a default value",
             varName);
    }

    if (isSessionSystemVar)
    {
        SystemVar *systemVar;
        bool found;

        systemVar = (SystemVar *)hash_search(sessionSystemVars, 
                                             varName,
                                             HASH_FIND, 
                                             &found);
        if (found)
        {
            sessionDefValueFromGlobal = systemVar->sessionDefValueFromGlobal;
        }
        else
        {
            elog(ERROR, 
                 "Can not find system variable \"%s\" from global.", 
                 varName);
        }
    }

    if (isSessionSystemVar && sessionDefValueFromGlobal)
    {
        SystemVar *systemVar;
        bool found;

        lockGlobalSystemVars();
        systemVar = (SystemVar *)hash_search(globalSystemVars, 
                                             varName,
                                             HASH_FIND, 
                                             &found);
        if (found)
        {
            *varValue = pstrdup(systemVar->varConfValue);
            unlockGlobalSystemVars();
        }
        else
        {
            unlockGlobalSystemVars();
            elog(ERROR, 
                 "Can not find system variable \"%s\" from global.", 
                 varName);
        }
    }
    else
    {
        int varNameLen = strlen(varName);
        for (int i = 0; i < baseSystemVarsNum; i++)
        {
            SystemVar *systemVar = baseSystemVars[i];
            int baseVarNameLen = strlen(systemVar->varName);
            if ((varNameLen == baseVarNameLen) && 
                (strncasecmp(varName, systemVar->varName, varNameLen) == 0))
            {
                *varValue = pstrdup(systemVar->varDefValue);
                return;
            }
        }

        elog(ERROR, 
             "Can not find system variable \"%s\" from base.", 
             varName);
    }
}


static void 
applySystemVarValue(const char *varName, const char *varValue)
{
    if (strncasecmp(varName, "autocommit", 10) == 0)
    {
        if ((strncasecmp(varValue, "1", 1) == 0) || 
            (strncasecmp(varValue, "on", 2) == 0))
        {
            if (autoCommit == 0)
            {
                /* 0 --> 1 */
                needCommitTrx = true;
                needStartNewTrx = false;
            }
            else 
            {
                /* 1 --> 1 */
                needCommitTrx = false;
                needStartNewTrx = false;
            }

            autoCommit = 2;
        }
        else 
        {
            if (autoCommit == 0)
            {
                /* 0 --> 0 */
                needCommitTrx = false;
                needStartNewTrx = false;
            }
            else 
            {
                /* 1 --> 0 */
                needCommitTrx = false;
                needStartNewTrx = true;
            }

            autoCommit = 0;
        }
    }
    else if (strncasecmp(varName, "time_zone", 9) == 0)
    {
        VariableSetStmt *variableSetStmt;
        char *newVarValue;

        if (strncasecmp(varValue, "system", 6) == 0)
        {
            newVarValue = getOSTimeZone();
        }
        else 
        {
            int newVarValueLen;

            newVarValue = pstrdup(varValue);
            newVarValueLen = strlen(newVarValue);
            for (int i = 0; i < newVarValueLen; i++)
            {
                if (newVarValue[i] == ':')
                {
                    newVarValue[i] = '\0';
                    break;
                }
            }
        }
        variableSetStmt = makeNode(VariableSetStmt);
        variableSetStmt->kind = VAR_SET_VALUE;
        variableSetStmt->name = "timezone";
        variableSetStmt->args = list_make1(makeStringConst(newVarValue, -1));
        ExecSetVariableStmt(variableSetStmt, true);
    }
    else if (strncasecmp(varName, "sql_mode", 8) == 0)
    {
        char *rawValue;
        List *valueList;
        uint64 tempSqlMode = 0;

        isStrictTransTablesOn = false;

        /* Need a modifiable copy of string */
        rawValue = pstrdup(varValue);
        if (SplitIdentifierString(rawValue, ',', &valueList))
        {
            ListCell *lc;

            foreach(lc, valueList)
            {
                char *curValue = (char *)lfirst(lc);
                if (strncasecmp(curValue, "STRICT_TRANS_TABLES", 19) == 0)
                {
                    //isStrictTransTablesOn = true;
                    isStrictTransTablesOn = false;
                }
                else if (strncasecmp(curValue, "NO_ZERO_IN_DATE", 15) == 0)
                {
                    tempSqlMode |= MYS_MODE_NO_ZERO_IN_DATE;
                }
                else if (strncasecmp(curValue, "NO_ZERO_DATE", 12) == 0)
                {
                    tempSqlMode |= MYS_MODE_NO_ZERO_DATE;
                }
                else if (strncasecmp(curValue, "ALLOW_INVALID_DATES", 12) == 0)
                {
                    tempSqlMode |= MYS_MODE_INVALID_DATES;
                }
            }

            mys_sqlMode = tempSqlMode;
        }
        else
        {
            pfree(rawValue);
		    list_free(valueList);
            elog(ERROR, "List syntax is invalid.");
        }

        pfree(rawValue);
		list_free(valueList);
    }
    else if (strncasecmp(varName, "default_week_format", 19) == 0)
    {
        char *endPtr;
        int value;

        errno = 0;
        value = strtol(varValue, &endPtr, 10);
        if (errno == ERANGE)
        {
            elog(ERROR, "incorrect default_week_format value: %s", varValue);
        }

        if (*endPtr != '\0')
        {
            elog(ERROR, "incorrect default_week_format value: %s", varValue);
        }

        if (value > 7 || value < 0)
        {
            elog(ERROR, "incorrect default_week_format value: %s", varValue);
        }
        else
        {
            default_week_format = value;
        }
    }
    else 
    {
        /* do nothing; */
    }
}


static int
getSystemVariableValueImpl(char *varName, 
                           bool isSessionSystemVar, 
                           int selectShowType, 
                           char **varValue)
{
    int ret;
    SystemVar *systemVar;
    bool found;

    if (sessionSystemVars == NULL)
    {
        initSystemVariables();
    }

    if (varName == NULL)
    {
        return 1;
    }

    lowerSystemVarName(varName);

    ret = 0;
    if (isSessionSystemVar)
    {
        systemVar = (SystemVar *)hash_search(sessionSystemVars, 
                                             varName, 
                                             HASH_FIND, 
                                             &found);
        if (found || (NULL != (systemVar = dynAddSystemVar(varName))))
        {
            *varValue = getConfValue(systemVar, selectShowType);
            ret = 0;
        }
        else
        {
            *varValue = NULL;
            ret = 1;
        }
    }
    else 
    {
        lockGlobalSystemVars();
        systemVar = (SystemVar *)hash_search(globalSystemVars, 
                                             varName, 
                                             HASH_FIND, 
                                             &found);
        if (found || (NULL != (systemVar = dynAddSystemVar(varName))))
        {
            if ((systemVar->sessionGlobalType == 0) || 
                (systemVar->sessionGlobalType == 1) || 
                (systemVar->sessionGlobalType == 3))
            {
                *varValue = getConfValue(systemVar, selectShowType);
                unlockGlobalSystemVars();
                ret = 0;
            }
            else 
            {
                unlockGlobalSystemVars();
                *varValue = NULL;
                ret = 2;
            }
        }
        else
        {
            unlockGlobalSystemVars();
            *varValue = NULL;
            ret = 1;
        }
    }

    return ret;
}


static char *
getConfValue(SystemVar *systemVar, int selectShowType)
{
    char *ret;

    ret = NULL;
    if (selectShowType == 1)
    {
        if (systemVar->selectResultNum == 0)
        {
            ret = pstrdup(systemVar->varConfValue);
        }
        else 
        {
            int varConfValueLen = strlen(systemVar->varConfValue);
            for (int i = 0; i < systemVar->selectResultNum; i++)
            {
                int resultItemLen = strlen(systemVar->result[i]);
                if ((varConfValueLen == resultItemLen) && 
                    (strncasecmp(systemVar->varConfValue, 
                                 systemVar->result[i], 
                                 varConfValueLen) == 0))
                {
                    ret = pstrdup(systemVar->selectResult[i]);
                    break;
                }
            }
        }
    }
    else 
    {
        if (systemVar->showResultNum == 0)
        {
            ret = pstrdup(systemVar->varConfValue);
        }
        else 
        {
            int varConfValueLen = strlen(systemVar->varConfValue);
            for (int i = 0; i < systemVar->showResultNum; i++)
            {
                int resultItemLen = strlen(systemVar->result[i]);
                if ((varConfValueLen == resultItemLen) && 
                    (strncasecmp(systemVar->varConfValue, 
                                 systemVar->result[i], 
                                 varConfValueLen) == 0))
                {
                    ret = pstrdup(systemVar->showResult[i]);
                    break;
                }
            }
        }
    }

    return ret;
}


static Node *
makeStringConst(char *str, int location)
{
	A_Const *n = makeNode(A_Const);

	n->val.type = T_String;
	n->val.val.str = str;
	n->location = location;

	return (Node *)n;
}


static char *
getOSTimeZone(void)
{
    char *ret;
    int timezone = 0;
    time_t localTime;
    time_t utcTime;
    time_t tt;
    struct tm *localTm;
    struct tm *utcTm;

    time(&tt);
    localTm = localtime(&tt);
    localTime = mktime(localTm);
    utcTm = gmtime(&tt);
    utcTime = mktime(utcTm);
    timezone = (localTime - utcTime) / 3600;
    ret = palloc0(64);
    snprintf(ret, 64, "%d", timezone);

    return ret;
}

