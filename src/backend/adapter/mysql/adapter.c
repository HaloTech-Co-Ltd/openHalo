/*-------------------------------------------------------------------------
 *
 * adapter.c
 *	  MySQL adapter routines
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
 *	  src/backend/adapter/mysql/adapter.c
 *
 *-------------------------------------------------------------------------
 */

#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>

#include "postgres.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "libpq/libpq.h"
#include "postgres_ext.h"
#include "postmaster/postmaster.h"
#include "postmaster/postmaster2.h"
#include "postmaster/protocol_interface.h"
#include "nodes/pg_list.h"
#include "utils/hsearch.h"
#include "utils/varlena.h"
#include "utils/pidfile.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "access/printtup.h"
#include "access/table.h"
#include "utils/portal.h"
#include "utils/acl.h"
#include "utils/rel.h"
#include "tcop/tcopprot.h"
#include "tcop/cmdtag.h"
#include "storage/ipc.h"
#include "common/ip.h"
#include "catalog/pg_type.h"
#include "libpq/libpq-be.h"

#include "./utils/lsyscache.h"
#include "adapter/mysql/adapter.h"
#include "adapter/mysql/userLogonAuth.h"
#include "adapter/mysql/common.h"
#include "adapter/mysql/errorConvertor.h"
#include "adapter/mysql/pwdEncryptDecrypt.h"
#include "adapter/mysql/netTransceiver.h"
#include "adapter/mysql/systemVar.h"
#include "commands/mysql/mys_uservar.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_collation_d.h"
#include "access/table.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "utils/fmgroids.h"


#define MYS_REQ_SLEEP 0
#define MYS_REQ_QUIT 1
#define MYS_REQ_USE_DATABASE 2
#define MYS_REQ_QUERY 3
#define MYS_REQ_FIELD_LIST 4
#define MYS_REQ_PING 14
#define MYS_REQ_PREPARE 22
#define MYS_REQ_EXECUTE 23
#define MYS_REQ_SEND_LONG_DATA 24
#define MYS_REQ_CLOSE 25
#define MYS_REQ_RESET 26
#define MYS_REQ_FRTCH 28
#define MYS_REQ_RESET_CONN 31
#define HALO_REQ_QUERY 'Q'
#define HALO_REQ_QUIT 'X'

#define PRE_STMT_NUM_MAX 5000
#define PRE_STMT_NAME_LEN 128
#define EXTEND_PRE_STMT_NAME_LEN 11  
#define MYS_HALO_DATA_TYPE_NUM_MAX 128

#define MYS_SYS_VARIABLES_NUM_MAX 256
#define MYS_SYS_VARIABLE_NAME_LEN_MAX 128
#define MYS_SYS_VARIABLE_VALUE_LEN_MAX 512

#define DDL_WORDS_NUM_MAX 512
#define DDL_WORD_LEN_MAX 256
#define PRE_SQL_SIZE 2048
#define POST_SQL_SIZE 8192

#define CUSTOMIZED_CASE_COLUMN_NAME_NUM_MAX 256
#define CUSTOMIZED_CASE_COLUMN_NAME_LEN 128

#define RESERVED_FOR_ADDITIONAL_SQL 64

/* 配置参数 */
char *halo_mysql_version = NULL;
bool halo_mysql_ci_collation = true;
int halo_mysql_column_name_case_control = 0;
bool halo_mysql_explicit_defaults_for_timestamp = false;
bool halo_mysql_auto_rollback_tx_on_error = false;
bool halo_mysql_support_multiple_table_update = true;

/* 全局变量 */
int moreResultsFlag = 0;
int warnings = 0;
bool isExtendPreStmt = false;
bool skipUtf8Verify;
Oid curSqlTableId = InvalidOid;
unsigned long foundRows = 1;
long long affectedRows = -1;
unsigned long lastInsertID = 0;
unsigned long lastInsertIDForPacket = 0;
Oid caseInsensitiveId = UINT_MAX;
bool needCommitTrx = false;
bool needStartNewTrx = false;
bool isStrictTransTablesOn = false;

static bool trxNotCommit = false;
static ulong frontEndCaps;   /* Front-End capabilities */
static int curLastInsertIDTimes = 0;
static bool addBeforeBeforeCurSQL = false;
static int needAddCommitOrRollbackAfterCurSQL = 1;
static bool addCommitOrRollBackBeforeCurSQL = false;
static bool isResetConnStmt = false;
static int serverStatus;
static int inTransactionFlag = 0;
static int setUserSystemVar = 0;
static int sessionStateType = -1;
static char sessionTrackSystemVariables[1024];
static bool inSsTrackSystemVariables = false;
static bool isSsTrackSchemaOn = true;
static bool isSsTrackStateChangeOn = false;
static char sessionTrackInfo[1024];
static char systemVarName[1024];
static char systemVarVal[1024];
static int sessionStateChanged = 0;
static List *sockPaths = NIL;
static char *warnErrLevel = "Error";
static int warnErrCode;
static char warnErrMsg[1024];
static Oid informationSchemaID = InvalidOid;
static int systemVarType = 0;
static bool isInformationSchema = false;
static int preOrPostSQLNum;
static char nullBitmap[256];
static bool isExtendExeStmt = false;
static bool isExtendCloseStmt = false;


typedef enum {
    SRWT_UNKNOWN = 0,
    SRWT_READ = 1,
    SRWT_WRITE = 2,
    SRWT_BEING = 3,     // unused
    SRWT_COMMIT = 4,
    SRWT_ROLLBACK = 5
} sqlReadWriteType;

static int curExtendPreStmtId = 0;
typedef struct 
{
    char name[PRE_STMT_NAME_LEN + 1];
    int paramNum;
    int nullBitmapLength;
    int paramTypeOffset;
    int paramValueOffset1;
    int paramValueOffset2;
    int paramTypes[256];
    int columnNum;
    int columnTypes[256];
    int columnCharacterSet[256];
    sqlReadWriteType readWriteType;
    bool isValid;
} preStmtInfo;
static preStmtInfo preStmtInfoes[PRE_STMT_NUM_MAX];
static int preStmtInfoNum = 0;

enum enum_field_types { 
    MYS_TYPE_DECIMAL, 
    MYS_TYPE_TINY, 
    MYS_TYPE_SHORT, 
    MYS_TYPE_LONG, 
    MYS_TYPE_FLOAT, 
    MYS_TYPE_DOUBLE, 
    MYS_TYPE_NULL, 
    MYS_TYPE_TIMESTAMP, 
    MYS_TYPE_LONGLONG, 
    MYS_TYPE_INT24, 
    MYS_TYPE_DATE, 
    MYS_TYPE_TIME, 
    MYS_TYPE_DATETIME, 
    MYS_TYPE_YEAR, 
    MYS_TYPE_NEWDATE, 
    MYS_TYPE_VARCHAR, 
    MYS_TYPE_BIT, 
    MYS_TYPE_TIMESTAMP2, 
    MYS_TYPE_DATETIME2, 
    MYS_TYPE_TIME2, 
    MYS_TYPE_TYPED_ARRAY, 
    MYS_TYPE_INVALID = 243, 
    MYS_TYPE_BOOL = 244, 
    MYS_TYPE_JSON = 245, 
    MYS_TYPE_NEWDECIMAL = 246, 
    MYS_TYPE_ENUM = 247, 
    MYS_TYPE_SET = 248, 
    MYS_TYPE_TINY_BLOB = 249, 
    MYS_TYPE_MEDIUM_BLOB = 250, 
    MYS_TYPE_LONG_BLOB = 251, 
    MYS_TYPE_BLOB = 252, 
    MYS_TYPE_VAR_STRING = 253, 
    MYS_TYPE_STRING = 254, 
    MYS_TYPE_GEOMETRY = 255
};
typedef struct 
{
    int haloDataType;   /* hash key must be first item */
    int mySQLDataType;
} HaloMySQLDataType;
static HTAB *haloMysqlDataTypes = NULL;
static int haloMysqlDataTypesNum = 0;

typedef struct 
{
    int tableId;   /* hash key must be first item */
    int schemaId;
    char *schemaName;
    char *tableName;
} TableInfoes;
static HTAB *tableInfoeses = NULL;

typedef struct
{
    char srcColName[CUSTOMIZED_CASE_COLUMN_NAME_LEN];
    char destColName[CUSTOMIZED_CASE_COLUMN_NAME_LEN];
    int destColNameLen;
} CustomizedCaseColumName2;
typedef struct
{
    char tabName[CUSTOMIZED_CASE_COLUMN_NAME_LEN];
    HTAB *colsCustomizedCaseColumNames;
} TabCustomizedCaseColumNames;
typedef struct
{
    char scheName[CUSTOMIZED_CASE_COLUMN_NAME_LEN];
    HTAB *tabsCustomizedCaseColumNames;
} ScheCustomizedCaseColumNames;
bool initedCustomizedCaseColumNames = false;
static HTAB *schesCustomizedCaseColumNames = NULL;

static void initListen(void);
static int acceptConn(pgsocket serverFd, Port *port);
static void closeListen(pgsocket serverFd);
static void initServer(void);
static int startServer(Port *port);
static void authenticate(Port *port, const char **username);
static void mainFunc(Port *port, int argc, char *argv[]) pg_attribute_noreturn();
static void sendErrorMessage(ErrorData *edata);
static void mysqlSendCancelKey(int pid, int32 key);
static void sendReadyForQuery(CommandDest dest);
static int readCommand(StringInfo inBuf);
static void endCommand(QueryCompletion *qc, CommandDest dest);
static bool printTup(TupleTableSlot *slot, DestReceiver *self, CommandTag commandTag);
static void printTupStartup(DestReceiver *self, int operation, 
                            TupleDesc typeinfo, CommandTag commandTag);
static void printTupShutdown(DestReceiver *self);
static void printTupDestroy(DestReceiver *self);
static int processCommand(int *firstChar, StringInfo inBuf);
static void reportParamStatus(const char *name, char *val);

static int haloAToI(const char *str, int strLen);
static int assembleColDefPacket41Payload(const char *schemaName, 
                                         const char *tableName, 
                                         const char *orgTableName, 
                                         const char *columnName, 
                                         const char *orgColumnName, 
                                         int characterSet, 
                                         int columnLen, 
                                         int columnType, 
                                         int columnFlags, 
                                         int decimals,
                                         char *payloadBuf);
static int assembleColDefPacket320Payload(const char *tableName, 
                                          const char *columnName, 
                                          int columnType, 
                                          int columnFlags,  
                                          char *payloadBuf);
static int assembleLenencUnsignedLong(unsigned long val, char *buf);
static int assembleLenencString(const char *str, char *buf);
static int assembleLenencString2(const char *str, int strLen, char *buf);
static uint64 processLenencString(const char *buf, int *lengthEncodeSize);
static int processDateType(const unsigned char *buf, 
                           char *outBuf, 
                           int outBufLen);
static int processTimeType(const unsigned char *buf, 
                           char *outBuf, 
                           int outBufLen);
static int processDatetimeTimestampType(const unsigned char *buf, 
                                        char *outBuf, 
                                        int outBufLen);
static int convertTime(char *value);
static void assembleOKEOFCommonPacketPayload(int statusFlags, 
                                             int warnings, 
                                             char *payloadBuf, 
                                             int *payloadBufOffset);
static int assembleOKPacketPayload(int statusFlags, 
                                   int warnings, 
                                   char *payloadBuf);
static int assembleErrorPacketPayload(int errCode, 
                                      char sqlStateMarker, 
                                      const char *sqlState, 
                                      const char *errMsg, 
                                      char *payloadBuf);
static int assembleColCountPacketPayload(int metadataFollowFlag, 
                                         int columnCount, 
                                         char *payloadBuf);
static int assembleColDefPacketPayload(const char *schemaName, 
                                       const char *tableName, 
                                       const char *orgTableName, 
                                       const char *columnName, 
                                       const char *orgColumnName, 
                                       int characterSet, 
                                       int columnLen, 
                                       int columnType, 
                                       int columnFlags, 
                                       int decimals,
                                       char *payloadBuf);
static int assembleEOFPacketPayload(int warnings, 
                                    int statusFlags, 
                                    char *payloadBuf);

static void initHaloMySqlDataTypesHashTable(void);
static void initPreStmtInfoes(void);
static void initTableInfoesHashTable(void);
static void initSessionTrackSystemVariables(void);
static void initCustomizedCaseColumnNames(void);
static void initCustomizedCaseColumnName(const char *schemaName, 
                                         const char *tableName, 
                                         const char *columnName);
static char * getTypeName(int pgDataType);
static int getMySQLDataType(int haloDataType);

static void printTupStartupForOther(char *schemaName, 
                                    char *tableName, 
                                    DestReceiver *self, 
                                    int operation, 
                                    TupleDesc typeinfo, 
                                    CommandTag commandTag);
static void printTupStartupForInformationSchema(char *schemaName, 
                                                char *tableName,
                                                DestReceiver *self, 
                                                int operation, 
                                                TupleDesc typeinfo, 
                                                CommandTag commandTag);
static void recoverColumnName(const char *schemaName, const char *tableName, 
                              char *columnName);
static char *recoverSysColumnName(char *tableName, char *columnName);
static bool printTupBinary(TupleTableSlot *slot, 
                           DestReceiver *self, 
                           CommandTag commandTag);
static bool printTupText(TupleTableSlot *slot, 
                         DestReceiver *self, 
                         CommandTag commandTag);
static bool printTupTextForSystemVars(TupleTableSlot *slot, 
                                      DestReceiver *self);
static void skipSpaces(char **secondWord);
static int createServerPort(int family, 
                            const char *hostName, 
                            unsigned short portNumber, 
                            const char *unixSocketDir, 
                            ProtocolInterface *protocolHandler);
static int lockAFUnix(const char *unixSocketDir, const char *unixSocketPath);
static int setupAFUnix(const char *sockPath);
static void closeSocket(int code, Datum arg);
static void simulateShowWarnErrReturn(void);
static void simulateShowFieldsReturn(void);
static bool simulateChecksumTable(char *stmt);
static bool simulateAnalyzeCheckOptimizeRepairTable(char *stmt);
static void simulateShowGrants(void);
static bool rectifyCommand(StringInfo inBuf);
static int rewriteExtendPreStmt_(StringInfo inBuf, 
                                 sqlReadWriteType readWriteType);
static int rewriteExtendPreStmt(StringInfo inBuf);
static sqlReadWriteType getRWTForExtendExeStmt(StringInfo inBuf);
static bool rewriteExtendExeStmt_(StringInfo inBuf);
static bool rewriteExtendExeStmt(StringInfo inBuf);
static void assembleExtendExeStmtParmVal(char *sql, 
                                         preStmtInfo *esi, 
                                         int paramIndex, 
                                         char *newStmt, 
                                         int *newStmtOffset, 
                                         int *paramsValueOffset);
static bool rewriteExtendCloseStmt(StringInfo inBuf);
static bool convertExtendPreStmt(char *stmt, int stmtLen, int remainSpace, 
                                 char *newStmt, int *newStmtIndex, int *paramNum);
static int cacheExtendPreStmt(StringInfo inBuf, 
                              sqlReadWriteType readWriteType, 
                              preStmtInfo **pSI);
static void resetConnection(StringInfo inBuf);
static void sendOKPacketSpecialInfo(const char *info);
static void sendEOFPacketFlush(void);
static void sendEOFPacketNoFlush(void);
static void sendErrPacket(int errCode, const char *errMsg, const char *sqlState);
static void sendSyntaxError(const char *command);
static void sendUnsupportError(const char *command);
static void endExtendPreStmt(void);
static void endNormalPreStmt(void);
static int calcLenencIntLen(int len);
static void convertColumnValue(TupleTableSlot *slot, 
                               Datum attr, 
                               int columnNum, 
                               char **columnVal, 
                               int *columnValLen);
static void convertBitToMySQLHex(char *bit, char *hex, int hexSize);
static char convertToHex(char ch1, char ch2);
static int convertCharToInt(char ch);
static int convertHexToMySQLBinary(char *str);
static void addAdditionalSQL(StringInfo inBuf);

static ProtocolInterface protocolHandler = {
    .type = T_MySQLProtocol,
    .listen_init = initListen,
    .accept = acceptConn,
    .close = closeListen,
	.init = initServer,
    .start = startServer,
	.authenticate = authenticate,
	.mainfunc = mainFunc,
	.send_message = sendErrorMessage,
	.send_cancel_key = mysqlSendCancelKey,
	.comm_reset = NULL,
	.is_reading_msg = NULL,
	.send_ready_for_query = sendReadyForQuery,
	.read_command = readCommand,
	.end_command = endCommand,
	.printtup = printTup,
	.printtup_startup = printTupStartup,
	.printtup_shutdown = printTupShutdown,
	.printtup_destroy = printTupDestroy,
	.process_command = processCommand,
    .report_param_status = reportParamStatus
};


ProtocolInterface *
getMysProtocolHandler(void)
{
    return &protocolHandler;
}

void 
sendOKPacket(void)
{
    char *payloadBuf;
    int payloadLen;

    netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);
    serverStatus = sessionStateChanged | moreResultsFlag | autoCommit | inTransactionFlag;
    payloadLen = assembleOKPacketPayload(serverStatus, 
                                         warnings, 
                                         payloadBuf);
    netTransceiver->writePacketHeaderNoFlush(payloadLen);
    netTransceiver->finishWriteToBufFlush(payloadLen);
}

void 
sendErrorPacket(int errCode, const char *errMsg)
{
    sendErrPacket(errCode, errMsg, "HY000");
}

static void
initListen(void)
{
    int		status;
	bool	listen_addr_saved = false;
    char	   *rawstring;
    List	   *elemlist;
	ListCell   *l;
	int			success = 0;

	if (ListenAddresses)
	{
		rawstring = pstrdup(ListenAddresses);

		if (!SplitGUCList(rawstring, ',', &elemlist))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"listen_addresses. Connection will be disconnect.")));
            proc_exit(1);
		}

		foreach(l, elemlist)
		{
			char	   *curhost = (char *) lfirst(l);

			if (strcmp(curhost, "*") == 0)
            {
                status = createServerPort(AF_UNSPEC, 
                                          NULL,
                                          (unsigned short) PostMySQLPortNumber,
                                          NULL,
                                          &protocolHandler);
            }
            else
            {
                status = createServerPort(AF_UNSPEC, 
                                          curhost,
                                          (unsigned short) PostMySQLPortNumber,
                                          NULL,
                                          &protocolHandler);
            }

            if (status == STATUS_OK)
            {
                success++;
                if (!listen_addr_saved)
                {
                    AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, curhost);
                    listen_addr_saved = true;
                }
            }
			else
            {
				ereport(WARNING,
						(errmsg("could not create listen socket for \"%s\"",
								curhost)));
            }
		}

		if (!success && elemlist != NIL)
        {
			ereport(WARNING,
					(errmsg("could not create any TCP/IP sockets to accept mysql connections")));
        }

		list_free(elemlist);
		pfree(rawstring);
	}

#ifdef HAVE_UNIX_SOCKETS
    success = 0;
	if (Unix_socket_directories)
	{

		rawstring = pstrdup(Unix_socket_directories);

		if (!SplitDirectoriesString(rawstring, ',', &elemlist))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\". Connection will be disconnect.",
							"unix_socket_directories")));
		}

		foreach(l, elemlist)
		{
			char	   *socketdir = (char *) lfirst(l);

            status = createServerPort(AF_UNIX, 
                                      NULL,
                                      (unsigned short) PostMySQLPortNumber,
                                      socketdir,
                                      &protocolHandler);

            if (status == STATUS_OK)
            {
                success++;
				if (success == 1)
					AddToDataDirLockFile(LOCK_FILE_LINE_SOCKET_DIR, socketdir);
			}
			else
            {
				ereport(WARNING,
						(errmsg("could not create Unix-domain socket in directory \"%s\"",
								socketdir)));
            }
		}

		if (!success && elemlist != NIL)
        {
			ereport(ERROR,
					(errmsg("could not create any Unix-domain sockets. Connection will be disconnect.")));
            proc_exit(1);
        }

		list_free_deep(elemlist);
		pfree(rawstring);
	}
#endif
}

static int
acceptConn(pgsocket serverFd, Port *port)
{
    return StreamConnection(serverFd, port);
}

static void
closeListen(pgsocket serverFd)
{
    StreamClose(serverFd);
}

static void
initServer(void)
{
    int	socket_pos PG_USED_FOR_ASSERTS_ONLY;
	int	latch_pos PG_USED_FOR_ASSERTS_ONLY;

    initNetTransceiver();
    
    on_proc_exit(closeSocket, 0);

#ifndef WIN32
	if (!pg_set_noblock(MyProcPort->sock))
    {
		ereport(COMMERROR,
				(errmsg("could not set socket to nonblocking mode: %m")));
    }
#endif

    FeBeWaitSet = CreateWaitEventSet(TopMemoryContext, 3);
	socket_pos = AddWaitEventToSet(FeBeWaitSet, WL_SOCKET_WRITEABLE,
								   MyProcPort->sock, NULL, NULL);
	latch_pos = AddWaitEventToSet(FeBeWaitSet, WL_LATCH_SET, PGINVALID_SOCKET,
								  MyLatch, NULL);
	AddWaitEventToSet(FeBeWaitSet, WL_POSTMASTER_DEATH, PGINVALID_SOCKET,
					  NULL, NULL);

	Assert(socket_pos == FeBeWaitSetSocketPos);
	Assert(latch_pos == FeBeWaitSetLatchPos);
}

static int
startServer(Port *port)
{
    MyBackendType = B_BACKEND;

    return STATUS_OK;
}

static void
authenticate(Port *port, const char **username)
{
    char *payloadBuf;
    int payloadBufSize;
    int payloadLen;
    int max_pkg_size;
    int db_name_len_max;
    int user_name_len_max;
    char auth_resp_buf[512];
    int auth_resp_buf_len;
    StringInfo inBuf;
    MemoryContext oldContext;

    initErrorCodeHashTable();

    ClientAuthInProgress = true;

    payloadBuf = NULL;
    payloadBufSize = 1024;
    db_name_len_max = 256;
    user_name_len_max = 256;
    auth_resp_buf_len = 512;
    inBuf = makeStringInfo();

    netTransceiver->getBufForPayload(&payloadBuf, payloadBufSize);
    payloadLen = assembleHandshakePacketPayload(halo_mysql_version, 
                                                payloadBuf, 
                                                payloadBufSize);
    netTransceiver->writePacketHeaderPayloadFlush(payloadBuf, payloadLen);
    netTransceiver->readPayloadForLogon(inBuf);
    max_pkg_size = 0;
    oldContext = MemoryContextSwitchTo(TopMemoryContext);
    MyProcPort->database_name = (char*)palloc(db_name_len_max);
    memset(MyProcPort->database_name, 0, db_name_len_max);
    MyProcPort->user_name = (char*)palloc(user_name_len_max);
    memset(MyProcPort->user_name, 0, user_name_len_max);
    MemoryContextSwitchTo(oldContext);
    parseHandshakeRespPacketPayload(inBuf->data,
                                    inBuf->len,
                                    &frontEndCaps, 
                                    &max_pkg_size, 
                                    MyProcPort->user_name, 
                                    user_name_len_max,
                                    auth_resp_buf, 
                                    auth_resp_buf_len, 
                                    MyProcPort->database_name, 
                                    db_name_len_max);

    if (MyProcPort->database_name != NULL)
    {
        int dbLen = strlen(MyProcPort->database_name);
        for (int i = 0; i < dbLen; i++)
        {
            if (('A' <= MyProcPort->database_name[i]) && (MyProcPort->database_name[i] <= 'Z'))
            {
                MyProcPort->database_name[i] = MyProcPort->database_name[i] + 32;
            }
        }
    }

    hba_getauthmethod(port);
    get_role_oid(port->user_name, false);
    switch(port->hba->auth_method)
    {
    case uaReject:
        sendErrorPacket(1105, "pg_hba.conf reject");
        elog(WARNING, "%s logon had been reject(ua).", MyProcPort->user_name);
        break;
    case uaImplicitReject:
        sendErrorPacket(1105, "no pg_hba.conf entry");
        elog(WARNING, "%s logon had been reject(no entry).", MyProcPort->user_name);
        exit(1);
        break;
    case uaMD5:
        if(!mysCheckAuth(MyProcPort->user_name, auth_resp_buf))
        {
            sendErrorPacket(1105, "\nauth failed: error password");
            elog(WARNING, "%s logon auth failed.", MyProcPort->user_name);
            exit(1);
        }
        break;
    default:
        break;
    }

    if (MyProcPort->database_name && (0 < strlen(MyProcPort->database_name)))
    {
        char new_search_path[1024] = {0};
        snprintf(new_search_path, 
                 sizeof(new_search_path), 
                 "%s, mysql, pg_catalog, \"$user\", public", 
                 MyProcPort->database_name);
        SetConfigOption("search_path", new_search_path, PGC_USERSET, PGC_S_SESSION);
        elog(NOTICE, "set search: [%s]", new_search_path);
    }
    else
    {
        char new_search_path[1024] = {0};
        SetConfigOption("search_path", new_search_path, PGC_USERSET, PGC_S_SESSION);
        elog(NOTICE, "set search: [%s]", new_search_path);
    }

    SetConfigOption("client_min_messages", "error", PGC_USERSET, PGC_S_SESSION);
    SetConfigOption("bytea_output", "hex", PGC_USERSET, PGC_S_SESSION);
    SetConfigOption("standard_parserengine_auxiliary", "off", PGC_USERSET, PGC_S_SESSION);
    
    initHaloMySqlDataTypesHashTable();
    initPreStmtInfoes();
    initTableInfoesHashTable();
    initSessionTrackSystemVariables();

    ClientAuthInProgress = false;
}

static void
mainFunc(Port *port, int argc, char *argv[])
{
    PostgresMain(argc, argv, port->database_name, port->user_name);
}

static void 
sendErrorMessage(ErrorData *edata)
{
    if (edata->elevel == ERROR || edata->elevel == FATAL)
    {
        affectedRows = -1;
        if (!isIgnoreStmt)
        {
            sendErrPacket(edata->sqlerrcode, edata->message, "HY000");
        }
        else 
        {
            sendOKPacket();
        }

        if (halo_mysql_auto_rollback_tx_on_error)
        {
            if (inTransactionFlag == 1)
            {
                needStartNewTrx = true;
                needAddCommitOrRollbackAfterCurSQL = 2;
                elog(WARNING, "transaction will be auto-rollbacked, and a new transaction will be auto-started");
            }
        }
    }

    return;
}

static void
mysqlSendCancelKey(int pid, int32 key)
{
    return;
}

static void
sendReadyForQuery(CommandDest dest)
{
    return;
}

static int
readCommand(StringInfo inBuf)
{
    int sqlType;

    inBuf->offset = 128;
    if (netTransceiver->readPayload(inBuf))
    {
        sqlType = inBuf->data[inBuf->offset];
        inBuf->offset++;
    }
    else
    {
        elog(ERROR, "Client has disconnect when read.");
        proc_exit(1);
    }
    return sqlType;
}

static void 
endCommand(QueryCompletion *qc, CommandDest dest)
{
    if (qc->commandTag == CMDTAG_EXECUTE)
    {
        sendEOFPacketFlush();
        isExtendExeStmt = false;
    }
    else if (qc->commandTag == CMDTAG_SELECT)
    {
        foundRows = qc->nprocessed;
        affectedRows = -1;
        sendEOFPacketFlush();
    }
    else if ((qc->commandTag == CMDTAG_INSERT) || 
             (qc->commandTag == CMDTAG_UPDATE) || 
             (qc->commandTag == CMDTAG_DELETE))
    {
        foundRows = 1;
        affectedRows = qc->nprocessed;
        sendOKPacket();
    }
    else if (qc->commandTag == CMDTAG_BEGIN)
    {
        inTransactionFlag = 0x0001;
        if (!addBeforeBeforeCurSQL)
        {
            sendOKPacket();
        }
        else 
        {
            addBeforeBeforeCurSQL = false;
        }
        needStartNewTrx = false;
        trxNotCommit = true;
    }
    else if (qc->commandTag == CMDTAG_COMMIT)
    {
        inTransactionFlag = 0x0000;
        if (!addCommitOrRollBackBeforeCurSQL)
        {
            affectedRows = qc->nprocessed;
            sendOKPacket();
        }
        else 
        {
            addCommitOrRollBackBeforeCurSQL = false;
        }
        if (autoCommit == 0)
        {
            needStartNewTrx = true;
        }
        trxNotCommit = false;
    }
    else if (qc->commandTag == CMDTAG_ROLLBACK)
    {
        inTransactionFlag = 0x0000;
        if (!addCommitOrRollBackBeforeCurSQL)
        {
            if (!isResetConnStmt)
            {
                sendOKPacket();
            }
            else 
            {
                /* do nothing; */
            }
        }
        else 
        {
            addCommitOrRollBackBeforeCurSQL = false;
        }
        if (autoCommit == 0)
        {
            needStartNewTrx = true;
        }
        trxNotCommit = false;
        needAddCommitOrRollbackAfterCurSQL = 1;
    }
    else if (qc->commandTag == CMDTAG_PREPARE)
    {
        if (isExtendPreStmt)
        {
            isExtendPreStmt = false;
            endExtendPreStmt();
        }
        else 
        {
            endNormalPreStmt();
        }
    }
    else if (qc->commandTag == CMDTAG_DEALLOCATE)
    {
        if (isExtendCloseStmt)
        {
            isExtendCloseStmt = false;
        }
        else
        {
            affectedRows = qc->nprocessed;
            sendOKPacket();
        }
    }
    else if (qc->commandTag == CMDTAG_DEALLOCATE_ALL)
    {
        isResetConnStmt = false;
        sendOKPacket();
    }
    else if ((qc->commandTag == CMDTAG_TRUNCATE_TABLE) || 
             (qc->commandTag == CMDTAG_SAVEPOINT) || 
             (qc->commandTag == CMDTAG_RELEASE) || 
             (qc->commandTag == CMDTAG_START_TRANSACTION))
    {
        affectedRows = qc->nprocessed;
        sendOKPacket();
    }
    else if (qc->commandTag == CMDTAG_CALL)
    {
        sendOKPacket();
    }
    else if (qc->commandTag == CMDTAG_SET)
    {
        if (setUserSystemVar == 0)
        {
            sessionStateChanged = 0;
            affectedRows = qc->nprocessed;
            sendOKPacket();
        }
        else if (setUserSystemVar == 1)
        {
            if (isSsTrackStateChangeOn || inSsTrackSystemVariables)
            {
                sessionStateChanged = 0x4000;
            }
            else 
            {
                sessionStateChanged = 0;
            }
            sessionStateType = 0;
            sendOKPacket();
        }
        else 
        {
            if (isSsTrackStateChangeOn || isSsTrackSchemaOn)
            {
                sessionStateChanged = 0x4000;
            }
            else 
            {
                sessionStateChanged = 0;
            }
            sessionStateType = 1;
            sendOKPacket();
        }
        sessionStateChanged = 0;
    }
    else if ((qc->commandTag == CMDTAG_CREATE_DATABASE) || 
             (qc->commandTag == CMDTAG_CREATE_ROLE) || 
             (qc->commandTag == CMDTAG_CREATE_SCHEMA) || 
             (qc->commandTag == CMDTAG_CREATE_TABLE) || 
             (qc->commandTag == CMDTAG_CREATE_TABLE_AS) || 
             (qc->commandTag == CMDTAG_CREATE_SEQUENCE) || 
             (qc->commandTag == CMDTAG_CREATE_FUNCTION) || 
             (qc->commandTag == CMDTAG_CREATE_PROCEDURE) ||
             (qc->commandTag == CMDTAG_CREATE_DOMAIN) || 
             (qc->commandTag == CMDTAG_CREATE_TYPE) || 
             (qc->commandTag == CMDTAG_CREATE_VIEW) || 
             (qc->commandTag == CMDTAG_CREATE_INDEX) || 
             (qc->commandTag == CMDTAG_CREATE_SEQUENCE) || 
             (qc->commandTag == CMDTAG_CREATE_CONSTRAINT) || 
             (qc->commandTag == CMDTAG_CREATE_TRIGGER) || 
             (qc->commandTag == CMDTAG_ALTER_DATABASE) || 
             (qc->commandTag == CMDTAG_ALTER_ROLE) || 
             (qc->commandTag == CMDTAG_ALTER_SCHEMA) || 
             (qc->commandTag == CMDTAG_ALTER_TABLE) || 
             (qc->commandTag == CMDTAG_ALTER_SEQUENCE) || 
             (qc->commandTag == CMDTAG_ALTER_VIEW) || 
             (qc->commandTag == CMDTAG_ALTER_INDEX) || 
             (qc->commandTag == CMDTAG_ALTER_SYSTEM) || 
             (qc->commandTag == CMDTAG_ALTER_CONSTRAINT) || 
             (qc->commandTag == CMDTAG_ALTER_TRIGGER) || 
             (qc->commandTag == CMDTAG_COMMENT) || 
             (qc->commandTag == CMDTAG_DROP_DATABASE) || 
             (qc->commandTag == CMDTAG_DROP_ROLE) || 
             (qc->commandTag == CMDTAG_DROP_SCHEMA) || 
             (qc->commandTag == CMDTAG_DROP_TABLE) || 
             (qc->commandTag == CMDTAG_DROP_DOMAIN) || 
             (qc->commandTag == CMDTAG_DROP_TYPE) || 
             (qc->commandTag == CMDTAG_DROP_VIEW) || 
             (qc->commandTag == CMDTAG_DROP_INDEX) || 
             (qc->commandTag == CMDTAG_DROP_CONSTRAINT) || 
             (qc->commandTag == CMDTAG_DROP_FUNCTION) || 
             (qc->commandTag == CMDTAG_DROP_PROCEDURE) || 
             (qc->commandTag == CMDTAG_DROP_SEQUENCE) || 
             (qc->commandTag == CMDTAG_DROP_TRIGGER) ||
             (qc->commandTag == CMDTAG_SELECT_INTO))
             { 
                 if (0 < preOrPostSQLNum) 
                 {
                     --preOrPostSQLNum;
                 }
                 else 
                 {
                     affectedRows = qc->nprocessed;
                     sendOKPacket();
                 }
             }
    else 
    {
        sendEOFPacketFlush();
    }
}

static bool 
printTup(TupleTableSlot *slot, DestReceiver *self, CommandTag commandTag)
{
    if (isExtendExeStmt) 
    {
        return printTupBinary(slot, self, commandTag);
    } 
    else 
    {
        if (systemVarType == 0)
        {
            return printTupText(slot, self, commandTag);
        }
        else
        {
            bool ret = printTupTextForSystemVars(slot, self);
            return ret;
        }
    }
}

static void 
printTupStartup(DestReceiver *self, int operation, 
                TupleDesc typeinfo, CommandTag commandTag)
{
    if (commandTag == CMDTAG_INSERT)
    {
        curLastInsertIDTimes = 0;
        return;
    }

    systemVarType = 0;

    if (!isInformationSchema)
    {
        TableInfoes *ret = NULL;
        bool found = false;
        ret = (TableInfoes *)hash_search(tableInfoeses, 
                                         &curSqlTableId, 
                                         HASH_FIND, 
                                         &found);
        if (found)
        {
            return printTupStartupForOther(ret->schemaName, 
                                           ret->tableName, 
                                           self, 
                                           operation, 
                                           typeinfo, 
                                           commandTag);
        }
        else 
        {
            char *schemaName;
            char *tableName;
            Relation relation = RelationIdGetRelation(curSqlTableId);
            if (relation != NULL)
            {
                Oid schemaId = RelationGetNamespace(relation); 
                RelationClose(relation);
                schemaName = get_namespace_name(schemaId); 
                tableName = get_rel_name(curSqlTableId);
                if (informationSchemaID != InvalidOid)
                {
                    if (schemaId != informationSchemaID) 
                    {
                        MemoryContext oldContext;
                        ret = (TableInfoes *)hash_search(tableInfoeses, 
                                                         &curSqlTableId, 
                                                         HASH_ENTER, 
                                                         &found);
                        ret->schemaId = schemaId;
                        oldContext = MemoryContextSwitchTo(TopMemoryContext);
                        ret->schemaName = pstrdup(schemaName);
                        ret->tableName = pstrdup(tableName);
                        MemoryContextSwitchTo(oldContext);
                        return printTupStartupForOther(schemaName, 
                                                       tableName,
                                                       self, 
                                                       operation, 
                                                       typeinfo, 
                                                       commandTag);
                    }
                    else
                    {
                        return printTupStartupForInformationSchema(schemaName, 
                                                                   tableName,
                                                                   self, 
                                                                   operation, 
                                                                   typeinfo, 
                                                                   commandTag);
                    }
                }
                else 
                {
                    if ((strncasecmp(schemaName, "mys_informa_schema", 18) == 0) || 
                        ((strncasecmp(schemaName, "mys_sys", 7) == 0) && (strlen(schemaName) == 7)))
                    {
                        informationSchemaID = schemaId;
                        return printTupStartupForInformationSchema(schemaName, 
                                                                   tableName,
                                                                   self, 
                                                                   operation, 
                                                                   typeinfo, 
                                                                   commandTag);
                    }
                    else 
                    {
                        MemoryContext oldContext;
                        ret = (TableInfoes *)hash_search(tableInfoeses, 
                                                         &curSqlTableId, 
                                                         HASH_ENTER, 
                                                         &found);
                        ret->schemaId = schemaId;
                        oldContext = MemoryContextSwitchTo(TopMemoryContext);
                        ret->schemaName = pstrdup(schemaName);
                        ret->tableName = pstrdup(tableName);
                        MemoryContextSwitchTo(oldContext);
                        return printTupStartupForOther(schemaName, 
                                                       tableName,
                                                       self, 
                                                       operation, 
                                                       typeinfo, 
                                                       commandTag);
                    }
                }
            }
            else 
            {
                schemaName = "";
                tableName = "";
                return printTupStartupForOther(schemaName, 
                                               tableName, 
                                               self, 
                                               operation, 
                                               typeinfo, 
                                               commandTag);
            }
        }
    }
    else 
    {
        char *schemaName;
        char *tableName;
        schemaName = "";
        tableName = "";
        return printTupStartupForInformationSchema(schemaName, 
                                                   tableName,
                                                   self, 
                                                   operation, 
                                                   typeinfo, 
                                                   commandTag);
    }
}

static void 
printTupShutdown(DestReceiver *self)
{
    DR_printtup *myState = (DR_printtup *) self;

    if (myState->myinfo)
        pfree(myState->myinfo);
    myState->myinfo = NULL;

    myState->attrinfo = NULL;

    if (myState->buf.data)
        pfree(myState->buf.data);
    myState->buf.data = NULL;

    if (myState->tmpcontext)
        MemoryContextDelete(myState->tmpcontext);

    myState->tmpcontext = NULL;
}

static void 
printTupDestroy(DestReceiver *self)
{
    pfree(self);
}

static int 
processCommand(int *firstChar, StringInfo inBuf)
{
    char *stmt = NULL;
    bool rectifyRet = false;
    char *secondWord;

    moreResultsFlag = 0;
    warnings = 0;
    preOrPostSQLNum = 0;
    lastInsertIDForPacket = 0;
    skipUtf8Verify = false;
    isExtendPreStmt = false;
    isExtendExeStmt = false;
    isExtendCloseStmt = false;
    isResetConnStmt = false;

    if (*firstChar == MYS_REQ_EXECUTE)
    {
        isExtendExeStmt = true;
        if (rewriteExtendExeStmt(inBuf))
        {
            addAdditionalSQL(inBuf);
            *firstChar = HALO_REQ_QUERY;
            warnErrCode = 0;
            return 0;
        }
        else
        {
            sendSyntaxError(stmt);
            warnErrCode = 0;
            return 1;
        }
    }
    else if (*firstChar == MYS_REQ_QUERY)
    {
        int origLen = inBuf->len - 1;
        rectifyRet = rectifyCommand(inBuf);
        if (!rectifyRet) {
            stmt = inBuf->data + inBuf->offset;
            sendSyntaxError(stmt);
            warnErrCode = 0;
            return 1;
        }

        stmt = inBuf->data + inBuf->offset;
        while (true)
        {
            if ((stmt[0] != ' ') && 
                (stmt[0] != '\t') && 
                (stmt[0] != '\n') &&
                (stmt[0] != '\r') &&
                (stmt[0] != '\0'))
            {
                break;
            }
            else 
            {
                ++inBuf->offset;
                --inBuf->len;
                ++stmt;
            }
        }

        if (inBuf->len <= 0)
        {
            if (4 < origLen)
            {
                sendOKPacket();
                warnErrCode = 0;
            }
            else 
            {
                int errCode = 1065;
                char *sqlState = "42000";
                char *errMsg = "Query was empty";
                sendErrPacket(errCode, errMsg, sqlState);
            }
            return 1;
        }

        if ((0 == strncasecmp(stmt, "select", 6)) || 
            (0 == strncasecmp(stmt, "insert", 6)) || 
            (0 == strncasecmp(stmt, "replace", 7)) || 
            (0 == strncasecmp(stmt, "update", 6)) || 
            (0 == strncasecmp(stmt, "delete", 6)))
        {
            warnErrCode = 0;
            addAdditionalSQL(inBuf);
            *firstChar = HALO_REQ_QUERY;
            return 0;
        }   
        else if (0 == strncasecmp(stmt, "begin", 5))
        {
            if (trxNotCommit)
            {
                if (needAddCommitOrRollbackAfterCurSQL == 1)
                {
                    inBuf->offset -= 7;
                    stmt = inBuf->data + inBuf->offset;
                    memcpy(stmt, "commit;", 7);
                    inBuf->len += 7;
                    addCommitOrRollBackBeforeCurSQL = true;
                }
                else 
                {
                    inBuf->offset -= 9;
                    stmt = inBuf->data + inBuf->offset;
                    memcpy(stmt, "rollback;", 9);
                    inBuf->len += 9;
                    addCommitOrRollBackBeforeCurSQL = true;
                }
            }

            warnErrCode = 0;
            *firstChar = HALO_REQ_QUERY;
            return 0;
        }
        else if ((0 == strncasecmp(stmt, "commit", 6)) || 
                 (0 == strncasecmp(stmt, "rollback", 8)))
        {
            warnErrCode = 0;
            *firstChar = HALO_REQ_QUERY;
            return 0;
        }
        else if (0 == strncasecmp(stmt, "set", 3))
        {
            warnErrCode = 0;
            *firstChar = HALO_REQ_QUERY;
            return 0;
        }   
        else if (0 == strncasecmp(stmt, "use", 3))
        {
            if ((inBuf->len - 5) <= 1024)
            {
                setUserSystemVar = 2;
                memcpy(systemVarVal, (stmt + 4), (inBuf->len - 5));
                systemVarVal[inBuf->len - 5] = '\0';

                *firstChar = HALO_REQ_QUERY;
                return 0;
            }
            else 
            {
                sendErrPacket(1049, "Invalid client data", "42000");
                return 1;
            }
        }
        else if (0 == strncasecmp(stmt, "show", 4))
        {
            secondWord = stmt + 5;
            skipSpaces(&secondWord);

            if (0 == strncasecmp(secondWord, "grants", 6))
            {
                simulateShowGrants();
                addAdditionalSQL(inBuf);
                warnErrCode = 0;
                return 1;
            }
            else if ((0 == strncasecmp(secondWord, "warnings", 8)) || 
                     (0 == strncasecmp(secondWord, "errors", 6)))
            {
                simulateShowWarnErrReturn();
                return 1;
            }
            else 
            {
                addAdditionalSQL(inBuf);

                warnErrCode = 0;
                *firstChar = HALO_REQ_QUERY;
                return 0;
            }
        }
        else if (0 == strncasecmp(stmt, "alter", 5))
        {
            secondWord = stmt + 6;
            skipSpaces(&secondWord);

            if (strncasecmp(secondWord, "database", 8) != 0)
            {
                warnErrCode = 0;
                addAdditionalSQL(inBuf);
                *firstChar = HALO_REQ_QUERY;
                return 0;
            }
            else
            {
                sendErrPacket(1049, "\"alter database\" is forbidden in Halo-MySQL", "42000");
                return 1;
            }
        }
        else if (0 == strncasecmp(stmt, ";", 1))
        {
            for (int l = 0; l < inBuf->len; l++)
            {
                if ((stmt[l] != ';') && 
                    (stmt[l] != ' ') && 
                    (stmt[l] != '\n') && 
                    (stmt[l] != '\0'))
                {
                    int errCode = 1064;
                    char *sqlState = "42000";
                    char *errMsg = "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near ';select 123 as abc' at line 1";
                    sendErrPacket(errCode, errMsg, sqlState);
                    return 1;
                }
            }
            if ((inBuf->len + 4) <= origLen)
            {
                sendOKPacket();
                warnErrCode = 0;
                return 1;
            }
            else 
            {
                int errCode = 1065;
                char *sqlState = "42000";
                char *errMsg = "Query was empty";
                sendErrPacket(errCode, errMsg, sqlState);
                return 1;
            }
        }
        else if ((strncasecmp(stmt, "checksum", 8) == 0))
        {
            if (simulateChecksumTable(stmt))
            {
                warnErrCode = 0;
            }
            else 
            {
                sendSyntaxError(stmt);
            }
            return 1;
        }
        else if ((strncasecmp(stmt, "analyze", 7) == 0) || 
                 (strncasecmp(stmt, "check", 5) == 0) || 
                 (strncasecmp(stmt, "optimize", 8) == 0) || 
                 (strncasecmp(stmt, "repair", 6) == 0))
        {
            if (simulateAnalyzeCheckOptimizeRepairTable(stmt))
            {
                warnErrCode = 0;
            }
            else 
            {
                sendSyntaxError(stmt);
            }
            return 1;
        }
        else if (strncasecmp(stmt, "flush", 5) == 0)
        {
            sendOKPacket();
            warnErrCode = 0;
            return 1;
        }
        else if (strncasecmp(stmt, "reset", 5) == 0)
        {
            sendOKPacket();
            warnErrCode = 0;
            return 1;
        }
        else 
        {
            warnErrCode = 0;
            addAdditionalSQL(inBuf);
            *firstChar = HALO_REQ_QUERY;
            return 0;
        }
    }
    else if (*firstChar == MYS_REQ_PREPARE)
    {
        int rewriteRet;

        rectifyRet = rectifyCommand(inBuf);

        if (!rectifyRet) {
            stmt = inBuf->data + inBuf->offset;
            sendSyntaxError(stmt);
            return 1;
        }

        stmt = inBuf->data + inBuf->offset;

        rewriteRet = rewriteExtendPreStmt(inBuf);
        if (rewriteRet == 1)
        {
            isExtendPreStmt = true;
            warnErrCode = 0;
            addAdditionalSQL(inBuf);
            *firstChar = HALO_REQ_QUERY;
            return 0;
        }
        else if (rewriteRet == 2)
        {
            warnErrCode = 0;
            endExtendPreStmt();
            return 1;
        }
        else if (rewriteRet == 3)
        {
            sendErrPacket(1295, "This command is not supported in the prepared statement protocol yet", "HY000");
            return 1;
        }
        else 
        {
            sendSyntaxError(stmt);
            return 1;
        }
    }
    else if (*firstChar == MYS_REQ_CLOSE)
    {
        if (rewriteExtendCloseStmt(inBuf))
        {
            isExtendCloseStmt = true;
            warnErrCode = 0;
            addAdditionalSQL(inBuf);
            *firstChar = HALO_REQ_QUERY;
            return 0;
        }
        else 
        {
            sendSyntaxError(stmt);
            return 1;
        }
    }
    else if (*firstChar == MYS_REQ_RESET)
    {
        sendOKPacket();
        warnErrCode = 0;
        return 1;
    }
    else if (*firstChar == MYS_REQ_FIELD_LIST)
    {
        simulateShowFieldsReturn();
        warnErrCode = 0;
        return 1;
    }
    else if (*firstChar == MYS_REQ_USE_DATABASE)
    {
        if ((inBuf->len - 1) <= 1024)
        {
            setUserSystemVar = 2;
            stmt = inBuf->data + inBuf->offset;
            memcpy(systemVarVal, stmt, (inBuf->len - 1));
            systemVarVal[inBuf->len - 1] = '\0';

            inBuf->offset -= 5;
            stmt = inBuf->data + inBuf->offset;
            memcpy(stmt, "use `", 5);
            inBuf->len += 5;
            stmt[inBuf->len - 1] = '`';
            stmt[inBuf->len] = '\0';
            inBuf->len += 1;

            addAdditionalSQL(inBuf);
            *firstChar = HALO_REQ_QUERY;
            return 0;
        }
        else 
        {
            sendErrPacket(1049, "Invalid client data", "42000");
            return 1;
        }
    }
    else if (*firstChar == MYS_REQ_SLEEP)
    {
        sendOKPacket();
        warnErrCode = 0;
        return 1;
    }
    else if (*firstChar == MYS_REQ_QUIT)
    {
        *firstChar = HALO_REQ_QUIT;
        return 0;
    }
    else if (*firstChar == MYS_REQ_PING)
    {
        sendOKPacket();
        warnErrCode = 0;
        return 1;
    }
    else if (*firstChar == MYS_REQ_RESET_CONN)
    {
        resetConnection(inBuf);
        *firstChar = HALO_REQ_QUERY;
        warnErrCode = 0;
        return 0;
    }
    else 
    {
        stmt = inBuf->data + inBuf->offset;
        sendUnsupportError(stmt);
        return 1;
    }
}

static void
reportParamStatus(const char *name, char *val)
{
    return;
}

static int 
haloAToI(const char *str, int strLen)
{
    int result = 0;
    int signal = 1;

    if ('-' == *str) 
    {   
        signal = -1; 
        str++;
        --strLen;
    }   
    else if ('+' == *str)
    {   
        str++;
        --strLen; 
    }   

    while (0 < strLen)
    {   
        if (('0' <= *str) && (*str <= '9')) 
        {   
            result = result * 10 + (*str++ - '0');
            --strLen;
        }   
        else 
        {   
            break;
        }   
    }   

    return signal * result;
}

static int                                                                                                                                                                        
assembleColDefPacket41Payload(const char *schemaName, 
                              const char *tableName, 
                              const char *orgTableName, 
                              const char *columnName, 
                              const char *orgColumnName, 
                              int characterSet, 
                              int columnLen, 
                              int columnType, 
                              int columnFlags, 
                              int decimals,
                              char *payloadBuf)
{
    int payloadBufIndex = 0;

    const char *catalog = "def";
    int fixed_len_fields_len = 0x0c;
    int filler = 0x00;

    int catalog_len = 0;
    int schemaNameLen = 0;
    int tableNameLen = 0;
    int orgTableName_len = 0;
    int columnNameLen = 0;
    int orgColumnName_len = 0;
    int fixed_len_fields_len_len = 0;
    const int characterSetLen = 2;
    const int columnLenLen = 4;
    const int columnTypeLen = 1;
    const int columnFlagsLen = 2;
    const int decimals_len = 1;
    const int filler_len = 2;

    catalog_len = assembleLenencString(catalog, (payloadBuf + payloadBufIndex));
    payloadBufIndex += catalog_len;

    schemaNameLen = assembleLenencString(schemaName, (payloadBuf + payloadBufIndex));
    payloadBufIndex += schemaNameLen;

    tableNameLen = assembleLenencString(tableName, (payloadBuf + payloadBufIndex));
    payloadBufIndex += tableNameLen;

    orgTableName_len = assembleLenencString(orgTableName, (payloadBuf + payloadBufIndex));
    payloadBufIndex += orgTableName_len;

    columnNameLen = assembleLenencString(columnName, (payloadBuf + payloadBufIndex));
    payloadBufIndex += columnNameLen;

    orgColumnName_len = assembleLenencString(orgColumnName, (payloadBuf + payloadBufIndex));
    payloadBufIndex += orgColumnName_len;

    fixed_len_fields_len_len = assembleLenencUnsignedLong(fixed_len_fields_len, (payloadBuf + payloadBufIndex));
    payloadBufIndex += fixed_len_fields_len_len;

    memcpy((payloadBuf + payloadBufIndex), &characterSet, characterSetLen);
    payloadBufIndex += characterSetLen;

    memcpy((payloadBuf + payloadBufIndex), &columnLen, columnLenLen);
    payloadBufIndex += columnLenLen;

    memcpy((payloadBuf + payloadBufIndex), &columnType, columnTypeLen);
    payloadBufIndex += columnTypeLen;

    memcpy((payloadBuf + payloadBufIndex), &columnFlags, columnFlagsLen);
    payloadBufIndex += columnFlagsLen;

    if (columnType == 0)
    {
        decimals = 0x51;
    }
    else if ((columnType == 4) || (columnType == 5) || (columnType == 253))
    {
        decimals = 31;
    }
    else {
        decimals = 0x00;
    }

    memcpy((payloadBuf + payloadBufIndex), &decimals, decimals_len);
    payloadBufIndex += decimals_len;

    memcpy((payloadBuf + payloadBufIndex), &filler, filler_len);
    payloadBufIndex += filler_len;

    return payloadBufIndex;
}

static int                                 
assembleColDefPacket320Payload(const char *tableName, 
                               const char *columnName, 
                               int columnType, 
                               int columnFlags, 
                               char *payloadBuf)
{
    int payloadBufIndex = 0;

    int tableNameLen = 0;
    int columnNameLen = 0;

    tableNameLen = assembleLenencString(tableName, (payloadBuf + payloadBufIndex));
    payloadBufIndex += tableNameLen;

    columnNameLen = assembleLenencString(columnName, (payloadBuf + payloadBufIndex));
    payloadBufIndex += columnNameLen;

    return payloadBufIndex;
}

static int
assembleLenencUnsignedLong(unsigned long val, char *buf) 
{
    if (val < 251)
    {
        memcpy(buf, &val, 1);
        return 1;
    }
    else if (val < 0xffff)
    {
        int prefix = 0xFC;
        memcpy(buf, &prefix, 1);
        memcpy((buf + 1), &val, 2);
        return 3;
    }
    else if (val < 0xffffff)
    {
        int prefix = 0xFD;
        memcpy(buf, &prefix, 1);
        memcpy((buf + 1), &val, 3);
        return 4;
    }
    else 
    {
        int prefix = 0xFE;
        memcpy(buf, &prefix, 1);
        memcpy((buf + 1), &val, 8);
        return 9;
    }
}

static int
assembleLenencString(const char *str, char *buf) 
{
    int strLen = 0;
    int strLen_len = 0;

    if (str != NULL)
    {
        strLen = (int)strlen(str);
        if (0 < strLen)
        {
            strLen_len = assembleLenencUnsignedLong(strLen, buf);
            memcpy((buf + strLen_len), str, strLen);
            return (strLen_len + strLen);
        }
        else 
        {
            buf[0] = 0;
            return 1;
        }
    }
    else 
    {
        strLen = 0;
        strLen_len = assembleLenencUnsignedLong(strLen, buf);
        return (strLen_len + strLen);
    }
}

static int
assembleLenencString2(const char *str, int strLen, char *buf) 
{
    int strLenLen = 0;

    if (0 < strLen)
    {
        strLenLen = assembleLenencUnsignedLong(strLen, buf);
        memcpy((buf + strLenLen), str, strLen);
        return (strLenLen + strLen);
    }
    else 
    {
        buf[0] = 0;
        return 1;
    }
}

static uint64
processLenencString(const char *buf, int *lengthEncodeSize)
{
    int prefix = (unsigned char)buf[0];
    int string_length_size = 0;
    uint64 string_length = 0;
    bool have_prefix = false;

    if (prefix < 251)
    {
        have_prefix = false;
        string_length_size = 1;
        string_length = prefix;
    }
    else if (prefix == 252)
    {
        have_prefix = true;
        string_length_size = 2;
        memcpy((void *)&string_length, (const void *)(buf+1), 2);
    }
    else if (prefix == 253) 
    {
        have_prefix = true;
        string_length_size = 3;
        memcpy((void *)&string_length, (const void *)(buf+1), 3);
    }
    else 
    {
        have_prefix = true;
        string_length_size = 8;
        memcpy((void *)&string_length, (const void *)(buf+1), 8);
    }

    *lengthEncodeSize = string_length_size + (have_prefix ? 1 : 0);

    return string_length;
}

static int
processDateType(const unsigned char *buf, char *outBuf, int outBufLen)
{
    uint date_len = buf[0];
    uint year = 0;

    memcpy(&year, buf+1, 2);
    memset(outBuf, 0, outBufLen);
    sprintf(outBuf, "%u-%u-%u", year, (uint)buf[3], (uint)buf[4]);

    return date_len + 1;
}

static int
processTimeType(const unsigned char *buf, char *outBuf, int outBufLen)
{
    int time_len = buf[0];
    uint negative = 0;
    uint day = 0;
    uint hour = 0;
    uint minute = 0;
    uint second = 0;
    uint second_part = 0;
    uint offset = 0;

    Assert(time_len >= 8);
    negative = buf[1];
    memcpy(&day, buf+2, 4);
    hour = (uint)buf[6] + day * 24;
    minute = (uint)buf[7];
    second = (uint)buf[8];

    if(time_len > 8) memcpy(&second_part, buf+9, 4);
    else second_part = 0;

    memset(outBuf, 0, outBufLen);
    if(negative)
    {
        offset = 1;
        outBuf[0] = '-';
    }
    if(time_len > 8)
    {
        sprintf(outBuf+offset, "%u:%u:%u.%06u", hour, minute, second, second_part);
    }
    else
    {
        sprintf(outBuf+offset, "%u:%u:%u", hour, minute, second);
    }

    return time_len + 1;
}

int
processDatetimeTimestampType(const unsigned char *buf, 
                             char *outBuf, 
                             int outBufLen)
{
    uint datetime_timestamp_len = buf[0];
    uint year = 0;
    uint second_part = 0;
    uint offset = 0;

    memcpy(&year, buf+1, 2);
    memset(outBuf, 0, outBufLen);

    if(datetime_timestamp_len >= 4)
    {
        sprintf(outBuf + offset, "%u-%u-%u", year, (uint)buf[3], (uint)buf[4]);
    }
    offset = strlen(outBuf);
    if(datetime_timestamp_len >= 7)
    {
        sprintf(outBuf + offset, " %u:%u:%u", (uint)buf[5], (uint)buf[6], (uint)buf[7]);
    }
    offset = strlen(outBuf);
    if(datetime_timestamp_len >= 11)
    {
        memcpy(&second_part, buf+8, 4);
        sprintf(outBuf + offset, ".%06u", second_part);
    }

    return datetime_timestamp_len + 1;
}


static int
convertTimestamp(char *value)
{
    int len = 0;
    int items[7] = {0};
    int valueLen = strlen(value);
    int i = 0;
    int j = 0;
    int k = 0;

    for (i = 0; i <= valueLen; i++)
    {
        if ((value[i] == '-') || (value[i] == ' ') || 
            (value[i] == ':') || (value[i] == '.') || 
            (value[i] == '\0'))
        {
            items[k++] = haloAToI((value + j), (i - j));
            j = i + 1;
        }
    }

    if (items[6] == 0)
    {
        if (0 < (items[3] + items[4] + items[5]))
        {
            len = 7;
            memcpy(value, &len, 1);
            memcpy((value + 1), &items[0], 2);
            memcpy((value + 3), &items[1], 1);
            memcpy((value + 4), &items[2], 1);
            memcpy((value + 5), &items[3], 1);
            memcpy((value + 6), &items[4], 1);
            memcpy((value + 7), &items[5], 1);
        }
        else
        {
            if (0 < (items[0] + items[1] + items[2]))
            {
                len = 4;
                memcpy(value, &len, 1);
                memcpy((value + 1), &items[0], 2);
                memcpy((value + 3), &items[1], 1);
                memcpy((value + 4), &items[2], 1);
            }
            else
            {
                len = 0;
                memcpy(value, &len, 1);
            }
        }
    }
    else 
    {
        len = 11;
        memcpy(value, &len, 1);
        memcpy((value + 1), &items[0], 2);
        memcpy((value + 3), &items[1], 1);
        memcpy((value + 4), &items[2], 1);
        memcpy((value + 5), &items[3], 1);
        memcpy((value + 6), &items[4], 1);
        memcpy((value + 7), &items[5], 1);
        memcpy((value + 8), &items[6], 4);
    }

    return len + 1;
}


static int
convertTime(char *value)
{
    int len = 0;
    int items[5] = {0};
    int valueLen = strlen(value);
    int isNegative = 0;
    int i = 0;
    int j = 0;
    int k = 1;

    for (i = 0; i <= valueLen; i++)
    {
        if ((value[i] == ':') || (value[i] == '.') || (value[i] == '\0'))
        {
            items[k++] = haloAToI((value + j), (i - j));
            j = i + 1;
        }
    }

    if (items[4] == 0)
    {
        if (0 < (items[0] + items[1] + items[2] + items[3]))
        {
            len = 8;
            memcpy(value, &len, 1);
            memcpy((value + 1), &isNegative, 1);
            memcpy((value + 2), &items[0], 4);
            memcpy((value + 6), &items[1], 1);
            memcpy((value + 7), &items[2], 1);
            memcpy((value + 8), &items[3], 1);
        }
        else 
        {
            len = 0;
            memcpy(value, &len, 1);
        }
    }
    else
    {
        len = 12;
        memcpy(value, &len, 1);
        memcpy((value + 1), &isNegative, 1);
        memcpy((value + 2), &items[0], 4);
        memcpy((value + 6), &items[1], 1);
        memcpy((value + 7), &items[2], 1);
        memcpy((value + 8), &items[3], 1);
        memcpy((value + 9), &items[4], 4);
    }

    return len + 1;
}


static void 
assembleOKEOFCommonPacketPayload(int statusFlags, 
                                 int warnings, 
                                 char *payloadBuf, 
                                 int *payloadBufOffset) 
{
    const int statusFlags_len = 2;
    const int warnings_len = 2;

    int assembledLen = 0;

    if (0 <= affectedRows)
    {
        assembledLen = assembleLenencUnsignedLong(affectedRows, (payloadBuf + *payloadBufOffset));
    }
    else 
    {
        assembledLen = assembleLenencUnsignedLong(0, (payloadBuf + *payloadBufOffset));
    }
    *payloadBufOffset += assembledLen;

    assembledLen = assembleLenencUnsignedLong(lastInsertIDForPacket, (payloadBuf + *payloadBufOffset));
    *payloadBufOffset += assembledLen;

    if (frontEndCaps & MYS_CLT_PROTOCOL_41)
    {
        memcpy((payloadBuf + *payloadBufOffset), &statusFlags, statusFlags_len);
        *payloadBufOffset += statusFlags_len;

        memcpy((payloadBuf + *payloadBufOffset), &warnings, warnings_len);
        *payloadBufOffset += warnings_len;
    }
    else if (frontEndCaps & MYS_CLT_TRANSACTIONS)
    {
        memcpy((payloadBuf + *payloadBufOffset), &statusFlags, statusFlags_len);
        *payloadBufOffset += statusFlags_len;
    }

    if (frontEndCaps & MYS_CLT_SESSION_TRACK) 
    {
        char sessionStateInfo[1024];
        int sessionStateInfoOffset;
        char dataOfTheChangedSessionInfo[1024];
        int dataOfTheChangedSessionInfoOffset;

        if (sessionStateChanged || (sessionTrackInfo[0] != '\0'))
        {
            assembledLen = assembleLenencString(sessionTrackInfo, 
                                                (payloadBuf + *payloadBufOffset));
            *payloadBufOffset += assembledLen;
        }

        memset(sessionStateInfo, '\0', 1024);
        sessionStateInfoOffset = 0;
        memset(dataOfTheChangedSessionInfo, '\0', 1024);
        dataOfTheChangedSessionInfoOffset = 0;

        if (sessionStateChanged)
        {
            if ((sessionStateType == 0) && inSsTrackSystemVariables)
            {
                assembledLen = assembleLenencUnsignedLong(sessionStateType, 
                                                          (sessionStateInfo + sessionStateInfoOffset));
                sessionStateInfoOffset += assembledLen;
                assembledLen = assembleLenencString(systemVarName, 
                                                    (dataOfTheChangedSessionInfo + dataOfTheChangedSessionInfoOffset));
                dataOfTheChangedSessionInfoOffset += assembledLen;
                if (systemVarVal[0] != '\0')
                {
                    assembledLen = assembleLenencString(systemVarVal, 
                                                        (dataOfTheChangedSessionInfo + dataOfTheChangedSessionInfoOffset));
                }
                else 
                {
                    assembledLen = assembleLenencString(NULL, 
                                                        (dataOfTheChangedSessionInfo + dataOfTheChangedSessionInfoOffset));
                }
                dataOfTheChangedSessionInfoOffset += assembledLen;
            }

            if ((sessionStateType == 1) && isSsTrackSchemaOn)
            {
                assembledLen = assembleLenencUnsignedLong(sessionStateType, 
                                                          (sessionStateInfo + sessionStateInfoOffset));
                sessionStateInfoOffset += assembledLen;
                if (systemVarVal[0] != '\0')
                {
                    assembledLen = assembleLenencString(systemVarVal, 
                                                        (dataOfTheChangedSessionInfo + dataOfTheChangedSessionInfoOffset));
                }
                else 
                {
                    assembledLen = assembleLenencString(NULL, 
                                                        (dataOfTheChangedSessionInfo + dataOfTheChangedSessionInfoOffset));
                }
                dataOfTheChangedSessionInfoOffset += assembledLen;
            }
        }

        if (isSsTrackStateChangeOn)
        {
            if (0 < dataOfTheChangedSessionInfoOffset)
            {
                assembledLen = assembleLenencUnsignedLong(2, 
                                                          (dataOfTheChangedSessionInfo + dataOfTheChangedSessionInfoOffset));
                dataOfTheChangedSessionInfoOffset += assembledLen;
                assembledLen = assembleLenencString("1", 
                                                    (dataOfTheChangedSessionInfo + dataOfTheChangedSessionInfoOffset));
                dataOfTheChangedSessionInfoOffset += assembledLen;
            }
            else 
            {
                assembledLen = assembleLenencUnsignedLong(2, 
                                                          (sessionStateInfo + sessionStateInfoOffset));
                sessionStateInfoOffset += assembledLen;
                assembledLen = assembleLenencString("1", 
                                                    (sessionStateInfo + sessionStateInfoOffset));
                sessionStateInfoOffset += assembledLen;
            }
        }

        if (0 < dataOfTheChangedSessionInfoOffset)
        {
            assembledLen = assembleLenencString2(dataOfTheChangedSessionInfo, 
                                                 dataOfTheChangedSessionInfoOffset,
                                                (sessionStateInfo + sessionStateInfoOffset));
            sessionStateInfoOffset += assembledLen;
        }

        if (0 < sessionStateInfoOffset)
        {
            assembledLen = assembleLenencString2(sessionStateInfo, 
                                                 sessionStateInfoOffset, 
                                                 (payloadBuf + *payloadBufOffset));
            *payloadBufOffset += assembledLen;
        }
    }
    else 
    {
        if (sessionTrackInfo[0] != '\0')
        {
            int sessionTrackInfoLen = (int)strlen(sessionTrackInfo);
            memcpy((payloadBuf + *payloadBufOffset), sessionTrackInfo, sessionTrackInfoLen);
            *payloadBufOffset += sessionTrackInfoLen;
        }
    }
}

static int 
assembleOKPacketPayload(int statusFlags, 
                        int warnings, 
                        char *payloadBuf) 
{
    int payloadBufIndex = 0;
    const int headerLen = 1;
    int header = 0x00;

    memcpy((payloadBuf + payloadBufIndex), &header, headerLen);
    payloadBufIndex += headerLen;

    assembleOKEOFCommonPacketPayload(statusFlags, 
                                     warnings, 
                                     payloadBuf, 
                                     &payloadBufIndex);

    sessionTrackInfo[0] = '\0';
    systemVarName[0] = '\0';
    systemVarVal[0] = '\0';

    return payloadBufIndex;
}


static int 
assembleErrorPacketPayload(int errCode, 
                           char sqlStateMarker, 
                           const char *sqlState, 
                           const char *errMsg, 
                           char *payloadBuf)
{
    int payloadBufIndex = 0;

    const int headerLen = 1;
    const int errCodeLen = 2;
    const int sqlStateMarkerLen = 1;
    const int sqlStateLen = 5;
    int errMsgLen = (int)strlen(errMsg);

    int header = 0xFF;

    sqlStateMarker = '#';

    memcpy((payloadBuf + payloadBufIndex), &header, headerLen);
    payloadBufIndex += headerLen;

    memcpy((payloadBuf + payloadBufIndex), &errCode, errCodeLen);
    payloadBufIndex += errCodeLen;

    if (frontEndCaps & MYS_CLT_PROTOCOL_41)
    {
        payloadBuf[payloadBufIndex] = sqlStateMarker;
        payloadBufIndex += sqlStateMarkerLen;

        memcpy((payloadBuf + payloadBufIndex), sqlState, sqlStateLen);
        payloadBufIndex += sqlStateLen;
    }

    if (1024 < errMsgLen)
    {
        errMsgLen = 1024;
    }

    memcpy((payloadBuf + payloadBufIndex), errMsg, errMsgLen);
    payloadBufIndex += errMsgLen;

    return payloadBufIndex;
}

static int                                                                     
assembleColCountPacketPayload(int metadataFollowFlag, 
                              int columnCount, 
                              char *payloadBuf)
{
    int payloadBufIndex = 0;

    const int metadataFollowFlag_len = 1;
    int columnCount_len = 0;

    const int RESULTSET_METADATA_FULL = 1;

    if (frontEndCaps & MYS_CLT_OPTIONAL_RESULTSET_METADATA)
    {
        if (metadataFollowFlag == RESULTSET_METADATA_FULL)
        {
            memcpy((payloadBuf + payloadBufIndex), &metadataFollowFlag, metadataFollowFlag_len);
            payloadBufIndex += metadataFollowFlag_len;
        }
    }

    columnCount_len = assembleLenencUnsignedLong(columnCount, (payloadBuf + payloadBufIndex));
    payloadBufIndex += columnCount_len;

    return payloadBufIndex;
}

static int                                                                                                                                                                        
assembleColDefPacketPayload(const char *schemaName, 
                            const char *tableName, 
                            const char *orgTableName, 
                            const char *columnName, 
                            const char *orgColumnName, 
                            int characterSet, 
                            int columnLen, 
                            int columnType, 
                            int columnFlags, 
                            int decimals,
                            char *payloadBuf)
{
    if (frontEndCaps & MYS_CLT_PROTOCOL_41)
    {
        return assembleColDefPacket41Payload(schemaName, 
                                             tableName, 
                                             orgTableName, 
                                             columnName, 
                                             orgColumnName, 
                                             characterSet, 
                                             columnLen, 
                                             columnType, 
                                             columnFlags, 
                                             decimals,
                                             payloadBuf);
    }
   else 
   {
       return assembleColDefPacket320Payload(tableName, 
                                             columnName, 
                                             columnType, 
                                             columnFlags, 
                                             payloadBuf);
   }
}


static int 
assembleEOFPacketPayload(int warnings, int statusFlags, char *payloadBuf)
{
    int payloadBufIndex = 0;

    const int headerLen = 1;
    const int statusFlagsLen = 2;
    const int warningsLen = 2;

    int header = 0xFE;

    memcpy((payloadBuf + payloadBufIndex), &header, headerLen);
    payloadBufIndex += headerLen;

    if (frontEndCaps & MYS_CLT_DEPRECATE_EOF)
    {
        assembleOKEOFCommonPacketPayload(statusFlags, 
                                         warnings, 
                                         payloadBuf, 
                                         &payloadBufIndex);
    }
    else 
    {
        if (frontEndCaps & MYS_CLT_PROTOCOL_41)
        {
            memcpy((payloadBuf + payloadBufIndex), &warnings, warningsLen);
            payloadBufIndex += warningsLen;

            memcpy((payloadBuf + payloadBufIndex), &statusFlags, statusFlagsLen);
            payloadBufIndex += statusFlagsLen;
        }
    }

    return payloadBufIndex;
}

static void 
printTupStartupForOther(char *schemaName, char *tableName, DestReceiver *self, 
                        int operation, TupleDesc typeinfo, CommandTag commandTag)
{
    char *payloadBuf;
    int payloadLen = 0;

    int metadataFollowFlag = 0;
    const int RESULTSET_METADATA_FULL = 1;
    int columnCount = typeinfo->natts;
    char *orgTableName = NULL;
    Form_pg_attribute att;
    char *columnName = NULL;
    char *orgColumnName = NULL;
    int characterSet = 33; 
    int columnLen = 0;
    int columnType = 0;
    int columnFlags = 1;   
    int decimals = 0x00;
    int i = 0;

    orgTableName = tableName;

    netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);
    payloadLen = assembleColCountPacketPayload(metadataFollowFlag, 
                                               columnCount, 
                                               payloadBuf);
    netTransceiver->writePacketHeaderNoFlush(payloadLen);
    netTransceiver->finishWriteToBufNoFlush(payloadLen);
    
    if ((!(frontEndCaps & MYS_CLT_OPTIONAL_RESULTSET_METADATA)) || 
        (metadataFollowFlag == RESULTSET_METADATA_FULL))
    {
        for (i = 0; i < columnCount; i++)
        {
            att = TupleDescAttr(typeinfo, i);
            columnName = att->attname.data;
            recoverColumnName(schemaName, tableName, columnName);
            orgColumnName = columnName;
            if (0 < att->attlen)
            {
                columnLen = att->attlen;
            }
            else 
            {
                columnLen = att->atttypmod;
            }
            columnType = getMySQLDataType(att->atttypid);
            if (columnType == MYS_TYPE_TINY)
            {
                const char *typeName = getTypeName(att->atttypid);
                if (strncmp(typeName, "tinyint1", 8) == 0)
                {
                    columnLen = 1;
                }
            }
            if ((columnType == MYS_TYPE_TINY) || 
                (columnType == MYS_TYPE_SHORT) || 
                (columnType == MYS_TYPE_LONG) || 
                (columnType == MYS_TYPE_INT24) || 
                (columnType == MYS_TYPE_LONGLONG) || 
                (columnType == MYS_TYPE_NEWDECIMAL) || 
                (columnType == MYS_TYPE_FLOAT) ||
                (columnType == MYS_TYPE_DOUBLE))
            {
                characterSet = 63;
            }
            else if (columnType == MYS_TYPE_STRING)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "binary", 6) != 0)
                {
                    characterSet = 33;
                }
                else 
                {
                    characterSet = 63;
                }
            }
            else if (columnType == MYS_TYPE_VAR_STRING)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "varbinary", 9) != 0)
                {
                    characterSet = 33;
                }
                else 
                {
                    characterSet = 63;
                }
            }
            else if (columnType == MYS_TYPE_BIT)
            {
                characterSet = 63;
            }
            else if (columnType == MYS_TYPE_BLOB)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "blob", 4) == 0)
                {
                    characterSet = 63;
                }
                else 
                {
                    characterSet = 33;
                }
            }
            else 
            {
                characterSet = 33;
            }
            netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);
            payloadLen = assembleColDefPacketPayload(schemaName, 
                                                     tableName, 
                                                     orgTableName, 
                                                     columnName, 
                                                     orgColumnName, 
                                                     characterSet, 
                                                     columnLen, 
                                                     columnType, 
                                                     columnFlags, 
                                                     decimals, 
                                                     payloadBuf);
            netTransceiver->writePacketHeaderNoFlush(payloadLen);
            netTransceiver->finishWriteToBufNoFlush(payloadLen);
        }
    }

    sendEOFPacketNoFlush();
}

static void 
printTupStartupForInformationSchema(char *schemaName, char *tableName, 
                                    DestReceiver *self, int operation, 
                                    TupleDesc typeinfo, CommandTag commandTag)
{
    char *payloadBuf;
    int payloadLen = 0;

    int metadataFollowFlag = 0;
    const int RESULTSET_METADATA_FULL = 1;
    int columnCount = typeinfo->natts;
    char *orgTableName = NULL;
    Form_pg_attribute att;
    char *columnName = NULL;
    char *orgColumnName = NULL;
    int characterSet = 33; 
    int columnLen = 0;
    int columnType = 0;
    int columnFlags = 1;   
    int decimals = 0x00;
    int i = 0;

    if (strncasecmp(tableName, "empty_session_variables", 23) == 0)
    {
        systemVarType = 1;
    }
    else if (strncasecmp(tableName, "empty_global_variables", 22) == 0)
    {
        systemVarType = 2;
    }
    else 
    {
        systemVarType = 0;
    }

    schemaName = "information_schema";
    orgTableName = tableName;
    isInformationSchema = false;

    netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);
    payloadLen = assembleColCountPacketPayload(metadataFollowFlag, 
                                               columnCount, 
                                               payloadBuf);
    netTransceiver->writePacketHeaderNoFlush(payloadLen);
    netTransceiver->finishWriteToBufNoFlush(payloadLen);
    
    if ((!(frontEndCaps & MYS_CLT_OPTIONAL_RESULTSET_METADATA)) || 
        (metadataFollowFlag == RESULTSET_METADATA_FULL))
    {
        for (i = 0; i < columnCount; i++)
        {
            att = TupleDescAttr(typeinfo, i);
            columnName = recoverSysColumnName(tableName, att->attname.data);
            orgColumnName = columnName;
            if (0 < att->attlen)
            {
                columnLen = att->attlen;
            }
            else 
            {
                columnLen = att->atttypmod;
            }
            columnType = getMySQLDataType(att->atttypid);
            if ((columnType == MYS_TYPE_TINY) || 
                (columnType == MYS_TYPE_SHORT) || 
                (columnType == MYS_TYPE_LONG) || 
                (columnType == MYS_TYPE_INT24) || 
                (columnType == MYS_TYPE_LONGLONG) || 
                (columnType == MYS_TYPE_NEWDECIMAL) || 
                (columnType == MYS_TYPE_FLOAT) ||
                (columnType == MYS_TYPE_DOUBLE))
            {
                characterSet = 63;
            }
            else if (columnType == MYS_TYPE_STRING)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "binary", 6) != 0)
                {
                    characterSet = 33;
                }
                else 
                {
                    characterSet = 63;
                }
            }
            else if (columnType == MYS_TYPE_VAR_STRING)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "varbinary", 9) != 0)
                {
                    characterSet = 33;
                }
                else 
                {
                    characterSet = 63;
                }
            }
            else if (columnType == MYS_TYPE_BIT)
            {
                characterSet = 63;
            }
            else if (columnType == MYS_TYPE_BLOB)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "blob", 4) == 0)
                {
                    characterSet = 63;
                }
                else 
                {
                    characterSet = 33;
                }
            }
            else 
            {
                characterSet = 33;
            }
            netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);
            payloadLen = assembleColDefPacketPayload(schemaName, 
                                                     tableName, 
                                                     orgTableName, 
                                                     columnName, 
                                                     orgColumnName, 
                                                     characterSet, 
                                                     columnLen, 
                                                     columnType, 
                                                     columnFlags, 
                                                     decimals, 
                                                     payloadBuf);
            netTransceiver->writePacketHeaderNoFlush(payloadLen);
            netTransceiver->finishWriteToBufNoFlush(payloadLen);
        }
    }

    sendEOFPacketNoFlush();
}

static void
recoverColumnName(const char *schemaName, const char *tableName, char *columnName)
{
    bool found;
    ScheCustomizedCaseColumNames *sCCCN = NULL;

    if (halo_mysql_column_name_case_control == 0)
    {
        return;
    }

    if (!initedCustomizedCaseColumNames)
    {
        initCustomizedCaseColumnNames();
        initedCustomizedCaseColumNames = true;
    }

    sCCCN = (ScheCustomizedCaseColumNames *)hash_search(schesCustomizedCaseColumNames,
                                                        schemaName,
                                                        HASH_FIND,
                                                        &found);
    if (found)
    {
        TabCustomizedCaseColumNames *tCCCN = NULL;
        tCCCN = (TabCustomizedCaseColumNames *)hash_search(sCCCN->tabsCustomizedCaseColumNames,
                                                           tableName,
                                                           HASH_FIND,
                                                           &found);
        if (found)
        {
            CustomizedCaseColumName2 *cCCCN = NULL;
            cCCCN = (CustomizedCaseColumName2 *)hash_search(tCCCN->colsCustomizedCaseColumNames,
                                                            columnName,
                                                            HASH_FIND,
                                                            &found);
            if (found)
            {
                memcpy(columnName, cCCCN->destColName, cCCCN->destColNameLen);
            }
        }
    }
}

static char *
recoverSysColumnName(char *tableName, char *columnName)
{
    if (strncasecmp(columnName, "character_set_client", 20) == 0)
    {
        return pstrdup("character_set_client");
    }
    else if (strncasecmp(columnName, "collation_connection", 20) == 0)
    {
        return pstrdup("collation_connection");
    }
    else if (strncasecmp(columnName, "Create Table", 12) == 0)
    {
        return pstrdup("Create Table");
    }
    else if (strncasecmp(columnName, "Create View", 11) == 0)
    {
        return pstrdup("Create View");
    }
    else if (strncasecmp(columnName, "Create Function", 15) == 0)
    {
        return pstrdup("Create Function");
    }
    else if (strncasecmp(columnName, "Create Procedure", 16) == 0)
    {
        return pstrdup("Create Procedure");
    }
    else if (strncasecmp(columnName, "Database Collation", 18) == 0)
    {
        return pstrdup("Database Collation");
    }
    else if (strncasecmp(columnName, "sql_mode", 8) == 0)
    {
        return pstrdup("sql_mode");
    }
    else 
    {
        if ((97 <= columnName[0]) && (columnName[0] <= 122))
        {
            columnName[0] -= 32;
        }
        return columnName;
    }
}

static bool 
printTupBinary(TupleTableSlot *slot, DestReceiver *self, CommandTag commandTag)
{
    char payloadBuf[102400];
    int payloadBufIndex = 0;
    int payloadBufSentLen = 0;
    int i = 0;

    int header = 0x00;
    const int header_len = 1;
    int nullBitmap_len = 0;

    TupleDesc typeinfo = slot->tts_tupleDescriptor;
    int columnCount = typeinfo->natts;
    Datum attr;
    bool is_null;
    Form_pg_attribute att = NULL;
    Oid typoutput;
    bool typisvarlena;
    char *columnValue = NULL;
    int columnType = 0;
    int len = 0;
    int lenencIntLen = 0;
    size_t rowRemainLen = 0;
    size_t packetFreeSize = 0;

    int tmp_int = 0;
    long long tmp_long_long = 0;
    double tmp_double = 0.0;
    float tmp_float = 0.0;

    int columnTypes[1017];
    char* columnVals[1017];
    int columnValLens[1017];

    if (1017 < columnCount)
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("table's column number %d bigger then 1017.", columnCount),
                 errhint("table's column number bigger then 1017.")));
    }

    nullBitmap_len = (columnCount + 9) / 8;

    memcpy((payloadBuf + payloadBufIndex), &header, header_len);
    payloadBufIndex += header_len;
    rowRemainLen += 1;
    memset((payloadBuf + payloadBufIndex), 0, nullBitmap_len);
    payloadBufIndex += nullBitmap_len;
    rowRemainLen += nullBitmap_len;

    for (i = 0; i < columnCount; i++)
    {
        attr = slot_getattr(slot, (i + 1), &is_null);
        if (!is_null) 
        {
            att = TupleDescAttr(typeinfo, i);
            getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);
            columnValue = OidOutputFunctionCall(typoutput, attr);

            if (commandTag == CMDTAG_INSERT)
            {
                curLastInsertIDTimes++;
                if (curLastInsertIDTimes == 1)
                {
                    lastInsertID = (unsigned long)atoll(columnValue);
                    lastInsertIDForPacket = lastInsertID;
                }
                return true;
            }

            columnVals[i] = columnValue;
            columnType = getMySQLDataType(att->atttypid);
            columnTypes[i] = columnType;
            if ((columnType == MYS_TYPE_LONG) || 
                (columnType == MYS_TYPE_INT24))
            {
                rowRemainLen += 4;
            }
            else if (columnType == MYS_TYPE_VAR_STRING)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "varbinary", 9) == 0)
                {
                    len = convertHexToMySQLBinary(columnValue);
                }
                else 
                {
                    len = strlen(columnValue);
                }
                columnValLens[i] = len;
                rowRemainLen += calcLenencIntLen(len);
                rowRemainLen += len;
            }
            else if ((columnType == MYS_TYPE_TIMESTAMP) || 
                     (columnType == MYS_TYPE_DATE) || 
                     (columnType == MYS_TYPE_DATETIME))
            {
                len = convertTimestamp(columnValue);
                columnValLens[i] = len;
                rowRemainLen += len;
            }
            else if (columnType == MYS_TYPE_TIME)
            {
                len = convertTime(columnValue);
                columnValLens[i] = len;
                rowRemainLen += len;
            }
            else if (columnType == MYS_TYPE_DECIMAL)
            {
                len = strlen(columnValue);
                columnValLens[i] = len;
                rowRemainLen += calcLenencIntLen(len);
                rowRemainLen += len;
            }
            else if (columnType == MYS_TYPE_FLOAT)
            {
                rowRemainLen += 4;
            }
            else if (columnType == MYS_TYPE_DOUBLE)
            {
                rowRemainLen += 8;
            }
            else if (columnType == MYS_TYPE_SHORT)
            {
                rowRemainLen += 2;
            }
            else if (columnType == MYS_TYPE_LONGLONG)
            {
                rowRemainLen += 8;
            }
            else if (columnType == MYS_TYPE_TINY)
            {
                rowRemainLen += 1;
            }
            else if (columnType == MYS_TYPE_STRING)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "binary", 6) == 0)
                {
                    len = convertHexToMySQLBinary(columnValue);
                }
                else 
                {
                    len = strlen(columnValue);
                }
                columnValLens[i] = len;
                rowRemainLen += calcLenencIntLen(len);
                rowRemainLen += len;
            }
            else if (columnType == MYS_TYPE_BLOB)
            {
                char *columnTypeName = getTypeName(att->atttypid);
                if (memcmp(columnTypeName, "blob", 4) == 0)
                {
                    len = convertHexToMySQLBinary(columnValue);
                }
                else 
                {
                    len = strlen(columnValue);
                }
                columnValLens[i] = len;
                rowRemainLen += calcLenencIntLen(len);
                rowRemainLen += len;
            }
            else if (columnType == MYS_TYPE_BIT)
            {
                int hexSize = (((att->atttypmod) + 8 - 1) & ~(8 - 1)) / 8;
                char *hex = palloc0(hexSize);
                convertBitToMySQLHex(columnValue, hex, hexSize);
                columnVals[i] = hex;
                columnValLens[i] = hexSize;
                rowRemainLen += calcLenencIntLen(hexSize);
                rowRemainLen += hexSize;
            }
            else if ((columnType == MYS_TYPE_ENUM) || 
                     (columnType == MYS_TYPE_SET) || 
                     (columnType == MYS_TYPE_GEOMETRY) || 
                     (columnType == MYS_TYPE_NEWDECIMAL))
            {
                len = strlen(columnValue);
                columnValLens[i] = len;
                rowRemainLen += calcLenencIntLen(len);
                rowRemainLen += len;
            }
            else if (columnType == MYS_TYPE_YEAR)
            {
                rowRemainLen += 2;
            }
            else 
            {
                elog(ERROR, "invalid column type (%d)", columnType);
                return false;
            }
        }
        else 
        {
            payloadBuf[((i + 2) / 8) + 1] |= (1 << ((i + 2) % 8));
            columnVals[i] = NULL;
        }
    }   /* for (i = 0; i < columnCount; i++) */

    if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
    {
        netTransceiver->writePacketHeaderNoFlush(rowRemainLen);

        for (i = 0; i < columnCount; i++)
        {
            if (columnVals[i] != NULL)
            {
                columnType = columnTypes[i];
                columnValue = columnVals[i];
                if ((columnType == MYS_TYPE_LONG) || 
                    (columnType == MYS_TYPE_INT24))
                {
                    tmp_int = atoi(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_int, 4);
                    payloadBufIndex += 4;
                }
                else if (columnType == MYS_TYPE_VAR_STRING)
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    netTransceiver->writePacketPayloadNoFlush(payloadBuf, payloadBufIndex);
                    payloadBufIndex = 0;
                    netTransceiver->writePacketPayloadNoFlush(columnValue, len);
                }
                else if ((columnType == MYS_TYPE_TIMESTAMP) || 
                         (columnType == MYS_TYPE_DATE) || 
                         (columnType == MYS_TYPE_DATETIME))
                {
                    memcpy((payloadBuf + payloadBufIndex), columnValue, columnValLens[i]);
                    payloadBufIndex += columnValLens[i];
                }
                else if (columnType == MYS_TYPE_TIME)
                {
                    memcpy((payloadBuf + payloadBufIndex), columnValue, columnValLens[i]);
                    payloadBufIndex += columnValLens[i];
                }
                else if (columnType == MYS_TYPE_DECIMAL)
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    memcpy((payloadBuf + payloadBufIndex), columnValue, len);
                    payloadBufIndex += len;
                }
                else if (columnType == MYS_TYPE_FLOAT)
                {
                    tmp_float = (float)atof(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_float, 4);
                    payloadBufIndex += 4;
                }
                else if (columnType == MYS_TYPE_DOUBLE)
                {
                    tmp_double = atof(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_double, 8);
                    payloadBufIndex += 8;
                }
                else if (columnType == MYS_TYPE_SHORT)
                {
                    tmp_int = atoi(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_int, 2);
                    payloadBufIndex += 2;
                }
                else if (columnType == MYS_TYPE_LONGLONG)
                {
                    tmp_long_long = atoll(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_long_long, 8);
                    payloadBufIndex += 8;
                }
                else if (columnType == MYS_TYPE_TINY)
                {
                    tmp_int = atoi(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_int, 1);
                    payloadBufIndex += 1;
                }
                else if (columnType == MYS_TYPE_STRING)
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    memcpy((payloadBuf + payloadBufIndex), columnValue, len);
                    payloadBufIndex += len;
                }
                else if (columnType == MYS_TYPE_BLOB)
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    netTransceiver->writePacketPayloadNoFlush(payloadBuf, payloadBufIndex);
                    payloadBufIndex = 0;
                    netTransceiver->writePacketPayloadNoFlush(columnValue, len);
                }
                else if (columnType == MYS_TYPE_BIT) 
                {
                    columnValue = columnVals[i];
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    memcpy((payloadBuf + payloadBufIndex), columnValue, len);
                    payloadBufIndex += len;
                }
                else if ((columnType == MYS_TYPE_ENUM) || 
                         (columnType == MYS_TYPE_SET) || 
                         (columnType == MYS_TYPE_GEOMETRY) || 
                         (columnType == MYS_TYPE_NEWDECIMAL))
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    memcpy((payloadBuf + payloadBufIndex), columnValue, len);
                    payloadBufIndex += len;
                }
                else if (columnType == MYS_TYPE_YEAR)
                {
                    tmp_int = atoi(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_int, 2);
                    payloadBufIndex += 2;
                }
            }   /* if (!is_null) */
        }   /* for (i = 0; i < columnCount; i++) */
        netTransceiver->writePacketPayloadNoFlush(payloadBuf, payloadBufIndex);
    }   /* if (rowRemainLen <= MYS_MAX_PACKET_LENGTH) */
    else 
    {
        netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);

        packetFreeSize = MYS_MAX_PACKET_LENGTH;
        packetFreeSize -= 1;
        packetFreeSize -= nullBitmap_len;
        for (i = 0; i < columnCount; i++)
        {
            if (columnVals[i] != NULL)
            {
                columnType = columnTypes[i];
                columnValue = columnVals[i];
                if ((columnType == MYS_TYPE_LONG) || 
                    (columnType == MYS_TYPE_INT24))
                {
                    tmp_int = atoi(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_int, 4);
                    payloadBufIndex += 4;
                }
                else if (columnType == MYS_TYPE_VAR_STRING)
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;

                    if (payloadBufIndex <= packetFreeSize)
                    {
                        netTransceiver->writePacketPayloadNoFlush(payloadBuf, payloadBufIndex);
                        packetFreeSize -= payloadBufIndex;
                        payloadBufIndex = 0;
                    }
                    else 
                    {
                        netTransceiver->writePacketPayloadNoFlush(payloadBuf, packetFreeSize);
                        payloadBufSentLen = packetFreeSize;

                        rowRemainLen -= MYS_MAX_PACKET_LENGTH;
                        if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
                        {
                            netTransceiver->writePacketHeaderNoFlush(rowRemainLen);
                        }
                        else 
                        {
                            netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);
                        }
                        packetFreeSize = MYS_MAX_PACKET_LENGTH;

                        netTransceiver->writePacketPayloadNoFlush((payloadBuf + payloadBufSentLen), (payloadBufIndex - payloadBufSentLen));
                        packetFreeSize -= (payloadBufIndex - payloadBufSentLen);
                        payloadBufIndex = 0;
                    }   /* if (payloadBufIndex <= packetFreeSize) */
                    while (true)
                    {
                        if (len <= packetFreeSize)
                        {
                            netTransceiver->writePacketPayloadNoFlush(columnValue, len);
                            packetFreeSize -= len;
                            break;
                        }
                        else 
                        {
                            netTransceiver->writePacketPayloadNoFlush(columnValue, packetFreeSize);
                            columnValue += packetFreeSize;
                            len -= packetFreeSize;

                            rowRemainLen -= MYS_MAX_PACKET_LENGTH;
                            if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
                            {
                                netTransceiver->writePacketHeaderNoFlush(rowRemainLen);
                            }
                            else 
                            {
                                netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);
                            }
                            packetFreeSize = MYS_MAX_PACKET_LENGTH;
                        }
                    }   /* while (true) */
                }
                else if ((columnType == MYS_TYPE_TIMESTAMP) || 
                         (columnType == MYS_TYPE_DATE) || 
                         (columnType == MYS_TYPE_DATETIME))
                {
                    memcpy((payloadBuf + payloadBufIndex), columnValue, columnValLens[i]);
                    payloadBufIndex += columnValLens[i];
                }
                else if (columnType == MYS_TYPE_TIME)
                {
                    memcpy((payloadBuf + payloadBufIndex), columnValue, columnValLens[i]);
                    payloadBufIndex += columnValLens[i];
                }
                else if (columnType == MYS_TYPE_DECIMAL)
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    memcpy((payloadBuf + payloadBufIndex), columnValue, len);
                    payloadBufIndex += len;
                }
                else if (columnType == MYS_TYPE_FLOAT)
                {
                    tmp_float = (float)atof(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_float, 4);
                    payloadBufIndex += 4;
                }
                else if (columnType == MYS_TYPE_DOUBLE)
                {
                    tmp_double = atof(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_double, 8);
                    payloadBufIndex += 8;
                }
                else if (columnType == MYS_TYPE_STRING)
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    memcpy((payloadBuf + payloadBufIndex), columnValue, len);
                    payloadBufIndex += len;
                }
                else if (columnType == MYS_TYPE_SHORT)
                {
                    tmp_int = atoi(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_int, 2);
                    payloadBufIndex += 2;
                }
                else if (columnType == MYS_TYPE_LONGLONG)
                {
                    tmp_long_long = atoll(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_long_long, 8);
                    payloadBufIndex += 8;
                }
                else if (columnType == MYS_TYPE_TINY)
                {
                    tmp_int = atoi(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_int, 1);
                    payloadBufIndex += 1;
                }
                else if (columnType == MYS_TYPE_BLOB) 
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;

                    if (payloadBufIndex <= packetFreeSize)
                    {
                        netTransceiver->writePacketPayloadNoFlush(payloadBuf, payloadBufIndex);
                        packetFreeSize -= payloadBufIndex;
                        payloadBufIndex = 0;
                    }
                    else 
                    {
                        netTransceiver->writePacketPayloadNoFlush(payloadBuf, packetFreeSize);
                        payloadBufSentLen = packetFreeSize;

                        rowRemainLen -= MYS_MAX_PACKET_LENGTH;
                        if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
                        {
                            netTransceiver->writePacketHeaderNoFlush(rowRemainLen);
                        }
                        else 
                        {
                            netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);
                        }
                        packetFreeSize = MYS_MAX_PACKET_LENGTH;

                        netTransceiver->writePacketPayloadNoFlush((payloadBuf + payloadBufSentLen), (payloadBufIndex - payloadBufSentLen));
                        packetFreeSize -= (payloadBufIndex - payloadBufSentLen);
                        payloadBufIndex = 0;
                    }   /* if (payloadBufIndex <= packetFreeSize) */
                    while (true)
                    {
                        if (len <= packetFreeSize)
                        {
                            netTransceiver->writePacketPayloadNoFlush(columnValue, len);
                            packetFreeSize -= len;
                            break;
                        }
                        else 
                        {
                            netTransceiver->writePacketPayloadNoFlush(columnValue, packetFreeSize);
                            columnValue += packetFreeSize;
                            len -= packetFreeSize;

                            rowRemainLen -= MYS_MAX_PACKET_LENGTH;
                            if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
                            {
                                netTransceiver->writePacketHeaderNoFlush(rowRemainLen);
                            }
                            else 
                            {
                                netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);
                            }
                            packetFreeSize = MYS_MAX_PACKET_LENGTH;
                        }
                    }   /* while (true) */
                }   /* if (columnType == MYS_TYPE_BLOB) */
                else if (columnType == MYS_TYPE_BIT)
                {
                    int hexSize = (((att->atttypmod) + 8 - 1) & ~(8 - 1)) / 8;
                    char *hex = palloc0(hexSize);
                    convertBitToMySQLHex(columnValue, hex, hexSize);
                    columnValue = hex;
                    len = hexSize;
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    memcpy((payloadBuf + payloadBufIndex), columnValue, len);
                    payloadBufIndex += len;
                }
                else if ((columnType == MYS_TYPE_ENUM) || 
                         (columnType == MYS_TYPE_SET) || 
                         (columnType == MYS_TYPE_GEOMETRY) || 
                         (columnType == MYS_TYPE_NEWDECIMAL))
                {
                    len = columnValLens[i];
                    lenencIntLen = assembleLenencUnsignedLong(len, (payloadBuf + payloadBufIndex));
                    payloadBufIndex += lenencIntLen;
                    memcpy((payloadBuf + payloadBufIndex), columnValue, len);
                    payloadBufIndex += len;
                }
                else if (columnType == MYS_TYPE_YEAR)
                {
                    tmp_int = atoi(columnValue);
                    memcpy((payloadBuf + payloadBufIndex), &tmp_int, 2);
                    payloadBufIndex += 2;
                }
            }   /* if (!is_null) */
        }   /* for (i = 0; i < columnCount; i++) */
        if (payloadBufIndex <= packetFreeSize)
        {
            netTransceiver->writePacketPayloadNoFlush(payloadBuf, payloadBufIndex);
        }
        else 
        {
            netTransceiver->writePacketPayloadNoFlush(payloadBuf, packetFreeSize);
            payloadBufSentLen = packetFreeSize;

            netTransceiver->writePacketHeaderNoFlush((payloadBufIndex - payloadBufSentLen));
            netTransceiver->writePacketPayloadNoFlush((payloadBuf + payloadBufSentLen), (payloadBufIndex - payloadBufSentLen));
        }
    }   /* else of if (rowRemainLen <= MYS_MAX_PACKET_LENGTH) */

    return true;
}

static bool 
printTupText(TupleTableSlot *slot, DestReceiver *self, CommandTag commandTag)
{
    int i = 0;
    char *payloadBuf = NULL;
    int payloadBufSentLen = 0;
    TupleDesc typeinfo = slot->tts_tupleDescriptor;
    int columnCount = typeinfo->natts;
    size_t rowRemainLen = 0;
    size_t packetFreeSize = 0;
    int lenencIntLen = 0;
    char* columnVals[1017];
    int columnValLens[1017];

    if (1017 < columnCount)
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("table's column number %d bigger then 1017.", columnCount),
                 errhint("table's column number bigger then 1017.")));
    }

    for (i = 0; i < columnCount; i++)
    {
        bool is_null;
        Datum attr = slot_getattr(slot, (i + 1), &is_null);
        if (!is_null) 
        {
            char *columnVal = NULL;
            int columnValLen = 0;
            convertColumnValue(slot, attr, i, &columnVal, &columnValLen);

            if (commandTag == CMDTAG_INSERT)
            {
                curLastInsertIDTimes++;
                if (curLastInsertIDTimes == 1)
                {
                    lastInsertID = (unsigned long)atoll(columnVal);
                    lastInsertIDForPacket = lastInsertID;
                }
                return true;
            }

            columnVals[i] = columnVal;
            columnValLens[i] = columnValLen;

            rowRemainLen += calcLenencIntLen(columnValLen);
            rowRemainLen += columnValLen;
        }   
        else 
        {
            columnVals[i] = NULL;
            rowRemainLen += 1;
        }
    }   /* for (i = 0; i < columnCount; i++) */

    if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
    {
        netTransceiver->writePacketHeaderNoFlush(rowRemainLen);

        for (i = 0; i < columnCount; i++)
        {
            if (columnVals[i] != NULL)
            {
                netTransceiver->getWriteBufForPayload(&payloadBuf);
                lenencIntLen = assembleLenencUnsignedLong(columnValLens[i], payloadBuf);
                netTransceiver->finishWriteToBufNoFlush(lenencIntLen);
                netTransceiver->writePacketPayloadNoFlush(columnVals[i], columnValLens[i]);
            }
            else 
            {
                netTransceiver->getWriteBufForPayload(&payloadBuf);
                payloadBuf[0] = (char)0xFB;    
                netTransceiver->finishWriteToBufNoFlush(1);
            }
        }
    }   /* if (rowRemainLen <= MYS_MAX_PACKET_LENGTH) */
    else 
    {
        netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);

        packetFreeSize = MYS_MAX_PACKET_LENGTH;
        for (i = 0; i < columnCount; i++)
        {
            if (columnVals[i] != NULL)
            {
                char *columnVal = columnVals[i];
                int columnValLen = columnValLens[i];
                netTransceiver->getWriteBufForPayload(&payloadBuf);
                lenencIntLen = assembleLenencUnsignedLong(columnValLen, payloadBuf);
                if (lenencIntLen <= packetFreeSize)
                {
                    netTransceiver->finishWriteToBufNoFlush(lenencIntLen);
                    packetFreeSize -= lenencIntLen;
                }
                else 
                {
                    char *buf[128];
                    netTransceiver->finishWriteToBufNoFlush(packetFreeSize);
                    payloadBufSentLen = packetFreeSize;
                    memcpy(buf, (payloadBuf + payloadBufSentLen), (lenencIntLen - payloadBufSentLen));

                    rowRemainLen -= MYS_MAX_PACKET_LENGTH;
                    if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
                    {
                        netTransceiver->writePacketHeaderNoFlush(rowRemainLen);
                    }
                    else 
                    {
                        netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);
                    }
                    packetFreeSize = MYS_MAX_PACKET_LENGTH;

                    netTransceiver->getWriteBufForPayload(&payloadBuf);
                    memcpy(payloadBuf, buf, (lenencIntLen - payloadBufSentLen));
                    netTransceiver->finishWriteToBufNoFlush(lenencIntLen - payloadBufSentLen);
                    packetFreeSize -= (lenencIntLen - payloadBufSentLen);
                }

                while (true)
                {
                    if (columnValLen <= packetFreeSize)
                    {
                        netTransceiver->writePacketPayloadNoFlush(columnVal, columnValLen);
                        packetFreeSize -= columnValLen;
                        break;
                    }
                    else 
                    {
                        netTransceiver->writePacketPayloadNoFlush(columnVal, packetFreeSize);
                        columnVal += packetFreeSize;
                        columnValLen -= packetFreeSize;   

                        rowRemainLen -= MYS_MAX_PACKET_LENGTH;
                        if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
                        {
                            netTransceiver->writePacketHeaderNoFlush(rowRemainLen);
                        }
                        else 
                        {
                            netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);
                        }
                        packetFreeSize = MYS_MAX_PACKET_LENGTH;
                    }
                }   /* while (true) */
            }   /* if (!is_null) */
            else 
            {
                if (1 <= packetFreeSize)
                {
                    netTransceiver->getWriteBufForPayload(&payloadBuf);
                    payloadBuf[0] = (char)0xFB;    
                    netTransceiver->finishWriteToBufNoFlush(1);
                    packetFreeSize -= 1;
                }
                else 
                {
                    rowRemainLen -= MYS_MAX_PACKET_LENGTH;
                    if (rowRemainLen <= MYS_MAX_PACKET_LENGTH)
                    {
                        netTransceiver->writePacketHeaderNoFlush(rowRemainLen);
                    }
                    else 
                    {
                        netTransceiver->writePacketHeaderNoFlush(MYS_MAX_PACKET_LENGTH);
                    }
                    packetFreeSize = MYS_MAX_PACKET_LENGTH;

                    netTransceiver->getWriteBufForPayload(&payloadBuf);
                    payloadBuf[0] = (char)0xFB;    
                    netTransceiver->finishWriteToBufNoFlush(1);
                    packetFreeSize -= 1;
                }
            }   /* else of if (!is_null) */
        }   /* for (i = 0; i < columnCount; i++) */
    }   /* else of if (rowRemainLen <= MYS_MAX_PACKET_LENGTH) */

    return true;
}

static bool 
printTupTextForSystemVars(TupleTableSlot *slot, DestReceiver *self)
{
    int i = 0;
    char *payloadBuf = NULL;
    TupleDesc typeinfo = slot->tts_tupleDescriptor;
    int columnCount = typeinfo->natts;
    size_t rowRemainLen = 0;
    int lenencIntLen = 0;
    char* columnVals[2];
    int columnValLens[2];

    if (columnCount != 2)
    {
        elog(ERROR, 
             "Invalid column numer '%d' for information_schema.variables. Connection will be disconnect.", 
             columnCount);
        proc_exit(1);
    }

    for (i = 0; i < columnCount; i++)
    {
        if (i == 0)
        {
            bool is_null;
            Datum attr = slot_getattr(slot, (i + 1), &is_null);
            if (!is_null) 
            {
                char *columnVal = NULL;
                int columnValLen = 0;
                convertColumnValue(slot, attr, i, &columnVal, &columnValLen);

                columnVals[i] = columnVal;
                columnValLens[i] = columnValLen;

                rowRemainLen += calcLenencIntLen(columnValLen);
                rowRemainLen += columnValLen;
            }   
            else 
            {
                columnVals[i] = NULL;
                rowRemainLen += 1;
            }
        }
        else /* if (i == 0) */
        {
            if (columnVals[0] != NULL)
            {
                bool isSessionSystemVar;
                bool getRet;
                char *columnVal = NULL;
                int columnValLen = 0;

                if (systemVarType == 1)
                {
                    isSessionSystemVar = true;
                }
                else 
                {
                    isSessionSystemVar = false;
                }
                getRet = getSystemVariableValueForShow(columnVals[0], 
                                                       isSessionSystemVar, 
                                                       &columnVal);
                if (getRet) 
                {
                    if (columnVal != NULL)
                    {
                        columnValLen = strlen(columnVal);
                        columnVals[i] = columnVal;
                        columnValLens[i] = columnValLen;

                        rowRemainLen += calcLenencIntLen(columnValLen);
                        rowRemainLen += columnValLen;
                    }   
                    else 
                    {
                        columnVals[i] = NULL;
                        rowRemainLen += 1;
                    }
                }   
                else 
                {
                    columnVals[i] = NULL;
                    rowRemainLen += 1;
                }
            }
            else 
            {
                elog(ERROR, "System variable name is null. Connection will be disconnect.");
                proc_exit(1);
            }
        }
    }   /* for (i = 0; i < columnCount; i++) */

    netTransceiver->writePacketHeaderNoFlush(rowRemainLen);

    for (i = 0; i < columnCount; i++)
    {
        if (columnVals[i] != NULL)
        {
            netTransceiver->getWriteBufForPayload(&payloadBuf);
            lenencIntLen = assembleLenencUnsignedLong(columnValLens[i], 
                                                      payloadBuf);
            netTransceiver->finishWriteToBufNoFlush(lenencIntLen);
            netTransceiver->writePacketPayloadNoFlush(columnVals[i], 
                                                      columnValLens[i]);
        }
        else 
        {
            netTransceiver->getWriteBufForPayload(&payloadBuf);
            payloadBuf[0] = (char)0xFB;    
            netTransceiver->finishWriteToBufNoFlush(1);
        }
    }

    return true;
}

static void
skipSpaces(char **secondWord)
{
    while (true)
    {
        if (((*secondWord)[0] != ' ') && 
            ((*secondWord)[0] != '\n') && 
            ((*secondWord)[0] != '\t'))
        {
            break;
        }
        else 
        {
            ++(*secondWord);
        }
    }
}

static int
createServerPort(int family, 
                 const char *hostName, 
                 unsigned short portNumber, 
                 const char *unixSocketDir, 
                 ProtocolInterface *protocolHandler)
{
    pgsocket	fd;
	int			err;
	int			maxconn;
	int			ret;
	char		portNumberStr[32];
	const char *familyDesc;
	char		familyDescBuf[64];
	const char *addrDesc;
	char		addrBuf[NI_MAXHOST];
	char	   *service;
	struct addrinfo *addrs = NULL,
			   *addr;
	struct addrinfo hint;
	int			added = 0;

#ifdef HAVE_UNIX_SOCKETS
	char		unixSocketPath[MAXPGPATH];
#endif
#if !defined(WIN32) || defined(IPV6_V6ONLY)
	int			one = 1;
#endif

	MemSet(&hint, 0, sizeof(hint));
	hint.ai_family = family;
	hint.ai_flags = AI_PASSIVE;
	hint.ai_socktype = SOCK_STREAM;

#ifdef HAVE_UNIX_SOCKETS
	if (family == AF_UNIX)
	{
		UNIXSOCK_PATH(unixSocketPath, portNumber, unixSocketDir);
		if (strlen(unixSocketPath) >= UNIXSOCK_PATH_BUFLEN)
		{
			ereport(LOG,
					(errmsg("Unix-domain socket path \"%s\" is too long (maximum %d bytes)",
							unixSocketPath, (int) (UNIXSOCK_PATH_BUFLEN - 1))));
			return STATUS_ERROR;
		}
		if (lockAFUnix(unixSocketDir, unixSocketPath) != STATUS_OK)
			return STATUS_ERROR;
		service = unixSocketPath;
	}
	else
#endif	
	{
		snprintf(portNumberStr, sizeof(portNumberStr), "%d", portNumber);
		service = portNumberStr;
	}

	ret = pg_getaddrinfo_all(hostName, service, &hint, &addrs);
	if (ret || !addrs)
	{
		if (hostName)
			ereport(LOG,
					(errmsg("could not translate host name \"%s\", service \"%s\" to address: %s",
							hostName, service, gai_strerror(ret))));
		else
			ereport(LOG,
					(errmsg("could not translate service \"%s\" to address: %s",
							service, gai_strerror(ret))));
		if (addrs)
			pg_freeaddrinfo_all(hint.ai_family, addrs);
		return STATUS_ERROR;
	}

	for (addr = addrs; addr; addr = addr->ai_next)
	{
		if (!IS_AF_UNIX(family) && IS_AF_UNIX(addr->ai_family))
		{
			continue;
		}

		if(!listen_have_free_slot())
            break;

		switch (addr->ai_family)
		{
			case AF_INET:
				familyDesc = _("IPv4");
				break;
#ifdef HAVE_IPV6
			case AF_INET6:
				familyDesc = _("IPv6");
				break;
#endif
#ifdef HAVE_UNIX_SOCKETS
			case AF_UNIX:
				familyDesc = _("Unix");
				break;
#endif
			default:
				snprintf(familyDescBuf, sizeof(familyDescBuf),
						 _("unrecognized address family %d"),
						 addr->ai_family);
				familyDesc = familyDescBuf;
				break;
		}

#ifdef HAVE_UNIX_SOCKETS
		if (addr->ai_family == AF_UNIX)
			addrDesc = unixSocketPath;
		else
#endif
		{
			pg_getnameinfo_all((const struct sockaddr_storage *) addr->ai_addr,
							   addr->ai_addrlen,
							   addrBuf, sizeof(addrBuf),
							   NULL, 0,
							   NI_NUMERICHOST);
			addrDesc = addrBuf;
		}

		if ((fd = socket(addr->ai_family, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not create %s socket for address \"%s\": %m",
							familyDesc, addrDesc)));
			continue;
		}

#ifndef WIN32

		if (!IS_AF_UNIX(addr->ai_family))
		{
			if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
							(char *) &one, sizeof(one))) == -1)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("%s(%s) failed for %s address \"%s\": %m",
								"setsockopt", "SO_REUSEADDR",
								familyDesc, addrDesc)));
				closesocket(fd);
				continue;
			}
		}
#endif

#ifdef IPV6_V6ONLY
		if (addr->ai_family == AF_INET6)
		{
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
						   (char *) &one, sizeof(one)) == -1)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("%s(%s) failed for %s address \"%s\": %m",
								"setsockopt", "IPV6_V6ONLY",
								familyDesc, addrDesc)));
				closesocket(fd);
				continue;
			}
		}
#endif

		err = bind(fd, addr->ai_addr, addr->ai_addrlen);
		if (err < 0)
		{
			int			saved_errno = errno;

			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not bind %s address \"%s\": %m",
							familyDesc, addrDesc),
					 saved_errno == EADDRINUSE ?
					 (IS_AF_UNIX(addr->ai_family) ?
					  errhint("Is another postmaster already running on port %d?",
							  (int) portNumber) :
					  errhint("Is another postmaster already running on port %d?"
							  " If not, wait a few seconds and retry.",
							  (int) portNumber)) : 0));
			closesocket(fd);
			continue;
		}

#ifdef HAVE_UNIX_SOCKETS
		if (addr->ai_family == AF_UNIX)
		{
			if (setupAFUnix(service) != STATUS_OK)
			{
				closesocket(fd);
				break;
			}
		}
#endif

		maxconn = MaxBackends * 2;
		if (maxconn > PG_SOMAXCONN)
			maxconn = PG_SOMAXCONN;

		err = listen(fd, maxconn);
		if (err < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not listen on %s address \"%s\": %m",
							familyDesc, addrDesc)));
			closesocket(fd);
			continue;
		}

#ifdef HAVE_UNIX_SOCKETS
		if (addr->ai_family == AF_UNIX)
			ereport(LOG,
					(errmsg("listening on Unix socket \"%s\"",
							addrDesc)));
		else
#endif
			ereport(LOG,
					(errmsg("listening on %s address \"%s\", port %d",
							familyDesc, addrDesc, (int) portNumber)));

		setSecondProtocolSocket(fd);
		added++;
	}

	pg_freeaddrinfo_all(hint.ai_family, addrs);

	if (!added)
		return STATUS_ERROR;

	return STATUS_OK;
}

static int
lockAFUnix(const char *unixSocketDir, const char *unixSocketPath)
{
	if (unixSocketPath[0] == '@')
		return STATUS_OK;

	CreateSocketLockFile(unixSocketPath, true, unixSocketDir);

	(void) unlink(unixSocketPath);

	sockPaths = lappend(sockPaths, pstrdup(unixSocketPath));

	return STATUS_OK;
}

static int
setupAFUnix(const char *sockPath)
{
	if (sockPath[0] == '@')
		return STATUS_OK;

	Assert(Unix_socket_group);
	if (Unix_socket_group[0] != '\0')
	{
#ifdef WIN32
		elog(WARNING, "configuration item unix_socket_group is not supported on this platform");
#else
		char	   *endptr;
		unsigned long val;
		gid_t		gid;

		val = strtoul(Unix_socket_group, &endptr, 10);
		if (*endptr == '\0')
		{						
			gid = val;
		}
		else
		{						
			struct group *gr;

			gr = getgrnam(Unix_socket_group);
			if (!gr)
			{
				ereport(LOG,
						(errmsg("group \"%s\" does not exist",
								Unix_socket_group)));
				return STATUS_ERROR;
			}
			gid = gr->gr_gid;
		}
		if (chown(sockPath, -1, gid) == -1)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not set group of file \"%s\": %m",
							sockPath)));
			return STATUS_ERROR;
		}
#endif
	}

	if (chmod(sockPath, Unix_socket_permissions) == -1)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not set permissions of file \"%s\": %m",
						sockPath)));
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static void
RemoveSocketFiles2(void)
{
	ListCell   *l;

	/* Loop through all created sockets... */
	foreach(l, sockPaths)
	{
		char	   *sock_path = (char *) lfirst(l);

		/* Ignore any error. */
		(void) unlink(sock_path);
	}
	/* Since we're about to exit, no need to reclaim storage */
	sockPaths = NIL;
}

/*
 * on_proc_exit callback to close server's listen sockets
 */
void
CloseServerPorts2(int status, Datum arg)
{
	/*
	 * remove any filesystem entries for Unix sockets.  To avoid race
	 * conditions against incoming postmasters, this must happen after closing
	 * the sockets and before removing lock files.
	 */
	RemoveSocketFiles2();
}

static void
closeSocket(int code, Datum arg)
{
	if (MyProcPort != NULL)
	{
#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)
#ifdef ENABLE_GSS
		OM_uint32	min_s;

		if (MyProcPort->gss->ctx != GSS_C_NO_CONTEXT)
			gss_delete_sec_context(&min_s, &MyProcPort->gss->ctx, NULL);

		if (MyProcPort->gss->cred != GSS_C_NO_CREDENTIAL)
			gss_release_cred(&min_s, &MyProcPort->gss->cred);
#endif	

		free(MyProcPort->gss);
#endif	

		secure_close(MyProcPort);

		MyProcPort->sock = PGINVALID_SOCKET;
	}
}


static void 
simulateShowWarnErrReturn(void)
{
    char payloadBuf[1024];

    char packet_bytes01[] = {
        0x03
    };
    char packet_bytes02[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x05, 0x4c, 0x65, 0x76, 0x65,
        0x6c, 0x00, 0x0c, 0x21, 0x00, 0x15, 0x00, 0x00,
        0x00, 0xfd, 0x01, 0x00, 0x1f, 0x00, 0x00
    };
    char packet_bytes03[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x04, 0x43, 0x6f, 0x64, 0x65,
        0x00, 0x0c, 0x3f, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x03, 0xa1, 0x00, 0x00, 0x00, 0x00
    };
    char packet_bytes04[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x07, 0x4d, 0x65, 0x73, 0x73,
        0x61, 0x67, 0x65, 0x00, 0x0c, 0x21, 0x00, 0x00,
        0x06, 0x00, 0x00, 0xfd, 0x01, 0x00, 0x1f, 0x00,
        0x00
    };
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes01[0], (sizeof(packet_bytes01) / sizeof(packet_bytes01[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes02[0], (sizeof(packet_bytes02) / sizeof(packet_bytes02[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes03[0], (sizeof(packet_bytes03) / sizeof(packet_bytes03[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes04[0], (sizeof(packet_bytes04) / sizeof(packet_bytes04[0])));
    sendEOFPacketNoFlush();

    if (0 < warnErrCode)
    {
        int len;
        int warnErrLevelLen;
        int warnErrCodeLen;
        int warnErrMsgLen;
        char warnErrCodeBuf[16];
        int lenencIntLen;

        len = 0;
        warnErrLevelLen = strlen(warnErrLevel);
        len += calcLenencIntLen(warnErrLevelLen);
        len += warnErrLevelLen;
        snprintf(warnErrCodeBuf, sizeof(warnErrCodeBuf), "%d", warnErrCode);
        warnErrCodeLen = strlen(warnErrCodeBuf);
        len += calcLenencIntLen(warnErrCodeLen);
        len += warnErrCodeLen;
        warnErrMsgLen = strlen(warnErrMsg);
        len += calcLenencIntLen(warnErrMsgLen);
        len += warnErrMsgLen;

        netTransceiver->writePacketHeaderNoFlush(len);
        lenencIntLen = assembleLenencUnsignedLong(warnErrLevelLen, payloadBuf);
        netTransceiver->writePacketHeaderPayloadNoFlush(payloadBuf, lenencIntLen);
        netTransceiver->writePacketHeaderPayloadNoFlush(warnErrLevel, warnErrLevelLen);
        lenencIntLen = assembleLenencUnsignedLong(warnErrCodeLen, payloadBuf);
        netTransceiver->writePacketHeaderPayloadNoFlush(payloadBuf, lenencIntLen);
        netTransceiver->writePacketHeaderPayloadNoFlush(warnErrCodeBuf, warnErrCodeLen);
        lenencIntLen = assembleLenencUnsignedLong(warnErrMsgLen, payloadBuf);
        netTransceiver->writePacketHeaderPayloadNoFlush(payloadBuf, lenencIntLen);
        netTransceiver->writePacketHeaderPayloadNoFlush(warnErrMsg, warnErrMsgLen);
    }

    sendEOFPacketFlush();
}

static void 
simulateShowFieldsReturn(void)
{
    sendEOFPacketFlush();
}

static bool 
simulateChecksumTable(char *stmt)
{
    char operation[128];
    int operationLen = 0;
    int tableNum = 0;
    char schemaNameTableNames[128][256];
    int tableNameSectionStartIndex = 0;
    int stmtLen = strlen(stmt) + 1;  
    int tableNameStartIndex = 0;
    bool hasDot = false;

    char packet_bytes1[] = {
        0x02
    };
    char packet_bytes2[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x05, 0x54, 0x61, 0x62, 0x6c,
        0x65, 0x00, 0x0c, 0x21, 0x00, 0x80, 0x04, 0x00,
        0x00, 0xfd, 0x00, 0x00, 0x1f, 0x00, 0x00
    };
    char packet_bytes3[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x08, 0x43, 0x68, 0x65, 0x63,
        0x6b, 0x73, 0x75, 0x6d, 0x00, 0x0c, 0x3f, 0x00,
        0x15, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00, 0x00,
        0x00, 0x00
    };
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes1[0], (sizeof(packet_bytes1) / sizeof(packet_bytes1[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes2[0], (sizeof(packet_bytes2) / sizeof(packet_bytes2[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes3[0], (sizeof(packet_bytes3) / sizeof(packet_bytes3[0])));
    sendEOFPacketNoFlush();

    for (int i = 0; i < stmtLen; i++)
    {
        if (stmt[i] == ' ')
        {
            memcpy(operation, stmt, i);
            operation[i] = '\0';
            operationLen = i;
            break;
        }
    }
    if (operationLen == 0)
    {
        return false;
    }

    for (int i = 0; i < stmtLen; i++)
    {
        if (strncasecmp(&stmt[i], " table ", 7) == 0)
        {
            tableNameSectionStartIndex = i + 7;
            break;
        }
    }
    if (tableNameSectionStartIndex == 0)
    {
        return false;
    }

    tableNameStartIndex = tableNameSectionStartIndex;
    hasDot = false;
    for (int i = tableNameSectionStartIndex; i < stmtLen; i++)
    {
        if ((stmt[i] == ',') || 
            (stmt[i] == ';') || 
            (stmt[i] == '\0') || 
            (strncasecmp(&stmt[i], " FOR", 4) == 0) || 
            (strncasecmp(&stmt[i], " UPGRADE", 8) == 0) || 
            (strncasecmp(&stmt[i], " QUICK", 6) == 0) || 
            (strncasecmp(&stmt[i], " FAST", 5) == 0) || 
            (strncasecmp(&stmt[i], " MEDIUM", 7) == 0) || 
            (strncasecmp(&stmt[i], " EXTENDED", 9) == 0) || 
            (strncasecmp(&stmt[i], " CHANGED", 8) == 0) || 
            (strncasecmp(&stmt[i], " USE_FRM", 8) == 0))
        {
            if (hasDot)
            {
                int j = 0;
                for (int k = tableNameStartIndex; k < i; k++)
                {
                    if (stmt[k] != ' ')
                    {
                        schemaNameTableNames[tableNum][j] = stmt[k];
                        j++;
                    }
                }
                schemaNameTableNames[tableNum][j] = '\0';
            }
            else 
            {
                int schemaNameLen = strlen(MyProcPort->database_name);
                int j = schemaNameLen + 1;
                memcpy(schemaNameTableNames[tableNum], 
                       MyProcPort->database_name, 
                       schemaNameLen);
                schemaNameTableNames[tableNum][schemaNameLen] = '.';
                for (int k = tableNameStartIndex; k < i; k++)
                {
                    if (stmt[k] != ' ')
                    {
                        schemaNameTableNames[tableNum][j] = stmt[k];
                        j++;
                    }
                }
                schemaNameTableNames[tableNum][j] = '\0';
            }

            tableNum++;
            tableNameStartIndex = i + 1;
            hasDot = false;

            if ((strncasecmp(&stmt[i], " FOR", 4) == 0) || 
                (strncasecmp(&stmt[i], " UPGRADE", 8) == 0) || 
                (strncasecmp(&stmt[i], " QUICK", 6) == 0) || 
                (strncasecmp(&stmt[i], " FAST", 5) == 0) || 
                (strncasecmp(&stmt[i], " MEDIUM", 7) == 0) || 
                (strncasecmp(&stmt[i], " EXTENDED", 9) == 0) || 
                (strncasecmp(&stmt[i], " CHANGED", 8) == 0) || 
                (strncasecmp(&stmt[i], " USE_FRM", 8) == 0))
            {
                break;
            }

            if ((stmt[i] == ';') || 
                (stmt[i] == '\0'))
            {
                break;
            }

            if (128 <= tableNum)
            {
                return false;
            }
        }
        else if (stmt[i] == '.')
        {
            hasDot = true;
        }
        else 
        {
            /* do nothing; */
        }
    }

    for (int i = 0; i < tableNum; i++) 
    {
        char payloadBuf[1024];
        int payloadOffset = 0;
        int payloadLen = 0;
        char *columnVal = NULL;
        int columnValLen = 0;

        columnVal = schemaNameTableNames[i];
        columnValLen = strlen(columnVal);
        memcpy((payloadBuf + payloadOffset), &columnValLen, 1);
        payloadOffset += 1;
        memcpy((payloadBuf + payloadOffset), columnVal, columnValLen);
        payloadOffset += columnValLen;

        columnVal = "2686538710";
        columnValLen = strlen(columnVal);
        memcpy((payloadBuf + payloadOffset), &columnValLen, 1);
        payloadOffset += 1;
        memcpy((payloadBuf + payloadOffset), columnVal, columnValLen);
        payloadOffset += columnValLen;

        payloadLen = payloadOffset;
        netTransceiver->writePacketHeaderPayloadNoFlush(payloadBuf, payloadLen);
    }

    sendEOFPacketFlush();

    return true;
}


static bool 
simulateAnalyzeCheckOptimizeRepairTable(char *stmt)
{
    char operation[128];
    int operationLen = 0;
    int tableNum = 0;
    char schemaNameTableNames[128][256];
    int tableNameSectionStartIndex = 0;
    int stmtLen = strlen(stmt) + 1;     /* include '\0' */
    int tableNameStartIndex = 0;
    bool hasDot = false;

    char packet_bytes1[] = {
        0x04
    };
    char packet_bytes2[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x05, 0x54, 0x61, 0x62, 0x6c,
        0x65, 0x00, 0x0c, 0x21, 0x00, 0x80, 0x01, 0x00,
        0x00, 0xfd, 0x00, 0x00, 0x1f, 0x00, 0x00
    };
    char packet_bytes3[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x02, 0x4f, 0x70, 0x00, 0x0c,
        0x21, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00,
        0x00, 0x1f, 0x00, 0x00
    };
    char packet_bytes4[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x08, 0x4d, 0x73, 0x67, 0x5f,
        0x74, 0x79, 0x70, 0x65, 0x00, 0x0c, 0x21, 0x00,
        0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x00, 0x1f,
        0x00, 0x00
    };
    char packet_bytes5[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x08, 0x4d, 0x73, 0x67, 0x5f,
        0x74, 0x65, 0x78, 0x74, 0x00, 0x0c, 0x21, 0x00,
        0x00, 0x00, 0x12, 0x00, 0xfa, 0x00, 0x00, 0x1f,
        0x00, 0x00
    };
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes1[0], (sizeof(packet_bytes1) / sizeof(packet_bytes1[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes2[0], (sizeof(packet_bytes2) / sizeof(packet_bytes2[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes3[0], (sizeof(packet_bytes3) / sizeof(packet_bytes3[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes4[0], (sizeof(packet_bytes4) / sizeof(packet_bytes4[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes5[0], (sizeof(packet_bytes5) / sizeof(packet_bytes5[0])));
    sendEOFPacketNoFlush();

    for (int i = 0; i < stmtLen; i++)
    {
        if (stmt[i] == ' ')
        {
            memcpy(operation, stmt, i);
            operation[i] = '\0';
            operationLen = i;
            break;
        }
    }
    if (operationLen == 0)
    {
        return false;
    }

    for (int i = 0; i < stmtLen; i++)
    {
        if (strncasecmp(&stmt[i], " table ", 7) == 0)
        {
            tableNameSectionStartIndex = i + 7;
            break;
        }
    }
    if (tableNameSectionStartIndex == 0)
    {
        return false;
    }

    tableNameStartIndex = tableNameSectionStartIndex;
    hasDot = false;
    for (int i = tableNameSectionStartIndex; i < stmtLen; i++)
    {
        if ((stmt[i] == ',') || 
            (stmt[i] == ';') || 
            (stmt[i] == '\0') || 
            (strncasecmp(&stmt[i], " FOR", 4) == 0) || 
            (strncasecmp(&stmt[i], " UPGRADE", 8) == 0) || 
            (strncasecmp(&stmt[i], " QUICK", 6) == 0) || 
            (strncasecmp(&stmt[i], " FAST", 5) == 0) || 
            (strncasecmp(&stmt[i], " MEDIUM", 7) == 0) || 
            (strncasecmp(&stmt[i], " EXTENDED", 9) == 0) || 
            (strncasecmp(&stmt[i], " CHANGED", 8) == 0) || 
            (strncasecmp(&stmt[i], " USE_FRM", 8) == 0))
        {
            if (hasDot)
            {
                int j = 0;
                for (int k = tableNameStartIndex; k < i; k++)
                {
                    if (stmt[k] != ' ')
                    {
                        schemaNameTableNames[tableNum][j] = stmt[k];
                        j++;
                    }
                }
                schemaNameTableNames[tableNum][j] = '\0';
            }
            else 
            {
                int schemaNameLen = strlen(MyProcPort->database_name);
                int j = schemaNameLen + 1;
                memcpy(schemaNameTableNames[tableNum], 
                       MyProcPort->database_name, 
                       schemaNameLen);
                schemaNameTableNames[tableNum][schemaNameLen] = '.';
                for (int k = tableNameStartIndex; k < i; k++)
                {
                    if (stmt[k] != ' ')
                    {
                        schemaNameTableNames[tableNum][j] = stmt[k];
                        j++;
                    }
                }
                schemaNameTableNames[tableNum][j] = '\0';
            }

            tableNum++;
            tableNameStartIndex = i + 1;
            hasDot = false;

            if ((strncasecmp(&stmt[i], " FOR", 4) == 0) || 
                (strncasecmp(&stmt[i], " UPGRADE", 8) == 0) || 
                (strncasecmp(&stmt[i], " QUICK", 6) == 0) || 
                (strncasecmp(&stmt[i], " FAST", 5) == 0) || 
                (strncasecmp(&stmt[i], " MEDIUM", 7) == 0) || 
                (strncasecmp(&stmt[i], " EXTENDED", 9) == 0) || 
                (strncasecmp(&stmt[i], " CHANGED", 8) == 0) || 
                (strncasecmp(&stmt[i], " USE_FRM", 8) == 0))
            {
                break;
            }

            if ((stmt[i] == ';') || 
                (stmt[i] == '\0'))
            {
                break;
            }

            if (128 <= tableNum)
            {
                return false;
            }
        }
        else if (stmt[i] == '.')
        {
            hasDot = true;
        }
        else 
        {
            /* do nothing; */
        }
    }

    for (int i = 0; i < tableNum; i++) 
    {
        char payloadBuf[1024];
        int payloadOffset = 0;
        int payloadLen = 0;
        char *columnVal = NULL;
        int columnValLen = 0;

        columnVal = schemaNameTableNames[i];
        columnValLen = strlen(columnVal);
        memcpy((payloadBuf + payloadOffset), &columnValLen, 1);
        payloadOffset += 1;
        memcpy((payloadBuf + payloadOffset), columnVal, columnValLen);
        payloadOffset += columnValLen;

        columnVal = operation;
        columnValLen = operationLen;
        memcpy((payloadBuf + payloadOffset), &columnValLen, 1);
        payloadOffset += 1;
        memcpy((payloadBuf + payloadOffset), columnVal, columnValLen);
        payloadOffset += columnValLen;

        columnVal = "status";
        columnValLen = strlen(columnVal);
        memcpy((payloadBuf + payloadOffset), &columnValLen, 1);
        payloadOffset += 1;
        memcpy((payloadBuf + payloadOffset), columnVal, columnValLen);
        payloadOffset += columnValLen;

        columnVal = "OK";
        columnValLen = strlen(columnVal);
        memcpy((payloadBuf + payloadOffset), &columnValLen, 1);
        payloadOffset += 1;
        memcpy((payloadBuf + payloadOffset), columnVal, columnValLen);
        payloadOffset += columnValLen;

        payloadLen = payloadOffset;
        netTransceiver->writePacketHeaderPayloadNoFlush(payloadBuf, payloadLen);
    }

    sendEOFPacketFlush();

    return true;
}

static void 
simulateShowGrants(void)
{
    char packet_bytes1[] = {
        0x01
    };
    char packet_bytes2[] = {
        0x03, 0x64, 0x65, 0x66,
        0x00, 0x00, 0x00, 0x12, 0x47, 0x72, 0x61, 0x6e,
        0x74, 0x73, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x75,
        0x73, 0x65, 0x72, 0x31, 0x40, 0x25, 0x00, 0x0c,
        0x21, 0x00, 0x00, 0x0c, 0x00, 0x00, 0xfd, 0x01,
        0x00, 0x1f, 0x00, 0x00
    };
    char packet_bytes3[] = {
        0x2a, 0x47, 0x52, 0x41,
        0x4e, 0x54, 0x20, 0x41, 0x4c, 0x4c, 0x20, 0x50,
        0x52, 0x49, 0x56, 0x49, 0x4c, 0x45, 0x47, 0x45,
        0x53, 0x20, 0x4f, 0x4e, 0x20, 0x2a, 0x2e, 0x2a,
        0x20, 0x54, 0x4f, 0x20, 0x27, 0x75, 0x73, 0x65,
        0x72, 0x31, 0x27, 0x40, 0x27, 0x25, 0x27
    };
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes1[0], (sizeof(packet_bytes1) / sizeof(packet_bytes1[0])));
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes2[0], (sizeof(packet_bytes2) / sizeof(packet_bytes2[0])));
    sendEOFPacketNoFlush();
    netTransceiver->writePacketHeaderPayloadNoFlush(&packet_bytes3[0], (sizeof(packet_bytes3) / sizeof(packet_bytes3[0])));
    sendEOFPacketFlush();
}


static bool 
rectifyCommand(StringInfo inBuf)
{
    int i = 0;
    int commentType = 0;
    int commentStartIndex = 0;
    char leftQuotationMark = ' ';
    int quotationNum = 0;
    char *oldStmt = inBuf->data + inBuf->offset;

    for (i = 0; i < inBuf->len; i++) 
    {
        if (commentType == 0)
        {
            if ((oldStmt[i] == '\'') || (oldStmt[i] == '"'))
            {
                if (oldStmt[i - 1] != '\\')
                {
                    if ((oldStmt[i + 1] == '\\') && 
                        (oldStmt[i + 2] == '0') && 
                        (oldStmt[i + 3] == oldStmt[i]))
                    {
                        oldStmt[i] = 'b';
                        oldStmt[i + 1] = '\'';
                        oldStmt[i + 2] = '0';
                        oldStmt[i + 3] = '\'';
                        i += 3;
                    }
                    else if ((oldStmt[i + 1] == '') && 
                             (oldStmt[i + 2] == oldStmt[i]))
                    {
                        if (oldStmt[i - 1] == ' ')
                        {
                            oldStmt[i - 1] = 'b';
                            oldStmt[i] = '\'';
                            oldStmt[i + 1] = '1';
                            oldStmt[i + 2] = '\'';
                            i += 2;
                        }
                        else
                        {
                            if (1 < (inBuf->maxlen - (inBuf->offset + inBuf->len)))
                            {
                                for (int l = (inBuf->len); i <= l; l--)
                                {
                                    oldStmt[l + 1] = oldStmt[l];
                                }
                                inBuf->len += 1;
                                i += 1;
                                oldStmt[i - 1] = 'b';
                                oldStmt[i] = '\'';
                                oldStmt[i + 1] = '1';
                                oldStmt[i + 2] = '\'';
                                i += 2;
                            }
                            else
                            {
                                return false;
                            }
                        }
                    }
                    else
                    {
                        if (oldStmt[i] == leftQuotationMark)
                        {
                            ++quotationNum;
                        }
                        else 
                        {
                            if (leftQuotationMark == ' ') 
                            {
                                leftQuotationMark = oldStmt[i];
                                quotationNum = 1;
                            }
                            else 
                            {
                                if ((quotationNum % 2) == 0)
                                {
                                    leftQuotationMark = oldStmt[i];
                                    quotationNum = 1;
                                }
                            }
                        }
                    }
                }
            } 
            else if (strncasecmp(&oldStmt[i], "_binary", 7) == 0)
            {
                if ((quotationNum % 2) == 0)
                {
                    skipUtf8Verify = true;
                    return true;
                }

                i += 6;
            }
            else if ((oldStmt[i] == '/') && (oldStmt[i + 1] == '*'))
            {
                if ((quotationNum % 2) == 0)
                {
                    commentType = 1;
                    commentStartIndex = i;
                }
                else 
                {
                    /* do nothing; */
                }
                ++i;
            }
            else if ((oldStmt[i] == '-') && (oldStmt[i + 1] == '-'))
            {
                if ((quotationNum % 2) == 0)
                {
                    commentType = 2;
                    commentStartIndex = i;
                }
                else 
                {
                    /* do nothing; */
                }
                ++i;
            }
            else 
            {
                /* do nothing; */
            }
        }
        else if (commentType == 1)    /* if (commentType == 0) */
        {
            if ((oldStmt[i] == '*') && (oldStmt[i + 1] == '/'))
            {
                if ((quotationNum % 2) == 0)
                {
                    commentType = 0;
                    if (oldStmt[commentStartIndex + 2] == '!')
                    {
                        int sp = commentStartIndex + 3;
                        while (true)
                        {
                            if (oldStmt[sp] != ' ')
                            {
                                break;
                            }
                            else 
                            {
                                sp++;
                            }
                            if (i <= sp)
                            {
                                return false;
                            }
                        }
                        if ((oldStmt[sp] == '5') || (oldStmt[sp] == '4'))
                        {
                            int versionEndIndex = sp + 1;
                            while (true)
                            {
                                if (oldStmt[versionEndIndex] == ',')
                                {
                                    break;
                                }
                                else if ((oldStmt[versionEndIndex] == ' ') || 
                                         (oldStmt[versionEndIndex] == '\n') || 
                                         (oldStmt[versionEndIndex] == '\t'))
                                {
                                    versionEndIndex++;
                                    break;
                                }
                                else 
                                {
                                    versionEndIndex++;
                                }
                                if (i <= versionEndIndex)
                                {
                                    return false;
                                }
                            }
                            for (int k = commentStartIndex; k < versionEndIndex; k++)
                            {
                                oldStmt[k] = ' ';
                            }
                            oldStmt[i] = ' ';
                            oldStmt[i + 1] = ' ';
                        }
                        else if (oldStmt[sp] == '8')
                        {
                            int ve = 0;
                            while (true)
                            {
                                if (halo_mysql_version[ve] != ' ')
                                {
                                    break;
                                }
                                else 
                                {
                                    ve++;
                                }
                            }
                            if (halo_mysql_version[ve] == '8')
                            {
                                int versionEndIndex = sp + 1;
                                while (true)
                                {
                                    if (oldStmt[versionEndIndex] == ',')
                                    {
                                        break;
                                    }
                                    else if ((oldStmt[versionEndIndex] == ' ') || 
                                             (oldStmt[versionEndIndex] == '\n') || 
                                             (oldStmt[versionEndIndex] == '\t'))
                                    {
                                        versionEndIndex++;
                                        break;
                                    }
                                    else 
                                    {
                                        versionEndIndex++;
                                    }
                                    if (i <= versionEndIndex)
                                    {
                                        return false;
                                    }
                                }
                                for (int k = commentStartIndex; k < versionEndIndex; k++)
                                {
                                    oldStmt[k] = ' ';
                                }
                                oldStmt[i] = ' ';
                                oldStmt[i + 1] = ' ';
                            }
                            else 
                            {
                                for (int k = commentStartIndex; k < (i + 2); k++)
                                {
                                    oldStmt[k] = ' ';
                                }
                            }
                        }
                        else 
                        {
                            if (0 < commentStartIndex)
                            {
                                oldStmt[commentStartIndex] = ' ';
                                oldStmt[commentStartIndex + 1] = ' ';
                                oldStmt[commentStartIndex + 2] = ' ';
                                oldStmt[i] = ' ';
                                oldStmt[i + 1] = ' ';
                            }
                            else 
                            {
                                elog(ERROR, "You have an error in your SQL syntax");
                            }
                        }
                    }
                    else 
                    {
                        for (int ind = commentStartIndex; ind <= (i + 1); ind++)
                        {
                            oldStmt[ind] = ' ';
                        }
                    }
                }
                ++i;
            }
        }
        else 
        {
            /* do nothing; */
        }   /* else (commentType == 2) */
    }   /* for (i = 0; i < len; i++) */

    return true;
}

static int 
rewriteExtendPreStmt_(StringInfo inBuf, sqlReadWriteType readWriteType)
{
    int cacheRet;
    preStmtInfo* pSI;

    cacheRet = 0;
    pSI = NULL;

    cacheRet = cacheExtendPreStmt(inBuf, readWriteType, &pSI);
    if (cacheRet == 1)
    {
        char *stmt;
        char *newStmt;
        int newStmtIndex;
        int remainSpace;
        int paramNum;
        int remainOffset;

        remainOffset = 128;
        stmt = inBuf->data + inBuf->offset;
        newStmt = inBuf->data + remainOffset;
        newStmtIndex = 0;
        remainSpace = inBuf->offset - remainOffset; 
        inBuf->offset = remainOffset; 
        paramNum = 0;

        snprintf(newStmt, 27, "prepare %s from \"", pSI->name);
        newStmtIndex += 26;
        remainSpace -= 26;

        if (!convertExtendPreStmt(stmt, (inBuf->len - 1), remainSpace, 
                                  newStmt, &newStmtIndex, &paramNum))
        {
            return 0;
        }

        memcpy((newStmt + newStmtIndex), "\"", 1);
        newStmtIndex += 1;
        newStmt[newStmtIndex] = '\0';
        newStmtIndex += 1;
        inBuf->len = newStmtIndex;

        pSI->paramNum = paramNum;
        pSI->nullBitmapLength = (paramNum + 7) / 8;
        pSI->paramTypeOffset = 10 + pSI->nullBitmapLength;
        pSI->paramValueOffset1 = pSI->paramTypeOffset + pSI->paramNum * 2;
        pSI->paramValueOffset2 = 10 + pSI->nullBitmapLength;
    }

    return cacheRet;
}


static int 
rewriteExtendPreStmt(StringInfo inBuf)
{
    char *stmt = inBuf->data + inBuf->offset;
    if (strncasecmp(stmt, "commit", 6) == 0)
    {
        return rewriteExtendPreStmt_(inBuf, SRWT_COMMIT);
    }
    else if (strncasecmp(stmt, "rollback", 8) == 0)
    {
        return rewriteExtendPreStmt_(inBuf, SRWT_ROLLBACK);
    }
    else if (strncasecmp(stmt, "begin", 5) == 0)
    {
        return 3;
    }
    else
    {
        return rewriteExtendPreStmt_(inBuf, SRWT_UNKNOWN);
    }
}

static int 
cacheExtendPreStmt(StringInfo inBuf, 
                   sqlReadWriteType readWriteType, 
                   preStmtInfo **pSI)
{
    *pSI = NULL;
    for (int i = 1; i < PRE_STMT_NUM_MAX; i++)
    {
        *pSI = &preStmtInfoes[i];
        if (!(*pSI)->isValid)
        {
            curExtendPreStmtId = i;
            break;
        }
    }

    if ((*pSI) != NULL)
    {
        snprintf((*pSI)->name, 
                 (EXTEND_PRE_STMT_NAME_LEN + 1), 
                 "unuTsfQ%04d", 
                 curExtendPreStmtId);
        (*pSI)->readWriteType = readWriteType;
        (*pSI)->isValid = true;
        if (readWriteType == SRWT_UNKNOWN)
        {
            return 1;
        }
        else if ((readWriteType == SRWT_COMMIT) || 
                 (readWriteType == SRWT_ROLLBACK))
        {
            return 2;
        }
        else 
        {
            return 0;
        }
    }
    else 
    {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("There are too many extend prepare statments."),
                 errhint("Please reconnect to database server.")));
    }
}

static sqlReadWriteType 
getRWTForExtendExeStmt(StringInfo inBuf)
{
    int preStmtId = 0;
    const char *stmt = inBuf->data + inBuf->offset;
    memcpy(&preStmtId, stmt, 4);
    if (PRE_STMT_NUM_MAX < preStmtId)
    {
        elog(ERROR, 
             "can not find extend prepare stmt for: %d / %s", 
             preStmtId,
             stmt);
        return SRWT_UNKNOWN;
    }

    return preStmtInfoes[preStmtId].readWriteType;
}


static bool
rewriteExtendExeStmt_(StringInfo inBuf)
{
    int newStmtOffset = 0;
    int prepare_stmt_id = 0;
    int paramNum = 0;
    const int nullBitmap_offset = 9;
    int new_params_bind_flag = 0;
    int paramsValueOffset = 0;
    int i = 0;
    char *oldStmt = NULL;
    preStmtInfo* pSI;

    oldStmt = inBuf->data + inBuf->offset;
    memcpy(&prepare_stmt_id, oldStmt, 4);
    if (PRE_STMT_NUM_MAX < prepare_stmt_id)
    {
        elog(ERROR, "extend stmt id(%d) does not exists!", prepare_stmt_id);
        return false;
    }

    pSI = &preStmtInfoes[prepare_stmt_id];
    paramNum = pSI->paramNum;
    if (0 < paramNum) 
    {
        char *newStmt = inBuf->data + RESERVED_FOR_ADDITIONAL_SQL;
        snprintf(newStmt, 27, "execute %s using ", pSI->name);
        newStmtOffset = 26;

        new_params_bind_flag = (int)(oldStmt[nullBitmap_offset + pSI->nullBitmapLength]);
        if (0 != new_params_bind_flag)
        {
            paramsValueOffset = pSI->paramValueOffset1;
            for (i = 0; i < paramNum; i++)
            {
                memcpy(&pSI->paramTypes[i], 
                       (oldStmt + pSI->paramTypeOffset + i * 2), 
                       2);
            }
        }
        else
        {
            paramsValueOffset = pSI->paramValueOffset2;
        }

        if (0 == memcmp((oldStmt + nullBitmap_offset), nullBitmap, pSI->nullBitmapLength))
        {
            for (i = 0; i < paramNum; i++)
            {
                assembleExtendExeStmtParmVal(oldStmt, 
                                             pSI, 
                                             i, 
                                             newStmt, 
                                             &newStmtOffset, 
                                             &paramsValueOffset);
                newStmt[newStmtOffset] = ',';
                ++newStmtOffset;
            }
        }
        else 
        {
            for (i = 0; i < paramNum; i++)
            {
                if ((*(oldStmt + nullBitmap_offset + i / 8) & (1 << (i % 8))) == 0)
                {
                    assembleExtendExeStmtParmVal(oldStmt, 
                                                 &preStmtInfoes[prepare_stmt_id], 
                                                 i, 
                                                 newStmt, 
                                                 &newStmtOffset, 
                                                 &paramsValueOffset);
                }
                else
                {
                    memcpy((newStmt + newStmtOffset), "NULL", 4);
                    newStmtOffset += 4;
                }
                newStmt[newStmtOffset] = ',';
                ++newStmtOffset;
            }
        }
        if (0 < paramNum)
        {
            --newStmtOffset;
        }

        newStmt[newStmtOffset] = '\0';
        inBuf->len = newStmtOffset + 1;
        inBuf->offset = RESERVED_FOR_ADDITIONAL_SQL;
        return true;
    }
    else 
    {
        snprintf(oldStmt, (inBuf->maxlen - 1), "execute %s", pSI->name);
        inBuf->len = 20;
        oldStmt[inBuf->len - 1] = '\0';

        return true;
    }
}


static bool
rewriteExtendExeStmt(StringInfo inBuf)
{
    sqlReadWriteType sRWT = getRWTForExtendExeStmt(inBuf);
    if (sRWT == SRWT_UNKNOWN)
    {
        rewriteExtendExeStmt_(inBuf);
        return true;
    }
    else if (sRWT == SRWT_COMMIT)
    {
        char *stmt = inBuf->data + inBuf->offset;
        memcpy(stmt, "commit\0", 7);
        inBuf->len = 7;

        return true;
    }
    else if (sRWT == SRWT_ROLLBACK)
    {
        char *stmt = inBuf->data + inBuf->offset;
        memcpy(stmt, "rollback\0", 9);
        inBuf->len = 9;

        return true;
    }
    else
    {
        return false;
    }
}


static void
assembleExtendExeStmtParmVal(char *sql, 
                             preStmtInfo *esi, 
                             int paramIndex, 
                             char *newStmt, 
                             int *newStmtOffset, 
                             int *paramsValueOffset)
{
    int parameter_type = 0;
    unsigned char unsigned_type = 0;
    uint64 string_length = 0;
    int lengthEncodeSize = 0;
    int64 value_int = 0;
    char value_buf[1024];
    int value_buf_len = 1024;
    int value_len = 0;
    float value_float = 0;
    double value_double = 0;

    parameter_type = esi->paramTypes[paramIndex] & 255;
    unsigned_type = esi->paramTypes[paramIndex] >> 8;
    switch (parameter_type)
    {
    case MYS_TYPE_DECIMAL:
    case MYS_TYPE_NEWDECIMAL:
        lengthEncodeSize = 0;
        string_length = processLenencString(sql + *paramsValueOffset, &lengthEncodeSize);
        memcpy((newStmt + *newStmtOffset), (sql + *paramsValueOffset + lengthEncodeSize), string_length);
        *paramsValueOffset += (lengthEncodeSize + string_length);
        *newStmtOffset += string_length;
        break;
    case MYS_TYPE_TINY:
    case MYS_TYPE_BOOL:
        memcpy(&value_int, (sql + *paramsValueOffset), 1);
        *paramsValueOffset += 1;
        memset(value_buf, 0, value_buf_len);
        if(unsigned_type)
        {
            snprintf(value_buf, (value_buf_len - 1), "%u", (uint8)value_int);
        }
        else
        {
            snprintf(value_buf, (value_buf_len - 1), "%d", (int8)value_int);
        }
        value_len = strlen(value_buf);
        memcpy((newStmt + *newStmtOffset), value_buf, value_len);
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_SHORT:
    case MYS_TYPE_YEAR:
        memcpy(&value_int, (sql + *paramsValueOffset), 2);
        *paramsValueOffset += 2;
        memset(value_buf, 0, value_buf_len);
        if(unsigned_type)
        {
            snprintf(value_buf, (value_buf_len - 1), "%u", (uint16)value_int);
        }
        else
        {
            snprintf(value_buf, (value_buf_len - 1), "%d", (int16)value_int);
        }
        value_len = strlen(value_buf);
        memcpy((newStmt + *newStmtOffset), value_buf, value_len);
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_LONG:
    case MYS_TYPE_INT24:
        memcpy(&value_int, (sql + *paramsValueOffset), 4);
        *paramsValueOffset += 4;
        memset(value_buf, 0, value_buf_len);
        if(unsigned_type)
        {
            snprintf(value_buf, (value_buf_len - 1), "%u", (uint)value_int);
        }
        else
        {
            snprintf(value_buf, (value_buf_len - 1), "%d", (int)value_int);
        }
        value_len = strlen(value_buf);
        memcpy((newStmt + *newStmtOffset), value_buf, value_len);
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_LONGLONG:
        memcpy(&value_int, (sql + *paramsValueOffset), 8);
        *paramsValueOffset += 8;
        memset(value_buf, 0, value_buf_len);
        if(unsigned_type)
        {
            snprintf(value_buf, (value_buf_len - 1), "%lu", (uint64)value_int);
        }
        else
        {
            snprintf(value_buf, (value_buf_len - 1), "%ld", (int64)value_int);
        }
        value_len = strlen(value_buf);
        memcpy((newStmt + *newStmtOffset), value_buf, value_len);
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_FLOAT:
        memcpy(&value_float, (sql + *paramsValueOffset), 4);
        *paramsValueOffset += 4;
        memset(value_buf, 0, value_buf_len);
        snprintf(value_buf, (value_buf_len - 1), "%f", value_float);
        value_len = strlen(value_buf);
        memcpy((newStmt + *newStmtOffset), value_buf, value_len);
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_DOUBLE:
        memcpy(&value_double, (sql + *paramsValueOffset), 8);
        *paramsValueOffset += 8;
        memset(value_buf, 0, value_buf_len);
        snprintf(value_buf, (value_buf_len - 1), "%f", value_double);
        value_len = strlen(value_buf);
        memcpy((newStmt + *newStmtOffset), value_buf, value_len);
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_DATE:
        *paramsValueOffset += processDateType((const unsigned char *)sql + *paramsValueOffset, value_buf, value_buf_len);
        value_len = strlen(value_buf) + 2;  /* 2: '' */
        snprintf((newStmt + *newStmtOffset), (value_len + 1), "'%s'", value_buf);    /* 1: '\0' */
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_TIME:
        *paramsValueOffset += processTimeType((const unsigned char *)sql + *paramsValueOffset, value_buf, value_buf_len);
        value_len = strlen(value_buf) + 2;  /* 2: '' */
        snprintf((newStmt + *newStmtOffset), (value_len + 1), "'%s'", value_buf);    /* 1: '\0' */
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_DATETIME:
    case MYS_TYPE_TIMESTAMP:
        *paramsValueOffset += processDatetimeTimestampType((const unsigned char *)sql + *paramsValueOffset, 
                                                           value_buf, 
                                                           value_buf_len);
        value_len = strlen(value_buf) + 2;  /* 2: '' */
        snprintf((newStmt + *newStmtOffset), (value_len + 1), "'%s'", value_buf);    /* 1: '\0' */
        *newStmtOffset += value_len;
        break;
    case MYS_TYPE_TINY_BLOB:
    case MYS_TYPE_MEDIUM_BLOB:
    case MYS_TYPE_LONG_BLOB:
    case MYS_TYPE_BLOB:
        lengthEncodeSize = 0;
        string_length = processLenencString(sql + *paramsValueOffset, &lengthEncodeSize);
        *paramsValueOffset += lengthEncodeSize;
        newStmt[(*newStmtOffset)++] = '\'';
        memcpy((newStmt + *newStmtOffset), (sql + *paramsValueOffset), string_length);
        *paramsValueOffset += string_length;
        *newStmtOffset += string_length;
        newStmt[(*newStmtOffset)++] = '\'';
        break;
    case MYS_TYPE_VARCHAR:
    case MYS_TYPE_VAR_STRING:
    case MYS_TYPE_STRING:
    case MYS_TYPE_ENUM:
    case MYS_TYPE_SET:
    case MYS_TYPE_JSON:
        lengthEncodeSize = 0;
        string_length = processLenencString(sql + *paramsValueOffset, &lengthEncodeSize);
        *paramsValueOffset += lengthEncodeSize;
        newStmt[(*newStmtOffset)++] = '\'';
        memcpy((newStmt + *newStmtOffset), (sql + *paramsValueOffset), string_length);
        *paramsValueOffset += string_length;
        *newStmtOffset += string_length;
        newStmt[(*newStmtOffset)++] = '\'';
        break;
    case MYS_TYPE_NULL:
    case MYS_TYPE_NEWDATE:
    case MYS_TYPE_BIT: 
    case MYS_TYPE_TIMESTAMP2:
    case MYS_TYPE_DATETIME2:
    case MYS_TYPE_TIME2:
    case MYS_TYPE_TYPED_ARRAY:
    case MYS_TYPE_INVALID:
        break;
    }   /* switch (parameter_type) */
}

static bool
rewriteExtendCloseStmt(StringInfo inBuf)
{
    char *stmt = inBuf->data + inBuf->offset;
    preStmtInfo *pSI;
    int prepare_stmt_id = 0;
    memcpy(&prepare_stmt_id, stmt, 4);
    if (PRE_STMT_NUM_MAX < prepare_stmt_id)
    {
        elog(ERROR, "extend stmt id(%d) does not exists!", prepare_stmt_id);
        return false;
    }

    pSI = &preStmtInfoes[prepare_stmt_id];
    snprintf(stmt, 
             (inBuf->maxlen - 1), 
             "deallocate prepare %s", 
             pSI->name);
    inBuf->len = 31;
    pSI->isValid = false;
    return true;
}

static bool 
convertExtendPreStmt(char *oldStmt, int oldStmtLen, int remainSpace, 
                     char *newStmt, int *newStmtIndex, int *paramNum)
{
    char leftQuotationMark;
    int leftQuotationMarksNum;

    leftQuotationMark = ' ';
    leftQuotationMarksNum = 0;
    *paramNum = 0;
    for (int i = 0; i < oldStmtLen; i++)
    {
        newStmt[(*newStmtIndex)++] = oldStmt[i];

        if (oldStmt[i] == '\\')
        {
            i += 1;
            newStmt[(*newStmtIndex)++] = oldStmt[i];
        }
        else if (oldStmt[i] == '"')
        {
            leftQuotationMarksNum += 1;
            if (leftQuotationMark == ' ')
            {
                leftQuotationMark = '"';
                newStmt[(*newStmtIndex)++] = oldStmt[i];
                remainSpace -= 1;
                if (remainSpace <= 0)
                {
                    return false;
                }
            }
            else
            {
                if ((leftQuotationMarksNum % 2) == 0)
                {
                    leftQuotationMarksNum = 0; 
                    leftQuotationMark = ' ';
                    newStmt[(*newStmtIndex)++] = oldStmt[i];
                    remainSpace -= 1;
                    if (remainSpace <= 0)
                    {
                        return false;
                    }
                }
            }
        }
        else if (oldStmt[i] == '?')
        {
            if (leftQuotationMark == ' ')
            {
                *paramNum += 1;
            }
        }
    }

    return true;
}

static void 
resetConnection(StringInfo inBuf)
{
    char *stmt = inBuf->data + inBuf->offset;

    isResetConnStmt = true;

    memcpy(stmt, "rollback;", 9);
    inBuf->len = 9;

    autoCommit = 2;

    affectedRows = -1;
    lastInsertID = 0;
    lastInsertIDForPacket = 0;

    memcpy((stmt + inBuf->len), "deallocate prepare all", 22);
    inBuf->len += 22;
    for (int i = 0; i < preStmtInfoNum; i++)
    {
        preStmtInfoes[i].isValid = false;
    }
    preStmtInfoNum = 0;

    clearUserVars();

    stmt[inBuf->len] = '\0';
    inBuf->len += 1;
}

static void 
sendOKPacketSpecialInfo(const char *info)
{
    char *payloadBuf = NULL;
    int payloadLen = 0;

    serverStatus = sessionStateChanged | moreResultsFlag | autoCommit | inTransactionFlag;
    netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);
    payloadLen = assembleOKPacketPayload(serverStatus, warnings, payloadBuf);
    netTransceiver->writePacketHeaderNoFlush(payloadLen);
    netTransceiver->finishWriteToBufFlush(payloadLen);
}

static void 
sendEOFPacketFlush()
{
    char *payloadBuf = NULL;
    int payloadLen = 0;

    serverStatus = sessionStateChanged | moreResultsFlag | autoCommit | inTransactionFlag;
    netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);
    payloadLen = assembleEOFPacketPayload(warnings, serverStatus, payloadBuf);
    netTransceiver->writePacketHeaderNoFlush(payloadLen);
    netTransceiver->finishWriteToBufFlush(payloadLen);
}

static void 
sendEOFPacketNoFlush()
{
    if (!(frontEndCaps & MYS_CLT_DEPRECATE_EOF))
    {
        char *payloadBuf = NULL;
        int payloadLen = 0;

        serverStatus = sessionStateChanged | moreResultsFlag | autoCommit | inTransactionFlag;
        netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);
        payloadLen = assembleEOFPacketPayload(warnings, serverStatus, payloadBuf);
        netTransceiver->writePacketHeaderNoFlush(payloadLen);
        netTransceiver->finishWriteToBufNoFlush(payloadLen);
    }
}

static void 
sendErrPacket(int errCode, const char *errMsg, const char *sqlState)
{
    char *payloadBuf;
    int payloadLen;
    int mySQLErrorCode;
    int sqlStateMarker;
    int msgLen;

    mySQLErrorCode = convertErrorCode(errCode);
    warnErrCode = mySQLErrorCode;
    msgLen = strlen(errMsg);
    msgLen = msgLen > 1023 ? 1023 : msgLen;
    memcpy(warnErrMsg, errMsg, msgLen);
    warnErrMsg[msgLen] = '\0';
    sqlStateMarker = 0;
    netTransceiver->getWriteBufForHeaderPayload(&payloadBuf);

    payloadLen = assembleErrorPacketPayload(mySQLErrorCode, 
                                            sqlStateMarker, 
                                            sqlState, 
                                            errMsg, 
                                            payloadBuf);
    netTransceiver->writePacketHeaderNoFlush(payloadLen);
    netTransceiver->finishWriteToBufFlush(payloadLen);
}

static void 
sendSyntaxError(const char *command)
{
    elog(ERROR, "Systax error. %s", command);
}

static void
sendUnsupportError(const char *command)
{
    int errCode = 1047;
    const char *errMsg = "Unknown command";
    const char *sqlState = "08S01";
    sendErrPacket(errCode, errMsg, sqlState);
    elog(WARNING, "Halo-MySQL unsupport sql: [%s]", command);
}

static void 
initHaloMySqlDataTypesHashTable(void)
{
	HASHCTL hashctl;
	hashctl.keysize = sizeof(int);
	hashctl.entrysize = sizeof(HaloMySQLDataType);
	haloMysqlDataTypes = hash_create("Halo MySQL(914) Data Types", 
                                     8192, 
                                     &hashctl, 
                                     HASH_ELEM | HASH_BLOBS);
    haloMysqlDataTypesNum = 0;
}

static void 
initPreStmtInfoes(void)
{
    int i = 0;
    int j = 0;
    for (i = 0; i < PRE_STMT_NUM_MAX; i++)
    {
        preStmtInfoes[i].name[PRE_STMT_NAME_LEN] = '\0';
        preStmtInfoes[i].paramNum = 0;
        preStmtInfoes[i].nullBitmapLength = 0;
        preStmtInfoes[i].paramTypeOffset = 0;
        preStmtInfoes[i].paramValueOffset1 = 0;
        preStmtInfoes[i].paramValueOffset2 = 0;
        for (j = 0; j < 256; j++)
        {
            preStmtInfoes[i].paramTypes[j] = 0;
        }
        preStmtInfoes[i].columnNum = 0;
        for (j = 0; j < 256; j++)
        {
            preStmtInfoes[i].columnTypes[j] = 0;
            preStmtInfoes[i].columnCharacterSet[j] = 0;
        }
        preStmtInfoes[i].readWriteType = SRWT_UNKNOWN;
        preStmtInfoes[i].isValid = false;
    }
    memset(nullBitmap, 0, 256);
}

void
mys_setCurrentPreStmtColumnInfo(int currentPreStmtColumnIndex, Oid currentPreStmtColumnType)
{
    preStmtInfo *currentPreStmt = &preStmtInfoes[curExtendPreStmtId];
    int currentPreStmtColumnMySQLType = getMySQLDataType(currentPreStmtColumnType);
    int currentPreStmtColumnCharacterSet;

    currentPreStmt->columnNum++;
    currentPreStmt->columnTypes[currentPreStmtColumnIndex] = currentPreStmtColumnMySQLType;

    if ((currentPreStmtColumnMySQLType == MYS_TYPE_TINY) || 
        (currentPreStmtColumnMySQLType == MYS_TYPE_SHORT) || 
        (currentPreStmtColumnMySQLType == MYS_TYPE_LONG) || 
        (currentPreStmtColumnMySQLType == MYS_TYPE_INT24) || 
        (currentPreStmtColumnMySQLType == MYS_TYPE_LONGLONG) || 
        (currentPreStmtColumnMySQLType == MYS_TYPE_NEWDECIMAL) || 
        (currentPreStmtColumnMySQLType == MYS_TYPE_FLOAT) ||
        (currentPreStmtColumnMySQLType == MYS_TYPE_DOUBLE))
    {
        currentPreStmtColumnCharacterSet = 63;
    }
    else if (currentPreStmtColumnMySQLType == MYS_TYPE_STRING)
    {
        char *columnTypeName = getTypeName(currentPreStmtColumnType);
        if (memcmp(columnTypeName, "binary", 6) != 0)
        {
            currentPreStmtColumnCharacterSet = 33;
        }
        else 
        {
            currentPreStmtColumnCharacterSet = 63;
        }
    }
    else if (currentPreStmtColumnMySQLType == MYS_TYPE_VAR_STRING)
    {
        char *columnTypeName = getTypeName(currentPreStmtColumnType);
        if (memcmp(columnTypeName, "varbinary", 9) != 0)
        {
            currentPreStmtColumnCharacterSet = 33;
        }
        else 
        {
            currentPreStmtColumnCharacterSet = 63;
        }
    }
    else if (currentPreStmtColumnMySQLType == MYS_TYPE_BIT)
    {
        currentPreStmtColumnCharacterSet = 63;
    }
    else if (currentPreStmtColumnMySQLType == MYS_TYPE_BLOB)
    {
        char *columnTypeName = getTypeName(currentPreStmtColumnType);
        if (memcmp(columnTypeName, "blob", 4) == 0)
        {
            currentPreStmtColumnCharacterSet = 63;
        }
        else 
        {
            currentPreStmtColumnCharacterSet = 33;
        }
    }
    else 
    {
        currentPreStmtColumnCharacterSet = 33;
    }

    currentPreStmt->columnCharacterSet[currentPreStmtColumnIndex] = currentPreStmtColumnCharacterSet;
}

static void 
initCustomizedCaseColumnNames(void)
{
    Relation relation;
    Snapshot snapshot;
    TableScanDesc scan;
    TupleTableSlot *slot;
    Oid schemaId;
    Oid tableId;
    HASHCTL hashctl;
    MemoryContext oldContext;

    oldContext = MemoryContextSwitchTo(TopMemoryContext);

    hashctl.keysize = CUSTOMIZED_CASE_COLUMN_NAME_LEN;
    hashctl.entrysize = sizeof(ScheCustomizedCaseColumNames);
    schesCustomizedCaseColumNames = hash_create("Halo MySQL(914) Case Column Names",
                                                1024,
                                                &hashctl,
                                                HASH_ELEM | HASH_STRINGS);

    schemaId = get_namespace_oid("mys_informa_schema", false);
    tableId = get_relname_relid("schema_table_columns", schemaId);
    relation = table_open(tableId, AccessShareLock);
    snapshot = RegisterSnapshot(GetLatestSnapshot());
    scan = table_beginscan(relation, snapshot, 0, NULL);
    slot = table_slot_create(relation, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
    {
        char *schemaName;
        char *tableName;
        char *columnName;

        TupleDesc typeinfo;
        int columnCount;

        typeinfo = slot->tts_tupleDescriptor;
        columnCount = typeinfo->natts;
        if (columnCount != 3)
        {
            elog(ERROR, "invalid data in mys_informa_schema.schema_table_columns tables");
        }

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
                char *val;
                getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);
                val = OidOutputFunctionCall(typoutput, attr);
                if (i == 0)
                {
                    schemaName = val;
                }
                else if (i == 1)
                {
                    tableName = val;
                }
                else
                {
                    columnName = val;
                }
            }
            else
            {
                elog(ERROR, "invalid data in mys_informa_schema.schema_table_columns tables(column values is null)");
            }
        }

        initCustomizedCaseColumnName(schemaName, tableName, columnName);
    }
    table_endscan(scan);
    UnregisterSnapshot(snapshot);
    ExecDropSingleTupleTableSlot(slot);
    table_close(relation, AccessShareLock);
    MemoryContextSwitchTo(oldContext);
}

static void
initCustomizedCaseColumnName(const char *schemaName, const char *tableName, const char *columnName)
{
    bool found;
    ScheCustomizedCaseColumNames *sCCCN = NULL;
    TabCustomizedCaseColumNames *tCCCN = NULL;
    CustomizedCaseColumName2 *cCCCN = NULL;
    char srcColName[CUSTOMIZED_CASE_COLUMN_NAME_LEN];
    int columnNameLen;

    sCCCN = (ScheCustomizedCaseColumNames *)hash_search(schesCustomizedCaseColumNames,
                                                        schemaName,
                                                        HASH_ENTER,
                                                        &found);
    if (!found)
    {
        HASHCTL hashctl;
        hashctl.keysize = CUSTOMIZED_CASE_COLUMN_NAME_LEN;
        hashctl.entrysize = sizeof(TabCustomizedCaseColumNames);
        sCCCN->tabsCustomizedCaseColumNames =
            hash_create("Halo MySQL(914) Table Case Column Names",
                        8192,
                        &hashctl,
                        HASH_ELEM | HASH_STRINGS);
    }

    tCCCN = (TabCustomizedCaseColumNames *)hash_search(sCCCN->tabsCustomizedCaseColumNames,
                                                       tableName,
                                                       HASH_ENTER,
                                                       &found);
    if (!found)
    {
        HASHCTL hashctl;
        hashctl.keysize = CUSTOMIZED_CASE_COLUMN_NAME_LEN;
        hashctl.entrysize = sizeof(CustomizedCaseColumName2);
        tCCCN->colsCustomizedCaseColumNames =
            hash_create("Halo MySQL(914) Column Case Column Names",
                        2048,
                        &hashctl,
                        HASH_ELEM | HASH_STRINGS);
    }

    columnNameLen = strlen(columnName);
    srcColName[columnNameLen] = '\0';
    for (int i = 0; i < columnNameLen; i++)
    {
        if (('A' <= columnName[i]) && (columnName[i] <= 'Z'))
        {
            srcColName[i] = columnName[i] + 32;
        }
        else
        {
            srcColName[i] = columnName[i];
        }
    }
    cCCCN = (CustomizedCaseColumName2 *)hash_search(tCCCN->colsCustomizedCaseColumNames,
                                                    &srcColName,
                                                    HASH_ENTER,
                                                    &found);
    if (!found)
    {
        memcpy(cCCCN->destColName, columnName, columnNameLen);
        cCCCN->destColNameLen = columnNameLen;
    }
}

static void 
initTableInfoesHashTable(void)
{
	HASHCTL hashctl;
	hashctl.keysize = sizeof(int);
	hashctl.entrysize = sizeof(TableInfoes);
    tableInfoeses = hash_create("Halo MySQL(914) TableInfoes", 
                                32768, 
                                &hashctl, 
                                HASH_ELEM | HASH_BLOBS);
}

static void 
initSessionTrackSystemVariables(void)
{
    memcpy(sessionTrackSystemVariables, 
           "time_zone,autocommit,character_set_client,character_set_results,character_set_connection", 
           88);
    sessionTrackSystemVariables[88] = '\0';
}

static void 
endExtendPreStmt(void)
{
    int payloadLen = 0;
    char payloadBuf[1024];
    int payloadBufIndex = 0;

    int header = 0x00;
    const int header_len = 1;
    const int stmt_id_len = 4;
    int columnNum = preStmtInfoes[curExtendPreStmtId].columnNum;
    const int columnNumLen = 2;
    const int params_num_len = 2;
    int reserved = 0;
    const int reserved_len = 1;
    const int warning_count_len = 2;
    int metadata_follows = 0;
    const int metadata_follows_len = 1;

    memcpy((payloadBuf + payloadBufIndex), &header, header_len);
    payloadBufIndex += header_len;
    memcpy((payloadBuf + payloadBufIndex), &curExtendPreStmtId, stmt_id_len);
    payloadBufIndex += stmt_id_len;
    memcpy((payloadBuf + payloadBufIndex), &columnNum, columnNumLen);
    payloadBufIndex += columnNumLen;
    memcpy((payloadBuf + payloadBufIndex), &preStmtInfoes[curExtendPreStmtId].paramNum, params_num_len);
    payloadBufIndex += params_num_len;
    memcpy((payloadBuf + payloadBufIndex), &reserved, reserved_len);
    payloadBufIndex += reserved_len;
    memcpy((payloadBuf + payloadBufIndex), &warnings, warning_count_len);
    payloadBufIndex += warning_count_len;
    if (frontEndCaps & MYS_CLT_OPTIONAL_RESULTSET_METADATA)
    {
        memcpy((payloadBuf + payloadBufIndex), &metadata_follows, metadata_follows_len);
        payloadBufIndex += metadata_follows_len;
    }
    netTransceiver->writePacketHeaderPayloadFlush(payloadBuf, payloadBufIndex);

    payloadBufIndex = 0;
    if (((0 < preStmtInfoes[curExtendPreStmtId].paramNum) && 
         !(frontEndCaps & MYS_CLT_OPTIONAL_RESULTSET_METADATA)) || 
        (metadata_follows == 1))
    {
        for (int i = 0; i < preStmtInfoes[curExtendPreStmtId].paramNum; i++)
        {
            const char *schemaName = "";
            const char *tableName = "";
            const char *orgTableName = "";
            const char *columnName = "?";
            const char *orgColumnName = "";
            int characterSet = 63;
            int columnLen = 0;
            int columnType = 253;
            int columnFlags = 0x0080; 
            int decimals = 0x00;
            payloadLen = assembleColDefPacketPayload(schemaName, 
                                                     tableName, 
                                                     orgTableName, 
                                                     columnName, 
                                                     orgColumnName, 
                                                     characterSet, 
                                                     columnLen, 
                                                     columnType, 
                                                     columnFlags, 
                                                     decimals, 
                                                     payloadBuf);
            netTransceiver->writePacketHeaderPayloadFlush(payloadBuf, payloadLen);
        }
        if (!(frontEndCaps & MYS_CLT_DEPRECATE_EOF))
        {
            sendEOFPacketFlush();
        }
    }

    if (((0 < columnNum) && !(frontEndCaps & MYS_CLT_OPTIONAL_RESULTSET_METADATA)) || 
        (metadata_follows == 1))
    {
        for (int i = 0; i < columnNum; i++)
        {
            const char *schemaName = "";
            const char *tableName = "";
            const char *orgTableName = "";
            const char *columnName = "?";
            const char *orgColumnName = "";
            int characterSet = preStmtInfoes[curExtendPreStmtId].columnCharacterSet[i];
            int columnLen = 0;
            int columnType = preStmtInfoes[curExtendPreStmtId].columnTypes[i];
            int columnFlags = 0x0080; 
            int decimals = 0x00;

            payloadLen = assembleColDefPacketPayload(schemaName, 
                                                    tableName, 
                                                    orgTableName, 
                                                    columnName, 
                                                    orgColumnName, 
                                                    characterSet, 
                                                    columnLen, 
                                                    columnType, 
                                                    columnFlags, 
                                                    decimals, 
                                                    payloadBuf);
            netTransceiver->writePacketHeaderPayloadFlush(payloadBuf, payloadLen);
        }
        if (!(frontEndCaps & MYS_CLT_DEPRECATE_EOF))
        {
            sendEOFPacketFlush();
        }
    }
}

static void 
endNormalPreStmt(void)
{
    char *info = "Statement prepared";
    sendOKPacketSpecialInfo(info);
}

static char *
getTypeName(int pgDataType)
{
    char *typeName = NULL;
    HeapTuple tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(pgDataType));
    if (HeapTupleIsValid(tuple))
    {
        Form_pg_type pgType = (Form_pg_type)GETSTRUCT(tuple);
        typeName = pgType->typname.data;
    }
    ReleaseSysCache(tuple);

    return typeName;
}


static int 
getMySQLDataType(int haloDataType)
{
    HaloMySQLDataType *ret = NULL;
    bool found = false;

    ret = (HaloMySQLDataType *)hash_search(haloMysqlDataTypes, 
                                           &haloDataType, 
                                           HASH_ENTER, 
                                           &found);
    if (found)
    {
        return ret->mySQLDataType;
    }
    else
    {
        char *type_name;
        haloMysqlDataTypesNum++;
        type_name = getTypeName(haloDataType);
        if (memcmp(type_name, "tinyint", 7) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_TINY;
        }
        else if ((memcmp(type_name, "smallint", 8) == 0) || 
                 (memcmp(type_name, "int2", 4) == 0))
        {
            ret->mySQLDataType = MYS_TYPE_SHORT;
        }
        else if (memcmp(type_name, "mediumint", 9) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_INT24;
        }
        else if ((memcmp(type_name, "bigint", 6) == 0) || 
                 (memcmp(type_name, "int8", 4) == 0))
        {
            ret->mySQLDataType = MYS_TYPE_LONGLONG;
        }
        else if ((memcmp(type_name, "int", 3) == 0) ||
                 (memcmp(type_name, "int4", 4) == 0))
        {
            ret->mySQLDataType = MYS_TYPE_LONG;
        }
        else if ((memcmp(type_name, "boolean", 7) == 0) || 
                 (memcmp(type_name, "bool", 4) == 0))
        {
            ret->mySQLDataType = MYS_TYPE_TINY;
        }
        else if ((memcmp(type_name, "bpchar", 6) == 0) || 
                 (memcmp(type_name, "char", 4) == 0))
        {
            ret->mySQLDataType = MYS_TYPE_STRING;
        }
        else if (memcmp(type_name, "varchar", 7) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_VAR_STRING;
        }
        else if (memcmp(type_name, "datetime", 8) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_DATETIME;
        }
        else if (memcmp(type_name, "date", 4) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_DATE;
        }
        else if (memcmp(type_name, "timestamp", 9) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_TIMESTAMP;
        }
        else if (memcmp(type_name, "time", 4) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_TIME;
        }
        else if (memcmp(type_name, "float", 5) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_FLOAT;
        }
        else if (memcmp(type_name, "real", 4) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_DOUBLE;
        }
        else if (memcmp(type_name, "double", 6) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_DOUBLE;
        }
        else if (memcmp(type_name, "numeric", 7) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_NEWDECIMAL;
        }
        else if (memcmp(type_name, "decimal", 7) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_NEWDECIMAL;
        }
        else if (memcmp(type_name, "dec", 3) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_NEWDECIMAL;
        }
        else if (memcmp(type_name, "bytea", 5) == 0)
        {
            elog(WARNING, "can not use bytea as column type! please use 'blob/longblob/varbit'");
            ret->mySQLDataType = MYS_TYPE_BLOB;
        }
        else if (memcmp(type_name, "blob", 4) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_BLOB;
        }
        else if (memcmp(type_name, "name", 4) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_BLOB;
        }
        else if (memcmp(type_name, "text", 4) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_BLOB;
        }
        else if (memcmp(type_name, "varbit", 6) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_BIT;
        }
        else if (memcmp(type_name, "binary", 6) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_STRING;
        }
        else if (memcmp(type_name, "varbinary", 9) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_VAR_STRING;
        }
        else if (memcmp(type_name, "enum_", 5) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_ENUM;
        }
        else if (memcmp(type_name, "set_", 4) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_SET;
        }
        else if (memcmp(type_name, "year_", 5) == 0)
        {
            ret->mySQLDataType = MYS_TYPE_YEAR;
        }
        else 
        {
            ret->mySQLDataType = MYS_TYPE_BLOB;
        }

        if (haloMysqlDataTypesNum <= 8192)
        {
            return ret->mySQLDataType;
        }
        else 
        {
            int rt = ret->mySQLDataType;
            hash_destroy(haloMysqlDataTypes);
            initHaloMySqlDataTypesHashTable();
            elog(NOTICE, "re-initHaloMySqlDataTypesHashTable");
            return rt;
        }
    }
}

static int 
calcLenencIntLen(int len) 
{
    int ret = 0;

    if (len < 251)
    {
        ret = 1;
    }
    else if (len < 0xffff)
    {
        ret = 3;
    }
    else if (len < 0xffffff)
    {
        ret = 4;
    }
    else 
    {
        ret = 9;
    }

    return ret;
}

static void
convertColumnValue(TupleTableSlot *slot, Datum attr, int columnNum, 
                   char **columnVal, int *columnValLen)
{
    TupleDesc typeinfo = slot->tts_tupleDescriptor;
    Form_pg_attribute att = TupleDescAttr(typeinfo, columnNum);
    Oid typoutput;
    bool typisvarlena;
    int columnType;
    Oid baseType;

    getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);
    *columnVal = OidOutputFunctionCall(typoutput, attr);
    columnType = getMySQLDataType(att->atttypid);
    baseType = getBaseType(att->atttypid);
    if (baseType == BYTEAOID)
    {
        *columnValLen = convertHexToMySQLBinary(*columnVal);
    }
    else
    {
        if (columnType == MYS_TYPE_STRING)
        {
            char *columnTypeName = getTypeName(att->atttypid);
            if (memcmp(columnTypeName, "binary", 6) == 0)
            {
                *columnValLen = convertHexToMySQLBinary(*columnVal);
            }
            else 
            {
                *columnValLen = strlen(*columnVal);
            }
        }
        else if (columnType == MYS_TYPE_VAR_STRING)
        {
            char *columnTypeName = getTypeName(att->atttypid);
            if (memcmp(columnTypeName, "varbinary", 9) == 0)
            {
                *columnValLen = convertHexToMySQLBinary(*columnVal);
            }
            else 
            {
                *columnValLen = strlen(*columnVal);
            }
        }
        else if (columnType == MYS_TYPE_BLOB)
        {
            char *columnTypeName = getTypeName(att->atttypid);
            if (memcmp(columnTypeName, "blob", 4) == 0)
            {
                *columnValLen = convertHexToMySQLBinary(*columnVal);
            }
            else 
            {
                *columnValLen = strlen(*columnVal);
            }
        }
        else if (columnType == MYS_TYPE_BIT)
        {  
            char *hex = NULL;
            int hexSize = 0;
            if (0 < att->atttypmod)
            {
                hexSize = (((att->atttypmod) + 8 - 1) & ~(8 - 1)) / 8;
            }
            else 
            {
                hexSize = strlen(*columnVal);
            }
            hex = palloc0(hexSize);
            convertBitToMySQLHex(*columnVal, hex, hexSize);
            *columnVal = hex;
            *columnValLen = hexSize;
        }
        else 
        {
            *columnValLen = strlen(*columnVal);
        }
    }
}

static void 
convertBitToMySQLHex(char *bit, char *hex, int hexSize)
{
    int bitLen = strlen(bit);
    int bitIndex = bitLen - 1;
    int hexIndex = hexSize * 2 - 1;
    while (true)
    {
        int decimal = 0;
        int subLen = 0;
        if (4 <= bitIndex)
        {
            subLen = 4;
        }
        else 
        {
            subLen = bitIndex + 1;
        }
        for (int j = 0; j < subLen; j++) 
        {
            decimal += ((bit[bitIndex + 1 - subLen + j] - '0') * (1 << (subLen - j - 1)));
        }
        if ((hexIndex % 2) == 1)
        {
            hex[hexIndex / 2] |= decimal;
        }
        else 
        {
            hex[hexIndex / 2] |= decimal << 4;
        }

        bitIndex -= subLen;
        --hexIndex;

        if (bitIndex < 0)
        {
            break;
        }
    }
}

static char 
convertToHex(char ch1, char ch2)
{
    char ret;

    if (ch1 == '0') 
    {
        ret = 0x0f;
    } 
    else if (ch1 == '1') 
    {
        ret = 0x1f;
    } 
    else if (ch1 == '2') 
    {
        ret = 0x2f;
    } 
    else if (ch1 == '3') 
    {
        ret = 0x3f;
    } 
    else if (ch1 == '4') 
    {
        ret = 0x4f;
    } 
    else if (ch1 == '5') 
    {
        ret = 0x5f;
    } 
    else if (ch1 == '6') 
    {
        ret = 0x6f;
    } 
    else if (ch1 == '7') 
    {
        ret = 0x7f;
    } 
    else if (ch1 == '8') 
    {
        ret = 0x8f;
    } 
    else if (ch1 == '9') 
    {
        ret = 0x9f;
    } 
    else if (ch1 == 'a') 
    {
        ret = 0xaf;
    } 
    else if (ch1 == 'b') 
    {
        ret = 0xbf;
    } 
    else if (ch1 == 'c') 
    {
        ret = 0xcf;
    } 
    else if (ch1 == 'd') 
    {
        ret = 0xdf;
    } 
    else if (ch1 == 'e') 
    {
        ret = 0xef;
    } 
    else {
        ret = 0xff;
    } 

    if (ch2 == '0') 
    {
        ret &= 0xf0;
    } 
    else if (ch2 == '1') 
    {
        ret &= 0xf1;
    } 
    else if (ch2 == '2') 
    {
        ret &= 0xf2;
    } 
    else if (ch2 == '3') 
    {
        ret &= 0xf3;
    } 
    else if (ch2 == '4') 
    {
        ret &= 0xf4;
    } 
    else if (ch2 == '5') 
    {
        ret &= 0xf5;
    } 
    else if (ch2 == '6') 
    {
        ret &= 0xf6;
    } 
    else if (ch2 == '7') 
    {
        ret &= 0xf7;
    } 
    else if (ch2 == '8') 
    {
        ret &= 0xf8;
    } 
    else if (ch2 == '9') 
    {
        ret &= 0xf9;
    } 
    else if (ch2 == 'a') 
    {
        ret &= 0xfa;
    } 
    else if (ch2 == 'b') 
    {
        ret &= 0xfb;
    } 
    else if (ch2 == 'c') 
    {
        ret &= 0xfc;
    } 
    else if (ch2 == 'd') 
    {
        ret &= 0xfd;
    } 
    else if (ch2 == 'e') 
    {
        ret &= 0xfe;
    } 
    else {
        ret &= 0xff;
    }

    return ret;
}

static int 
convertCharToInt(char ch)
{
    int ret;

    if (ch == '0')
    {
        ret = 0x0;
    }
    else if (ch == '1')
    {
        ret = 0x1;
    }
    else if (ch == '2')
    {
        ret = 0x2;
    }
    else if (ch == '3')
    {
        ret = 0x3;
    }
    else if (ch == '4')
    {
        ret = 0x4;
    }
    else if (ch == '5')
    {
        ret = 0x5;
    }
    else if (ch == '6')
    {
        ret = 0x6;
    }
    else if (ch == '7')
    {
        ret = 0x7;
    }
    else if (ch == '8')
    {
        ret = 0x8;
    }
    else if (ch == '9')
    {
        ret = 0x9;
    }
    else if (ch == 'a' || ch == 'A')
    {
        ret = 0xA;
    }
    else if (ch == 'b' || ch == 'B')
    {
        ret = 0xB;
    }
    else if (ch == 'c' || ch == 'C')
    {
        ret = 0xC;
    }
    else if (ch == 'd' || ch == 'D')
    {
        ret = 0xD;
    }
    else if (ch == 'e' || ch == 'E')
    {
        ret = 0xE;
    }
    else 
    {
        ret = 0xF;
    }

    return ret;
}

static int 
convertHexToMySQLBinary(char *str)
{
    if (memcmp((str + 2), "5f62696e617279", 14) == 0)
    {
        int len = (int)strlen(str);
        int j = 0;
        char *newStr = str;
        for (int i = 16; i < len; i += 2)
        {
            newStr[j++] = convertToHex(str[i], str[i + 1]);
        }

        len = j;
        j = 0;
        for (int i = 0; i < len; i++)
        {
            if (str[i] == '\\') 
            {
                if (str[i + 1] == '0') 
                {
                    newStr[j++] = 0;
                    ++i;
                }
                else 
                {
                    newStr[j++] = str[++i];
                }
            }
            else 
            {
                newStr[j++] = str[i];
            }
        }

        return j;
    }
    else if (memcmp((str + 2), "78", 2) == 0)
    {
        int len = (int)strlen(str);
        int j = 0;
        char *newStr = str;
        for (int i = 4; i < len; i += 4)
        {
            int h;
            int l;
            char high;
            char lower;
            high = convertToHex(str[i], str[i + 1]);
            h = convertCharToInt(high);
            lower = convertToHex(str[i + 2], str[i + 3]);
            l = convertCharToInt(lower);
            newStr[j] = h << 4;
            newStr[j++] |= l;
        }

        return j;
    }
    else 
    {
        int len = (int)strlen(str);
        char *newStr = str;
        int j = 0;
        for (int i = 2; i < len; i += 2)
        {
            newStr[j++] = convertToHex(str[i], str[i + 1]);
        }

        return j;
    }
}

static void 
addAdditionalSQL(StringInfo inBuf)
{
    if (RESERVED_FOR_ADDITIONAL_SQL <= inBuf->offset)
    {
        if (needAddCommitOrRollbackAfterCurSQL != 2)
        {
            if (needStartNewTrx && !trxNotCommit)
            {
                inBuf->offset -= 6;
                inBuf->len += 6;
                memcpy((inBuf->data + inBuf->offset), "begin;", 6);
                addBeforeBeforeCurSQL = true;
            }

            if (needCommitTrx && trxNotCommit)
            {
                inBuf->offset -= 7;
                inBuf->len += 7;
                memcpy((inBuf->data + inBuf->offset), "commit;", 7);
                addCommitOrRollBackBeforeCurSQL = true;
            }
        }
        else
        {
            inBuf->offset -= 6;
            inBuf->len += 6;
            memcpy((inBuf->data + inBuf->offset), "begin;", 6);
            addBeforeBeforeCurSQL = true;

            inBuf->offset -= 9;
            inBuf->len += 9;
            memcpy((inBuf->data + inBuf->offset), "rollback;", 9);
            addCommitOrRollBackBeforeCurSQL = true;
        }

        needCommitTrx = false;
    }
    else
    {
        elog(ERROR, "internal error in addAdditionalSQL!");
    }
}

void 
getCaseInsensitiveId(void)
{
	HeapTuple tup;
	Form_pg_collation form;
    char *collName;
    int encoding;
    Oid namespaceId;

    if ((0 < caseInsensitiveId) && (caseInsensitiveId != UINT_MAX))
    {
        return;
    }

    collName = "case_insensitive";
    encoding = -1;
    namespaceId = get_namespace_oid("mysql", false);

    caseInsensitiveId = GetSysCacheOid3(COLLNAMEENCNSP, 
                                        Anum_pg_collation_oid,
                                        PointerGetDatum(collName),
                                        Int32GetDatum(encoding),
                                        ObjectIdGetDatum(namespaceId));
    if (OidIsValid(caseInsensitiveId))
    {
        return;
    }

    tup = SearchSysCache3(COLLNAMEENCNSP,
                          PointerGetDatum(collName),
                          Int32GetDatum(encoding),
                          ObjectIdGetDatum(namespaceId));
    if (!HeapTupleIsValid(tup))
    {
        elog(ERROR, "could not get oid for caseInsensitiveId");
    }

	form = (Form_pg_collation) GETSTRUCT(tup);
	if (form->collprovider == COLLPROVIDER_ICU)
	{
        caseInsensitiveId = form->oid;
	}
	else
	{
		caseInsensitiveId = form->oid;
	}
	ReleaseSysCache(tup);

    if (!OidIsValid(caseInsensitiveId))
    {
        elog(ERROR, "caseInsensitiveId is invalid");
    }
}

