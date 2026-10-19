// Strict bounded JSON reader/writer for relay frames. No allocation.
#include "RelayJson.h"
#include <errno.h>
#include <stdlib.h>
#include <initializer_list>

namespace opensmarttimer { namespace relay {
namespace {
size_t ws(const char* s, size_t n, size_t i) {
    while (i<n && (s[i]==' ' || s[i]=='\n' || s[i]=='\r' || s[i]=='\t')) ++i;
    return i;
}
int hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
bool hex4(const char* s, size_t n, size_t& i, uint32_t& value) {
    value=0;
    for (int k=0;k<4;++k) {
        if (i>=n || hex(s[i])<0) return false;
        value=(value<<4)|uint32_t(hex(s[i++]));
    }
    return true;
}
bool stringEnd(const char* s,size_t n,size_t& i) {
    if (i>=n || s[i++]!='"') return false;
    while (i<n) {
        unsigned char c=s[i++];
        if (c=='"') return true;
        if (c<0x20) return false;
        if (c!='\\') continue;
        if (i>=n) return false;
        c=s[i++];
        if (c=='"' || c=='\\' || c=='/' || c=='b' || c=='f' || c=='n' || c=='r' || c=='t') continue;
        if (c!='u') return false;
        uint32_t cp;
        if (!hex4(s,n,i,cp)) return false;
        if (cp>=0xd800 && cp<=0xdbff) {
            if (i+2>n || s[i++]!='\\' || s[i++]!='u') return false;
            if (!hex4(s,n,i,cp) || cp<0xdc00 || cp>0xdfff) return false;
        } else if (cp>=0xdc00 && cp<=0xdfff) return false;
    }
    return false;
}
bool numberEnd(const char* s,size_t n,size_t& i) {
    if(i<n && s[i]=='-') ++i;
    if(i>=n) return false;
    if(s[i]=='0') ++i;
    else {
        if(s[i]<'1' || s[i]>'9') return false;
        while(i<n && s[i]>='0' && s[i]<='9') ++i;
    }
    if(i<n && s[i]=='.') {
        ++i; size_t start=i;
        while(i<n && s[i]>='0' && s[i]<='9') ++i;
        if(i==start) return false;
    }
    if(i<n && (s[i]=='e' || s[i]=='E')) {
        ++i; if(i<n && (s[i]=='+' || s[i]=='-')) ++i;
        size_t start=i; while(i<n && s[i]>='0' && s[i]<='9') ++i;
        if(i==start) return false;
    }
    return true;
}
bool valueEnd(const char* s,size_t n,size_t& i,unsigned depth) {
    i=ws(s,n,i); if(i>=n || depth>16) return false;
    if(s[i]=='"') return stringEnd(s,n,i);
    if(s[i]=='{' || s[i]=='[') {
        bool object=s[i++]=='{'; char close=object?'}':']'; i=ws(s,n,i);
        if(i<n && s[i]==close) {++i;return true;}
        size_t starts[64],lengths[64],count=0;
        while(i<n) {
            if(object) {
                size_t start=i;
                if(!stringEnd(s,n,i)) return false;
                // Protocol field names are unescaped ASCII; avoiding aliases
                // makes duplicate detection unambiguous across implementations.
                for(size_t p=start+1;p+1<i;++p)
                    if(s[p]=='\\' || static_cast<unsigned char>(s[p])>=0x80) return false;
                if(count==64) return false;
                for(size_t k=0;k<count;++k)
                    if(lengths[k]==i-start && !memcmp(s+starts[k],s+start,i-start)) return false;
                starts[count]=start; lengths[count++]=i-start;
                i=ws(s,n,i); if(i>=n || s[i++]!=':') return false;
            }
            if(!valueEnd(s,n,i,depth+1)) return false;
            i=ws(s,n,i); if(i>=n) return false;
            if(s[i]==close) {++i;return true;}
            if(s[i++]!=',') return false;
            i=ws(s,n,i); if(i>=n || s[i]==close) return false;
        }
        return false;
    }
    for(const char* literal : {"true","false","null"}) {
        size_t len=strlen(literal);
        if(i+len<=n && !memcmp(s+i,literal,len)) { i+=len;return true; }
    }
    return numberEnd(s,n,i);
}
}

bool jsonFindKey(const char* body,size_t len,const char* key,JsonView* out) {
    if(!out) return false;
    memset(out,0,sizeof(*out));
    if(!body || !key) return false;
    size_t i=ws(body,len,0), end=i;
    if(i>=len || body[i]!='{' || !valueEnd(body,len,end,0) || ws(body,len,end)!=len) return false;
    ++i;
    while((i=ws(body,len,i))<len && body[i]!='}') {
        size_t key_start=i; if(!stringEnd(body,len,i)) return false;
        bool match=i-key_start==strlen(key)+2 && !memcmp(body+key_start+1,key,strlen(key));
        i=ws(body,len,i)+1; i=ws(body,len,i);
        size_t start=i; if(!valueEnd(body,len,i,0)) return false;
        if(match) {
            out->found=true; out->data=body+start; out->len=i-start;
            char c=body[start];
            if(c=='"') {out->is_string=true; ++out->data; out->len-=2;}
            else if(c=='{') out->is_object=true;
            else if(c=='[') out->is_array=true;
            else if(c=='t') out->is_bool_true=true;
            else if(c=='f') out->is_bool_false=true;
            else if(c=='n') out->is_null=true;
            else out->is_number=true;
            return true;
        }
        i=ws(body,len,i); if(i<len && body[i]==',') ++i;
    }
    return false;
}
bool jsonGetString(const char* b,size_t n,const char* key,const char** out,size_t* len) {
    JsonView v; if(!jsonFindKey(b,n,key,&v) || !v.is_string) return false;
    *out=v.data; *len=v.len; return true;
}
bool jsonGetInt(const char* b,size_t n,const char* key,int64_t* out) {
    JsonView v; if(!jsonFindKey(b,n,key,&v) || !v.is_number || !v.len || v.len>=32) return false;
    char scratch[32]; memcpy(scratch,v.data,v.len); scratch[v.len]=0;
    size_t i=scratch[0]=='-'?1:0;
    if(i==v.len || (scratch[i]=='0' && v.len-i>1)) return false;
    for(;i<v.len;++i) if(scratch[i]<'0' || scratch[i]>'9') return false;
    errno=0; char* end; long long parsed=strtoll(scratch,&end,10);
    if(errno==ERANGE || end!=scratch+v.len) return false;
    *out=static_cast<int64_t>(parsed); return true;
}
bool jsonGetBool(const char* b,size_t n,const char* key,bool* out) {
    JsonView v; if(!jsonFindKey(b,n,key,&v) || (!v.is_bool_true && !v.is_bool_false)) return false;
    *out=v.is_bool_true; return true;
}
int jsonCopyString(const char* b,size_t n,const char* key,char* out,size_t cap) {
    const char* s;size_t len;
    if(!jsonGetString(b,n,key,&s,&len)) return -1;
    size_t pos=0;
    auto emit=[&](char c) {if(!c || pos+1>=cap) return false;out[pos++]=c;return true;};
    for(size_t i=0;i<len;) {
        char c=s[i++]; if(c!='\\') {if(!emit(c)) return -2;continue;}
        c=s[i++];
        if(c=='u') {
            uint32_t cp; if(!hex4(s,len,i,cp)) return -1;
            if(cp>=0xd800 && cp<=0xdbff) {
                i+=2;uint32_t low;if(!hex4(s,len,i,low)) return -1;
                cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
            }
            if(cp<0x80) {if(!emit(char(cp))) return -2;}
            else if(cp<0x800) {if(!emit(char(0xc0|(cp>>6))) || !emit(char(0x80|(cp&63)))) return -2;}
            else if(cp<0x10000) {
                if(!emit(char(0xe0|(cp>>12))) || !emit(char(0x80|((cp>>6)&63))) || !emit(char(0x80|(cp&63)))) return -2;
            } else if(!emit(char(0xf0|(cp>>18))) || !emit(char(0x80|((cp>>12)&63)))
                || !emit(char(0x80|((cp>>6)&63))) || !emit(char(0x80|(cp&63)))) return -2;
        } else {
            if(c=='b') c='\b'; else if(c=='f') c='\f'; else if(c=='n') c='\n';
            else if(c=='r') c='\r'; else if(c=='t') c='\t';
            if(!emit(c)) return -2;
        }
    }
    if(pos>=cap) return -2;
    out[pos]=0;return int(pos);
}
}}  // namespace opensmarttimer::relay
