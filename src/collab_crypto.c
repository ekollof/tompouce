#include "collab_crypto.h"
#include "collab_proto.h"
#include <cjson/cJSON.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
/* E2E crypto (docs/multiplayer.md sections 13.5, 14.1, 14.5). Fail closed:
   every entry point validates lengths and room ids before touching sodium,
   and every failure path wipes stack key material it can. */
int ccr_room_ok(const char *room_id) {
    size_t i;
    if (!room_id) return 0;
    for (i=0;i<CCR_ROOM_ID_LEN;i++) {
        unsigned char c=(unsigned char)room_id[i];
        if (!((c>='a' && c<='z') || (c>='2' && c<='7'))) return 0;
    }
    return room_id[CCR_ROOM_ID_LEN]==0;
}
static void put_u16be(uint8_t *out, uint16_t value) {
    out[0]=(uint8_t)(value>>8);
    out[1]=(uint8_t)value;
}
static void put_u32be(uint8_t *out, uint32_t value) {
    out[0]=(uint8_t)(value>>24);
    out[1]=(uint8_t)(value>>16);
    out[2]=(uint8_t)(value>>8);
    out[3]=(uint8_t)value;
}
static uint16_t get_u16be(const uint8_t *in) {
    return (uint16_t)(((uint16_t)in[0]<<8)|in[1]);
}
int ccr_init(void) {
    return sodium_init()>=0;
}
/* --- master seed (section 14.1) --- */
static int ccr_config_dir(char *buf, size_t cap) {
    const char *base=getenv("XDG_CONFIG_HOME");
    if (base && base[0]=='/') {
        if (snprintf(buf,cap,"%s/tompouce/collab",base)>=(int)cap) return 0;
        return 1;
    }
    base=getenv("HOME");
    if (!base || !*base) return 0;
    if (snprintf(buf,cap,"%s/.config/tompouce/collab",base)>=(int)cap)
        return 0;
    return 1;
}
int ccr_identity_load_or_create(uint8_t seed[CCR_SEED_BYTES], char *err,
    size_t errlen) {
    char dir[1024], path[1100], tmp[1120];
    FILE *file=NULL;
    size_t got;
    int fd;
    if (!seed) return 0;
    if (!ccr_config_dir(dir,sizeof(dir))) {
        if (err && errlen>0) snprintf(err,errlen,"no config directory");
        return 0;
    }
    if (snprintf(path,sizeof(path),"%s/identity.key",dir)>=(int)sizeof(path)) {
        if (err && errlen>0) snprintf(err,errlen,"path too long");
        return 0;
    }
    file=fopen(path,"rb");
    if (file) {
        /* The file must hold exactly one seed: a short or trailing-garbage
           file is never mistaken for a key. */
        long size;
        got=fread(seed,1,CCR_SEED_BYTES,file);
        if (got==CCR_SEED_BYTES && fseek(file,0,SEEK_END)==0 &&
            (size=ftell(file))==CCR_SEED_BYTES) {
            fclose(file);
            return 1;
        }
        fclose(file);
        if (err && errlen>0)
            snprintf(err,errlen,"identity.key unreadable, recreating");
    }
    /* Create parents as needed: plain mkdir -p over the absolute path,
       one level at a time, without following symlinks. */
    {
        size_t i, length=strlen(dir);
        char prefix[1024];
        if (length==0 || length>=sizeof(prefix)) {
            if (err && errlen>0) snprintf(err,errlen,"bad config path");
            return 0;
        }
        for (i=1;i<=length;i++) {
            struct stat st;
            if (dir[i]!='/' && dir[i]!=0) continue;
            if (i>=sizeof(prefix)) {
                if (err && errlen>0) snprintf(err,errlen,"path too long");
                return 0;
            }
            memcpy(prefix,dir,i);
            prefix[i]=0;
            if (mkdir(prefix,0700)!=0) {
                if (stat(prefix,&st)!=0 || !S_ISDIR(st.st_mode)) {
                    if (err && errlen>0)
                        snprintf(err,errlen,"cannot create %s",prefix);
                    return 0;
                }
            }
        }
    }
    randombytes_buf(seed,CCR_SEED_BYTES);
    if (snprintf(tmp,sizeof(tmp),"%s/.identity.XXXXXX",dir)>=(int)sizeof(tmp))
        return 0;
    fd=mkstemp(tmp);
    if (fd<0) {
        if (err && errlen>0) snprintf(err,errlen,"cannot write identity.key");
        return 0;
    }
    if (fchmod(fd,0600)!=0) {
        close(fd);
        unlink(tmp);
        return 0;
    }
    file=fdopen(fd,"wb");
    if (!file) {
        close(fd);
        unlink(tmp);
        return 0;
    }
    if (fwrite(seed,1,CCR_SEED_BYTES,file)!=CCR_SEED_BYTES ||
        fclose(file)!=0) {
        unlink(tmp);
        return 0;
    }
    if (rename(tmp,path)!=0) {
        unlink(tmp);
        return 0;
    }
    return 1;
}
/* --- key derivation (sections 13.5, 14.1) --- */
int ccr_room_keypair(const uint8_t master[CCR_SEED_BYTES],
    const char *room_id, uint8_t pk[CCR_PK_BYTES],
    uint8_t sk[CCR_SK_BYTES]) {
    uint8_t digest[crypto_hash_sha256_BYTES];
    uint8_t seed[crypto_sign_SEEDBYTES];
    uint64_t subkey;
    int i;
    if (!master || !pk || !sk || !ccr_room_ok(room_id)) return 0;
    crypto_hash_sha256(digest,(const uint8_t *)room_id,CCR_ROOM_ID_LEN);
    subkey=0;
    for (i=0;i<8;i++) subkey|=(uint64_t)digest[i]<<(8*i);
    if (crypto_kdf_derive_from_key(seed,sizeof(seed),subkey,CCR_KDF_MEMBER,
            master)!=0) {
        sodium_memzero(seed,sizeof(seed));
        return 0;
    }
    if (crypto_sign_seed_keypair(pk,sk,seed)!=0) {
        sodium_memzero(seed,sizeof(seed));
        return 0;
    }
    sodium_memzero(seed,sizeof(seed));
    return 1;
}
void ccr_admission_key(const uint8_t room_key[CCR_KEY_BYTES],
    uint8_t out[CCR_KEY_BYTES]) {
    crypto_kdf_derive_from_key(out,CCR_KEY_BYTES,0,CCR_KDF_ADMISSION,
        room_key);
}
void ccr_subkey(const uint8_t room_key[CCR_KEY_BYTES], uint64_t id,
    uint8_t out[CCR_KEY_BYTES]) {
    crypto_kdf_derive_from_key(out,CCR_KEY_BYTES,id,CCR_KDF_SUBKEYS,
        room_key);
}
/* --- auth (section 13.5) --- */
static int ccr_auth_msg(const uint8_t challenge[CCR_CHALLENGE_BYTES],
    const char *room_id, const uint8_t pk[CCR_PK_BYTES], uint8_t *msg,
    size_t cap) {
    static const char context[]="tompouce-collab-v1";
    size_t need=sizeof(context)-1+CCR_CHALLENGE_BYTES+CCR_ROOM_ID_LEN+
        CCR_PK_BYTES;
    if (!challenge || !pk || !ccr_room_ok(room_id) || !msg || cap<need)
        return 0;
    memcpy(msg,context,sizeof(context)-1);
    memcpy(msg+sizeof(context)-1,challenge,CCR_CHALLENGE_BYTES);
    memcpy(msg+sizeof(context)-1+CCR_CHALLENGE_BYTES,room_id,
        CCR_ROOM_ID_LEN);
    memcpy(msg+sizeof(context)-1+CCR_CHALLENGE_BYTES+CCR_ROOM_ID_LEN,pk,
        CCR_PK_BYTES);
    return 1;
}
int ccr_auth_sign(const uint8_t sk[CCR_SK_BYTES],
    const uint8_t challenge[CCR_CHALLENGE_BYTES], const char *room_id,
    const uint8_t pk[CCR_PK_BYTES], uint8_t sig[CCR_SIG_BYTES]) {
    uint8_t msg[18+CCR_CHALLENGE_BYTES+CCR_ROOM_ID_LEN+CCR_PK_BYTES];
    unsigned long long siglen=0;
    if (!sk || !sig || !ccr_auth_msg(challenge,room_id,pk,msg,sizeof(msg)))
        return 0;
    if (crypto_sign_detached(sig,&siglen,msg,sizeof(msg),sk)!=0 ||
        siglen!=CCR_SIG_BYTES) {
        sodium_memzero(msg,sizeof(msg));
        return 0;
    }
    sodium_memzero(msg,sizeof(msg));
    return 1;
}
int ccr_auth_verify(const uint8_t pk[CCR_PK_BYTES],
    const uint8_t challenge[CCR_CHALLENGE_BYTES], const char *room_id,
    const uint8_t sig[CCR_SIG_BYTES]) {
    uint8_t msg[18+CCR_CHALLENGE_BYTES+CCR_ROOM_ID_LEN+CCR_PK_BYTES];
    int ok;
    if (!pk || !sig || !ccr_auth_msg(challenge,room_id,pk,msg,sizeof(msg)))
        return 0;
    ok=crypto_sign_verify_detached(sig,msg,sizeof(msg),pk)==0;
    sodium_memzero(msg,sizeof(msg));
    return ok;
}
int ccr_proof(const uint8_t admission[CCR_KEY_BYTES],
    const uint8_t challenge[CCR_CHALLENGE_BYTES], const char *room_id,
    const uint8_t pk[CCR_PK_BYTES], uint8_t mac[CCR_KEY_BYTES]) {
    uint8_t material[CCR_CHALLENGE_BYTES+CCR_ROOM_ID_LEN+CCR_PK_BYTES];
    if (!admission || !mac || !ccr_room_ok(room_id) || !challenge || !pk)
        return 0;
    memcpy(material,challenge,CCR_CHALLENGE_BYTES);
    memcpy(material+CCR_CHALLENGE_BYTES,room_id,CCR_ROOM_ID_LEN);
    memcpy(material+CCR_CHALLENGE_BYTES+CCR_ROOM_ID_LEN,pk,CCR_PK_BYTES);
    if (crypto_generichash(mac,CCR_KEY_BYTES,material,sizeof(material),
            admission,CCR_KEY_BYTES)!=0) {
        sodium_memzero(material,sizeof(material));
        return 0;
    }
    sodium_memzero(material,sizeof(material));
    return 1;
}
/* --- sealed records (section 14.5) --- */
#define CCR_CTRL_AD "tpc-ctrl-v1"
#define CCR_FRAME_AD "tpc-frame-v1"
static int ccr_ctrl_ad(const char *room_id, uint16_t epoch, uint8_t type,
    uint16_t slot, const uint8_t author_pk[CCR_PK_BYTES],
    uint8_t *ad, size_t cap) {
    size_t need=11+CCR_ROOM_ID_LEN+2+1+2+CCR_PK_BYTES;
    if (!ccr_room_ok(room_id) || !author_pk || !ad || cap<need) return 0;
    memcpy(ad,CCR_CTRL_AD,11);
    memcpy(ad+11,room_id,CCR_ROOM_ID_LEN);
    put_u16be(ad+11+CCR_ROOM_ID_LEN,epoch);
    ad[11+CCR_ROOM_ID_LEN+2]=type;
    put_u16be(ad+11+CCR_ROOM_ID_LEN+3,slot);
    memcpy(ad+11+CCR_ROOM_ID_LEN+5,author_pk,CCR_PK_BYTES);
    return 1;
}
static int ccr_frame_ad(const char *room_id, uint16_t epoch, uint16_t slot,
    uint8_t kind, uint32_t seq, const uint8_t author_pk[CCR_PK_BYTES],
    uint8_t *ad, size_t cap) {
    size_t need=12+CCR_ROOM_ID_LEN+2+2+1+4+CCR_PK_BYTES;
    if (!ccr_room_ok(room_id) || !author_pk || !ad || cap<need) return 0;
    memcpy(ad,CCR_FRAME_AD,12);
    memcpy(ad+12,room_id,CCR_ROOM_ID_LEN);
    put_u16be(ad+12+CCR_ROOM_ID_LEN,epoch);
    put_u16be(ad+12+CCR_ROOM_ID_LEN+2,slot);
    ad[12+CCR_ROOM_ID_LEN+4]=kind;
    put_u32be(ad+12+CCR_ROOM_ID_LEN+5,seq);
    memcpy(ad+12+CCR_ROOM_ID_LEN+9,author_pk,CCR_PK_BYTES);
    return 1;
}
int ccr_seal_ctrl_nonce(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint8_t type, uint16_t slot,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES],
    const uint8_t nonce[CCR_NONCE_BYTES], const uint8_t *msg, size_t msglen,
    uint8_t **out, size_t *outlen) {
    uint8_t ad[11+CCR_ROOM_ID_LEN+2+1+2+CCR_PK_BYTES];
    uint8_t sig[CCR_SIG_BYTES];
    uint8_t *blob=NULL, *ct, *signed_msg=NULL;
    unsigned long long ctlen=0, siglen=0;
    size_t total, signed_len;
    if (!key || !author_pk || !author_sk || !nonce || !msg || !msglen ||
        !out || !outlen) return 0;
    if (msglen>CP_COMPOSITE_MAX) return 0;
    if (!ccr_ctrl_ad(room_id,epoch,type,slot,author_pk,ad,sizeof(ad)))
        return 0;
    total=2+CCR_NONCE_BYTES+msglen+CCR_TAG_BYTES+CCR_SIG_BYTES;
    blob=malloc(total);
    if (!blob) return 0;
    put_u16be(blob,epoch);
    memcpy(blob+2,nonce,CCR_NONCE_BYTES);
    ct=blob+2+CCR_NONCE_BYTES;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(ct,&ctlen,msg,msglen,ad,
            sizeof(ad),NULL,nonce,key)!=0) {
        sodium_memzero(blob,total);
        free(blob);
        return 0;
    }
    /* Sign AD ‖ nonce ‖ ciphertext ‖ tag, exactly as verifiers rebuild. */
    signed_len=sizeof(ad)+CCR_NONCE_BYTES+(size_t)ctlen;
    signed_msg=malloc(signed_len);
    if (!signed_msg) {
        sodium_memzero(blob,total);
        free(blob);
        return 0;
    }
    memcpy(signed_msg,ad,sizeof(ad));
    memcpy(signed_msg+sizeof(ad),nonce,CCR_NONCE_BYTES);
    memcpy(signed_msg+sizeof(ad)+CCR_NONCE_BYTES,ct,(size_t)ctlen);
    if (crypto_sign_detached(sig,&siglen,signed_msg,signed_len,
            author_sk)!=0 || siglen!=CCR_SIG_BYTES) {
        sodium_memzero(signed_msg,signed_len);
        free(signed_msg);
        sodium_memzero(blob,total);
        free(blob);
        return 0;
    }
    sodium_memzero(signed_msg,signed_len);
    free(signed_msg);
    memcpy(blob+2+CCR_NONCE_BYTES+msglen+CCR_TAG_BYTES,sig,CCR_SIG_BYTES);
    *out=blob;
    *outlen=total;
    return 1;
}
int ccr_seal_ctrl(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint8_t type, uint16_t slot,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES], const uint8_t *msg, size_t msglen,
    uint8_t **out, size_t *outlen) {
    uint8_t nonce[CCR_NONCE_BYTES];
    int ok;
    randombytes_buf(nonce,sizeof(nonce));
    ok=ccr_seal_ctrl_nonce(key,room_id,epoch,type,slot,author_pk,author_sk,
        nonce,msg,msglen,out,outlen);
    sodium_memzero(nonce,sizeof(nonce));
    return ok;
}
/* Typed open: the caller supplies the expected epoch/type/slot (routing
   fields from the director) and they are verified as part of the AD. */
int ccr_open_ctrl_typed(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t exp_epoch, uint8_t exp_type, uint16_t exp_slot,
    const uint8_t author_pk[CCR_PK_BYTES], const uint8_t *in, size_t inlen,
    uint8_t **msg, size_t *msglen, uint16_t *got_epoch, uint8_t *got_type,
    uint16_t *got_slot);
int ccr_open_ctrl_typed(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t exp_epoch, uint8_t exp_type, uint16_t exp_slot,
    const uint8_t author_pk[CCR_PK_BYTES], const uint8_t *in, size_t inlen,
    uint8_t **msg, size_t *msglen, uint16_t *got_epoch, uint8_t *got_type,
    uint16_t *got_slot) {
    uint8_t ad[11+CCR_ROOM_ID_LEN+2+1+2+CCR_PK_BYTES];
    const uint8_t *nonce, *ct, *tag, *sig;
    size_t ctlen;
    uint8_t *signed_msg=NULL, *plain=NULL;
    unsigned long long plainlen=0;
    uint16_t in_epoch;
    size_t signed_len;
    if (!key || !author_pk || !in || !msg || !msglen) return 0;
    if (!ccr_room_ok(room_id)) return 0;
    if (inlen<2+CCR_NONCE_BYTES+CCR_TAG_BYTES+CCR_SIG_BYTES+1) return 0;
    if (inlen>2+CCR_NONCE_BYTES+CP_COMPOSITE_MAX+CCR_TAG_BYTES+
        CCR_SIG_BYTES) return 0;
    in_epoch=get_u16be(in);
    if (in_epoch!=exp_epoch) return 0;
    nonce=in+2;
    ctlen=inlen-(2+CCR_NONCE_BYTES+CCR_TAG_BYTES+CCR_SIG_BYTES);
    ct=nonce+CCR_NONCE_BYTES;
    tag=ct+ctlen;
    sig=tag+CCR_TAG_BYTES;
    if (!ccr_ctrl_ad(room_id,exp_epoch,exp_type,exp_slot,author_pk,ad,
            sizeof(ad))) return 0;
    signed_len=sizeof(ad)+CCR_NONCE_BYTES+ctlen+CCR_TAG_BYTES;
    signed_msg=malloc(signed_len);
    if (!signed_msg) return 0;
    memcpy(signed_msg,ad,sizeof(ad));
    memcpy(signed_msg+sizeof(ad),nonce,CCR_NONCE_BYTES);
    memcpy(signed_msg+sizeof(ad)+CCR_NONCE_BYTES,ct,ctlen+CCR_TAG_BYTES);
    if (crypto_sign_verify_detached(sig,signed_msg,signed_len,
            author_pk)!=0) {
        sodium_memzero(signed_msg,signed_len);
        free(signed_msg);
        return -1;
    }
    sodium_memzero(signed_msg,signed_len);
    free(signed_msg);
    plain=malloc(ctlen+1);
    if (!plain) return 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain,&plainlen,NULL,ct,
            ctlen+CCR_TAG_BYTES,ad,sizeof(ad),nonce,key)!=0) {
        sodium_memzero(plain,ctlen+1);
        free(plain);
        return -2;
    }
    plain[plainlen]=0;
    *msg=plain;
    *msglen=(size_t)plainlen;
    if (got_epoch) *got_epoch=in_epoch;
    if (got_type) *got_type=exp_type;
    if (got_slot) *got_slot=exp_slot;
    return 1;
}
int ccr_seal_frame(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint16_t slot, uint8_t kind, uint32_t seq,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES], const uint8_t *payload,
    size_t payloadlen, uint8_t **out, size_t *outlen) {
    uint8_t nonce[CCR_NONCE_BYTES];
    randombytes_buf(nonce,sizeof(nonce));
    {
        int ok;
        uint8_t fixed[CCR_NONCE_BYTES];
        memcpy(fixed,nonce,sizeof(fixed));
        sodium_memzero(nonce,sizeof(nonce));
        ok=ccr_seal_frame_nonce(key,room_id,epoch,slot,kind,seq,author_pk,
            author_sk,fixed,payload,payloadlen,out,outlen);
        sodium_memzero(fixed,sizeof(fixed));
        return ok;
    }
}
int ccr_seal_frame_nonce(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint16_t slot, uint8_t kind, uint32_t seq,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES],
    const uint8_t nonce[CCR_NONCE_BYTES], const uint8_t *payload,
    size_t payloadlen, uint8_t **out, size_t *outlen);
int ccr_seal_frame_nonce(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint16_t slot, uint8_t kind, uint32_t seq,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES],
    const uint8_t nonce[CCR_NONCE_BYTES], const uint8_t *payload,
    size_t payloadlen, uint8_t **out, size_t *outlen) {
    uint8_t ad[12+CCR_ROOM_ID_LEN+2+2+1+4+CCR_PK_BYTES];
    uint8_t sig[CCR_SIG_BYTES];
    uint8_t *blob=NULL, *ct;
    unsigned long long ctlen=0, siglen=0;
    size_t total;
    uint8_t *signed_msg=NULL;
    if (!key || !author_pk || !author_sk || !nonce || !payload ||
        !payloadlen || !out || !outlen) return 0;
    if (payloadlen>CP_BIN_PAYLOAD_MAX) return 0;
    if (!ccr_frame_ad(room_id,epoch,slot,kind,seq,author_pk,ad,sizeof(ad)))
        return 0;
    total=CCR_NONCE_BYTES+payloadlen+CCR_TAG_BYTES+CCR_SIG_BYTES;
    blob=malloc(total);
    if (!blob) return 0;
    memcpy(blob,nonce,CCR_NONCE_BYTES);
    ct=blob+CCR_NONCE_BYTES;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(ct,&ctlen,payload,
            payloadlen,ad,sizeof(ad),NULL,nonce,key)!=0) {
        sodium_memzero(blob,total);
        free(blob);
        return 0;
    }
    signed_msg=malloc(sizeof(ad)+CCR_NONCE_BYTES+(size_t)ctlen);
    if (!signed_msg) {
        sodium_memzero(blob,total);
        free(blob);
        return 0;
    }
    memcpy(signed_msg,ad,sizeof(ad));
    memcpy(signed_msg+sizeof(ad),nonce,CCR_NONCE_BYTES);
    memcpy(signed_msg+sizeof(ad)+CCR_NONCE_BYTES,ct,(size_t)ctlen);
    if (crypto_sign_detached(sig,&siglen,signed_msg,
            sizeof(ad)+CCR_NONCE_BYTES+(size_t)ctlen,author_sk)!=0 ||
        siglen!=CCR_SIG_BYTES) {
        sodium_memzero(signed_msg,sizeof(ad)+CCR_NONCE_BYTES+(size_t)ctlen);
        free(signed_msg);
        sodium_memzero(blob,total);
        free(blob);
        return 0;
    }
    sodium_memzero(signed_msg,sizeof(ad)+CCR_NONCE_BYTES+(size_t)ctlen);
    free(signed_msg);
    memcpy(blob+CCR_NONCE_BYTES+payloadlen+CCR_TAG_BYTES,sig,CCR_SIG_BYTES);
    *out=blob;
    *outlen=total;
    return 1;
}
int ccr_open_frame(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint16_t slot, uint8_t kind, uint32_t seq,
    const uint8_t author_pk[CCR_PK_BYTES], const uint8_t *in, size_t inlen,
    uint8_t **payload, size_t *payloadlen) {
    uint8_t ad[12+CCR_ROOM_ID_LEN+2+2+1+4+CCR_PK_BYTES];
    const uint8_t *nonce, *ct, *tag, *sig;
    size_t ctlen;
    uint8_t *signed_msg=NULL, *plain=NULL;
    unsigned long long plainlen=0;
    if (!key || !author_pk || !in || !payload || !payloadlen) return 0;
    if (inlen<CCR_NONCE_BYTES+CCR_TAG_BYTES+CCR_SIG_BYTES+1) return 0;
    if (inlen>CCR_NONCE_BYTES+CP_BIN_PAYLOAD_MAX+CCR_TAG_BYTES+
        CCR_SIG_BYTES) return 0;
    if (!ccr_frame_ad(room_id,epoch,slot,kind,seq,author_pk,ad,sizeof(ad)))
        return 0;
    nonce=in;
    ctlen=inlen-(CCR_NONCE_BYTES+CCR_TAG_BYTES+CCR_SIG_BYTES);
    ct=nonce+CCR_NONCE_BYTES;
    tag=ct+ctlen;
    sig=tag+CCR_TAG_BYTES;
    signed_msg=malloc(sizeof(ad)+CCR_NONCE_BYTES+ctlen+CCR_TAG_BYTES);
    if (!signed_msg) return 0;
    memcpy(signed_msg,ad,sizeof(ad));
    memcpy(signed_msg+sizeof(ad),nonce,CCR_NONCE_BYTES);
    memcpy(signed_msg+sizeof(ad)+CCR_NONCE_BYTES,ct,ctlen+CCR_TAG_BYTES);
    if (crypto_sign_verify_detached(sig,signed_msg,sizeof(ad)+
            CCR_NONCE_BYTES+ctlen+CCR_TAG_BYTES,author_pk)!=0) {
        sodium_memzero(signed_msg,sizeof(ad)+CCR_NONCE_BYTES+ctlen+
            CCR_TAG_BYTES);
        free(signed_msg);
        return -1;
    }
    sodium_memzero(signed_msg,sizeof(ad)+CCR_NONCE_BYTES+ctlen+CCR_TAG_BYTES);
    free(signed_msg);
    plain=malloc(ctlen+1);
    if (!plain) return 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain,&plainlen,NULL,ct,
            ctlen+CCR_TAG_BYTES,ad,sizeof(ad),nonce,key)!=0) {
        sodium_memzero(plain,ctlen+1);
        free(plain);
        return -2;
    }
    plain[plainlen]=0;
    *payload=plain;
    *payloadlen=(size_t)plainlen;
    return 1;
}
/* --- eval/profile records --- */
char *ccr_eval_json(int slot, uint64_t seq, const char *body) {
    cJSON *root=NULL;
    char *text=NULL;
    size_t length;
    if (slot<0 || slot>=CP_SLOTS_MAX || !body) return NULL;
    length=strlen(body);
    if (length>CP_EVAL_BODY_MAX || seq>9007199254740992ULL) return NULL;
    root=cJSON_CreateObject();
    if (!root) return NULL;
    if (!cJSON_AddNumberToObject(root,"v",1) ||
        !cJSON_AddNumberToObject(root,"slot",(double)slot) ||
        !cJSON_AddNumberToObject(root,"seq",(double)seq) ||
        !cJSON_AddStringToObject(root,"body",body)) {
        cJSON_Delete(root);
        return NULL;
    }
    text=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}
int ccr_eval_parse(const char *text, size_t len, int *slot, uint64_t *seq,
    char **body) {
    cJSON *root=NULL;
    const cJSON *version, *slot_item, *seq_item, *body_item;
    double slot_value, seq_value;
    int ok=0;
    if (!text || !len || len>CP_EVAL_BODY_MAX+1024) return 0;
    root=cJSON_ParseWithLength(text,len);
    if (!root) return 0;
    version=cJSON_GetObjectItemCaseSensitive(root,"v");
    slot_item=cJSON_GetObjectItemCaseSensitive(root,"slot");
    seq_item=cJSON_GetObjectItemCaseSensitive(root,"seq");
    body_item=cJSON_GetObjectItemCaseSensitive(root,"body");
    if (!cJSON_IsNumber(version) || version->valuedouble!=1) goto done;
    if (!cJSON_IsNumber(slot_item)) goto done;
    slot_value=slot_item->valuedouble;
    if (slot_value<0 || slot_value>=CP_SLOTS_MAX ||
        slot_value!=(double)(int)slot_value)
        goto done;
    if (!cJSON_IsNumber(seq_item)) goto done;
    seq_value=seq_item->valuedouble;
    if (!(seq_value>=0) || seq_value>9007199254740992.0 ||
        seq_value!=(double)(uint64_t)seq_value) goto done;
    if (!cJSON_IsString(body_item) || !body_item->valuestring) goto done;
    if (strlen(body_item->valuestring)>CP_EVAL_BODY_MAX) goto done;
    if (slot) *slot=(int)slot_value;
    if (seq) *seq=(uint64_t)seq_value;
    if (body) {
        *body=strdup(body_item->valuestring);
        if (!*body) goto done;
    }
    ok=1;
done:
    cJSON_Delete(root);
    return ok;
}
char *ccr_profile_json(const char *nick, const char *color_or_null) {
    cJSON *root=NULL;
    char *text=NULL;
    if (!nick || !nick[0] || strlen(nick)>32) return NULL;
    if (color_or_null && strlen(color_or_null)>64) return NULL;
    root=cJSON_CreateObject();
    if (!root) return NULL;
    if (!cJSON_AddNumberToObject(root,"v",1) ||
        !cJSON_AddStringToObject(root,"nick",nick) ||
        (color_or_null && !cJSON_AddStringToObject(root,"color",
            color_or_null))) {
        cJSON_Delete(root);
        return NULL;
    }
    text=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}
int ccr_profile_parse(const char *text, size_t len, char *nick,
    size_t nickcap, char *color, size_t colorcap) {
    cJSON *root=NULL;
    const cJSON *version, *nick_item, *color_item;
    size_t nick_length;
    int ok=0;
    if (!text || !len || len>2048 || !nick || !nickcap) return 0;
    root=cJSON_ParseWithLength(text,len);
    if (!root) return 0;
    version=cJSON_GetObjectItemCaseSensitive(root,"v");
    nick_item=cJSON_GetObjectItemCaseSensitive(root,"nick");
    color_item=cJSON_GetObjectItemCaseSensitive(root,"color");
    if (!cJSON_IsNumber(version) || version->valuedouble!=1) goto done;
    if (!cJSON_IsString(nick_item) || !nick_item->valuestring) goto done;
    nick_length=strlen(nick_item->valuestring);
    if (!nick_length || nick_length>32 || nick_length>=nickcap) goto done;
    if (!cJSON_IsString(color_item) && color_item) goto done;
    if (cJSON_IsString(color_item)) {
        if (strlen(color_item->valuestring)>64) goto done;
        if (color && colorcap) {
            snprintf(color,colorcap,"%s",color_item->valuestring);
        }
    } else if (color && colorcap) color[0]=0;
    snprintf(nick,nickcap,"%s",nick_item->valuestring);
    ok=1;
done:
    cJSON_Delete(root);
    return ok;
}
/* --- fingerprints (section 13.5) --- */
void ccr_fingerprint(const uint8_t pk[CCR_PK_BYTES], char out[17]) {
    static const char alphabet[]="abcdefghijklmnopqrstuvwxyz234567";
    uint8_t digest[crypto_hash_sha256_BYTES];
    uint32_t window=0;
    int have=0, at=0, i;
    if (!pk || !out) return;
    crypto_hash_sha256(digest,pk,CCR_PK_BYTES);
    for (i=0;i<10;i++) {
        window=(window<<8)|digest[i];
        have+=8;
        while (have>=5 && at<16) {
            have-=5;
            out[at++] = alphabet[(window>>have)&31];
        }
    }
    out[at]=0;
    sodium_memzero(digest,sizeof(digest));
}
