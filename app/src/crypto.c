/*******************************************************************************
*   (c) 2019 Zondax GmbH
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include "crypto.h"
#include "coin.h"
#include "zxmacros.h"
#include "apdu_codes.h"
#include "tx.h"

#include <bech32.h>

uint32_t hdPath[HDPATH_LEN_DEFAULT];

uint8_t bech32_hrp_len;
char bech32_hrp[MAX_BECH32_HRP_LEN + 1];

#if defined(TARGET_NANOS) || defined(TARGET_NANOX) || defined(TARGET_NANOS2)
#include "cx.h"

zxerr_t crypto_extractPublicKey(const uint32_t path[HDPATH_LEN_DEFAULT], uint8_t *pubKey, uint16_t pubKeyLen) {
    cx_ecfp_public_key_t cx_publicKey;
    cx_ecfp_private_key_t cx_privateKey;
    uint8_t privateKeyData[32];

    if (pubKeyLen < PK_LEN_SECP256K1) {
        return zxerr_invalid_crypto_settings;
    }

    zxerr_t err = zxerr_ok;
    BEGIN_TRY
    {
        TRY {
            os_perso_derive_node_bip32(CX_CURVE_256K1,
                                       path,
                                       HDPATH_LEN_DEFAULT,
                                       privateKeyData, NULL);

            cx_ecfp_init_private_key(CX_CURVE_256K1, privateKeyData, 32, &cx_privateKey);
            cx_ecfp_init_public_key(CX_CURVE_256K1, NULL, 0, &cx_publicKey);
            cx_ecfp_generate_pair(CX_CURVE_256K1, &cx_publicKey, &cx_privateKey, 1);
        }
        CATCH_OTHER(e) {
            err = zxerr_ledger_api_error;
        }
        FINALLY {
            MEMZERO(&cx_privateKey, sizeof(cx_privateKey));
            MEMZERO(privateKeyData, 32);
        }
    }
    END_TRY;

    if (err != zxerr_ok) {
        return err;
    }

    // Format pubkey
    for (int i = 0; i < 32; i++) {
        pubKey[i] = cx_publicKey.W[64 - i];
    }
    cx_publicKey.W[0] = cx_publicKey.W[64] & 1 ? 0x03 : 0x02; // "Compress" public key in place
    if ((cx_publicKey.W[32] & 1) != 0) {
        pubKey[31] |= 0x80;
    }
    //////////////////////
    MEMCPY(pubKey, cx_publicKey.W, PK_LEN_SECP256K1);

    return err;
}

void keccak_hash_message(const uint8_t *message,uint16_t message_len ,uint8_t* signature) {
    cx_sha3_t global_sha3;
    cx_keccak_init(&global_sha3, 256);
    cx_hash((cx_hash_t *) &global_sha3, CX_LAST, message, message_len, signature, CX_SHA256_SIZE);
}

zxerr_t crypto_sign(uint8_t *signature,
                   uint16_t signatureMaxlen,
                   uint16_t *sigSize) {
    uint8_t messageDigest[CX_SHA256_SIZE];
    MEMZERO(messageDigest,sizeof(messageDigest));

    // Hash it
    const uint8_t *message = tx_get_buffer();
    const uint16_t messageLen = tx_get_buffer_length();
    keccak_hash_message(message,messageLen,messageDigest);

    cx_ecfp_private_key_t cx_privateKey;
    uint8_t privateKeyData[32];
    unsigned int info = 0;
    int signatureLength = 0;

    zxerr_t err = zxerr_ok;
    BEGIN_TRY
    {
        TRY
        {
            // Generate keys
            os_perso_derive_node_bip32(CX_CURVE_SECP256K1,
                                       hdPath,
                                       HDPATH_LEN_DEFAULT,
                                       privateKeyData, NULL);

            cx_ecfp_init_private_key(CX_CURVE_SECP256K1, privateKeyData, 32, &cx_privateKey);

            // Sign
            signatureLength = cx_ecdsa_sign(&cx_privateKey,
                                            CX_RND_RFC6979 | CX_LAST,
                                            CX_SHA256,
                                            messageDigest,
                                            CX_SHA256_SIZE,
                                            signature,
                                            signatureMaxlen,
                                            &info);
        }
        CATCH_OTHER(e) {
            err = zxerr_ledger_api_error;
        }
        FINALLY {
            MEMZERO(&cx_privateKey, sizeof(cx_privateKey));
            MEMZERO(privateKeyData, 32);
        }
    }
    END_TRY;

    *sigSize = signatureLength;
    return err;
}

#else

void crypto_extractPublicKey(const uint32_t path[HDPATH_LEN_DEFAULT], uint8_t *pubKey, uint16_t pubKeyLen) {
    ///////////////////////////////////////
    // THIS IS ONLY USED FOR TEST PURPOSES
    ///////////////////////////////////////

    // Empty version for non-Ledger devices
    MEMZERO(pubKey, pubKeyLen);
}

uint16_t crypto_sign(uint8_t *signature,
                     uint16_t signatureMaxlen,
                     const uint8_t *message,
                     uint16_t messageLen) {
    // Empty version for non-Ledger devices
    return 0;
}

#endif

uint8_t extractHRP(uint32_t rx, uint32_t offset) {
    if (rx < offset + 1) {
        THROW(APDU_CODE_DATA_INVALID);
    }
    MEMZERO(bech32_hrp, MAX_BECH32_HRP_LEN);

    bech32_hrp_len = G_io_apdu_buffer[offset];

    if (bech32_hrp_len == 0 || bech32_hrp_len > MAX_BECH32_HRP_LEN) {
        THROW(APDU_CODE_DATA_INVALID);
    }

    memcpy(bech32_hrp, G_io_apdu_buffer + offset + 1, bech32_hrp_len);
    bech32_hrp[bech32_hrp_len] = 0;     // zero terminate

    return bech32_hrp_len;
}

void ripemd160_32(uint8_t *out, uint8_t *in) {
    cx_ripemd160_t rip160;
    cx_ripemd160_init(&rip160);
    cx_hash(&rip160.header, CX_LAST, in, CX_SHA256_SIZE, out, CX_RIPEMD160_SIZE);
}

void crypto_set_hrp(char *p) {
    bech32_hrp_len = strlen(p);
    if (bech32_hrp_len < MAX_BECH32_HRP_LEN) {
        snprintf(bech32_hrp, sizeof(bech32_hrp), "%s", p);
    }
}

zxerr_t crypto_fillAddress(uint8_t *buffer, uint16_t buffer_len, uint16_t *addrResponseLen) {
    if (buffer_len < PK_LEN_SECP256K1 + 50) {
        return zxerr_buffer_too_small;
    }

    // extract pubkey
    CHECK_ZXERR(crypto_extractPublicKey(hdPath, buffer, buffer_len))

    // Hash it
    uint8_t hashed1_pk[CX_SHA256_SIZE];
    cx_hash_sha256(buffer, PK_LEN_SECP256K1, hashed1_pk, CX_SHA256_SIZE);

    uint8_t hashed2_pk[CX_RIPEMD160_SIZE];
    ripemd160_32(hashed2_pk, hashed1_pk);

    char *addr = (char *) (buffer + PK_LEN_SECP256K1);
    CHECK_ZXERR(bech32EncodeFromBytes(addr, buffer_len - PK_LEN_SECP256K1, bech32_hrp, hashed2_pk, CX_RIPEMD160_SIZE, 1))

    *addrResponseLen = PK_LEN_SECP256K1 + strlen(addr);

    return zxerr_ok;
}

static const char HEXDIGITS[] = "0123456789abcdef";
size_t crypto_eth_address_bytes_to_string(cx_sha3_t * sha3Context,uint8_t *address, uint8_t * result) {
    result[0] = '0';
    result[1] = 'x';
    result = result + 2;
    union locals_union {
        uint8_t hashChecksum[32];
        uint8_t tmp[51];
    } locals_union;
    uint8_t i;
    uint32_t offset = 0;
    for (i = 0; i < 20; i++) {
        uint8_t digit = address[i];
        locals_union.tmp[offset + 2 * i] = HEXDIGITS[(digit >> 4) & 0x0f];
        locals_union.tmp[offset + 2 * i + 1] = HEXDIGITS[digit & 0x0f];
    }
    cx_keccak_init(sha3Context, 256);
    cx_hash((cx_hash_t *) sha3Context,
            CX_LAST,
            locals_union.tmp,
            offset + 40,
            locals_union.hashChecksum,
            32);

    for (i = 0; i < 40; i++) {
        uint8_t digit = address[i / 2];
        if ((i % 2) == 0) {
            digit = (digit >> 4) & 0x0f;
        } else {
            digit = digit & 0x0f;
        }
        if (digit < 10) {
            result[i] = HEXDIGITS[digit];
        } else {
            int v = (locals_union.hashChecksum[i / 2] >> (4 * (1 - i % 2))) & 0x0f;
            if (v >= 8) {
                result[i] = HEXDIGITS[digit] - 'a' + 'A';
            } else {
                result[i] = HEXDIGITS[digit];
            }
        }
    }
    return 42;
}

zxerr_t crypto_fillETHAddress(uint8_t *buffer, uint16_t buffer_len, uint16_t *addrResponseLen) {
    if (buffer_len < PK_LEN_SECP256K1 + 20) {
        return zxerr_buffer_too_small;
    }

    // extract publickey
    cx_ecfp_public_key_t cx_publicKey;
    cx_ecfp_private_key_t cx_privateKey;
    uint8_t privateKeyData[32];
    zxerr_t err = zxerr_ok;
    BEGIN_TRY
    {
        TRY {
            os_perso_derive_node_bip32(CX_CURVE_256K1,
                                       hdPath,
                                       HDPATH_LEN_DEFAULT,
                                       privateKeyData, NULL);

            cx_ecfp_init_private_key(CX_CURVE_256K1, privateKeyData, 32, &cx_privateKey);
            cx_ecfp_init_public_key(CX_CURVE_256K1, NULL, 0, &cx_publicKey);
            cx_ecfp_generate_pair(CX_CURVE_256K1, &cx_publicKey, &cx_privateKey, 1);
        }
        CATCH_OTHER(e) {
            err = zxerr_ledger_api_error;
        }
        FINALLY {
            MEMZERO(&cx_privateKey, sizeof(cx_privateKey));
            MEMZERO(privateKeyData, 32);
        }
    }
    END_TRY;

    if (err != zxerr_ok) {
        return err;
    }

    // Format pubkey
    for (int i = 0; i < 32; i++) {
        buffer[i] = cx_publicKey.W[64 - i];
    }
    cx_publicKey.W[0] = cx_publicKey.W[64] & 1 ? 0x03 : 0x02; // "Compress" public key in place
    if ((cx_publicKey.W[32] & 1) != 0) {
        buffer[31] |= 0x80;
    }
    // Copy compressed public to buffer
    MEMCPY(buffer, cx_publicKey.W, PK_LEN_SECP256K1);

    // uncompressed public key to ETH address bytes
    uint8_t *addr = (uint8_t *) (buffer + PK_LEN_SECP256K1);
    uint8_t hashAddress[32];
    cx_sha3_t global_sha3;
    cx_keccak_init(&global_sha3, 256);
    cx_hash((cx_hash_t *) &global_sha3, CX_LAST, cx_publicKey.W + 1, 64, hashAddress, 32);

    // convert Address Bytes to String
    *addrResponseLen = PK_LEN_SECP256K1 + crypto_eth_address_bytes_to_string(&global_sha3,hashAddress + 12,addr);
    return zxerr_ok;
}
