/*-------------------------------------------------------------------------
 *
 * userLogonAuth.c
 *	  MySQL adapter userLogonAuth routines
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
 *	  src/backend/adapter/mysql/userLogonAuth.c
 *
 *-------------------------------------------------------------------------
 */

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "c.h"
#include "postgres.h"
#include "postmaster/postmaster.h"
#include "miscadmin.h"
#include "pg_config_manual.h"
#include "libpq/libpq.h"
#include "libpq/crypt.h"

#include "adapter/mysql/userLogonAuth.h"
#include "adapter/mysql/common.h"
#include "adapter/mysql/pwdEncryptDecrypt.h"
#include "adapter/mysql/netTransceiver.h"


static int 
parseHandshakeResp41PacketPayload(char *payload, 
                                  size_t payloadLen, 
                                  unsigned long *clientFlag, 
                                  int *maxPacketSize,
                                  char *userNameBuf, 
                                  size_t userNameBufLen, 
                                  char *authRespBuf, 
                                  size_t authRespBufLen, 
                                  char *dbBuf, 
                                  size_t dbBufLen);

static int 
parseHandshakeResp320PacketPayload(char* payload, 
                                   size_t payloadLen, 
                                   unsigned long* clientFlag, 
                                   int* maxPacketSize,
                                   char* userNameBuf, 
                                   size_t userNameBufLen, 
                                   char* authRespBuf, 
                                   size_t authRespBufLen, 
                                   char* dbBuf, 
                                   size_t dbBufLen);

static int
assembleAuthSwitchRequestPacketPayload(unsigned char* payloadBuf, 
                                       size_t payloadBufLen);

size_t 
assembleHandshakePacketPayload(const char* haloMysVersion, 
                               char* payloadBuf, 
                               size_t payloadBufLen)
{
    size_t payloadLen = 0;
    int payload_index = 0;
    const int cstr_end_char_len = 1;
    const int protocol_version_len = 1;
    int server_version_len = 0;
    const int thread_id_len = 4;
    const int auth_plugin_data_part1_len = 8;
    const int filler_len = 1;
    const int capability_flags_1_len = 2;
    const int character_set_len = 1;
    const int status_flags_len = 2;
    const int capability_flags_2_len = 2;
    const int auth_plugin_data_len_len = 1;
    const int reserved_len = 10;
    int auth_plugin_data_part2_len = 0;
    int auth_plugin_name_len = 0;

    int protocol_version = 10;
    char* server_version = NULL;
    int thread_id = 1;
    char* auth_plugin_data_part1 = "v\021\001\037wg\021\002";
    int filler = 0x00;
    int capability_flags_1 = 0b1111011011011111;
    int character_set = 0;
    int status_flags = 2;
    int capability_flags_2 = 0b1000000111101111;
    int capability = 0;
    int auth_plugin_data_len = 21;
    char* auth_plugin_data_part2 = "z\020X}VNSCgc|\021";
    char* auth_plugin_name = "mysql_native_password";

    if (EnableSSL)
    {
        capability_flags_1 |= MYS_CLT_SSL;
    }

    if ((strncmp(haloMysVersion, "5.7", 3) < 0) || (strncmp(haloMysVersion, "8.0", 3) < 0))
        character_set = 45;
    else 
        character_set = 255;

    server_version = pstrdup(haloMysVersion);
    server_version_len = (int)strlen(server_version);
    auth_plugin_data_part2_len = 13;
    auth_plugin_name_len = (int)strlen(auth_plugin_name);

    memcpy((payloadBuf + payload_index), &protocol_version, protocol_version_len);
    payload_index += protocol_version_len;

    strncpy((payloadBuf + payload_index), server_version, server_version_len);
    payload_index += server_version_len;
    payload_index += cstr_end_char_len;

    memcpy((payloadBuf + payload_index), &thread_id, thread_id_len);
    payload_index += thread_id_len;

    strncpy((payloadBuf + payload_index), auth_plugin_data_part1, auth_plugin_data_part1_len);
    payload_index += auth_plugin_data_part1_len;

    memcpy((payloadBuf + payload_index), &filler, filler_len);
    payload_index += filler_len;

    memcpy((payloadBuf + payload_index), &capability_flags_1, capability_flags_1_len);
    payload_index += capability_flags_1_len;

    memcpy((payloadBuf + payload_index), &character_set, character_set_len);
    payload_index += character_set_len;

    memcpy((payloadBuf + payload_index), &status_flags, status_flags_len);
    payload_index += status_flags_len;

    memcpy((payloadBuf + payload_index), &capability_flags_2, capability_flags_2_len);
    payload_index += capability_flags_2_len;

    memcpy(&capability, &capability_flags_1, capability_flags_1_len);
    memcpy((((char*)&capability) + 2), &capability_flags_2, capability_flags_2_len);
    if (capability & MYS_CLT_PLUGIN_AUTH)
    {
        memcpy((payloadBuf + payload_index), &auth_plugin_data_len, auth_plugin_data_len_len);
        payload_index += auth_plugin_data_len_len;
    }
    else 
    {
        memcpy((payloadBuf + payload_index), &filler, filler_len);
        payload_index += filler_len;
    }

    payload_index += reserved_len;

    strncpy((payloadBuf + payload_index), auth_plugin_data_part2, auth_plugin_data_part2_len);
    payload_index += auth_plugin_data_part2_len;

    if (capability & MYS_CLT_PLUGIN_AUTH)
    {
        strncpy((payloadBuf + payload_index), auth_plugin_name, auth_plugin_name_len);
        payload_index += auth_plugin_name_len;
        payload_index += cstr_end_char_len;
    }

    payloadLen = payload_index;
    memset((payloadBuf + payloadLen), '\0', (payloadBufLen - payloadLen));
    pfree(server_version);
    return payloadLen;
}

static int
assembleAuthSwitchRequestPacketPayload(unsigned char* payloadBuf, 
                                       size_t payloadBufLen)
{
    char* auth_plugin_data = "v\021\001\037wg\021\002z\020X}VNSCgc|\021";
    size_t auth_plugin_data_len = strlen((const char *)auth_plugin_data);
    char* auth_plugin_name = "mysql_native_password";
    size_t auth_plugin_name_len = strlen((const char *)auth_plugin_name);

    memset(payloadBuf, 0, payloadBufLen);

    payloadBuf[0] = 254;

    memcpy((payloadBuf + 1), auth_plugin_name, auth_plugin_name_len);

    payloadBuf[auth_plugin_name_len + 1] = '\0';

    memcpy((payloadBuf + auth_plugin_name_len + 2), auth_plugin_data, auth_plugin_data_len);

    payloadBuf[auth_plugin_name_len + auth_plugin_data_len + 2] = '\0';

    return auth_plugin_name_len + auth_plugin_data_len + 3;
}

int 
parseHandshakeRespPacketPayload(char* payload, 
                                size_t payloadLen,
                                unsigned long* clientFlag, 
                                int* maxPacketSize,
                                char* userNameBuf, 
                                size_t userNameBufLen, 
                                char* authRespBuf, 
                                size_t authRespBufLen, 
                                char* dbBuf, 
                                size_t dbBufLen)
{
    int tmp_clientFlag = 0;
    const int tmp_clientFlag_len = 2;
    memcpy(&tmp_clientFlag, payload, tmp_clientFlag_len);

    if (tmp_clientFlag & MYS_CLT_PROTOCOL_41)
    {
        return parseHandshakeResp41PacketPayload(payload, 
                                                 payloadLen, 
                                                 clientFlag, 
                                                 maxPacketSize, 
                                                 userNameBuf, 
                                                 userNameBufLen, 
                                                 authRespBuf, 
                                                 authRespBufLen, 
                                                 dbBuf, 
                                                 dbBufLen);
    }
    else 
    {
        return parseHandshakeResp320PacketPayload(payload, 
                                                  payloadLen, 
                                                  clientFlag, 
                                                  maxPacketSize, 
                                                  userNameBuf, 
                                                  userNameBufLen, 
                                                  authRespBuf, 
                                                  authRespBufLen, 
                                                  dbBuf, 
                                                  dbBufLen);
    }
}

bool
mysCheckAuth(const char* userName, const char* userPwd)
{
    bool auth_result = true;
    char *logdetail;
    char *shadow_pass;
    PasswordType pwtype;

    Assert(MyProcPort->hba->auth_method == uaMD5);

	shadow_pass = get_role_password(userName, &logdetail);

    if (shadow_pass == NULL && strlen(userPwd) == 0) return true;
    if (shadow_pass == NULL || strlen(userPwd) == 0) return false;

    if ((strncmp(shadow_pass, mysql_native_password_prefix, MYS_NATIVE_PASSWORD_PREFIX_LEN) == 0) &&
		(strlen(shadow_pass) == MYS_NATIVE_PASSWORD_LEN) &&
		(strspn(shadow_pass + MYS_NATIVE_PASSWORD_PREFIX_LEN, "0123456789abcdef") == MYS_NATIVE_PASSWORD_LEN - MYS_NATIVE_PASSWORD_PREFIX_LEN))
    {
		pwtype = PASSWORD_TYPE_MYS_NATIVE_PASSWORD;
    }
    
    switch(pwtype)
    {
        case PASSWORD_TYPE_MYS_NATIVE_PASSWORD:
            auth_result = checkMysNativeAuth(userPwd, shadow_pass);
            break;
        default:
            elog(WARNING, "not support this mysql auth method: %d", pwtype);
            auth_result = false;
            break;
    }

    return auth_result;
}

static int 
parseHandshakeResp41PacketPayload(char *payload, 
                                  size_t payloadLen,
                                  unsigned long *clientFlag, 
                                  int *maxPacketSize,
                                  char *userNameBuf, 
                                  size_t userNameBufLen, 
                                  char *authRespBuf, 
                                  size_t authRespBufLen, 
                                  char *dbBuf, 
                                  size_t dbBufLen)
{
    int clientFlag_temp = 0;
    const int clientFlag_start_index = 0;
    const int clientFlag_len = 4;
    const int clientFlag_end_index = clientFlag_start_index + clientFlag_len;

    int maxPacketSize_temp = 0;
    const int maxPacketSize_start_index = clientFlag_end_index;
    const int maxPacketSize_len = 4;
    const int maxPacketSize_end_index = maxPacketSize_start_index + maxPacketSize_len;

    int character_set = 0;
    int character_set_temp = 0;
    const int character_set_start_index = maxPacketSize_end_index;
    const int character_set_len = 1;
    const int character_set_end_index = character_set_start_index + character_set_len;

    const int filler_start_index = character_set_end_index;
    const int filler_len = 23;
    const int filler_end_index = filler_start_index + filler_len;

    const int userName_start_index = filler_end_index;
    size_t userName_len = 0;

    long auth_resp_len = 0;
    int auth_resp_len_start_index = 0;
    int auth_resp_len_prefix_len = 0;
    int auth_resp_len_prefix = 0;
    int auth_resp_len_len = 0;
    int auth_resp_len_end_index = 0;
    int auth_resp_start_index = 0;
    long auth_resp_end_index = 0;

    long db_start_index = 0;
    size_t db_len = 0;

    char clt_plugin_name_buf[100];
    int clt_plugin_name_start_index = 0;
    size_t clt_plugin_name_buf_len = 100;
    size_t clt_plugin_name_len = 0;

    char tmp = ' ';

    *clientFlag = 0;
    *maxPacketSize = 0;

    memcpy(&clientFlag_temp, (payload + clientFlag_start_index), clientFlag_len);

    memcpy(&maxPacketSize_temp, (payload + maxPacketSize_start_index), maxPacketSize_len);

    memcpy(&character_set_temp, (payload + character_set_start_index), character_set_len);

    if (clientFlag_temp & MYS_CLT_SSL)
    {
        StringInfoData inBuf;
#ifdef USE_SSL
        if (secure_open_server(MyProcPort) == -1)
        {
            elog(ERROR, "0, failed to create SSL connect ");
            exit(1);
        }
#endif
        initStringInfo(&inBuf);
        netTransceiver->readPayloadForLogon(&inBuf);
        payload = inBuf.data + inBuf.offset;
        payloadLen = inBuf.len;

        memcpy(clientFlag, (payload + clientFlag_start_index), clientFlag_len);

        memcpy(maxPacketSize, (payload + maxPacketSize_start_index), maxPacketSize_len);

        memcpy(&character_set, (payload + character_set_start_index), character_set_len);
    }
    else
    {
        *clientFlag = clientFlag_temp;
        *maxPacketSize = maxPacketSize_temp;
        character_set = character_set_temp;
    }

    userName_len = strlen(payload + userName_start_index);
    auth_resp_len_start_index = userName_start_index + userName_len + 1;

    memset(userNameBuf, 0, userNameBufLen);
    if ((userNameBufLen - 1) < userName_len)
    {
        return 0;
    }
    strncpy(userNameBuf, (payload + userName_start_index), (userNameBufLen - 1));

    if (*clientFlag & MYS_CLT_PLUGIN_AUTH_LENENC_CLIENT_DATA)
    {
        memcpy(&auth_resp_len_prefix, (payload + auth_resp_len_start_index), 1);
        if (0xFC == auth_resp_len_prefix)
        {
            auth_resp_len_prefix_len = 1;
            auth_resp_len_len = 2;
        } 
        else if (0xFD == auth_resp_len_prefix)
        {
            auth_resp_len_prefix_len = 1;
            auth_resp_len_len = 3;
        }
        else if (0xFE == auth_resp_len_prefix)
        {
            auth_resp_len_prefix_len = 1;
            auth_resp_len_len = 8;
        }
        else 
        {
            auth_resp_len_prefix_len = 0;
            auth_resp_len_len = 1;
        }
    }
    else
    {
        auth_resp_len_prefix_len = 0;
        auth_resp_len_len = 1;
    }
    memcpy(&auth_resp_len, (payload + auth_resp_len_start_index + auth_resp_len_prefix_len), auth_resp_len_len);
    auth_resp_len_end_index = auth_resp_len_start_index + auth_resp_len_prefix_len + auth_resp_len_len;
    auth_resp_start_index = auth_resp_len_end_index;
    auth_resp_end_index = auth_resp_start_index + auth_resp_len; 
    tmp = payload[auth_resp_end_index];
    if ((authRespBufLen - 1) < auth_resp_len)
    {
        return 0;
    }
    memset(authRespBuf, 0, authRespBufLen);
    payload[auth_resp_end_index] = '\0';
    memcpy(authRespBuf, (payload + auth_resp_start_index), auth_resp_len);
    payload[auth_resp_end_index] = tmp;

    db_start_index = auth_resp_end_index;
    if (*clientFlag & MYS_CLT_CONNECT_WITH_DB)
    {
        memset(dbBuf, 0, dbBufLen);
        db_len = strlen(payload + db_start_index);
        if ((dbBufLen - 1) < db_len)
        {
            return 0;
        }
        strncpy(dbBuf, (payload + db_start_index), (dbBufLen - 1));
        db_len += 1;
    }
    else 
    {
        db_len = 0;
    }

    clt_plugin_name_start_index = db_start_index + db_len;
    if (*clientFlag & MYS_CLT_PLUGIN_AUTH)
    {
        memset(clt_plugin_name_buf, 0, clt_plugin_name_buf_len);
        clt_plugin_name_len = strlen(payload + clt_plugin_name_start_index);
        if ((clt_plugin_name_buf_len - 1) < clt_plugin_name_len)
        {
            return 0;
        }
        strncpy(clt_plugin_name_buf, (payload + clt_plugin_name_start_index), (clt_plugin_name_buf_len - 1));
        clt_plugin_name_len += 1;
        
        if (strcmp(clt_plugin_name_buf, "mysql_native_password") != 0)
        {
            StringInfoData inBuf;
            unsigned char payload[1024];
            int len = assembleAuthSwitchRequestPacketPayload(payload, 1024);
            netTransceiver->writePacketHeaderPayloadFlush((const char *)payload, len);
            initStringInfo(&inBuf);
            netTransceiver->readPayloadForLogon(&inBuf);

            memset(authRespBuf, 0, authRespBufLen);
            strncpy(authRespBuf, inBuf.data + inBuf.offset, inBuf.len);
            authRespBuf[inBuf.len] = 0;
        }
    }
    else 
    {
        clt_plugin_name_len = 0;
    }

    return 1;
}

static int 
parseHandshakeResp320PacketPayload(char *payload, 
                                   size_t payloadLen,
                                   unsigned long *clientFlag, 
                                   int *maxPacketSize,
                                   char *userNameBuf, 
                                   size_t userNameBufLen, 
                                   char *authRespBuf, 
                                   size_t authRespBufLen, 
                                   char *dbBuf, 
                                   size_t dbBufLen)
{
    const int clientFlag_start_index = 0;
    const int clientFlag_len = 2;
    const int clientFlag_end_index = clientFlag_start_index + clientFlag_len;
    const int maxPacketSize_start_index = clientFlag_end_index;
    const int maxPacketSize_len = 3;
    const int maxPacketSize_end_index = maxPacketSize_start_index + maxPacketSize_len;
    const int userName_start_index = maxPacketSize_end_index;
    size_t userName_len = strlen(payload + userName_start_index);
    const int auth_resp_start_index = userName_start_index + userName_len;
    size_t auth_resp_len = strlen(payload + auth_resp_start_index);
    const int db_start_index = auth_resp_start_index + auth_resp_len;
    size_t db_len = strlen(payload + db_start_index);

    memcpy(clientFlag, (payload + clientFlag_start_index), clientFlag_len);

    memcpy(maxPacketSize, (payload + maxPacketSize_start_index), maxPacketSize_len);

    memset(userNameBuf, 0, userNameBufLen);
    if ((userNameBufLen - 1) < userName_len)
    {
        return 0;
    }
    strncpy(userNameBuf, (payload + userName_start_index), (userNameBufLen - 1));

    if (*clientFlag & MYS_CLT_CONNECT_WITH_DB)
    {
        memset(authRespBuf, 0, authRespBufLen);
        if ((authRespBufLen - 1) < auth_resp_len)
        {
            return 0;
        }
        memcpy(authRespBuf, (payload + auth_resp_start_index), auth_resp_len);

        memset(dbBuf, 0, dbBufLen);
        if ((dbBufLen - 1) < db_len)
        {
            return 0;
        }
        strncpy(dbBuf, (payload + db_start_index), (dbBufLen - 1));
    }
    else 
    {
        memset(authRespBuf, 0, authRespBufLen);
        auth_resp_len = payloadLen - auth_resp_start_index;
        if ((authRespBufLen - 1) < auth_resp_len)
        {
            return 0;
        }
        memcpy(authRespBuf, (payload + auth_resp_start_index), auth_resp_len);
    }

    return 1;
}

