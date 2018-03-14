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
/** @file   ln_signer.h
 *  @brief  [LN]秘密鍵管理
 *  @author ueno@nayuta.co
 */
#ifndef LN_SIGNER_H__
#define LN_SIGNER_H__

#include "ln_local.h"

/**************************************************************************
 * macros
 **************************************************************************/

/**************************************************************************
 * prototypes
 **************************************************************************/

void ln_signer_init(ln_self_t *self, const uint8_t *pSeed);


void ln_signer_term(ln_self_t *self);


void ln_signer_keys_update(ln_self_t *self, int64_t Offset);


void ln_signer_keys_update_force(ln_self_t *self, uint64_t Index);


/** 1つ前のper_commit_secret取得
 *
 * @param[in,out]   self            チャネル情報
 * @param[out]      pSecret         1つ前のper_commit_secret
 */
void ln_signer_get_prevkey(ln_self_t *self, uint8_t *pSecret);


void ln_signer_dec_index(ln_self_t *self);

#endif /* LN_SIGNER_H__ */