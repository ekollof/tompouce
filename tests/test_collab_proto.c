/* Exactness and rejection behavior of the collab wire codec. */
#include "../src/collab_proto.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void test_control_ok(void) {
    CpMsg msg={0};
    const char *text="{\"t\":\"hello\",\"id\":17,\"proto\":[1]}";
    assert(cp_parse_control(text,strlen(text),&msg)==CP_E_NONE);
    assert(msg.type==CP_T_HELLO && msg.has_id && msg.id==17);
    cp_msg_free(&msg);
    assert(cp_type_from_string("welcome")==CP_T_WELCOME);
    assert(!strcmp(cp_type_string(CP_T_WELCOME),"welcome"));
    assert(cp_type_from_string("nope")==CP_T_UNKNOWN);
    assert(cp_error_string(CP_E_SIZE)!=NULL);
    assert(!strcmp(cp_error_string(CP_E_PROTO),"proto"));
}
static void test_control_reject(void) {
    CpMsg msg={0};
    static char big[CP_CONTROL_MAX+16];
    static char deep[512];
    unsigned i, at=0;
    const char *bad[]={
        "{\"id\":1}",
        "{\"t\":17}",
        "{\"t\":\"nope\"}",
        "{\"t\":\"hello\",\"id\":-1}",
        "{\"t\":\"hello\",\"id\":1.5}",
        "{\"t\":\"hello\",\"id\":4294967296}",
        "{\"t\":\"hello\",\"id\":\"x\"}",
        "[1,2]",
        "42",
        "{",
    };
    for (i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        CpError error=cp_parse_control(bad[i],strlen(bad[i]),&msg);
        assert(error==CP_E_SCHEMA || error==CP_E_TYPE);
        cp_msg_free(&msg);
    }
    assert(cp_parse_control("",0,&msg)==CP_E_SIZE);
    cp_msg_free(&msg);
    memset(big,'{',sizeof(big));
    assert(cp_parse_control(big,sizeof(big),&msg)==CP_E_SCHEMA);
    cp_msg_free(&msg);
    {
        /* Valid JSON over the default 16 KiB control cap. */
        static char padded[CP_CONTROL_MAX+64];
        size_t prefix=snprintf(padded,sizeof(padded),
            "{\"t\":\"ping\",\"pad\":\"");
        memset(padded+prefix,'x',sizeof(padded)-prefix-3);
        padded[sizeof(padded)-3]='\"';
        padded[sizeof(padded)-2]='}';
        padded[sizeof(padded)-1]=0;
        assert(cp_parse_control(padded,strlen(padded),&msg)==CP_E_SIZE);
        cp_msg_free(&msg);
    }
    for (i=0;i<10;i++) deep[at++]='[';
    deep[at++]='1';
    for (i=0;i<10;i++) deep[at++]=']';
    deep[at]=0;
    assert(cp_parse_control(deep,at,&msg)==CP_E_SCHEMA);
    cp_msg_free(&msg);
    assert(cp_parse_control("{\"t\":\"ping\"}",strlen("{\"t\":\"ping\"}"),&msg)==CP_E_NONE);
    cp_msg_free(&msg);
}
static void test_fields(void) {
    CpMsg msg={0};
    const char *text="{\"t\":\"auth\",\"room\":\"abcdefghijklmnopqrstuvwxyz\","
        "\"n\":[513,2],\"bad\":[1,3],\"f\":1.5,\"b\":true}";
    const char *room=NULL, *missing=NULL;
    uint64_t num=0;
    unsigned den=0;
    double f=0;
    int b=0;
    assert(cp_parse_control(text,strlen(text),&msg)==CP_E_NONE);
    assert(cp_get_room_id(msg.owned,"room",&room));
    assert(!cp_get_room_id(msg.owned,"absent",&missing));
    assert(!cp_get_room_id(msg.owned,"f",&missing));
    assert(cp_get_cycle(msg.owned,"n",&num,&den) && num==513 && den==2);
    assert(!cp_get_cycle(msg.owned,"bad",&num,&den));
    assert(!cp_get_cycle(msg.owned,"f",&num,&den));
    assert(cp_get_double(msg.owned,"f",&f) && f==1.5);
    assert(!cp_get_double(msg.owned,"b",&f));
    assert(cp_get_bool(msg.owned,"b",&b) && b==1);
    assert(!cp_get_bool(msg.owned,"f",&b));
    cp_msg_free(&msg);
    assert(cp_parse_control("{\"t\":\"x\",\"room\":\"BAD!!!\"}",
        strlen("{\"t\":\"x\",\"room\":\"BAD!!!\"}"),&msg)==CP_E_TYPE);
    cp_msg_free(&msg);
}
static void test_b64(void) {
    uint8_t raw[256], decoded[256];
    char encoded[512];
    size_t i, outlen=0;
    for (i=0;i<sizeof(raw);i++) raw[i]=(uint8_t)(i*13+7);
    assert(cp_b64_encode(raw,0,encoded,sizeof(encoded))==0 && encoded[0]==0);
    assert(cp_b64_decode("",decoded,sizeof(decoded),&outlen) && outlen==0);
    for (i=1;i<sizeof(raw);i++) {
        size_t length=cp_b64_encode(raw,i,encoded,sizeof(encoded));
        assert(length>0);
        assert(cp_b64_decode(encoded,decoded,sizeof(decoded),&outlen));
        assert(outlen==i && memcmp(raw,decoded,i)==0);
    }
    assert(!cp_b64_decode("A",decoded,sizeof(decoded),&outlen));
    assert(!cp_b64_decode("A=B",decoded,sizeof(decoded),&outlen));
    assert(!cp_b64_decode("AB=C",decoded,sizeof(decoded),&outlen));
    assert(!cp_b64_decode("A B C",decoded,sizeof(decoded),&outlen));
    assert(!cp_b64_decode("AB+D",decoded,sizeof(decoded),&outlen));
    assert(!cp_b64_decode("AB",decoded,0,&outlen));
    /* Non-canonical trailing bits are rejected: "AA" is zero, "AB" and
       "AC" carry nonzero leftover bits. */
    assert(cp_b64_decode("AA",decoded,sizeof(decoded),&outlen));
    assert(outlen==1 && decoded[0]==0);
    assert(!cp_b64_decode("AB",decoded,sizeof(decoded),&outlen));
    assert(!cp_b64_decode("AC",decoded,sizeof(decoded),&outlen));
    /* "ABA" is {0,16}; "ABC" and "ABD" set leftover bits. */
    assert(cp_b64_decode("ABA",decoded,sizeof(decoded),&outlen));
    assert(outlen==2 && decoded[0]==0 && decoded[1]==16);
    assert(!cp_b64_decode("ABC",decoded,sizeof(decoded),&outlen));
    assert(!cp_b64_decode("ABD",decoded,sizeof(decoded),&outlen));
}
static void binary_frame(uint8_t *frame, size_t *len, uint8_t kind,
    uint32_t declared) {
    frame[0]=1; frame[1]=kind;
    frame[2]=0; frame[3]=7;
    frame[4]=0; frame[5]=0; frame[6]=0; frame[7]=3;
    frame[8]=0; frame[9]=0; frame[10]=0; frame[11]=9;
    frame[12]=0; frame[13]=5;
    frame[14]=0; frame[15]=0;
    frame[16]=(uint8_t)(declared>>24); frame[17]=(uint8_t)(declared>>16);
    frame[18]=(uint8_t)(declared>>8); frame[19]=(uint8_t)declared;
    *len=20+4;
    frame[20]=1; frame[21]=2; frame[22]=3; frame[23]=4;
}
static void test_binary(void) {
    uint8_t frame[32];
    size_t len=0;
    CpBinHeader hdr={0};
    const uint8_t *payload=NULL;
    size_t payload_len=0;
    binary_frame(frame,&len,1,4);
    assert(cp_parse_binary(frame,len,&hdr,&payload,&payload_len)==CP_E_NONE);
    assert(hdr.kind==1 && hdr.slot==7 && hdr.member==3 && hdr.seq==9);
    assert(hdr.epoch==5 && hdr.flags==0 && payload_len==4);
    assert(payload[0]==1 && payload[3]==4);
    frame[0]=2;
    assert(cp_parse_binary(frame,len,&hdr,&payload,&payload_len)==CP_E_PROTO);
    frame[0]=1; frame[1]=9;
    assert(cp_parse_binary(frame,len,&hdr,&payload,&payload_len)==CP_E_SCHEMA);
    frame[1]=1; frame[19]=9;
    assert(cp_parse_binary(frame,len,&hdr,&payload,&payload_len)==CP_E_SCHEMA);
    frame[19]=4; frame[15]=1;
    assert(cp_parse_binary(frame,len,&hdr,&payload,&payload_len)==CP_E_SCHEMA);
    frame[15]=0; frame[14]=2;
    assert(cp_parse_binary(frame,len,&hdr,&payload,&payload_len)==CP_E_SCHEMA);
    frame[14]=1;
    assert(cp_parse_binary(frame,len,&hdr,&payload,&payload_len)==CP_E_NONE);
    assert(cp_parse_binary(frame,10,&hdr,&payload,&payload_len)==CP_E_SCHEMA);
    assert(cp_parse_binary(NULL,len,&hdr,&payload,&payload_len)==CP_E_SCHEMA);
}
int main(void) {
    test_control_ok();
    test_control_reject();
    test_fields();
    test_b64();
    test_binary();
    puts("collab wire codec validation ok");
    return 0;
}
