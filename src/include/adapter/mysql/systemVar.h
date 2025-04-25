/*-------------------------------------------------------------------------
 *
 * systemVar.h
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
 *	  src/include/adapter/mysql/systemVar.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SYSTEMVAR_H
#define SYSTEMVAR_H


#include "utils/hsearch.h"


/* Bits for different SQL modes modes (including ANSI mode) */
#define MYS_MODE_REAL_AS_FLOAT              1
#define MYS_MODE_PIPES_AS_CONCAT            2
#define MYS_MODE_ANSI_QUOTES                4
#define MYS_MODE_IGNORE_SPACE               8
#define MYS_MODE_NOT_USED                   16
#define MYS_MODE_ONLY_FULL_GROUP_BY         32
#define MYS_MODE_NO_UNSIGNED_SUBTRACTION    64
#define MYS_MODE_NO_DIR_IN_CREATE           128
#define MYS_MODE_POSTGRESQL                 256
#define MYS_MODE_ORACLE                     512
#define MYS_MODE_MSSQL                      1024
#define MYS_MODE_DB2                        2048
#define MYS_MODE_MAXDB                      4096
#define MYS_MODE_NO_KEY_OPTIONS             8192
#define MYS_MODE_NO_TABLE_OPTIONS           16384
#define MYS_MODE_NO_FIELD_OPTIONS           32768
#define MYS_MODE_MYSQL323                   65536L
#define MYS_MODE_MYSQL40                    (1 << 17)
#define MYS_MODE_ANSI                       (1 << 18)
#define MYS_MODE_NO_AUTO_VALUE_ON_ZERO      (1 << 19)
#define MYS_MODE_NO_BACKSLASH_ESCAPES       (1 << 20)
#define MYS_MODE_STRICT_TRANS_TABLES        (1 << 21)
#define MYS_MODE_STRICT_ALL_TABLES          (1 << 22)
/*
 * NO_ZERO_DATE, NO_ZERO_IN_DATE and ERROR_FOR_DIVISION_BY_ZERO modes are
 * removed in 5.7 and their functionality is merged with STRICT MODE.
 * However, For backward compatibility during upgrade, these modes are kept
 * but they are not used. Setting these modes in 5.7 will give warning and
 * have no effect.
 */
#define MYS_MODE_NO_ZERO_IN_DATE            (1 << 23)
#define MYS_MODE_NO_ZERO_DATE               (1 << 24)
#define MYS_MODE_INVALID_DATES              (1 << 25)
#define MYS_MODE_ERROR_FOR_DIVISION_BY_ZERO (1 << 26)
#define MYS_MODE_TRADITIONAL                (1 << 27)
#define MYS_MODE_NO_AUTO_CREATE_USER        (1 << 28)
#define MYS_MODE_HIGH_NOT_PRECEDENCE        (1 << 29)
#define MYS_MODE_NO_ENGINE_SUBSTITUTION     (1 << 30)
#define MYS_MODE_PAD_CHAR_TO_FULL_LENGTH    (1ULL << 31)


extern int autoCommit;
extern uint64 mys_sqlMode;
extern int default_week_format;

extern HTAB *globalSystemVars;
extern HTAB *globalSystemVarsLock;

Size GlobalSystemVariablesShmemSize(void);
void GlobalSystemVariablesShmemInit(void);
void getSystemVariableValueForSelect(char *varName, 
                                     bool isSessionSystemVar, 
                                     char **varValue);
bool getSystemVariableValueForShow(char *varName, 
                                   bool isSessionSystemVar, 
                                   char **varValue);
void setSystemVariableValue(char *varName, char *varValue, 
                            bool isSessionSystemVar);
void setSystemVariableDatum(char *varName, Datum varConfValue, 
                            Oid varValueType, bool isNull, 
                            bool isSessionSystemVar);
bool isSystemVariable(char *varName);


#endif                          /* SYSTEMVAR_H */

