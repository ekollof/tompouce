/* Jam-mode E2E crypto (sections 13.5, 14.1, 14.5): master seed, member
   keys, admission, auth signatures, sealed control/frame records, eval and
   profile JSON, fingerprints. No network, no engine. Known-answer checks
   recompute with raw libsodium calls instead of the helpers under test.
   With TOMPOUCE_CRYPTO_VECTORS=1 the fixed vectors are also written to
   tests/vectors/collab-crypto.json for the web front end (M6). */
#include "collab_crypto.h"
#include <assert.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char room_id[]="mfrggzdfmztwq2lknnwg23tpoa";
static const uint8_t room_key[32]={
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f};
static const uint8_t master_seed[32]={
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f};
static const uint8_t fixed_nonce[24]={
    0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
    0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7};
static void to_hex(const uint8_t *in, size_t len, char *out) {
    static const char digits[]="0123456789abcdef";
    size_t i;
    for (i=0;i<len;i++) {
        out[2*i]=digits[in[i]>>4];
        out[2*i+1]=digits[in[i]&15];
    }
    out[2*len]=0;
}
static void test_identity(void) {
    char template[]="/tmp/tompouce-crypto-id-XXXXXX";
    char *tmpdir=mkdtemp(template);
    char path[1024], err[128]="";
    struct stat st;
    uint8_t first[32], second[32];
    FILE *file;
    assert(tmpdir && setenv("XDG_CONFIG_HOME",tmpdir,1)==0);
    assert(!getenv("HOME") || setenv("HOME","/nonexistent-test-home",1)==0);
    assert(ccr_identity_load_or_create(first,err,sizeof(err)));
    snprintf(path,sizeof(path),"%s/tompouce/collab/identity.key",tmpdir);
    assert(stat(path,&st)==0 && (st.st_mode&0777)==0600);
    assert(ccr_identity_load_or_create(second,err,sizeof(err)));
    assert(!memcmp(first,second,sizeof(first)));
    file=fopen(path,"wb");
    assert(file);
    assert(fwrite("short",1,5,file)==5);
    fclose(file);
    assert(ccr_identity_load_or_create(second,err,sizeof(err)));
    assert(memcmp(first,second,sizeof(first)));
    /* Corrupt files never read as keys: a fresh seed replaces garbage. */
    sodium_memzero(first,sizeof(first));
    sodium_memzero(second,sizeof(second));
    puts("crypto identity ok");
}
static void test_member_keys(void) {
    uint8_t pk[32], sk[64], pk2[32], sk2[64];
    uint8_t digest[crypto_hash_sha256_BYTES], seed[32];
    uint64_t subkey=0;
    int i;
    assert(ccr_room_keypair(master_seed,room_id,pk,sk));
    assert(ccr_room_keypair(master_seed,room_id,pk2,sk2));
    assert(!memcmp(pk,pk2,sizeof(pk)) && !memcmp(sk,sk2,sizeof(sk)));
    /* Independent derivation: LE64(SHA-256(room)[0..8]), ctx tpcroom1. */
    crypto_hash_sha256(digest,(const uint8_t *)room_id,26);
    for (i=0;i<8;i++) subkey|=(uint64_t)digest[i]<<(8*i);
    assert(crypto_kdf_derive_from_key(seed,sizeof(seed),subkey,"tpcroom1",
        master_seed)==0);
    assert(crypto_sign_seed_keypair(pk2,sk2,seed)==0);
    assert(!memcmp(pk,pk2,sizeof(pk)));
    sodium_memzero(seed,sizeof(seed));
    /* Different rooms and seeds unlink keys. */
    assert(ccr_room_keypair(master_seed,"mfrggzdfmztwq2lknnwg23tpoo",pk2,sk2));
    assert(memcmp(pk,pk2,sizeof(pk)));
    assert(!ccr_room_keypair(master_seed,"short",pk2,sk2));
    assert(!ccr_room_keypair(master_seed,NULL,pk2,sk2));
    assert(!ccr_room_keypair(NULL,room_id,pk2,sk2));
    sodium_memzero(sk,sizeof(sk));
    sodium_memzero(sk2,sizeof(sk2));
    puts("crypto member keys ok");
}
static void test_kdf(void) {
    uint8_t admission[32], expect[32], f1[32], f2[32], c2[32], b3[32];
    assert(crypto_kdf_derive_from_key(expect,sizeof(expect),0,"tpcadm01",
        room_key)==0);
    ccr_admission_key(room_key,admission);
    assert(!memcmp(admission,expect,sizeof(admission)));
    ccr_subkey(room_key,1,f1);
    ccr_subkey(room_key,2,c2);
    ccr_subkey(room_key,3,b3);
    ccr_subkey(room_key,1,f2);
    assert(!memcmp(f1,f2,sizeof(f1)));
    assert(memcmp(f1,c2,sizeof(f1)) && memcmp(c2,b3,sizeof(c2)) &&
        memcmp(f1,b3,sizeof(f1)));
    assert(crypto_kdf_derive_from_key(expect,sizeof(expect),2,"tpcsub01",
        room_key)==0);
    assert(!memcmp(c2,expect,sizeof(c2)));
    sodium_memzero(admission,sizeof(admission));
    puts("crypto kdf ok");
}
static void test_auth(void) {
    uint8_t pk[32], sk[64], challenge[32], sig[64], bad[64], mac[32], mac2[32];
    uint8_t admission[32], msg[18+32+26+32];
    int i;
    assert(sodium_init()>=0);
    for (i=0;i<32;i++) challenge[i]=(uint8_t)i;
    assert(ccr_room_keypair(master_seed,room_id,pk,sk));
    assert(ccr_auth_sign(sk,challenge,room_id,pk,sig));
    assert(ccr_auth_verify(pk,challenge,room_id,sig));
    /* Independent recomputation with raw sodium. */
    memcpy(msg,"tompouce-collab-v1",18);
    memcpy(msg+18,challenge,32);
    memcpy(msg+18+32,room_id,26);
    memcpy(msg+18+32+26,pk,32);
    assert(crypto_sign_verify_detached(sig,msg,sizeof(msg),pk)==0);
    memcpy(bad,sig,sizeof(bad));
    bad[0]^=1;
    assert(!ccr_auth_verify(pk,challenge,room_id,bad));
    challenge[0]^=1;
    assert(!ccr_auth_verify(pk,challenge,room_id,sig));
    challenge[0]^=1;
    {
        uint8_t wrong_challenge[32];
        memset(wrong_challenge,0x5a,sizeof(wrong_challenge));
        assert(!ccr_auth_verify(pk,wrong_challenge,room_id,sig));
    }
    assert(!ccr_auth_verify(NULL,challenge,room_id,sig));
    assert(!ccr_auth_verify(pk,challenge,"short",sig));
    /* Bootstrap proof matches an independent keyed hash. */
    ccr_admission_key(room_key,admission);
    assert(ccr_proof(admission,challenge,room_id,pk,mac));
    {
        uint8_t material[32+26+32];
        memcpy(material,challenge,32);
        memcpy(material+32,room_id,26);
        memcpy(material+32+26,pk,32);
        assert(crypto_generichash(mac2,sizeof(mac2),material,sizeof(material),
            admission,sizeof(admission))==0);
    }
    assert(sodium_memcmp(mac,mac2,sizeof(mac))==0);
    admission[0]^=1;
    assert(ccr_proof(admission,challenge,room_id,pk,mac2));
    assert(sodium_memcmp(mac,mac2,sizeof(mac))!=0);
    assert(!ccr_proof(admission,challenge,"short",pk,mac2));
    sodium_memzero(sk,sizeof(sk));
    puts("crypto auth ok");
}
static void test_ctrl_roundtrip(void) {
    uint8_t pk[32], sk[64], sub[32];
    const char body[]="$: note(\"c2 [~ c2] eb2 g1\")";
    uint8_t *enc=NULL, *plain=NULL;
    size_t enclen=0, plainlen=0;
    uint16_t epoch=0;
    uint8_t type=0;
    uint16_t slot=0;
    int rc;
    assert(ccr_room_keypair(master_seed,room_id,pk,sk));
    ccr_subkey(room_key,2,sub);
    assert(ccr_seal_ctrl_nonce(sub,room_id,7,CCR_CTRL_EVAL,3,pk,sk,
        fixed_nonce,(const uint8_t *)body,strlen(body),&enc,&enclen));
    assert(enclen==2+24+strlen(body)+16+64);
    rc=ccr_open_ctrl_typed(sub,room_id,7,CCR_CTRL_EVAL,3,pk,enc,enclen,
        &plain,&plainlen,&epoch,&type,&slot);
    assert(rc==1 && plainlen==strlen(body) && !memcmp(plain,body,plainlen));
    assert(epoch==7 && type==CCR_CTRL_EVAL && slot==3);
    free(plain);
    /* Wrong routing fields, room, key, epoch and any tampering fail. */
    assert(ccr_open_ctrl_typed(sub,room_id,7,CCR_CTRL_EVAL,4,pk,enc,enclen,
        &plain,&plainlen,NULL,NULL,NULL)==-1);
    assert(ccr_open_ctrl_typed(sub,room_id,8,CCR_CTRL_EVAL,3,pk,enc,enclen,
        &plain,&plainlen,NULL,NULL,NULL)==0);
    assert(ccr_open_ctrl_typed(sub,room_id,7,CCR_CTRL_PROFILE,3,pk,enc,enclen,
        &plain,&plainlen,NULL,NULL,NULL)==-1);
    {
        uint8_t wrong[32];
        memcpy(wrong,sub,sizeof(wrong));
        wrong[0]^=1;
        /* The signature does not involve the key, so a wrong key passes
           it and fails at decrypt instead. */
        assert(ccr_open_ctrl_typed(wrong,room_id,7,CCR_CTRL_EVAL,3,pk,enc,
            enclen,&plain,&plainlen,NULL,NULL,NULL)==-2);
        assert(ccr_open_ctrl_typed(sub,"mfrggzdfmztwq2lknnwg23tpoo",7,
            CCR_CTRL_EVAL,3,pk,enc,enclen,&plain,&plainlen,NULL,NULL,
            NULL)==-1);
    }
    {
        size_t i;
        /* Flip one byte in each region: epoch, nonce, ct, tag, sig. */
        size_t spots[]={0,2,10,2+24,2+24+strlen(body)-1,
            2+24+strlen(body),enclen-1,enclen-65};
        for (i=0;i<sizeof(spots)/sizeof(spots[0]);i++) {
            int got;
            enc[spots[i]]^=0x40;
            got=ccr_open_ctrl_typed(sub,room_id,7,CCR_CTRL_EVAL,3,pk,enc,
                enclen,&plain,&plainlen,NULL,NULL,NULL);
            if (spots[i]==0) assert(got==0);
            else assert(got==-1);
            enc[spots[i]]^=0x40;
        }
        /* A one-byte truncation shifts every offset, so the signature
           (not the length gate) rejects it. */
        assert(ccr_open_ctrl_typed(sub,room_id,7,CCR_CTRL_EVAL,3,pk,enc,
            enclen-1,&plain,&plainlen,NULL,NULL,NULL)==-1);
    }
    free(enc);
    /* Random-nonce seals open too. */
    assert(ccr_seal_ctrl(sub,room_id,0,CCR_CTRL_PROFILE,0,pk,sk,
        (const uint8_t *)"x",1,&enc,&enclen));
    assert(ccr_open_ctrl_typed(sub,room_id,0,CCR_CTRL_PROFILE,0,pk,enc,enclen,
        &plain,&plainlen,NULL,NULL,NULL)==1);
    assert(plainlen==1 && plain[0]=='x');
    free(enc);
    free(plain);
    assert(!ccr_seal_ctrl(sub,"short",0,1,0,pk,sk,(const uint8_t *)"x",1,
        &enc,&enclen));
    assert(!ccr_seal_ctrl(sub,room_id,0,1,0,pk,sk,NULL,0,&enc,&enclen));
    sodium_memzero(sk,sizeof(sk));
    sodium_memzero(sub,sizeof(sub));
    puts("crypto control records ok");
}
static void test_frame_roundtrip(void) {
    uint8_t pk[32], sk[64], sub[32];
    const uint8_t payload[]={0x01,0xc8,0xa1,0x00,0x5a};
    uint8_t *enc=NULL, *plain=NULL;
    size_t enclen=0, plainlen=0;
    assert(ccr_room_keypair(master_seed,room_id,pk,sk));
    ccr_subkey(room_key,1,sub);
    assert(ccr_seal_frame_nonce(sub,room_id,3,2,1,42,pk,sk,fixed_nonce,
        payload,sizeof(payload),&enc,&enclen));
    assert(enclen==24+sizeof(payload)+16+64);
    assert(ccr_open_frame(sub,room_id,3,2,1,42,pk,enc,enclen,&plain,
        &plainlen)==1);
    assert(plainlen==sizeof(payload) && !memcmp(plain,payload,plainlen));
    free(plain);
    /* Header fields are bound: any mismatch fails the signature first. */
    assert(ccr_open_frame(sub,room_id,3,2,1,43,pk,enc,enclen,&plain,
        &plainlen)==-1);
    assert(ccr_open_frame(sub,room_id,3,9,1,42,pk,enc,enclen,&plain,
        &plainlen)==-1);
    assert(ccr_open_frame(sub,room_id,4,2,1,42,pk,enc,enclen,&plain,
        &plainlen)==-1);
    enc[30]^=0x01;
    assert(ccr_open_frame(sub,room_id,3,2,1,42,pk,enc,enclen,&plain,
        &plainlen)==-1);
    enc[30]^=0x01;
    enc[0]^=0x80;
    assert(ccr_open_frame(sub,room_id,3,2,1,42,pk,enc,enclen,&plain,
        &plainlen)==-1);
    enc[0]^=0x80;
    assert(ccr_open_frame(sub,room_id,3,2,1,42,pk,enc,enclen-1,&plain,
        &plainlen)==-1);
    free(enc);
    sodium_memzero(sk,sizeof(sk));
    sodium_memzero(sub,sizeof(sub));
    puts("crypto frames ok");
}
static void test_records(void) {
    char *json=NULL, *body=NULL, nick[64]="", color[64]="";
    int slot=-1;
    uint64_t seq=0;
    assert(sodium_init()>=0);
    json=ccr_eval_json(3,41,"$: note(60)");
    assert(json);
    assert(ccr_eval_parse(json,strlen(json),&slot,&seq,&body));
    assert(slot==3 && seq==41 && !strcmp(body,"$: note(60)"));
    free(json);
    free(body);
    body=NULL;
    assert(!ccr_eval_parse("{\"v\":2,\"slot\":1,\"seq\":1,\"body\":\"x\"}",38,
        &slot,&seq,&body));
    /* Room slots are 0..15 on the wire (CP_SLOTS_MAX), matching the
       director's cap. */
    {
        int parsed_slot=-1;
        assert(!ccr_eval_parse("{\"v\":1,\"slot\":16,\"seq\":1,\"body\":\"x\"}",
            41,&parsed_slot,&seq,&body));
    }
    {
        /* Eval bodies cap at 256 KiB (CP_EVAL_BODY_MAX); one byte over
           rejects. */
        char big[262150];
        memset(big,'x',sizeof(big)-1);
        big[sizeof(big)-1]=0;
        assert(!ccr_eval_json(0,1,big));
        assert(!ccr_eval_json(-1,1,"x"));
        assert(!ccr_eval_json(0,9007199254740993ULL,"x"));
    }
    assert(!ccr_eval_parse("not json",8,&slot,&seq,&body));
    json=ccr_profile_json("andrath",NULL);
    assert(json);
    assert(ccr_profile_parse(json,strlen(json),nick,sizeof(nick),color,
        sizeof(color)));
    assert(!strcmp(nick,"andrath") && !color[0]);
    free(json);
    json=ccr_profile_json("mira","#80ff20");
    assert(json);
    assert(ccr_profile_parse(json,strlen(json),nick,sizeof(nick),color,
        sizeof(color)));
    assert(!strcmp(nick,"mira") && !strcmp(color,"#80ff20"));
    free(json);
    assert(!ccr_profile_json("",NULL));
    assert(!ccr_profile_json("012345678901234567890123456789012",NULL));
    assert(!ccr_profile_parse("{\"v\":1,\"nick\":\"\"}",17,nick,sizeof(nick),
        color,sizeof(color)));
    assert(!ccr_profile_parse("{\"v\":1}",9,nick,sizeof(nick),color,
        sizeof(color)));
    puts("crypto records ok");
}
static void test_fingerprint(void) {
    uint8_t pk[32], sk[64], pk2[32], sk2[64];
    char fp[17], fp2[17];
    size_t i;
    assert(ccr_room_keypair(master_seed,room_id,pk,sk));
    ccr_fingerprint(pk,fp);
    assert(strlen(fp)==16);
    for (i=0;i<16;i++) assert((fp[i]>='a' && fp[i]<='z') ||
        (fp[i]>='2' && fp[i]<='7'));
    ccr_fingerprint(pk,fp2);
    assert(!strcmp(fp,fp2));
    assert(ccr_room_keypair(master_seed,"mfrggzdfmztwq2lknnwg23tpoo",pk2,sk2));
    ccr_fingerprint(pk2,fp2);
    assert(strcmp(fp,fp2));
    sodium_memzero(sk,sizeof(sk));
    sodium_memzero(sk2,sizeof(sk2));
    printf("crypto fingerprint ok (%s)\n",fp);
}
static void dump_vectors(void) {
    /* Deterministic vectors for the web front end (M6): fixed keys and
       nonces, so the file is byte-stable. Only written on demand. */
    uint8_t pk[32], sk[64], sub2[32], sub1[32], admission[32], mac[32];
    uint8_t challenge[32], sig[64];
    const char eval_body[]="$: note(60)";
    char *eval_text=NULL, hex[2200];
    uint8_t *enc=NULL, *plain=NULL;
    size_t enclen=0, plainlen=0;
    uint16_t epoch=0;
    uint8_t type=0;
    uint16_t slot=0;
    FILE *file;
    int i;
    for (i=0;i<32;i++) challenge[i]=(uint8_t)(0x40+i);
    assert(ccr_room_keypair(master_seed,room_id,pk,sk));
    ccr_subkey(room_key,2,sub2);
    ccr_subkey(room_key,1,sub1);
    ccr_admission_key(room_key,admission);
    assert(ccr_auth_sign(sk,challenge,room_id,pk,sig));
    assert(ccr_proof(admission,challenge,room_id,pk,mac));
    eval_text=ccr_eval_json(3,41,eval_body);
    assert(eval_text);
    assert(ccr_seal_ctrl_nonce(sub2,room_id,7,CCR_CTRL_EVAL,3,pk,sk,
        fixed_nonce,(const uint8_t *)eval_text,strlen(eval_text),&enc,
        &enclen));
    assert(ccr_open_ctrl_typed(sub2,room_id,7,CCR_CTRL_EVAL,3,pk,enc,enclen,
        &plain,&plainlen,&epoch,&type,&slot)==1);
    free(plain);
    file=fopen("tests/vectors/collab-crypto.json","w");
    assert(file);
    fprintf(file,"{\n");
    fprintf(file,"  \"room_id\": \"%s\",\n",room_id);
    to_hex(master_seed,sizeof(master_seed),hex);
    fprintf(file,"  \"master_seed_hex\": \"%s\",\n",hex);
    to_hex(room_key,sizeof(room_key),hex);
    fprintf(file,"  \"room_key_hex\": \"%s\",\n",hex);
    to_hex(pk,sizeof(pk),hex);
    fprintf(file,"  \"member_pk_hex\": \"%s\",\n",hex);
    to_hex(sk,sizeof(sk),hex);
    fprintf(file,"  \"member_sk_hex\": \"%s\",\n",hex);
    to_hex(challenge,sizeof(challenge),hex);
    fprintf(file,"  \"challenge_hex\": \"%s\",\n",hex);
    to_hex(sig,sizeof(sig),hex);
    fprintf(file,"  \"auth_sig_hex\": \"%s\",\n",hex);
    to_hex(admission,sizeof(admission),hex);
    fprintf(file,"  \"admission_key_hex\": \"%s\",\n",hex);
    to_hex(mac,sizeof(mac),hex);
    fprintf(file,"  \"proof_mac_hex\": \"%s\",\n",hex);
    to_hex(fixed_nonce,sizeof(fixed_nonce),hex);
    fprintf(file,"  \"nonce_hex\": \"%s\",\n",hex);
    fprintf(file,"  \"eval_record\": %s,\n",eval_text);
    to_hex(enc,enclen,hex);
    fprintf(file,"  \"eval_enc_hex\": \"%s\",\n",hex);
    to_hex(sub2,sizeof(sub2),hex);
    fprintf(file,"  \"ctrl_subkey_hex\": \"%s\",\n",hex);
    to_hex(sub1,sizeof(sub1),hex);
    fprintf(file,"  \"frame_subkey_hex\": \"%s\"\n",hex);
    fprintf(file,"}\n");
    fclose(file);
    free(eval_text);
    free(enc);
    sodium_memzero(sk,sizeof(sk));
    printf("vectors written to tests/vectors/collab-crypto.json\n");
}
int main(void) {
    assert(ccr_init());
    test_identity();
    test_member_keys();
    test_kdf();
    test_auth();
    test_ctrl_roundtrip();
    test_frame_roundtrip();
    test_records();
    test_fingerprint();
    if (getenv("TOMPOUCE_CRYPTO_VECTORS")) dump_vectors();
    puts("collab crypto ok");
    return 0;
}
