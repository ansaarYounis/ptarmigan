/*
 *  Copyright (C) 2017, Nayuta, Inc. All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed to the Apache Software Foundation (ASF) under one
 *  or more contributor license agreements.  See the NOTICE file
 *  distributed with this work for additional information
 *  regarding copyright ownership.  The ASF licenses this file
 *  to you under the Apache License, Version 2.0 (the
 *  "License"); you may not use this file except in compliance
 *  with the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an
 *  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *  KIND, either express or implied.  See the License for the
 *  specific language governing permissions and limitations
 *  under the License.
 */
/** @file   cmd_json.c
 *  @brief  ucoind JSON-RPC process
 */
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/limits.h>
#include <assert.h>

#include "jsonrpc-c.h"
#include "segwit_addr.h"

#include "cmd_json.h"
#include "ln_db.h"
#include "ln_db_lmdb.h"
#include "btcrpc.h"

#include "p2p_svr.h"
#include "p2p_cli.h"
#include "lnapp.h"
#include "monitoring.h"


/********************************************************************
 * macros
 ********************************************************************/

#define M_SZ_JSONSTR            (8192)
#define M_SZ_PAYERR             (128)


/********************************************************************
 * static variables
 ********************************************************************/

static struct jrpc_server   mJrpc;
static char                 mLastPayErr[M_SZ_PAYERR];       //最後に送金エラーが発生した時刻
static int                  mPayTryCount = 0;               //送金トライ回数

static const char *kOK = "OK";
static const char *kNG = "NG";


/********************************************************************
 * prototypes
 ********************************************************************/

static int json_connect(cJSON *params, int Index, daemon_connect_t *pConn);

static cJSON *cmd_connect(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_getinfo(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_disconnect(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_stop(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_fund(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_invoice(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_eraseinvoice(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_listinvoice(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_pay(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_routepay_first(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_routepay(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_close(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_getlasterror(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_debug(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_getcommittx(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_disautoconn(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_removechannel(jrpc_context *ctx, cJSON *params, cJSON *id);
static cJSON *cmd_setfeerate(jrpc_context *ctx, cJSON *params, cJSON *id);

static bool routepay_param(cJSON *params, int index,
                    char *pStrPayhash,
                    char *pStrPayee,
                    char *pStrPayer,
                    uint64_t *pAmountMsat,
                    uint32_t *pMinFinalCltvExpiry,
                    uint8_t *pAddNum,
                    ln_fieldr_t **ppRField);
static char *create_bolt11(const uint8_t *pPayHash, uint64_t Amount);
static lnapp_conf_t *search_connected_lnapp_node(const uint8_t *p_node_id);


/********************************************************************
 * public functions
 ********************************************************************/

void cmd_json_start(uint16_t Port)
{
    jrpc_server_init(&mJrpc, Port);
    jrpc_register_procedure(&mJrpc, cmd_connect,     "connect", NULL);
    jrpc_register_procedure(&mJrpc, cmd_getinfo,     "getinfo", NULL);
    jrpc_register_procedure(&mJrpc, cmd_disconnect,  "disconnect", NULL);
    jrpc_register_procedure(&mJrpc, cmd_stop,        "stop", NULL);
    jrpc_register_procedure(&mJrpc, cmd_fund,        "fund", NULL);
    jrpc_register_procedure(&mJrpc, cmd_invoice,     "invoice", NULL);
    jrpc_register_procedure(&mJrpc, cmd_eraseinvoice,"eraseinvoice", NULL);
    jrpc_register_procedure(&mJrpc, cmd_listinvoice, "listinvoice", NULL);
    jrpc_register_procedure(&mJrpc, cmd_pay,         "PAY", NULL);
    jrpc_register_procedure(&mJrpc, cmd_routepay_first, "routepay", NULL);
    jrpc_register_procedure(&mJrpc, cmd_routepay,    "routepay_cont", NULL);
    jrpc_register_procedure(&mJrpc, cmd_close,       "close", NULL);
    jrpc_register_procedure(&mJrpc, cmd_getlasterror,"getlasterror", NULL);
    jrpc_register_procedure(&mJrpc, cmd_debug,       "debug", NULL);
    jrpc_register_procedure(&mJrpc, cmd_getcommittx, "getcommittx", NULL);
    jrpc_register_procedure(&mJrpc, cmd_disautoconn, "disautoconn", NULL);
    jrpc_register_procedure(&mJrpc, cmd_removechannel,"removechannel", NULL);
    jrpc_register_procedure(&mJrpc, cmd_setfeerate,   "setfeerate", NULL);
    jrpc_server_run(&mJrpc);
    jrpc_server_destroy(&mJrpc);
}


uint16_t cmd_json_get_port(void)
{
    return (uint16_t)mJrpc.port_number;
}


void cmd_json_pay_retry(const uint8_t *pPayHash, const char *pInvoice)
{
    bool ret;
    char *p_invoice;
    if (pInvoice == NULL) {
        ret = ln_db_annoskip_invoice_load(&p_invoice, pPayHash);     //p_invoiceはmalloc()される
    } else {
        p_invoice = (char *)pInvoice;   //constはずし
        ret = true;
    }
    if (ret) {
        DBG_PRINTF("invoice:%s\n", p_invoice);
        char *json = (char *)APP_MALLOC(8192);      //APP_FREE: この中
        strcpy(json, "{\"method\":\"routepay_cont\",\"params\":");
        strcat(json, p_invoice);
        strcat(json, "}");
        int retval = misc_sendjson(json, "127.0.0.1", cmd_json_get_port());
        DBG_PRINTF("retval=%d\n", retval);
        APP_FREE(json);     //APP_MALLOC: この中
    } else {
        DBG_PRINTF("fail: invoice not found\n");
    }
    if (pInvoice == NULL) {
        free(p_invoice);
    }
}


/********************************************************************
 * private functions
 ********************************************************************/

static int json_connect(cJSON *params, int Index, daemon_connect_t *pConn)
{
    cJSON *json;

    //peer_nodeid, peer_addr, peer_port
    json = cJSON_GetArrayItem(params, Index++);
    if (json && (json->type == cJSON_String)) {
        bool ret = misc_str2bin(pConn->node_id, UCOIN_SZ_PUBKEY, json->valuestring);
        if (ret) {
            DBG_PRINTF("pConn->node_id=%s\n", json->valuestring);
        } else {
            DBG_PRINTF("fail: invalid node_id string\n");
            Index = -1;
            goto LABEL_EXIT;
        }
    } else {
        DBG_PRINTF("fail: node_id\n");
        Index = -1;
        goto LABEL_EXIT;
    }
    if (memcmp(ln_node_getid(), pConn->node_id, UCOIN_SZ_PUBKEY) == 0) {
        //node_idが自分と同じ
        DBG_PRINTF("fail: same own node_id\n");
        Index = -1;
        goto LABEL_EXIT;
    }
    json = cJSON_GetArrayItem(params, Index++);
    if (json && (json->type == cJSON_String)) {
        strcpy(pConn->ipaddr, json->valuestring);
        DBG_PRINTF("pConn->ipaddr=%s\n", json->valuestring);
    } else {
        DBG_PRINTF("fail: ipaddr\n");
        Index = -1;
        goto LABEL_EXIT;
    }
    json = cJSON_GetArrayItem(params, Index++);
    if (json && (json->type == cJSON_Number)) {
        pConn->port = json->valueint;
        DBG_PRINTF("pConn->port=%d\n", json->valueint);
    } else {
        DBG_PRINTF("fail: port\n");
        Index = -1;
    }

LABEL_EXIT:
    return Index;
}


static cJSON *cmd_connect(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    daemon_connect_t conn;
    cJSON *result = NULL;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //connect parameter
    index = json_connect(params, 0, &conn);
    if (index < 0) {
        goto LABEL_EXIT;
    }

    SYSLOG_INFO("connect");

    lnapp_conf_t *p_appconf = search_connected_lnapp_node(conn.node_id);
    if (p_appconf != NULL) {
        ctx->error_code = RPCERR_ALCONN;
        ctx->error_message = ucoind_error_str(RPCERR_ALCONN);
        goto LABEL_EXIT;
    }

    p2p_cli_start(&conn, ctx);
    if (ctx->error_code != 0) {
        ctx->error_code = RPCERR_CONNECT;
        ctx->error_message = ucoind_error_str(RPCERR_CONNECT);
        goto LABEL_EXIT;
    }

    //チェック
    sleep(2);

    p_appconf = search_connected_lnapp_node(conn.node_id);
    if ((p_appconf == NULL) || !lnapp_is_looping(p_appconf) || !lnapp_is_inited(p_appconf)) {
        ctx->error_code = RPCERR_CONNECT;
        ctx->error_message = ucoind_error_str(RPCERR_CONNECT);
        goto LABEL_EXIT;
    }
    result = cJSON_CreateString(kOK);

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


static cJSON *cmd_getinfo(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params; (void)id;

    cJSON *result = cJSON_CreateObject();
    cJSON *result_peer = cJSON_CreateArray();

    uint64_t amount = ln_node_total_msat();

    //basic info
    char node_id[UCOIN_SZ_PUBKEY * 2 + 1];
    misc_bin2str(node_id, ln_node_getid(), UCOIN_SZ_PUBKEY);
    cJSON_AddItemToObject(result, "node_id", cJSON_CreateString(node_id));
    cJSON_AddItemToObject(result, "node_port", cJSON_CreateNumber(ln_node_addr()->port));
    cJSON_AddItemToObject(result, "jsonrpc_port", cJSON_CreateNumber(cmd_json_get_port()));
    cJSON_AddNumber64ToObject(result, "total_our_msat", amount);

    //peer info
    p2p_svr_show_self(result_peer);
    p2p_cli_show_self(result_peer);
    cJSON_AddItemToObject(result, "peers", result_peer);

    //payment info
    uint8_t *p_hash;
    int cnt = ln_db_annoskip_invoice_get(&p_hash);
    if (cnt > 0) {
        cJSON *result_hash = cJSON_CreateArray();
        uint8_t *p = p_hash;
        for (int lp = 0; lp < cnt; lp++) {
            char hash_str[LN_SZ_HASH * 2 + 1];
            misc_bin2str(hash_str, p, LN_SZ_HASH);
            p += LN_SZ_HASH;
            cJSON_AddItemToArray(result_hash, cJSON_CreateString(hash_str));
        }
        free(p_hash);       //ln_lmdbでmalloc/realloc()している
        cJSON_AddItemToObject(result, "paying_hash", result_hash);
    }
    cJSON_AddItemToObject(result, "last_errpay_date", cJSON_CreateString(mLastPayErr));

    return result;
}


static cJSON *cmd_disconnect(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    daemon_connect_t conn;
    cJSON *result = NULL;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //connect parameter
    index = json_connect(params, index, &conn);
    if (index < 0) {
        goto LABEL_EXIT;
    }

    SYSLOG_INFO("disconnect");

    lnapp_conf_t *p_appconf = search_connected_lnapp_node(conn.node_id);
    if (p_appconf != NULL) {
        lnapp_stop(p_appconf);
        result = cJSON_CreateString(kOK);
    } else {
        ctx->error_code = RPCERR_NOCONN;
        ctx->error_message = ucoind_error_str(RPCERR_NOCONN);
    }

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


static cJSON *cmd_stop(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params; (void)id;

    SYSLOG_INFO("stop");
    p2p_svr_stop_all();
    p2p_cli_stop_all();
    jrpc_server_stop(&mJrpc);

    monitor_stop();

    return cJSON_CreateString("OK");
}


static cJSON *cmd_fund(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    cJSON *json;
    daemon_connect_t conn;
    funding_conf_t fundconf;
    cJSON *result = NULL;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //connect parameter
    index = json_connect(params, index, &conn);
    if (index < 0) {
        goto LABEL_EXIT;
    }

    lnapp_conf_t *p_appconf = search_connected_lnapp_node(conn.node_id);
    if (p_appconf == NULL) {
        //未接続
        ctx->error_code = RPCERR_NOCONN;
        ctx->error_message = ucoind_error_str(RPCERR_NOCONN);
        goto LABEL_EXIT;
    }

    bool haveCnl = ln_node_search_channel(NULL, conn.node_id);
    if (haveCnl) {
        //開設しようとしてチャネルが開いている
        ctx->error_code = RPCERR_ALOPEN;
        ctx->error_message = ucoind_error_str(RPCERR_ALOPEN);
        goto LABEL_EXIT;
    }

    bool is_funding = ln_is_funding(p_appconf->p_self);
    if (is_funding) {
        //開設しようとしてチャネルが開設中
        ctx->error_code = RPCERR_OPENING;
        ctx->error_message = ucoind_error_str(RPCERR_OPENING);
        goto LABEL_EXIT;
    }

    bool inited = lnapp_is_inited(p_appconf);
    if (!inited) {
        //BOLTメッセージとして初期化が完了していない(init/channel_reestablish交換できていない)
        ctx->error_code = RPCERR_NOINIT;
        ctx->error_message = ucoind_error_str(RPCERR_NOINIT);
        goto LABEL_EXIT;
    }

    //txid, txindex, signaddr, funding_sat, push_sat

    //txid
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_String)) {
        misc_str2bin_rev(fundconf.txid, UCOIN_SZ_TXID, json->valuestring);
        DBG_PRINTF("txid=%s\n", json->valuestring);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    //txindex
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        fundconf.txindex = json->valueint;
        DBG_PRINTF("txindex=%d\n", json->valueint);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    //signaddr
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_String)) {
        strcpy(fundconf.signaddr, json->valuestring);
        DBG_PRINTF("signaddr=%s\n", json->valuestring);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    //funding_sat
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        fundconf.funding_sat = json->valueu64;
        DBG_PRINTF("funding_sat=%" PRIu64 "\n", fundconf.funding_sat);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    //push_sat
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        fundconf.push_sat = json->valueu64;
        DBG_PRINTF("push_sat=%" PRIu64 "\n", fundconf.push_sat);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    //feerate_per_kw
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        fundconf.feerate_per_kw = (uint32_t)json->valueu64;
        DBG_PRINTF("feerate_per_kw=%" PRIu32 "\n", fundconf.feerate_per_kw);
    } else {
        //スルー
    }

    print_funding_conf(&fundconf);

    SYSLOG_INFO("fund");

    bool ret = lnapp_funding(p_appconf, &fundconf);
    if (ret) {
        result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "status", cJSON_CreateString("Progressing"));
        cJSON_AddItemToObject(result, "feerate_per_kw", cJSON_CreateNumber64(ln_feerate_per_kw(p_appconf->p_self)));
    } else {
        ctx->error_code = RPCERR_FUNDING;
        ctx->error_message = ucoind_error_str(RPCERR_FUNDING);
    }

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


static cJSON *cmd_invoice(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    cJSON *json;
    uint64_t amount = 0;
    cJSON *result = NULL;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //amount
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        amount = json->valueu64;
        DBG_PRINTF("amount=%" PRIu64 "\n", amount);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }

    SYSLOG_INFO("invoice");

    result = cJSON_CreateObject();
    ucoind_preimage_lock();

    uint8_t preimage[LN_SZ_PREIMAGE];
    uint8_t preimage_hash[LN_SZ_HASH];
    char str_hash[LN_SZ_HASH * 2 + 1];

    ucoin_util_random(preimage, LN_SZ_PREIMAGE);
    ln_db_preimg_save(preimage, amount, NULL);
    ln_calc_preimage_hash(preimage_hash, preimage);

    misc_bin2str(str_hash, preimage_hash, LN_SZ_HASH);
    DBG_PRINTF("preimage=")
    DUMPBIN(preimage, LN_SZ_PREIMAGE);
    DBG_PRINTF("hash=")
    DUMPBIN(preimage_hash, LN_SZ_HASH);
    cJSON_AddItemToObject(result, "hash", cJSON_CreateString(str_hash));
    cJSON_AddItemToObject(result, "amount", cJSON_CreateNumber64(amount));
    ucoind_preimage_unlock();

    char *p_invoice = create_bolt11(preimage_hash, amount);
    if (p_invoice != NULL) {
        cJSON_AddItemToObject(result, "bolt11", cJSON_CreateString(p_invoice));
        free(p_invoice);
    } else {
        DBG_PRINTF("fail: BOLT11 format\n");
        index = -1;
    }

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


static cJSON *cmd_eraseinvoice(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    cJSON *json;
    cJSON *result = NULL;
    uint8_t preimage_hash[LN_SZ_HASH];
    int index = 0;
    bool ret;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    json = cJSON_GetArrayItem(params, index++);
    if ((json == NULL) || (json->type != cJSON_String)) {
        index = -1;
        goto LABEL_EXIT;
    }
    if (strlen(json->valuestring) > 0) {
        DBG_PRINTF("erase hash: %s\n", json->valuestring);
        misc_str2bin(preimage_hash, sizeof(preimage_hash), json->valuestring);
        ret = ln_db_preimg_del_hash(preimage_hash);
    } else {
        ret = ln_db_preimg_del(NULL);
    }
    if (ret) {
        result = cJSON_CreateString(kOK);
    } else {
        ctx->error_code = RPCERR_INVOICE_ERASE;
        ctx->error_message = ucoind_error_str(RPCERR_INVOICE_ERASE);
    }

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


static cJSON *cmd_listinvoice(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    cJSON *result = NULL;
    int index = 0;
    uint8_t preimage[LN_SZ_PREIMAGE];
    uint8_t preimage_hash[LN_SZ_HASH];
    uint64_t amount;
    void *p_cur;
    bool ret;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    result = cJSON_CreateArray();
    ret = ln_db_preimg_cur_open(&p_cur);
    while (ret) {
        ret = ln_db_preimg_cur_get(p_cur, preimage, &amount);
        if (ret) {
            ln_calc_preimage_hash(preimage_hash, preimage);
            cJSON *json = cJSON_CreateArray();

            char str_hash[LN_SZ_HASH * 2 + 1];
            misc_bin2str(str_hash, preimage_hash, LN_SZ_HASH);
            cJSON_AddItemToArray(json, cJSON_CreateString(str_hash));
            cJSON_AddItemToArray(json, cJSON_CreateNumber64(amount));
            char *p_invoice = create_bolt11(preimage_hash, amount);
            if (p_invoice != NULL) {
                cJSON_AddItemToArray(json, cJSON_CreateString(p_invoice));
                free(p_invoice);
            }
            cJSON_AddItemToArray(result, json);
        }
    }
    ln_db_preimg_cur_close(p_cur);

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


static cJSON *cmd_pay(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    cJSON *json;
    payment_conf_t payconf;
    cJSON *result = NULL;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //blockcount
    int32_t blockcnt = btcprc_getblockcount();
    DBG_PRINTF("blockcnt=%d\n", blockcnt);
    if (blockcnt < 0) {
        index = -1;
        goto LABEL_EXIT;
    }

    //payment_hash, hop_num
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_String)) {
        misc_str2bin(payconf.payment_hash, LN_SZ_HASH, json->valuestring);
        DBG_PRINTF("payment_hash=%s\n", json->valuestring);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        payconf.hop_num = json->valueint;
        DBG_PRINTF("hop_num=%d\n", json->valueint);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    //array
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Array)) {
        DBG_PRINTF("trace array\n");
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    //[ [...], [...], ..., [...] ]
    for (int lp = 0; lp < payconf.hop_num; lp++) {
        ln_hop_datain_t *p = &payconf.hop_datain[lp];

        DBG_PRINTF("loop=%d\n", lp);
        cJSON *jarray = cJSON_GetArrayItem(json, lp);
        if (jarray && (jarray->type == cJSON_Array)) {
            //[node_id, short_channel_id, amt_to_forward, outgoing_cltv_value]

            //node_id
            cJSON *jprm = cJSON_GetArrayItem(jarray, 0);
            DBG_PRINTF("jprm=%p\n", jprm);
            if (jprm && (jprm->type == cJSON_String)) {
                misc_str2bin(p->pubkey, UCOIN_SZ_PUBKEY, jprm->valuestring);
                DBG_PRINTF("  node_id=");
                DUMPBIN(p->pubkey, UCOIN_SZ_PUBKEY);
            } else {
                DBG_PRINTF("fail: p=%p\n", jprm);
                index = -1;
                goto LABEL_EXIT;
            }
            //short_channel_id
            jprm = cJSON_GetArrayItem(jarray, 1);
            if (jprm && (jprm->type == cJSON_String)) {
                p->short_channel_id = strtoull(jprm->valuestring, NULL, 16);
                DBG_PRINTF("  short_channel_id=%016" PRIx64 "\n", p->short_channel_id);
            } else {
                DBG_PRINTF("fail: p=%p\n", jprm);
                index = -1;
                goto LABEL_EXIT;
            }
            //amt_to_forward
            jprm = cJSON_GetArrayItem(jarray, 2);
            if (jprm && (jprm->type == cJSON_Number)) {
                p->amt_to_forward = jprm->valueu64;
                DBG_PRINTF("  amt_to_forward=%" PRIu64 "\n", p->amt_to_forward);
            } else {
                DBG_PRINTF("fail: p=%p\n", jprm);
                index = -1;
                goto LABEL_EXIT;
            }
            //outgoing_cltv_value
            jprm = cJSON_GetArrayItem(jarray, 3);
            if (jprm && (jprm->type == cJSON_Number)) {
                p->outgoing_cltv_value = jprm->valueint + blockcnt;
                DBG_PRINTF("  outgoing_cltv_value=%u\n", p->outgoing_cltv_value);
            } else {
                DBG_PRINTF("fail: p=%p\n", jprm);
                index = -1;
                goto LABEL_EXIT;
            }
        } else {
            DBG_PRINTF("fail: p=%p\n", jarray);
            index = -1;
            goto LABEL_EXIT;
        }
    }

    SYSLOG_INFO("payment");

    lnapp_conf_t *p_appconf = search_connected_lnapp_node(payconf.hop_datain[1].pubkey);
    if (p_appconf != NULL) {

        bool inited = lnapp_is_inited(p_appconf);
        if (inited) {
            bool ret;
            ret = lnapp_payment(p_appconf, &payconf);
            if (ret) {
                result = cJSON_CreateString("Progressing");
            } else {
                ctx->error_code = RPCERR_PAY_STOP;
                ctx->error_message = ucoind_error_str(RPCERR_PAY_STOP);
            }
        } else {
            //BOLTメッセージとして初期化が完了していない(init/channel_reestablish交換できていない)
            ctx->error_code = RPCERR_NOINIT;
            ctx->error_message = ucoind_error_str(RPCERR_NOINIT);
        }
    } else {
        ctx->error_code = RPCERR_NOCONN;
        ctx->error_message = ucoind_error_str(RPCERR_NOCONN);
    }

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    if (ctx->error_code != 0) {
        ln_db_annoskip_invoice_del(payconf.payment_hash);
        //一時的なスキップは削除する
        ln_db_annoskip_drop(true);
    }

    return result;
}


/** 送金開始
 *
 * 一時ルーティング除外リストをクリアしてから送金する
 */
static cJSON *cmd_routepay_first(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    SYSLOG_INFO("routepay_first");
    ln_db_annoskip_drop(true);
    mPayTryCount = 0;
    return cmd_routepay(ctx, params, id);
}


/** 送金・再送金
 *
 */
static cJSON *cmd_routepay(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    cJSON *result = NULL;
    char str_payhash[2 * LN_SZ_HASH + 1];
    char str_payee[2 * UCOIN_SZ_PUBKEY + 1];
    char str_payer[2 * UCOIN_SZ_PUBKEY + 1];
    uint64_t amount_msat = 0;
    uint32_t min_final_cltv_expiry = LN_MIN_FINAL_CLTV_EXPIRY;
    uint8_t addnum = 0;
    ln_fieldr_t *p_rfield = NULL;

    bool ret = false;
    bool retry = false;

    if (params == NULL) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
        goto LABEL_EXIT;
    }

    ret = routepay_param(params, 0,
                    str_payhash, str_payee, str_payer,
                    &amount_msat, &min_final_cltv_expiry,
                    &addnum, &p_rfield);
    if (!ret) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
        goto LABEL_EXIT;
    }

    uint8_t payhash[LN_SZ_HASH];
    ret = misc_str2bin(payhash, LN_SZ_HASH, str_payhash);
    if (!ret) {
        DBG_PRINTF("invalid arg: payemtn_hash\n");
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
        goto LABEL_EXIT;
    }

    uint8_t node_payee[UCOIN_SZ_PUBKEY];
    ret = misc_str2bin(node_payee, sizeof(node_payee), str_payee);
    if (!ret) {
        DBG_PRINTF("invalid arg: payee node id\n");
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
        goto LABEL_EXIT;
    }

    //blockcount
    int32_t blockcnt = btcprc_getblockcount();
    DBG_PRINTF("blockcnt=%d\n", blockcnt);
    if (blockcnt < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
        goto LABEL_EXIT;
    }
    ln_routing_result_t rt_ret;
    lnerr_route_t rerr = ln_routing_calculate(&rt_ret, ln_node_getid(), node_payee,
                    blockcnt + min_final_cltv_expiry, amount_msat,
                    addnum, p_rfield);
    APP_FREE(p_rfield);
    if (rerr != LNROUTE_NONE) {
        DBG_PRINTF("fail: routing\n");
        ret = false;
        switch (rerr) {
        case LNROUTE_NOTFOUND:
            ctx->error_code = LNERR_ROUTE_NOTFOUND;
            break;
        case LNROUTE_TOOMANYHOP:
            ctx->error_code = LNERR_ROUTE_TOOMANYHOP;
            break;
        default:
            ctx->error_code = LNERR_ROUTE_ERROR;
            break;
        }
        ctx->error_message = ucoind_error_str(ctx->error_code);
        goto LABEL_EXIT;
    }

    // 送金開始
    //      これ以降は失敗してもリトライする
    SYSLOG_INFO("routepay");
    ret = false;

    //再送のためにinvoice保存
    char *p_invoice = cJSON_PrintUnformatted(params);
    (void)ln_db_annoskip_invoice_save(p_invoice, payhash);

    DBG_PRINTF("-----------------------------------\n");
    for (int lp = 0; lp < rt_ret.hop_num; lp++) {
        DBG_PRINTF("node_id[%d]: ", lp);
        DUMPBIN(rt_ret.hop_datain[lp].pubkey, UCOIN_SZ_PUBKEY);
        DBG_PRINTF("  amount_msat: %" PRIu64 "\n", rt_ret.hop_datain[lp].amt_to_forward);
        DBG_PRINTF("  cltv_expiry: %" PRIu32 "\n", rt_ret.hop_datain[lp].outgoing_cltv_value);
        DBG_PRINTF("  short_channel_id: %" PRIx64 "\n", rt_ret.hop_datain[lp].short_channel_id);
    }
    DBG_PRINTF("-----------------------------------\n");

    lnapp_conf_t *p_appconf = search_connected_lnapp_node(rt_ret.hop_datain[1].pubkey);
    if (p_appconf != NULL) {
        bool inited = lnapp_is_inited(p_appconf);
        if (inited) {
            payment_conf_t payconf;

            memcpy(payconf.payment_hash, payhash, LN_SZ_HASH);
            payconf.hop_num = rt_ret.hop_num;
            memcpy(payconf.hop_datain, rt_ret.hop_datain, sizeof(ln_hop_datain_t) * (1 + LN_HOP_MAX));

            ret = lnapp_payment(p_appconf, &payconf);
            if (ret) {
                DBG_PRINTF("start payment\n");
            } else {
                DBG_PRINTF("fail: lnapp_payment\n");
            }
        } else {
            //BOLTメッセージとして初期化が完了していない(init/channel_reestablish交換できていない)
            DBG_PRINTF("fail: not inited\n");
        }
    } else {
        DBG_PRINTF("fail: not connect\n");
    }

    mPayTryCount++;
    result = cJSON_CreateString("start payment");
    if (mPayTryCount == 1) {
        //初回ログ
        uint64_t total_amount = ln_node_total_msat();
        misc_save_event(NULL, "payment: payment_hash=%s payee=%s total_msat=%" PRIu64" amount_msat=%" PRIu64,
                    str_payhash, str_payee, total_amount, amount_msat);
    }
    if (!ret) {
        //送金リトライ
        ln_db_annoskip_save(rt_ret.hop_datain[0].short_channel_id, true);

        cmd_json_pay_retry(payhash, p_invoice);
        DBG_PRINTF("retry: %" PRIx64 "\n", rt_ret.hop_datain[0].short_channel_id);
        retry = true;
    }
    free(p_invoice);

LABEL_EXIT:
    if (!ret && !retry) {
        //送金失敗
        ln_db_annoskip_invoice_del(payhash);
        ln_db_annoskip_drop(true);

        //最後に失敗した時間
        char date[50];
        misc_datetime(date, sizeof(date));
        sprintf(mLastPayErr, "[%s]payment fail", date);
        DBG_PRINTF("%s\n", mLastPayErr);
        misc_save_event(NULL, "payment fail: payment_hash=%s try=%d", str_payhash, mPayTryCount);
    }

    return result;
}


static cJSON *cmd_close(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    daemon_connect_t conn;
    cJSON *result = NULL;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //connect parameter
    index = json_connect(params, index, &conn);
    if (index < 0) {
        goto LABEL_EXIT;
    }

    SYSLOG_INFO("close");

    lnapp_conf_t *p_appconf = search_connected_lnapp_node(conn.node_id);
    if ((p_appconf != NULL) && (ln_htlc_num(p_appconf->p_self) == 0)) {
        //接続中
        bool ret = lnapp_close_channel(p_appconf);
        if (ret) {
            result = cJSON_CreateString("Progressing");
        } else {
            ctx->error_code = RPCERR_CLOSE_START;
            ctx->error_message = ucoind_error_str(RPCERR_CLOSE_START);
        }
    } else {
        //未接続
        bool haveCnl = ln_node_search_channel(NULL, conn.node_id);
        if (haveCnl) {
            //チャネルあり
            //  相手とのチャネルがあるので、接続自体は可能かもしれない。
            //  closeの仕方については、仕様や運用とも関係が深いので、後で変更することになるだろう。
            //  今は、未接続の場合は mutual close以外で閉じることにする。
            DBG_PRINTF("チャネルはあるが接続していない\n");
            bool ret = lnapp_close_channel_force(conn.node_id);
            if (ret) {
                result = cJSON_CreateString("unilateral close");
                DBG_PRINTF("force closed\n");
            } else {
                DBG_PRINTF("fail: force close\n");
                ctx->error_code = RPCERR_CLOSE_FAIL;
                ctx->error_message = ucoind_error_str(RPCERR_CLOSE_FAIL);
            }
        } else {
            //チャネルなし
            ctx->error_code = RPCERR_NOCHANN;
            ctx->error_message = ucoind_error_str(RPCERR_NOCHANN);
        }
    }

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


static cJSON *cmd_getlasterror(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    daemon_connect_t conn;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //connect parameter
    index = json_connect(params, index, &conn);
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
        goto LABEL_EXIT;
    }

    SYSLOG_INFO("getlasterror");

    lnapp_conf_t *p_appconf = search_connected_lnapp_node(conn.node_id);
    if (p_appconf != NULL) {
        //接続中
        DBG_PRINTF("error code: %d\n", p_appconf->err);
        ctx->error_code = p_appconf->err;
        if (p_appconf->p_errstr != NULL) {
            DBG_PRINTF("error msg: %s\n", p_appconf->p_errstr);
            ctx->error_message = p_appconf->p_errstr;
        }
    } else {
        ctx->error_code = RPCERR_NOCONN;
        ctx->error_message = ucoind_error_str(RPCERR_NOCONN);
    }

LABEL_EXIT:
    return NULL;
}


static cJSON *cmd_debug(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)id;

    const char *ret;
    char str[10];
    cJSON *json;

    json = cJSON_GetArrayItem(params, 0);
    if (json && (json->type == cJSON_Number)) {
        unsigned long dbg = ln_get_debug() ^ json->valueint;
        ln_set_debug(dbg);
        sprintf(str, "%08lx", dbg);
        if (!LN_DBG_FULFILL()) {
            DBG_PRINTF("no fulfill return\n");
        }
        if (!LN_DBG_CLOSING_TX()) {
            DBG_PRINTF("no closing tx\n");
        }
        if (!LN_DBG_MATCH_PREIMAGE()) {
            DBG_PRINTF("force preimage mismatch\n");
        }
        if (!LN_DBG_NODE_AUTO_CONNECT()) {
            DBG_PRINTF("no node Auto connect\n");
        }
        ret = str;
    } else {
        ret = kNG;
    }

    return cJSON_CreateString(ret);
}


static cJSON *cmd_getcommittx(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    daemon_connect_t conn;
    cJSON *result = NULL;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //connect parameter
    index = json_connect(params, index, &conn);
    if (index < 0) {
        goto LABEL_EXIT;
    }

    SYSLOG_INFO("getcommittx");

    lnapp_conf_t *p_appconf = search_connected_lnapp_node(conn.node_id);
    if (p_appconf != NULL) {
        //接続中
        result = cJSON_CreateObject();
        bool ret = lnapp_get_committx(p_appconf, result);
        if (!ret) {
            ctx->error_code = RPCERR_ERROR;
            ctx->error_message = ucoind_error_str(RPCERR_ERROR);
        }
    } else {
        //未接続
        ctx->error_code = RPCERR_NOCHANN;
        ctx->error_message = ucoind_error_str(RPCERR_NOCHANN);
    }

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


static cJSON *cmd_disautoconn(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    const char *p_str = NULL;

    cJSON *json = cJSON_GetArrayItem(params, 0);
    if (json && (json->type == cJSON_String)) {
        if (json->valuestring[0] == '1') {
            monitor_disable_autoconn(true);
            p_str = "disable auto connect";
        } else if (json->valuestring[0] == '0') {
            monitor_disable_autoconn(false);
            p_str = "enable auto connect";
        } else {
            //none
        }
    }
    if (p_str != NULL) {
        return cJSON_CreateString(p_str);
    } else {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
        return NULL;
    }
}


static cJSON *cmd_removechannel(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    bool ret = false;

    cJSON *json = cJSON_GetArrayItem(params, 0);
    if (json && (json->type == cJSON_String)) {
        uint8_t channel_id[LN_SZ_CHANNEL_ID];
        misc_str2bin(channel_id, sizeof(channel_id), json->valuestring);
        ret = ln_db_self_del(channel_id);
    }
    if (ret) {
        return cJSON_CreateString(kOK);
    } else {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
        return NULL;
    }
}


static cJSON *cmd_setfeerate(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)id;

    cJSON *json;
    uint32_t feerate_per_kw = 0;
    cJSON *result = NULL;
    int index = 0;

    if (params == NULL) {
        index = -1;
        goto LABEL_EXIT;
    }

    //feerate_per_kw
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number) && (json->valueu64 <= UINT32_MAX)) {
        feerate_per_kw = (uint32_t)json->valueu64;
        DBG_PRINTF("feerate_per_kw=%" PRIu32 "\n", feerate_per_kw);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }

    SYSLOG_INFO("setfeerate");
    monitor_set_feerate_per_kw(feerate_per_kw);
    result = cJSON_CreateString(kOK);

LABEL_EXIT:
    if (index < 0) {
        ctx->error_code = RPCERR_PARSE;
        ctx->error_message = ucoind_error_str(RPCERR_PARSE);
    }
    return result;
}


/** #cmd_routepay()のJSON解析部
 *
 */
static bool routepay_param(cJSON *params, int index,
                    char *pStrPayhash,
                    char *pStrPayee,
                    char *pStrPayer,
                    uint64_t *pAmountMsat,
                    uint32_t *pMinFinalCltvExpiry,
                    uint8_t *pAddNum,
                    ln_fieldr_t **ppRField)
{
    bool ret = false;
    cJSON *json;

    //str_payhash, amount_msat, str_payee, str_payer
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_String)) {
        strcpy(pStrPayhash, json->valuestring);
        DBG_PRINTF("str_payhash=%s\n", pStrPayhash);
    } else {
        goto LABEL_EXIT;
    }
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        *pAmountMsat = json->valueu64;
        DBG_PRINTF("  amount_msat=%" PRIu64 "\n", *pAmountMsat);
    } else {
        goto LABEL_EXIT;
    }
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_String)) {
        strcpy(pStrPayee, json->valuestring);
        DBG_PRINTF("str_payee=%s\n", pStrPayee);
    } else {
        goto LABEL_EXIT;
    }
    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_String)) {
        if (strlen(json->valuestring) > 0) {
            strcpy(pStrPayer, json->valuestring);
        } else {
            //自分をpayerにする
            misc_bin2str(pStrPayer, ln_node_getid(), UCOIN_SZ_PUBKEY);
        }
        DBG_PRINTF("str_payer=%s\n", pStrPayer);
    } else {
        index = -1;
        goto LABEL_EXIT;
    }
    ret = true;

    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        *pMinFinalCltvExpiry = (uint32_t)json->valueu64;
    }
    DBG_PRINTF("  min_final_cltv_expiry=%" PRIu32 "\n", *pMinFinalCltvExpiry);

    json = cJSON_GetArrayItem(params, index++);
    if (json && (json->type == cJSON_Number)) {
        ret = false;

        *pAddNum = (uint8_t)json->valueint;
        DBG_PRINTF("  r field num=%u\n", *pAddNum);
        if (*pAddNum > 0) {
            //array
            json = cJSON_GetArrayItem(params, index++);
            if (json && (json->type == cJSON_Array)) {
                DBG_PRINTF("trace array\n");
            } else {
                goto LABEL_EXIT;
            }

            //[ [...], [...], ..., [...] ]
            *ppRField = (ln_fieldr_t *)APP_MALLOC(sizeof(ln_fieldr_t) * (*pAddNum));
            for (uint8_t lp = 0; lp < *pAddNum; lp++) {
                ln_fieldr_t *p_fieldr = ppRField[lp];
                cJSON *jarray = cJSON_GetArrayItem(json, lp);
                if (jarray && (jarray->type == cJSON_Array)) {
                    //[node_id, short_channel_id, fee_base_msat, fee_prop_millionths,cltv_expiry_delta]

                    cJSON *jprm = cJSON_GetArrayItem(jarray, 0);
                    if (jprm && (jprm->type == cJSON_String)) {
                        bool retval = misc_str2bin(p_fieldr->node_id, UCOIN_SZ_PUBKEY, jprm->valuestring);
                        if (!retval) {
                            goto LABEL_EXIT;
                        }
                    } else {
                        goto LABEL_EXIT;
                    }
                    DBG_PRINTF("node_id[%d]: ", lp);
                    DUMPBIN(p_fieldr->node_id, UCOIN_SZ_PUBKEY);

                    jprm = cJSON_GetArrayItem(jarray, 1);
                    if (jprm && (jprm->type == cJSON_Number)) {
                        p_fieldr->short_channel_id = jprm->valueu64;
                    } else {
                        goto LABEL_EXIT;
                    }
                    DBG_PRINTF("short_channel_id[%d]: %" PRIx64 "\n", lp, p_fieldr->short_channel_id);

                    jprm = cJSON_GetArrayItem(jarray, 2);
                    if (jprm && (jprm->type == cJSON_Number)) {
                        p_fieldr->fee_base_msat = (uint32_t)jprm->valueu64;
                    } else {
                        goto LABEL_EXIT;
                    }
                    DBG_PRINTF("fee_base_msat[%d]: %" PRIu32 "\n", lp, p_fieldr->fee_base_msat);

                    jprm = cJSON_GetArrayItem(jarray, 3);
                    if (jprm && (jprm->type == cJSON_Number)) {
                        p_fieldr->fee_prop_millionths = (uint32_t)jprm->valueu64;
                    } else {
                        goto LABEL_EXIT;
                    }
                    DBG_PRINTF("fee_prop_millionths[%d]: %" PRIu32 "\n", lp, p_fieldr->fee_prop_millionths);

                    jprm = cJSON_GetArrayItem(jarray, 4);
                    if (jprm && (jprm->type == cJSON_Number)) {
                        p_fieldr->cltv_expiry_delta = (uint16_t)jprm->valueu64;
                    } else {
                        goto LABEL_EXIT;
                    }
                    DBG_PRINTF("cltv_expiry_delta[%d]: %" PRIu16 "\n", lp, p_fieldr->cltv_expiry_delta);
                } else {
                    goto LABEL_EXIT;
                }
            }
        }

        ret = true;
    }


LABEL_EXIT:
    return ret;
}


static char *create_bolt11(const uint8_t *pPayHash, uint64_t Amount)
{
    uint8_t type;
    ucoin_genesis_t gtype = ucoin_util_get_genesis(ln_get_genesishash());
    switch (gtype) {
    case UCOIN_GENESIS_BTCMAIN:
        type = LN_INVOICE_MAINNET;
        break;
    case UCOIN_GENESIS_BTCTEST:
        type = LN_INVOICE_TESTNET;
        break;
    case UCOIN_GENESIS_BTCREGTEST:
        type = LN_INVOICE_REGTEST;
        break;
    default:
        type = UCOIN_GENESIS_UNKNOWN;
        break;
    }
    char *p_invoice = NULL;
    if (type != UCOIN_GENESIS_UNKNOWN) {
        ln_invoice_create(&p_invoice, type, pPayHash, Amount);
    }
    return p_invoice;
}


static lnapp_conf_t *search_connected_lnapp_node(const uint8_t *p_node_id)
{
    lnapp_conf_t *p_appconf;

    p_appconf = p2p_cli_search_node(p_node_id);
    if (p_appconf == NULL) {
        p_appconf = p2p_svr_search_node(p_node_id);
    }
    return p_appconf;
}
