// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mbedtls/bignum.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

/* Keep byte-addressed vectors in RAM on the ESP32-S3 FDPIC/XIP target. */
static uint8_t sha_input[] = { 'a', 'b', 'c' };
static uint8_t sha_expected[] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};
static char rsa_modulus[] =
    "9292758453063D803DD603D5E777D7888ED1D5BF35786190FA2F23EBC0848AEA"
    "DDA92CA6C3D80B32C4D109BE0F36D6AE7130B9CED7ACDF54CFC7555AC14EEBAB"
    "93A89813FBF3C4F8066D2D800F7C38A81AE31942917403FF4946B0A83D3D3E05"
    "EE57C6F5F5606FB5D4BC6CD34EE0801A5E94BB77B07507233A0BC7BAC8F90F79";
static char rsa_exponent[] = "10001";
static char rsa_verify_message_hex[] =
    "206ef4bf396c6087f8229ef196fd35f37ccb8de5efcdb238f20d556668f11425"
    "7a11fbe038464a67830378e62ae9791453953dac1dbd7921837ba98e84e856eb"
    "80ed9487e656d0b20c28c8ba5e35db1abbed83ed1c7720a97701f709e3547a4"
    "bfcabca9c89c57ad15c3996577a0ae36d7c7b699035242f37954646c1cd5c08ac";
static char rsa_verify_modulus_hex[] =
    "e28a13548525e5f36dccb24ecb7cc332cc689dfd64012604c9c7816d72a16c3f"
    "5fcdc0e86e7c03280b1c69b586ce0cd8aec722cc73a5d3b730310bf7dfebdc77"
    "ce5d94bbc369dc18a2f7b07bd505ab0f82224aef09fdc1e5063234255e0b3c40"
    "a52e9e8ae60898eb88a766bdd788fe9493d8fd86bcdd2884d5c06216c65469e5";
static char rsa_verify_signature_hex[] =
    "5abc01f5de25b70867ff0c24e222c61f53c88daf42586fddcd56f3c4588f074"
    "be3c328056c063388688b6385a8167957c6e5355a510e005b8a851d69c96b36e"
    "c6036644078210e5d7d326f96365ee0648882921492bc7b753eb9c26cdbab3755"
    "5f210df2ca6fec1b25b463d38b81c0dcea202022b04af5da58aa03d77be949b7";

static int
hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

static int
decode_hex(char *input, uint8_t *output, size_t output_length)
{
    size_t i;

    if (strlen(input) != output_length * 2U)
        return -1;
    for (i = 0; i < output_length; i++) {
        int high = hex_value(input[i * 2U]);
        int low = hex_value(input[i * 2U + 1U]);
        if (high < 0 || low < 0)
            return -1;
        output[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

static int
check_public_key(void)
{
    mbedtls_mpi modulus;
    mbedtls_mpi exponent;
    mbedtls_rsa_context rsa;
    int result;

    mbedtls_mpi_init(&modulus);
    mbedtls_mpi_init(&exponent);
    mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);
    result = mbedtls_mpi_read_string(&modulus, 16, rsa_modulus);
    if (result == 0)
        result = mbedtls_mpi_read_string(&exponent, 16, rsa_exponent);
    if (result == 0)
        result = mbedtls_rsa_import(&rsa, &modulus, NULL, NULL, NULL,
                                    &exponent);
    if (result == 0)
        result = mbedtls_rsa_complete(&rsa);
    if (result == 0)
        result = mbedtls_rsa_check_pubkey(&rsa);
    mbedtls_rsa_free(&rsa);
    mbedtls_mpi_free(&exponent);
    mbedtls_mpi_free(&modulus);
    return result;
}

static int
check_public_signature(void)
{
    uint8_t message[128];
    uint8_t signature[128];
    uint8_t hash[20];
    mbedtls_mpi modulus;
    mbedtls_mpi exponent;
    mbedtls_rsa_context rsa;
    int result;

    mbedtls_mpi_init(&modulus);
    mbedtls_mpi_init(&exponent);
    mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);
    result = decode_hex(rsa_verify_message_hex, message, sizeof(message));
    if (result == 0)
        result = decode_hex(rsa_verify_signature_hex, signature,
                            sizeof(signature));
    if (result == 0)
        result = mbedtls_mpi_read_string(&modulus, 16,
                                         rsa_verify_modulus_hex);
    if (result == 0)
        result = mbedtls_mpi_lset(&exponent, 3);
    if (result == 0)
        result = mbedtls_rsa_import(&rsa, &modulus, NULL, NULL, NULL,
                                    &exponent);
    if (result == 0)
        result = mbedtls_sha1_ret(message, sizeof(message), hash);
    if (result == 0)
        result = mbedtls_rsa_pkcs1_verify(&rsa, NULL, NULL,
                                          MBEDTLS_RSA_PUBLIC,
                                          MBEDTLS_MD_SHA1, 0, hash,
                                          signature);
    mbedtls_rsa_free(&rsa);
    mbedtls_mpi_free(&exponent);
    mbedtls_mpi_free(&modulus);
    return result;
}

int
main(void)
{
    uint8_t digest[32];
    int sha_result;
    int mpi_result;
    int public_key_result;
    int public_signature_result;

    setvbuf(stdout, NULL, _IONBF, 0);

    sha_result = mbedtls_sha256_ret(sha_input, sizeof(sha_input), digest, 0);
    if (sha_result == 0 && memcmp(digest, sha_expected, sizeof(digest)) != 0)
        sha_result = -1;
    printf("FX_CRYPTO_SHA256=%s rc=%d\n",
           sha_result == 0 ? "PASS" : "FAIL", sha_result);

    mpi_result = mbedtls_mpi_self_test(0);
    printf("FX_CRYPTO_MPI=%s rc=%d\n",
           mpi_result == 0 ? "PASS" : "FAIL", mpi_result);

    public_key_result = check_public_key();
    printf("FX_CRYPTO_RSA_PUBLIC_KEY=%s rc=%d\n",
           public_key_result == 0 ? "PASS" : "FAIL", public_key_result);

    public_signature_result = check_public_signature();
    printf("FX_CRYPTO_RSA_VERIFY=%s rc=%d\n",
           public_signature_result == 0 ? "PASS" : "FAIL",
           public_signature_result);

    if (sha_result == 0 && mpi_result == 0 && public_key_result == 0
        && public_signature_result == 0) {
        puts("FX_CRYPTO_SELFTEST=PASS");
        return 0;
    }
    puts("FX_CRYPTO_SELFTEST=FAIL");
    return 1;
}
