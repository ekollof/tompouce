#include "osc_output.h"
#include "common.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <float.h>
#include <ctype.h>

static int sockets[2]={-1,-1};
static int put_string(unsigned char *out,size_t capacity,size_t *at,const char *text,size_t limit) {
    size_t n=strlen(text); if (n>limit) return 0;
    size_t bytes=(n+4)&~(size_t)3;
    if (*at>capacity || bytes>capacity-*at) return 0;
    memset(out+*at,0,bytes); memcpy(out+*at,text,n); *at+=bytes; return 1;
}
static void put_u32(unsigned char *out,uint32_t value) { value=htonl(value); memcpy(out,&value,4); }

int osc_output_prepare(OscOutputMessage *message,const cJSON *value,double cycle,double cps,double duration) {
    if (!message || !cJSON_IsObject(value) || !isfinite(cycle) || !isfinite(cps) || cps<=0 || !isfinite(duration)) return 0;
    memset(message,0,sizeof(*message));
    const cJSON *host=cJSON_GetObjectItemCaseSensitive(value,"oschost"), *port=cJSON_GetObjectItemCaseSensitive(value,"oscport");
    const char *name=host?(cJSON_IsString(host)?host->valuestring:NULL):"127.0.0.1";
    if (!name || strlen(name)>64 || (port && (!cJSON_IsNumber(port) || !isfinite(port->valuedouble) ||
        port->valuedouble<1 || port->valuedouble>65535 || port->valuedouble!=floor(port->valuedouble)))) return 0;
    if (!strcmp(name,"localhost")) name="127.0.0.1";
    message->family=strchr(name,':')?AF_INET6:AF_INET;
    if (inet_pton(message->family,name,message->address)!=1) return 0;
    message->port=(uint16_t)(port?port->valueint:57120);
    cJSON *controls=cJSON_Duplicate(value,1);
    if (!controls) return 0;
    const char *remove[]={"oschost","oscport","workletSrc","workletInputs","color","colour","markcss",NULL};
    for (int i=0;remove[i];i++) cJSON_DeleteItemFromObjectCaseSensitive(controls,remove[i]);
    cJSON *entry=controls->child;
    while (entry) { cJSON *next=entry->next; if (entry->string && entry->string[0]=='_') cJSON_DeleteItemFromObjectCaseSensitive(controls,entry->string); entry=next; }
    cJSON *number=cJSON_GetObjectItemCaseSensitive(controls,"n");
    if (cJSON_IsString(number)) {
        char *end; double n=strtod(number->valuestring,&end);
        if (end!=number->valuestring && !*end && isfinite(n)) cJSON_ReplaceItemInObjectCaseSensitive(controls,"n",cJSON_CreateNumber(n));
    }
    cJSON *note=cJSON_GetObjectItemCaseSensitive(controls,"note");
    if (cJSON_IsString(note)) {
        char *end; double n=strtod(note->valuestring,&end);
        if (end!=note->valuestring && !*end && isfinite(n)) cJSON_ReplaceItemInObjectCaseSensitive(controls,"note",cJSON_CreateNumber(n));
        else {
            const char *p=note->valuestring, *letters="c d ef g a b", *found=strchr(letters,tolower((unsigned char)*p));
            if (!*p || !found || *p==' ') { cJSON_Delete(controls); return 0; }
            int semitone=(int)(found-letters); p++;
            while (*p=='#' || *p=='s' || *p=='b') { semitone+=*p=='b'?-1:1; p++; }
            long octave=*p?strtol(p,&end,10):3;
            if ((*p && (end==p || *end)) || octave<-10 || octave>20 ||
                !cJSON_AddNumberToObject(controls,"midinote",(octave+1)*12+semitone)) { cJSON_Delete(controls); return 0; }
            cJSON_DeleteItemFromObjectCaseSensitive(controls,"note");
        }
    }
    const cJSON *bank=cJSON_GetObjectItemCaseSensitive(controls,"bank"), *sound=cJSON_GetObjectItemCaseSensitive(controls,"s");
    if (bank && cJSON_IsString(bank) && cJSON_IsString(sound)) {
        char joined[256]; int n=snprintf(joined,sizeof(joined),"%s%s",bank->valuestring,sound->valuestring);
        if (n<0 || (size_t)n>=sizeof(joined) || !cJSON_ReplaceItemInObjectCaseSensitive(controls,"s",cJSON_CreateString(joined))) { cJSON_Delete(controls); return 0; }
    }
    const cJSON *room=cJSON_GetObjectItemCaseSensitive(controls,"roomsize");
    if (cJSON_IsNumber(room)) { cJSON_DeleteItemFromObjectCaseSensitive(controls,"size"); if (!cJSON_AddNumberToObject(controls,"size",room->valuedouble)) { cJSON_Delete(controls); return 0; } }
    cJSON *speed=cJSON_GetObjectItemCaseSensitive(controls,"speed");
    const cJSON *unit=cJSON_GetObjectItemCaseSensitive(controls,"unit");
    if (cJSON_IsString(unit) && !strcmp(unit->valuestring,"c") && cJSON_IsNumber(speed)) speed->valuedouble/=cps;
    const char *keys[]={"cps","cycle","delta"}; const double values[]={cps,cycle,duration};
    for (int i=0;i<3;i++) {
        cJSON_DeleteItemFromObjectCaseSensitive(controls,keys[i]);
        if (!cJSON_AddNumberToObject(controls,keys[i],values[i])) { cJSON_Delete(controls); return 0; }
    }
    char tags[130]={','}; size_t count=1, at=0; int ok=1;
    cJSON_ArrayForEach(entry,controls) {
        if (count+2>=sizeof(tags) || !entry->string || strlen(entry->string)>48) { ok=0; break; }
        tags[count++]='s';
        if (cJSON_IsString(entry)) tags[count++]='s';
        else if (cJSON_IsNumber(entry) && isfinite(entry->valuedouble) && fabs(entry->valuedouble)<=FLT_MAX)
            tags[count++]=entry->valuedouble==floor(entry->valuedouble) && entry->valuedouble>=INT32_MIN && entry->valuedouble<=INT32_MAX?'i':'f';
        else { ok=0; break; }
    }
    if (ok) ok=put_string(message->data,sizeof(message->data),&at,"/dirt/play",128) && put_string(message->data,sizeof(message->data),&at,tags,sizeof(tags)-1);
    count=1;
    cJSON_ArrayForEach(entry,controls) {
        if (!ok) break;
        ok=put_string(message->data,sizeof(message->data),&at,entry->string,48); count++;
        if (tags[count++]=='s') ok=ok && put_string(message->data,sizeof(message->data),&at,entry->valuestring,512);
        else if (ok && at+4<=sizeof(message->data)) {
            uint32_t bits;
            if (tags[count-1]=='i') bits=(uint32_t)(int32_t)entry->valuedouble;
            else { float n=(float)entry->valuedouble; memcpy(&bits,&n,4); }
            put_u32(message->data+at,bits); at+=4;
        } else ok=0;
    }
    cJSON_Delete(controls); if (!ok) return 0;
    message->size=(uint16_t)at; return 1;
}

int osc_output_send(const OscOutputMessage *message,double seconds_until_due) {
    if (!message || !message->size || message->size>OSC_OUTPUT_BYTES ||
        (message->family!=AF_INET && message->family!=AF_INET6) || !isfinite(seconds_until_due)) return 0;
    int which=message->family==AF_INET6;
    if (sockets[which]<0) sockets[which]=socket(message->family,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if (sockets[which]<0) return 0;
    unsigned char packet[OSC_OUTPUT_BYTES+20]={0}; memcpy(packet,"#bundle",7);
    struct timespec now; if (clock_gettime(CLOCK_REALTIME,&now)) return 0;
    double unix_time=now.tv_sec+now.tv_nsec/1e9+fmax(0,seconds_until_due), whole=floor(unix_time);
    put_u32(packet+8,(uint32_t)fmod(whole+2208988800.0,4294967296.0)); put_u32(packet+12,(uint32_t)((unix_time-whole)*4294967296.0));
    put_u32(packet+16,message->size); memcpy(packet+20,message->data,message->size);
    struct sockaddr_storage address={0}; socklen_t length;
    if (which) {
        struct sockaddr_in6 *a=(struct sockaddr_in6 *)&address; a->sin6_family=AF_INET6; a->sin6_port=htons(message->port);
        memcpy(&a->sin6_addr,message->address,16); length=sizeof(*a);
    } else {
        struct sockaddr_in *a=(struct sockaddr_in *)&address; a->sin_family=AF_INET; a->sin_port=htons(message->port);
        memcpy(&a->sin_addr,message->address,4); length=sizeof(*a);
    }
    return sendto(sockets[which],packet,message->size+20,MSG_DONTWAIT,(struct sockaddr *)&address,length)==message->size+20;
}
void osc_output_shutdown(void) { for (int i=0;i<2;i++) if (sockets[i]>=0) { close(sockets[i]); sockets[i]=-1; } }
