#include "RelaySecure.h"
#include "Crypto.h"
#include <string.h>
#include <initializer_list>
#if defined(ESP_PLATFORM)
#include "esp_random.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/gcm.h"
#else
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif
namespace opensmarttimer { namespace relay {
void secureErase(void* data, size_t len) { volatile uint8_t* p = (volatile uint8_t*)data; while (len--) *p++ = 0; }
void secureNonce(uint64_t seq, uint8_t out[12]) {
    memset(out, 0, 12); for (int i=0;i<8;++i) out[11-i]=uint8_t(seq>>(8*i));
}
bool hkdfSha256(const uint8_t* ikm, size_t ikm_len, const uint8_t* salt, size_t salt_len,
                const uint8_t* info, size_t info_len, uint8_t* out, size_t out_len) {
    if (!out_len || out_len>64 || info_len>192) return false;
    uint8_t prk[32]={}, previous[32]={}, message[225]={}; size_t done=0, previous_len=0;
    bool ok=hmacSha256(salt,salt_len,ikm,ikm_len,prk);
    for (uint8_t block=1;ok && done<out_len;++block) {
        memcpy(message,previous,previous_len);
        if (info_len) memcpy(message+previous_len,info,info_len);
        message[previous_len+info_len]=block;
        ok=hmacSha256(prk,32,message,previous_len+info_len+1,previous);
        size_t n=out_len-done; if (n>32) n=32;
        if (ok) { memcpy(out+done,previous,n); done+=n; previous_len=32; }
    }
    secureErase(prk,32); secureErase(previous,32); secureErase(message,sizeof(message));
    if (!ok) secureErase(out,out_len);
    return ok;
}
size_t secureOpenTranscript(const char* timer, const char* boot, const uint8_t fp_hash[16],
                           const uint8_t client[16], const uint8_t public_key[65], uint8_t out[192]) {
    static const char label[]="OST-RELAY-E2E-OPEN-v1";
    if (strlen(timer)>23 || strlen(boot)>16) return 0;
    size_t n=sizeof(label)-1; memcpy(out,label,n);
    for (const char* s : {timer,boot}) { size_t z=strlen(s)+1; memcpy(out+n,s,z); n+=z; }
    memcpy(out+n,fp_hash,16); n+=16; memcpy(out+n,client,16); n+=16;
    memcpy(out+n,public_key,65); return n+65;
}
size_t secureAAD(uint8_t direction, const uint8_t fp_hash[16], const uint8_t client[16],
                 const uint8_t server[16], uint64_t seq, const char* request, uint8_t out[160]) {
    static const char label[]="OST-RELAY-E2E-DATA-v1";
    size_t r=strlen(request); if(r>64) return 0;
    size_t n=sizeof(label)-1; memcpy(out,label,n); out[n++]=direction;
    for (const uint8_t* p : {fp_hash,client,server}) { memcpy(out+n,p,16); n+=16; }
    uint8_t nonce[12]; secureNonce(seq,nonce); memcpy(out+n,nonce+4,8); n+=8;
    memcpy(out+n,request,r); return n+r;
}
size_t secureAcceptTranscript(const uint8_t* opened, size_t opened_len,
                             const uint8_t server[16], const uint8_t public_key[65],
                             uint8_t out[kSecureAcceptTranscriptBytes]) {
    static const char label[]="OST-RELAY-E2E-ACCEPT-v1";
    if (!opened || !opened_len || opened_len > 192 || !server || !public_key || !out) return 0;
    size_t n=sizeof(label)-1; memcpy(out,label,n);
    memcpy(out+n,opened,opened_len); n+=opened_len;
    memcpy(out+n,server,16); n+=16; memcpy(out+n,public_key,65); return n+65;
}
#if defined(ESP_PLATFORM)
namespace {
int rng(void*, unsigned char* out, size_t len) { esp_fill_random(out,len); return 0; }
struct Curve {
    mbedtls_ecp_group g; mbedtls_mpi d,z; mbedtls_ecp_point q;
    Curve() { mbedtls_ecp_group_init(&g); mbedtls_mpi_init(&d); mbedtls_mpi_init(&z); mbedtls_ecp_point_init(&q); }
    ~Curve() { mbedtls_ecp_group_free(&g); mbedtls_mpi_free(&d); mbedtls_mpi_free(&z); mbedtls_ecp_point_free(&q); }
};
}
bool secureRandom(uint8_t* out,size_t len) { esp_fill_random(out,len); return true; }
bool ecdhGenerate(uint8_t priv[32],uint8_t pub[65]) {
    Curve c; size_t n=0;
    bool ok=mbedtls_ecp_group_load(&c.g,MBEDTLS_ECP_DP_SECP256R1)==0
        && mbedtls_ecp_gen_keypair(&c.g,&c.d,&c.q,rng,nullptr)==0
        && mbedtls_mpi_write_binary(&c.d,priv,32)==0
        && mbedtls_ecp_point_write_binary(&c.g,&c.q,MBEDTLS_ECP_PF_UNCOMPRESSED,&n,pub,65)==0 && n==65;
    if(!ok) { secureErase(priv,32); secureErase(pub,65); } return ok;
}
bool ecdhShared(const uint8_t priv[32],const uint8_t pub[65],uint8_t shared[32]) {
    Curve c;
    bool ok=pub[0]==4 && mbedtls_ecp_group_load(&c.g,MBEDTLS_ECP_DP_SECP256R1)==0
        && mbedtls_mpi_read_binary(&c.d,priv,32)==0 && mbedtls_ecp_check_privkey(&c.g,&c.d)==0
        && mbedtls_ecp_point_read_binary(&c.g,&c.q,pub,65)==0 && mbedtls_ecp_check_pubkey(&c.g,&c.q)==0
        && mbedtls_ecdh_compute_shared(&c.g,&c.z,&c.q,&c.d,rng,nullptr)==0
        && mbedtls_mpi_write_binary(&c.z,shared,32)==0;
    if(!ok) secureErase(shared,32);
    return ok;
}
bool gcmEncrypt(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,size_t al,
                const uint8_t* plain,size_t len,uint8_t* cipher) {
    mbedtls_gcm_context c; mbedtls_gcm_init(&c);
    bool ok=mbedtls_gcm_setkey(&c,MBEDTLS_CIPHER_ID_AES,key,256)==0
        && mbedtls_gcm_crypt_and_tag(&c,MBEDTLS_GCM_ENCRYPT,len,nonce,12,aad,al,plain,cipher,16,cipher+len)==0;
    mbedtls_gcm_free(&c); if(!ok) secureErase(cipher,len+16); return ok;
}
bool gcmDecrypt(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,size_t al,
                const uint8_t* cipher,size_t len,uint8_t* plain) {
    if(len<16) return false;
    mbedtls_gcm_context c; mbedtls_gcm_init(&c);
    bool ok=mbedtls_gcm_setkey(&c,MBEDTLS_CIPHER_ID_AES,key,256)==0
        && mbedtls_gcm_auth_decrypt(&c,len-16,nonce,12,aad,al,cipher+len-16,16,cipher,plain)==0;
    mbedtls_gcm_free(&c); if(!ok) secureErase(plain,len-16); return ok;
}
#else
bool secureRandom(uint8_t* out,size_t len) { return len<=4096 && RAND_bytes(out,int(len))==1; }
bool ecdhGenerate(uint8_t priv[32],uint8_t pub[65]) {
    EC_KEY* key=EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    bool ok=key && EC_KEY_generate_key(key)==1 && BN_bn2binpad(EC_KEY_get0_private_key(key),priv,32)==32
        && EC_POINT_point2oct(EC_KEY_get0_group(key),EC_KEY_get0_public_key(key),POINT_CONVERSION_UNCOMPRESSED,pub,65,nullptr)==65;
    EC_KEY_free(key); if(!ok) { secureErase(priv,32); secureErase(pub,65); } return ok;
}
bool ecdhShared(const uint8_t priv[32],const uint8_t pub[65],uint8_t shared[32]) {
    EC_KEY* key=EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM* d=BN_bin2bn(priv,32,nullptr); const EC_GROUP* g=key?EC_KEY_get0_group(key):nullptr;
    EC_POINT* q=g?EC_POINT_new(g):nullptr; BIGNUM* order=BN_new();
    bool ok=key && d && q && order && pub[0]==4 && EC_GROUP_get_order(g,order,nullptr)==1
        && !BN_is_zero(d) && BN_cmp(d,order)<0 && EC_KEY_set_private_key(key,d)==1
        && EC_POINT_oct2point(g,q,pub,65,nullptr)==1 && EC_POINT_is_on_curve(g,q,nullptr)==1
        && EC_POINT_is_at_infinity(g,q)==0 && ECDH_compute_key(shared,32,q,key,nullptr)==32;
    BN_clear_free(d); BN_free(order); EC_POINT_free(q); EC_KEY_free(key);
    if(!ok) secureErase(shared,32);
    return ok;
}
bool gcmEncrypt(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,size_t al,
                const uint8_t* plain,size_t len,uint8_t* cipher) {
    EVP_CIPHER_CTX* c=EVP_CIPHER_CTX_new(); int n=0,total=0;
    bool ok=c && EVP_EncryptInit_ex(c,EVP_aes_256_gcm(),nullptr,key,nonce)==1
        && EVP_EncryptUpdate(c,nullptr,&n,aad,int(al))==1
        && EVP_EncryptUpdate(c,cipher,&n,plain,int(len))==1;
    total=n; ok=ok && EVP_EncryptFinal_ex(c,cipher+total,&n)==1
        && EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_GET_TAG,16,cipher+len)==1;
    EVP_CIPHER_CTX_free(c); if(!ok) secureErase(cipher,len+16); return ok;
}
bool gcmDecrypt(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,size_t al,
                const uint8_t* cipher,size_t len,uint8_t* plain) {
    if(len<16) return false;
    EVP_CIPHER_CTX* c=EVP_CIPHER_CTX_new(); int n=0,total=0;
    bool ok=c && EVP_DecryptInit_ex(c,EVP_aes_256_gcm(),nullptr,key,nonce)==1
        && EVP_DecryptUpdate(c,nullptr,&n,aad,int(al))==1
        && EVP_DecryptUpdate(c,plain,&n,cipher,int(len-16))==1;
    total=n; ok=ok && EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_SET_TAG,16,(void*)(cipher+len-16))==1
        && EVP_DecryptFinal_ex(c,plain+total,&n)==1;
    EVP_CIPHER_CTX_free(c); if(!ok) secureErase(plain,len-16); return ok;
}
#endif
}}
