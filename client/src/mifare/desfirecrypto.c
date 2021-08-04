/*-
 * Copyright (C) 2010, Romain Tartiere.
 * Copyright (C) 2021 Merlok
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 * $Id$
 */

#include "desfirecrypto.h"

#include <stdlib.h>
#include <string.h>
#include <util.h>
#include "ui.h"
#include "aes.h"
#include "des.h"
#include <mbedtls/cmac.h>
#include "crc.h"
#include "crc16.h"        // crc16 ccitt
#include "crc32.h"
#include "commonutil.h"
#include "mifare/desfire_crypto.h"

void DesfireClearContext(DesfireContext *ctx) {
    ctx->keyNum = 0;
    ctx->keyType = T_DES;
    memset(ctx->key, 0, sizeof(ctx->key));

    ctx->secureChannel = DACNone;
    ctx->cmdSet = DCCNative;
    ctx->commMode = DCMNone;

    ctx->appSelected = false;

    ctx->kdfAlgo = 0;
    ctx->kdfInputLen = 0;
    memset(ctx->kdfInput, 0, sizeof(ctx->kdfInput));

    DesfireClearSession(ctx);
}

void DesfireClearSession(DesfireContext *ctx) {
    ctx->secureChannel = DACNone; // here none - not authenticared

    memset(ctx->IV, 0, sizeof(ctx->IV));
    memset(ctx->sessionKeyMAC, 0, sizeof(ctx->sessionKeyMAC));
    memset(ctx->sessionKeyEnc, 0, sizeof(ctx->sessionKeyEnc));
    memset(ctx->lastIV, 0, sizeof(ctx->lastIV));
    ctx->lastCommand = 0;
    ctx->lastRequestZeroLen = false;
    ctx->cmdCntr = 0;
    memset(ctx->TI, 0, sizeof(ctx->TI));
}

void DesfireClearIV(DesfireContext *ctx) {
    memset(ctx->IV, 0, sizeof(ctx->IV));
}

void DesfireSetKey(DesfireContext *ctx, uint8_t keyNum, enum DESFIRE_CRYPTOALGO keyType, uint8_t *key) {
    DesfireClearContext(ctx);

    ctx->keyNum = keyNum;
    ctx->keyType = keyType;
    memcpy(ctx->key, key, desfire_get_key_length(keyType));
}

void DesfireSetCommandSet(DesfireContext *ctx, DesfireCommandSet cmdSet) {
    ctx->cmdSet = cmdSet;
}

void DesfireSetCommMode(DesfireContext *ctx, DesfireCommunicationMode commMode) {
    ctx->commMode = commMode;
}

void DesfireSetKdf(DesfireContext *ctx, uint8_t kdfAlgo, uint8_t *kdfInput, uint8_t kdfInputLen) {
    ctx->kdfAlgo = kdfAlgo;
    ctx->kdfInputLen = kdfInputLen;
    if (kdfInputLen)
        memcpy(ctx->kdfInput, kdfInput, kdfInputLen);
}

bool DesfireIsAuthenticated(DesfireContext *dctx) {
    return dctx->secureChannel != DACNone;
}

size_t DesfireGetMACLength(DesfireContext *ctx) {
    size_t mac_length = MAC_LENGTH;
    switch (ctx->secureChannel) {
        case DACNone:
            mac_length = 0;
            break;
        case DACd40:
            mac_length = 4;
            break;
        case DACEV1:
            mac_length = 8;
            break;
        case DACEV2:
            mac_length = 8;
            break;
    }
    return mac_length;
}

size_t DesfireSearchCRCPos(uint8_t *data, size_t datalen, uint8_t respcode, uint8_t crclen) {
    size_t crcpos = datalen - 1;
    while (crcpos > 0)
        if (data[crcpos] == 0)
            crcpos--;
        else
            break;
    crcpos++; // crc may be 0x00000000 or 0x0000
    if (crcpos < crclen) {
        PrintAndLogEx(WARNING, "No space for crc. pos: %zu", crcpos);
        return 0;
    }

    uint8_t crcdata[1024] = {0};
    size_t crcposfound = 0;
    // crc may be 00..00 and at the end of file may be padding 0x80. so we search from last zero to crclen + 2 (one for crc=0 and one for padding 0x80)
    for (int i = 0; i < crclen + 2; i++) {
        if (crcpos - i == 0)
            break;
        if (crcpos - i + crclen > datalen)
            continue;

        memcpy(crcdata, data, crcpos - i);
        crcdata[crcpos - i] = respcode;
        bool res;
        if (crclen == 4)
            res = desfire_crc32_check(crcdata, crcpos - i + 1, &data[crcpos - i]);
        else
            res = iso14443a_crc_check(data, crcpos - i, &data[crcpos - i]);
        if (res) {
            crcposfound = crcpos - i;
        }
    }

    return crcposfound;
}

static void DesfireCryptoEncDecSingleBlock(uint8_t *key, DesfireCryptoAlgorythm keyType, uint8_t *data, uint8_t *dstdata, uint8_t *ivect, bool dir_to_send, bool encode) {
    size_t block_size = desfire_get_key_block_length(keyType);
    uint8_t sdata[MAX_CRYPTO_BLOCK_SIZE] = {0};
    memcpy(sdata, data, block_size);
    if (dir_to_send) {
        bin_xor(sdata, ivect, block_size);
    }

    uint8_t edata[MAX_CRYPTO_BLOCK_SIZE] = {0};

    switch (keyType) {
        case T_DES:
            if (encode)
                des_encrypt(edata, sdata, key);
            else
                des_decrypt(edata, sdata, key);
            break;
        case T_3DES:
            if (encode) {
                mbedtls_des3_context ctx3;
                mbedtls_des3_set2key_enc(&ctx3, key);
                mbedtls_des3_crypt_ecb(&ctx3, sdata, edata);
            } else {
                mbedtls_des3_context ctx3;
                mbedtls_des3_set2key_dec(&ctx3, key);
                mbedtls_des3_crypt_ecb(&ctx3, sdata, edata);
            }
            break;
        case T_3K3DES:
            if (encode) {
                mbedtls_des3_context ctx3;
                mbedtls_des3_set3key_enc(&ctx3, key);
                mbedtls_des3_crypt_ecb(&ctx3, sdata, edata);
            } else {
                mbedtls_des3_context ctx3;
                mbedtls_des3_set3key_dec(&ctx3, key);
                mbedtls_des3_crypt_ecb(&ctx3, sdata, edata);
            }
            break;
        case T_AES:
            if (encode) {
                mbedtls_aes_context actx;
                mbedtls_aes_init(&actx);
                mbedtls_aes_setkey_enc(&actx, key, 128);
                mbedtls_aes_crypt_ecb(&actx, MBEDTLS_AES_ENCRYPT, sdata, edata);
                mbedtls_aes_free(&actx);
            } else {
                mbedtls_aes_context actx;
                mbedtls_aes_init(&actx);
                mbedtls_aes_setkey_dec(&actx, key, 128);
                mbedtls_aes_crypt_ecb(&actx, MBEDTLS_AES_DECRYPT, sdata, edata);
                mbedtls_aes_free(&actx);
            }
            break;
    }

    if (dir_to_send) {
        memcpy(ivect, edata, block_size);
    } else {
        bin_xor(edata, ivect, block_size);
        memcpy(ivect, data, block_size);
    }

    memcpy(dstdata, edata, block_size);
}

void DesfireCryptoEncDecEx(DesfireContext *ctx, DesfireCryptoOpKeyType key_type, uint8_t *srcdata, size_t srcdatalen, uint8_t *dstdata, bool dir_to_send, bool encode, uint8_t *iv) {
    uint8_t data[1024] = {0};
    uint8_t xiv[DESFIRE_MAX_CRYPTO_BLOCK_SIZE] = {0};

    if (ctx->secureChannel == DACd40) {
        memset(ctx->IV, 0, DESFIRE_MAX_CRYPTO_BLOCK_SIZE);
    }

    size_t block_size = desfire_get_key_block_length(ctx->keyType);

    if (iv == NULL)
        memcpy(xiv, ctx->IV, block_size);
    else
        memcpy(xiv, iv, block_size);

    size_t offset = 0;
    while (offset < srcdatalen) {
        if (key_type == DCOSessionKeyMac) {
            DesfireCryptoEncDecSingleBlock(ctx->sessionKeyMAC, ctx->keyType, srcdata + offset, data + offset, xiv, dir_to_send, encode);
        } else if (key_type == DCOSessionKeyEnc) {
            DesfireCryptoEncDecSingleBlock(ctx->sessionKeyEnc, ctx->keyType, srcdata + offset, data + offset, xiv, dir_to_send, encode);
        } else {
            DesfireCryptoEncDecSingleBlock(ctx->key, ctx->keyType, srcdata + offset, data + offset, xiv, dir_to_send, encode);
        }
        offset += block_size;
    }

    if (iv == NULL)
        memcpy(ctx->IV, xiv, block_size);
    else
        memcpy(iv, xiv, block_size);

    if (dstdata)
        memcpy(dstdata, data, srcdatalen);
}

void DesfireCryptoEncDec(DesfireContext *ctx, DesfireCryptoOpKeyType key_type, uint8_t *srcdata, size_t srcdatalen, uint8_t *dstdata, bool encode) {
    bool dir_to_send = encode;
    bool xencode = encode;
    if (ctx->secureChannel == DACd40)
        xencode = false;

    DesfireCryptoEncDecEx(ctx, key_type, srcdata, srcdatalen, dstdata, dir_to_send, xencode, NULL);
}

static void DesfireCMACGenerateSubkeys(DesfireContext *ctx, uint8_t *sk1, uint8_t *sk2) {
    int kbs = desfire_get_key_block_length(ctx->keyType);
    const uint8_t R = (kbs == 8) ? 0x1B : 0x87;

    uint8_t l[kbs];
    memset(l, 0, kbs);

    uint8_t ivect[kbs];
    memset(ivect, 0, kbs);

    DesfireCryptoEncDecEx(ctx, DCOSessionKeyMac, l, kbs, l, true, true, ivect);

    bool txor = false;

    // Used to compute CMAC on complete blocks
    memcpy(sk1, l, kbs);
    txor = l[0] & 0x80;
    lsl(sk1, kbs);
    if (txor) {
        sk1[kbs - 1] ^= R;
    }

    // Used to compute CMAC on the last block if non-complete
    memcpy(sk2, sk1, kbs);
    txor = sk1[0] & 0x80;
    lsl(sk2, kbs);
    if (txor) {
        sk2[kbs - 1] ^= R;
    }
}

void DesfireCryptoCMAC(DesfireContext *ctx, uint8_t *data, size_t len, uint8_t *cmac) {
    int kbs = desfire_get_key_block_length(ctx->keyType);
    if (kbs == 0)
        return;

    uint8_t buffer[padded_data_length(len, kbs)];
    memset(buffer, 0, sizeof(buffer));

    uint8_t sk1[DESFIRE_MAX_CRYPTO_BLOCK_SIZE] = {0};
    uint8_t sk2[DESFIRE_MAX_CRYPTO_BLOCK_SIZE] = {0};
    DesfireCMACGenerateSubkeys(ctx, sk1, sk2);

    memcpy(buffer, data, len);

    if ((!len) || (len % kbs)) {
        buffer[len++] = 0x80;
        while (len % kbs) {
            buffer[len++] = 0x00;
        }
        bin_xor(buffer + len - kbs, sk2, kbs);
    } else {
        bin_xor(buffer + len - kbs, sk1, kbs);
    }

    DesfireCryptoEncDec(ctx, DCOSessionKeyMac, buffer, len, NULL, true);

    if (cmac != NULL)
        memcpy(cmac, ctx->IV, kbs);
}

void DesfireDESKeySetVersion(uint8_t *key, DesfireCryptoAlgorythm keytype, uint8_t version) {
    if (keytype == T_AES)
        return;

    // clear version
    for (int n = 0; n < desfire_get_key_length(keytype); n++)
        key[n] &= 0xFE;

    // set version
    for (int n = 0; n < 8; n++) {
        uint8_t version_bit = ((version & (1 << (7 - n))) >> (7 - n));

        key[n] &= 0xFE;
        key[n] |= version_bit;

        if (keytype == T_DES) {
            key[n + 8] = key[n];
        } else {
            // Write ~version to avoid turning a 3DES key into a DES key
            key[n + 8] &= 0xFE;
            key[n + 8] |= (~version_bit) & 0x01;
        }
    }
}

uint8_t DesfireDESKeyGetVersion(uint8_t *key) {
    uint8_t version = 0;
    for (int n = 0; n < 8; n++)
        version |= ((key[n] & 1) << (7 - n));

    return version;
}

DesfireCryptoAlgorythm DesfireKeyTypeToAlgo(uint8_t keyType) {
    switch (keyType) {
        case 00:
            return T_3DES;
        case 01:
            return T_3K3DES;
        case 02:
            return T_AES;
        default:
            return T_3DES; // unknown....
    }
}

uint8_t DesfireKeyAlgoToType(DesfireCryptoAlgorythm keyType) {
    switch (keyType) {
        case T_DES:
            return 0x00;
        case T_3DES:
            return 0x00;
        case T_3K3DES:
            return 0x01;
        case T_AES:
            return 0x02;
    }
    return 0;
}

void DesfirePrintCardKeyType(uint8_t keyType) {
    switch (keyType) {
        case 00:
            PrintAndLogEx(SUCCESS, "Key: 2TDEA");
            break;
        case 01:
            PrintAndLogEx(SUCCESS, "Key: 3TDEA");
            break;
        case 02:
            PrintAndLogEx(SUCCESS, "Key: AES");
            break;
        default:
            PrintAndLogEx(SUCCESS, "Key: unknown: 0x%02x", keyType);
            break;
    }
}

DesfireCommunicationMode DesfireFileCommModeToCommMode(uint8_t file_comm_mode) {
    DesfireCommunicationMode mode = DCMNone;
    switch (file_comm_mode & 0x03) {
        case 0x00:
        case 0x02:
            mode = DCMPlain;
            break;
        case 0x01:
            mode = DCMMACed;
            break;
        case 0x03:
            mode = DCMEncrypted;
            break;
        default:
            break;
    }
    return mode;
}

uint8_t DesfireCommModeToFileCommMode(DesfireCommunicationMode comm_mode) {
    uint8_t fmode = DCMNone;
    switch (comm_mode) {
        case DCMPlain:
            fmode = 0x00;
            break;
        case DCMMACed:
            fmode = 0x01;
            break;
        case DCMEncrypted:
        case DCMEncryptedPlain:
            fmode = 0x11;
            break;
        case DCMNone:
            break;
    }
    return fmode;
}

void DesfireGenSessionKeyEV1(const uint8_t rnda[], const uint8_t rndb[], DesfireCryptoAlgorythm keytype, uint8_t *key) {
    switch (keytype) {
        case T_DES:
            memcpy(key, rnda, 4);
            memcpy(key + 4, rndb, 4);
            break;
        case T_3DES:
            memcpy(key, rnda, 4);
            memcpy(key + 4, rndb, 4);
            memcpy(key + 8, rnda + 4, 4);
            memcpy(key + 12, rndb + 4, 4);
            break;
        case T_3K3DES:
            memcpy(key, rnda, 4);
            memcpy(key + 4, rndb, 4);
            memcpy(key + 8, rnda + 6, 4);
            memcpy(key + 12, rndb + 6, 4);
            memcpy(key + 16, rnda + 12, 4);
            memcpy(key + 20, rndb + 12, 4);
            break;
        case T_AES:
            memcpy(key, rnda, 4);
            memcpy(key + 4, rndb, 4);
            memcpy(key + 8, rnda + 12, 4);
            memcpy(key + 12, rndb + 12, 4);
            break;
    }
}

// https://www.nxp.com/docs/en/application-note/AN12343.pdf
// page 35
void DesfireGenSessionKeyEV2(uint8_t *key, uint8_t *rndA, uint8_t *rndB, bool enckey, uint8_t *sessionkey) {
    uint8_t data[64] = {0};
    memset(sessionkey, 0, CRYPTO_AES_BLOCK_SIZE);

    if (enckey) {
        data[0] = 0xa5;
        data[1] = 0x5a;
    } else {
        data[0] = 0x5a;
        data[1] = 0xa5;
    }
    data[3] = 0x01;
    data[5] = 0x80;

    // data+6 - start of rnd part
    memcpy(data + 6, rndA, 8);
    bin_xor(data + 8, rndB, 6); // xor rndb 6b
    memcpy(data + 14, rndB + 6, 10);
    memcpy(data + 24, rndA + 8, 8);

    uint8_t cmac[CRYPTO_AES_BLOCK_SIZE] = {0};
    DesfireContext ctx = {0};
    ctx.keyType = T_AES;
    memcpy(ctx.key, key, 16); // aes-128
    DesfireCryptoCMAC(&ctx, data, 32, cmac);

    memcpy(sessionkey, cmac, CRYPTO_AES_BLOCK_SIZE);
}

void DesfireEV2FillIV(DesfireContext *ctx, bool ivforcommand, uint8_t *iv) {
    uint8_t xiv[CRYPTO_AES_BLOCK_SIZE] = {0};

    if (ivforcommand) {
        xiv[0] = 0xa5;
        xiv[1] = 0x5a;
    } else {
        xiv[0] = 0x5a;
        xiv[1] = 0xa5;
    }

    memcpy(xiv + 2, ctx->TI, 4);
    Uint2byteToMemLe(xiv + 2 + 4, ctx->cmdCntr);

    if (aes_encode(NULL, ctx->sessionKeyEnc, xiv, xiv, CRYPTO_AES_BLOCK_SIZE))
        return;

    if (iv == NULL)
        memcpy(ctx->IV, xiv, CRYPTO_AES_BLOCK_SIZE);
    else
        memcpy(iv, xiv, CRYPTO_AES_BLOCK_SIZE);
}

int DesfireEV2CalcCMAC(DesfireContext *ctx, uint8_t cmd, uint8_t *data, size_t datalen, uint8_t *mac) {
    uint8_t mdata[1050] = {0};
    size_t mdatalen = 0;
    
    mdata[0] = cmd;
    Uint2byteToMemLe(&mdata[1], ctx->cmdCntr);
    memcpy(&mdata[3], ctx->TI, 4);
    if (data != NULL && datalen > 0)
        memcpy(&mdata[7], data, datalen);
    mdatalen = 1 + 2 + 4 + datalen;

    return aes_cmac8(NULL, ctx->sessionKeyMAC, mdata, mac, mdatalen);
}

void desfire_crc32(const uint8_t *data, const size_t len, uint8_t *crc) {
    crc32_ex(data, len, crc);
}

void desfire_crc32_append(uint8_t *data, const size_t len) {
    crc32_ex(data, len, data + len);
}

bool desfire_crc32_check(uint8_t *data, const size_t len, uint8_t *crc) {
    uint8_t ccrc[4] = {0};
    desfire_crc32(data, len, ccrc);
    return (memcmp(ccrc, crc, 4) == 0);
}

void iso14443a_crc_append(uint8_t *data, size_t len) {
    return compute_crc(CRC_14443_A, data, len, data + len, data + len + 1);
}

void iso14443a_crc(uint8_t *data, size_t len, uint8_t *pbtCrc) {
    return compute_crc(CRC_14443_A, data, len, pbtCrc, pbtCrc + 1);
}

bool iso14443a_crc_check(uint8_t *data, const size_t len, uint8_t *crc) {
    uint8_t ccrc[2] = {0};
    iso14443a_crc(data, len, ccrc);
    return (memcmp(ccrc, crc, 2) == 0);
}