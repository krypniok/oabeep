// oabeep.c - OpenAL "beep"-Ersatz mit Mono/Stereo, Glide, Chords, Rests.
// Usage: oabeep [global opts] token [token...]
// Tokens: mono: F[:ms]  | stereo: L,R[:ms]  | glide: A~B[:ms]  | chord: f1+f2+...[:ms] | rest: r:ms / 0:ms
// Global: -g gain(0..1) -sr samplerate -l default_ms -fade fadems

#define _GNU_SOURCE
#include <AL/al.h>
#include <AL/alc.h>
#include <ctype.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float *chan[2];
    int channels;
    int length;
    int sample_rate;
} SampleData;

typedef struct {
    char *path;
    SampleData data;
} SampleCacheEntry;

static uint32_t noise_state = 0x12345678u;

static SampleCacheEntry *sample_cache=NULL;
static size_t sample_cache_len=0, sample_cache_cap=0;

static inline float frand_unit(void){
    noise_state = noise_state * 1664525u + 1013904223u;
    return ((noise_state >> 8) * (1.0f / 8388608.0f)) * 2.f - 1.f;
}

typedef enum {
    SPEC_SILENCE=0,
    SPEC_CONST,
    SPEC_GLIDE,
    SPEC_CHORD,
    SPEC_KICK,
    SPEC_SNARE,
    SPEC_HIHAT,
    SPEC_BASS,
    SPEC_FLUTE,
    SPEC_PIANO,
    SPEC_GUITAR,
    SPEC_EGTR,
    SPEC_SAMPLE,
    SPEC_BIRDS,
    SPEC_STRPAD,
    SPEC_BELL,
    SPEC_BRASS,
    SPEC_KALIMBA
} SpecType;

typedef struct {
    SpecType type;
    float f_const;           // CONST
    float f0, f1;            // GLIDE
    float chord[16]; int n;  // CHORD (max 16 partials)
    SampleData *sample;
    int sample_channel;
} Spec;

typedef struct {
    Spec L, R;
    bool stereo;
    int dur_ms;
    int samples;
    bool explicit_dur;
    bool sample_override;
} Token;

typedef struct {
    char *cols[5];
} CsvRow;

typedef struct {
    CsvRow *items;
    size_t len;
    size_t cap;
} CsvRowVec;

typedef struct {
    Spec L, R;
    int samples;
    size_t start;
} RenderEvent;

typedef struct {
    RenderEvent *items;
    size_t len;
    size_t cap;
} RenderVec;

typedef struct {
    int64_t start_ms;
    char *voice;
    char *text;
    char **args;
    int arg_count;
} SpeechEvent;

typedef struct {
    SpeechEvent *items;
    size_t len;
    size_t cap;
} SpeechVec;

typedef struct {
    char *name;
    CsvRowVec rows;
} MacroDef;

typedef struct {
    MacroDef *items;
    size_t len;
    size_t cap;
} MacroVec;

static void die(const char *msg);

static char *strdup_upper(const char *s){
    if(!s) return NULL;
    size_t len=strlen(s);
    char *r=malloc(len+1);
    if(!r) die("oom");
    for(size_t i=0;i<len;i++){
        unsigned char c=(unsigned char)s[i];
        r[i]=(char)toupper(c);
    }
    r[len]='\0';
    return r;
}

static void sample_data_free(SampleData *sd){
    if(!sd) return;
    free(sd->chan[0]);
    free(sd->chan[1]);
    sd->chan[0]=sd->chan[1]=NULL;
    sd->channels=0;
    sd->length=0;
    sd->sample_rate=0;
}

static void macro_vec_free(MacroVec *vec);
static void csv_row_free(CsvRow *row);
static void csv_rowvec_free(CsvRowVec *vec);
static void synth_spec_into(const Spec *sp, float *dst, int n, int sr);
static void apply_fade(float *x, int n, int sr, int fade_ms);
static int play_buffer_with_openal(const float *L, const float *R, size_t total_samples, float gain, int sr, const SpeechVec *speech, const char *espeak_bin);

static void trim(char *s){
    // strip spaces
    size_t n=strlen(s); size_t i=0,j=n;
    while(i<n && isspace((unsigned char)s[i])) i++;
    while(j>i && isspace((unsigned char)s[j-1])) j--;
    memmove(s, s+i, j-i); s[j-i]='\0';
}

static bool parse_float(const char *s, float *out){
    char *e=NULL; float v=strtof(s,&e);
    if(e==s || (e && *e!='\0')) return false;
    *out=v; return true;
}

static char *dup_unquoted(const char *s){
    if(!s) return NULL;
    while(*s==' '||*s=='\t') s++;
    size_t len=strlen(s);
    while(len>0 && isspace((unsigned char)s[len-1])) len--;
    if(len>=2 && ((s[0]=='\"' && s[len-1]=='\"') || (s[0]=='\'' && s[len-1]=='\''))){
        char *out=malloc(len-1);
        if(!out) die("oom");
        memcpy(out,s+1,len-2);
        out[len-2]='\0';
        return out;
    }
    char *out=malloc(len+1);
    if(!out) die("oom");
    memcpy(out,s,len);
    out[len]='\0';
    return out;
}

static bool load_wav_file(const char *path, SampleData *out){
    memset(out,0,sizeof(*out));
    FILE *fp=fopen(path,"rb");
    if(!fp){
        fprintf(stderr,"wav open failed %s: %s\n", path, strerror(errno));
        return false;
    }
    char id[4];
    if(fread(id,1,4,fp)!=4 || memcmp(id,"RIFF",4)!=0){ fclose(fp); fprintf(stderr,"wav %s missing RIFF\n",path); return false; }
    uint32_t riff_size=0;
    fread(&riff_size,4,1,fp);
    if(fread(id,1,4,fp)!=4 || memcmp(id,"WAVE",4)!=0){ fclose(fp); fprintf(stderr,"wav %s missing WAVE\n",path); return false; }
    uint16_t audio_format=0, channels=0, bits_per_sample=0;
    uint32_t sample_rate=0;
    bool fmt_found=false, data_found=false;
    uint8_t *raw=NULL;
    size_t raw_frames=0;
    while(fread(id,1,4,fp)==4){
        uint32_t chunk_size=0;
        if(fread(&chunk_size,4,1,fp)!=1) break;
        long chunk_start=ftell(fp);
        if(memcmp(id,"fmt ",4)==0){
            if(chunk_size < 16){ fprintf(stderr,"wav %s bad fmt chunk\n",path); goto fail; }
            fread(&audio_format,2,1,fp);
            fread(&channels,2,1,fp);
            fread(&sample_rate,4,1,fp);
            uint32_t byte_rate=0; fread(&byte_rate,4,1,fp);
            uint16_t block_align=0; fread(&block_align,2,1,fp);
            fread(&bits_per_sample,2,1,fp);
            if(chunk_size>16) fseek(fp, chunk_size-16, SEEK_CUR);
            if(audio_format!=1 || (channels!=1 && channels!=2) || bits_per_sample!=16){
                fprintf(stderr,"wav %s unsupported format (pcm16 mono/stereo only)\n",path);
                goto fail;
            }
            fmt_found=true;
        }else if(memcmp(id,"data",4)==0){
            if(!fmt_found){ fprintf(stderr,"wav %s data before fmt\n",path); goto fail; }
            raw = malloc(chunk_size);
            if(!raw) die("oom");
            if(fread(raw,1,chunk_size,fp)!=chunk_size){ fprintf(stderr,"wav %s truncated data\n",path); goto fail; }
            raw_frames = chunk_size / (channels * (bits_per_sample/8));
            data_found=true;
        }else{
            fseek(fp, chunk_size, SEEK_CUR);
        }
        if(chunk_size & 1) fseek(fp,1,SEEK_CUR);
        if(data_found) break;
    }
    fclose(fp);
    if(!fmt_found || !data_found || !raw){
        fprintf(stderr,"wav %s missing chunks\n",path);
        free(raw);
        return false;
    }
    out->channels = channels;
    out->length = (int)raw_frames;
    out->sample_rate = (int)sample_rate;
    for(int c=0;c<channels;c++){
        out->chan[c]=calloc(raw_frames,sizeof(float));
        if(!out->chan[c]) die("oom");
    }
    const int16_t *src=(const int16_t*)raw;
    for(size_t i=0;i<raw_frames;i++){
        for(int c=0;c<channels;c++){
            int16_t v = src[i*channels + c];
            out->chan[c][i] = (float)v / 32768.f;
        }
    }
    if(channels==1) out->chan[1]=NULL;
    free(raw);
    return true;
fail:
    fclose(fp);
    free(raw);
    sample_data_free(out);
    return false;
}

static SampleData *sample_cache_get(const char *path){
    for(size_t i=0;i<sample_cache_len;i++){
        if(strcmp(sample_cache[i].path,path)==0) return &sample_cache[i].data;
    }
    SampleCacheEntry entry={0};
    entry.path=strdup(path);
    if(!entry.path) die("oom");
    if(!load_wav_file(path,&entry.data)){
        free(entry.path);
        return NULL;
    }
    if(sample_cache_len==sample_cache_cap){
        size_t n = sample_cache_cap?sample_cache_cap*2:8;
        SampleCacheEntry *tmp=realloc(sample_cache, n*sizeof(*tmp));
        if(!tmp) die("oom");
        sample_cache=tmp;
        sample_cache_cap=n;
    }
    sample_cache[sample_cache_len++] = entry;
    return &sample_cache[sample_cache_len-1].data;
}

static bool extract_note_symbol(const char *s, char *out, size_t out_sz){
    if(out_sz<3) return false;
    while(*s){
        unsigned char c=(unsigned char)*s;
        if(isalpha(c)){
            char note=(char)toupper(c);
            if(note<'A' || note>'G'){ s++; continue; }
            s++;
            char accidental='\0';
            if(*s=='#' || *s=='b' || *s=='B'){
                accidental = (*s=='#') ? '#' : 'B';
                s++;
            }
            if(isdigit((unsigned char)*s)){
                char oct=*s++;
                size_t idx=0;
                out[idx++]=note;
                if(accidental && idx<out_sz-1) out[idx++]=accidental;
                if(idx<out_sz-1) out[idx++]=oct;
                out[idx]='\0';
                return true;
            }
        }else if(*s=='_' || *s==' '){
            s++;
        }else{
            s++;
        }
    }
    return false;
}

static float hz_from_note(const char *name){
    if(!name) return 0.f;
    char buf[8];
    if(!extract_note_symbol(name, buf, sizeof(buf))) return 0.f;
    static const int base[7]={9,11,0,2,4,5,7}; // A,B,C,D,E,F,G mapping
    int noteIdx=-1;
    switch(buf[0]){
        case 'A': noteIdx=0; break;
        case 'B': noteIdx=1; break;
        case 'C': noteIdx=2; break;
        case 'D': noteIdx=3; break;
        case 'E': noteIdx=4; break;
        case 'F': noteIdx=5; break;
        case 'G': noteIdx=6; break;
        default: return 0.f;
    }
    int semi = base[noteIdx];
    int pos=1;
    if(buf[pos]=='#'){ semi+=1; pos++; }
    else if(buf[pos]=='B'){ semi-=1; pos++; }
    if(!isdigit((unsigned char)buf[pos])) return 0.f;
    int oct = buf[pos]-'0';
    int midi = (oct+1)*12 + semi;
    return 440.0f * powf(2.0f, (midi-69)/12.0f);
}

static bool parse_freq_value(const char *s, float *out){
    if(parse_float(s,out)) return true;
    float hz = hz_from_note(s);
    if(hz>0.f){ *out=hz; return true; }
    return false;
}

static bool parse_named_spec(const char *s, Spec *sp){
    if(!s || !*s) return false;
    char buf[128];
    size_t len=strlen(s);
    if(len>=sizeof(buf)) len=sizeof(buf)-1;
    memcpy(buf,s,len);
    buf[len]='\0';
    trim(buf);
    char *param=NULL;
    for(char *p=buf; *p; ++p){
        if(*p=='@' || *p=='='){
            *p='\0';
            param=p+1;
            trim(param);
            break;
        }
        if(*p=='('){
            *p='\0';
            param=p+1;
            char *end=strrchr(param,')');
            if(end) *end='\0';
            trim(param);
            break;
        }
    }
    if(strncasecmp(buf,"WAV",3)==0 || strcasecmp(buf,"SAMPLE")==0){
        if(!param || !*param) return false;
        char *path = dup_unquoted(param);
        if(!path) return false;
        SampleData *sd = sample_cache_get(path);
        free(path);
        if(!sd) return false;
        sp->type=SPEC_SAMPLE;
        sp->sample=sd;
        sp->sample_channel=0;
        return true;
    }
    if(strcasecmp(buf,"KICK")==0 || strcasecmp(buf,"BD")==0){
        float start=140.f, endf=40.f;
        if(param && *param){
            char tmp[128];
            strncpy(tmp,param,sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
            trim(tmp);
            char *arrow=strstr(tmp,"->");
            if(arrow){
                *arrow='\0';
                char *rhs=arrow+2;
                trim(tmp); trim(rhs);
                float a=0,b=0;
                if(parse_freq_value(tmp,&a)) start=a;
                if(parse_freq_value(rhs,&b)) endf=b;
            }else{
                float base=0;
                if(parse_freq_value(tmp,&base)){
                    start=base;
                    endf=fmaxf(20.f, base*0.4f);
                }
            }
        }
        sp->type=SPEC_KICK;
        sp->f0=start;
        sp->f1=endf;
        return true;
    }
    if(strcasecmp(buf,"SNARE")==0 || strcasecmp(buf,"SD")==0){
        float tone=200.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) tone=v;
        }
        sp->type=SPEC_SNARE;
        sp->f0=tone;
        return true;
    }
    if(strcasecmp(buf,"HAT")==0 || strcasecmp(buf,"HIHAT")==0 || strcasecmp(buf,"HH")==0){
        float bright=8000.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) bright=v;
        }
        sp->type=SPEC_HIHAT;
        sp->f0=bright;
        return true;
    }
    if(strncasecmp(buf,"BASS",4)==0 || strcasecmp(buf,"SUB")==0){
        float freq=55.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        else if(buf[4]=='\0'){
            // no suffix
        }else{
            const char *suffix = buf+4;
            if(*suffix){
                float v=0;
                if(parse_freq_value(suffix,&v) && v>0) freq=v;
            }
        }
        sp->type=SPEC_BASS;
        sp->f_const=freq;
        return true;
    }
    if(strncasecmp(buf,"FLUTE",5)==0){
        float freq=523.25f; // C5
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        sp->type=SPEC_FLUTE;
        sp->f_const=freq;
        return true;
    }
    if(strncasecmp(buf,"PIANO",5)==0){
        float freq=440.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        sp->type=SPEC_PIANO;
        sp->f_const=freq;
        return true;
    }
    if(strcasecmp(buf,"GUITAR")==0 || strcasecmp(buf,"GT")==0){
        float freq=330.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        sp->type=SPEC_GUITAR;
        sp->f_const=freq;
        return true;
    }
    if(strcasecmp(buf,"EGTR")==0 || strcasecmp(buf,"EGUITAR")==0){
        float freq=196.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        sp->type=SPEC_EGTR;
        sp->f_const=freq;
        return true;
    }
    if(strcasecmp(buf,"BIRDS")==0){
        sp->type=SPEC_BIRDS;
        sp->f_const = (param && *param) ? atof(param) : 6000.f;
        if(sp->f_const<=0.f) sp->f_const=6000.f;
        return true;
    }
    if(strcasecmp(buf,"STRPAD")==0 || strcasecmp(buf,"PAD")==0){
        float freq=440.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        sp->type=SPEC_STRPAD;
        sp->f_const=freq;
        return true;
    }
    if(strcasecmp(buf,"BELL")==0){
        float freq=880.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        sp->type=SPEC_BELL;
        sp->f_const=freq;
        return true;
    }
    if(strcasecmp(buf,"BRASS")==0){
        float freq=330.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        sp->type=SPEC_BRASS;
        sp->f_const=freq;
        return true;
    }
    if(strcasecmp(buf,"KALIMBA")==0 || strcasecmp(buf,"KORA")==0){
        float freq=392.f;
        if(param && *param){
            float v=0;
            if(parse_freq_value(param,&v) && v>0) freq=v;
        }
        sp->type=SPEC_KALIMBA;
        sp->f_const=freq;
        return true;
    }
    return false;
}

static Spec parse_spec(const char *s){
    Spec sp={.type=SPEC_CONST,.f_const=0,.sample=NULL,.sample_channel=0};
    if(!s || !*s){ sp.type=SPEC_SILENCE; return sp; }
    if ((s[0]=='r' || s[0]=='R') && (s[1]==0 || s[1]==':')){ sp.type=SPEC_SILENCE; return sp; }
    Spec named;
    if(parse_named_spec(s,&named)) return named;
    // contains '~' -> glide
    const char *tilde=strchr(s,'~');
    const char *plus=strchr(s,'+');
    if(tilde){
        char a[64], b[64];
        size_t la = (size_t)(tilde - s);
        if(la>=sizeof(a)) la=sizeof(a)-1;
        memcpy(a,s,la); a[la]='\0';
        strncpy(b,tilde+1,sizeof(b)-1); b[sizeof(b)-1]='\0';
        trim(a); trim(b);
        float f0=0,f1=0;
        if(!parse_freq_value(a,&f0) || !parse_freq_value(b,&f1)){ sp.type=SPEC_SILENCE; return sp; }
        sp.type=SPEC_GLIDE; sp.f0=f0; sp.f1=f1; return sp;
    }
    if(plus){ // chord: split by '+'
        sp.type=SPEC_CHORD; sp.n=0;
        const char *p=s; char tmp[64];
        while(*p && sp.n<16){
            const char *q=strchr(p,'+');
            size_t ln = q? (size_t)(q-p) : strlen(p);
            if(ln>=sizeof(tmp)) ln=sizeof(tmp)-1;
            memcpy(tmp,p,ln); tmp[ln]='\0'; trim(tmp);
            float f=0; if(parse_freq_value(tmp,&f)) sp.chord[sp.n++]=f;
            if(!q) break; p=q+1;
        }
        if(sp.n==0) { sp.type=SPEC_SILENCE; }
        return sp;
    }
    // const freq or 0 (rest)
    float f=0;
    if(!parse_freq_value(s,&f) || f<=0){ sp.type=SPEC_SILENCE; }
    else { sp.type=SPEC_CONST; sp.f_const=f; }
    return sp;
}

static bool parse_int_strict(const char *s, int *out){
    if(!s || !*s) return false;
    char *end=NULL;
    long v = strtol(s, &end, 10);
    if(end==s || *end!='\0') return false;
    if(v<INT_MIN || v>INT_MAX) return false;
    *out = (int)v;
    return true;
}

static int samples_from_ms(int ms, int sr){
    if(ms<=0) return 0;
    int64_t prod = (int64_t)sr * ms;
    int samples = (int)(prod / 1000);
    if(samples > INT_MAX) samples = INT_MAX;
    return samples;
}

static int ms_to_samples(int ms, int sr){
    int samples = samples_from_ms(ms, sr);
    if(samples < 1) samples = 1;
    return samples;
}

static int ms_to_samples_allow_zero(int ms, int sr){
    int samples = samples_from_ms(ms, sr);
    if(samples < 0) samples = 0;
    return samples;
}

static int sample_default_length(const Spec *sp, int sr){
    if(!sp || sp->type!=SPEC_SAMPLE || !sp->sample || sp->sample->sample_rate<=0) return 0;
    double seconds = (double)sp->sample->length / (double)sp->sample->sample_rate;
    int n = (int)lrint(seconds * (double)sr);
    if(n<1) n=1;
    return n;
}

static int token_target_samples(const Token *tok, int sr){
    if(tok->sample_override){
        int n = sample_default_length(&tok->L, sr);
        if(n<=0) n = sample_default_length(&tok->R, sr);
        if(n>0) return n;
    }
    return ms_to_samples(tok->dur_ms, sr);
}

static bool parse_token(const char *arg, int def_ms, Token *out){
    char *dup=strdup(arg); if(!dup) return false;
    char *col=strrchr(dup,':'); // last ':' as duration sep
    int dur = def_ms;
    bool explicit_dur=false;
    if(col){
        *col='\0';
        char *d=col+1;
        trim(d);
        if(parse_int_strict(d, &dur) && dur>0){
            explicit_dur=true;
        }else{
            dur = def_ms;
        }
    }
    char *body=dup; trim(body);
    // rest token can be "r:ms" or "0:ms"
    if( (body[0]=='r'||body[0]=='R'||body[0]=='0') && (body[1]==0) ){
        out->L.type=SPEC_SILENCE; out->R.type=SPEC_SILENCE; out->stereo=false; out->dur_ms=dur; out->samples=0; out->explicit_dur=explicit_dur; out->sample_override=false; free(dup); return true;
    }
    // stereo if comma present at top-level
    char *comma = NULL;
    int depth=0;
    for(char *p=body; *p; ++p){ if(*p==','){ comma=p; break; } }
    if(comma){
        *comma='\0'; char *ls=body; char *rs=comma+1; trim(ls); trim(rs);
        out->L = parse_spec(ls);
        out->R = parse_spec(rs);
        out->stereo=true;
    }else{
        out->L = parse_spec(body);
        out->R = out->L;
        out->stereo=false;
        if(out->L.type==SPEC_SAMPLE && out->L.sample){
            if(out->L.sample->channels>1){
                out->stereo=true;
                out->L.sample_channel=0;
                out->R.sample_channel=1;
            }else{
                out->L.sample_channel=0;
                out->R.sample_channel=0;
            }
        }
    }
    out->dur_ms = dur;
    out->samples = 0;
    out->explicit_dur = explicit_dur;
    out->sample_override = (!explicit_dur) && ((out->L.type==SPEC_SAMPLE && out->L.sample) || (out->R.type==SPEC_SAMPLE && out->R.sample));
    free(dup); return true;
}

static char *dup_trimmed(const char *s){
    if(!s) return NULL;
    size_t len=strlen(s);
    char *cp=malloc(len+1);
    if(!cp) die("oom");
    memcpy(cp,s,len+1);
    trim(cp);
    return cp;
}

static void csv_row_free(CsvRow *row){
    if(!row) return;
    for(int i=0;i<5;i++){
        free(row->cols[i]);
        row->cols[i]=NULL;
    }
}

static CsvRow csv_row_clone(const CsvRow *src){
    CsvRow r={{0}};
    for(int i=0;i<5;i++){
        if(src->cols[i]) r.cols[i]=dup_trimmed(src->cols[i]);
    }
    return r;
}

static void csv_rowvec_push(CsvRowVec *vec, CsvRow row){
    if(vec->len==vec->cap){
        size_t n = vec->cap?vec->cap*2:32;
        CsvRow *tmp = realloc(vec->items, n*sizeof(CsvRow));
        if(!tmp) die("oom");
        vec->items=tmp;
        vec->cap=n;
    }
    vec->items[vec->len++] = row;
}

static void csv_rowvec_free(CsvRowVec *vec){
    if(!vec) return;
    for(size_t i=0;i<vec->len;i++) csv_row_free(&vec->items[i]);
    free(vec->items);
    vec->items=NULL;
    vec->len=vec->cap=0;
}

static void macro_def_free(MacroDef *def){
    if(!def) return;
    free(def->name);
    csv_rowvec_free(&def->rows);
}

static void macro_vec_push(MacroVec *vec, MacroDef def){
    if(vec->len==vec->cap){
        size_t n = vec->cap?vec->cap*2:8;
        MacroDef *tmp=realloc(vec->items, n*sizeof(MacroDef));
        if(!tmp) die("oom");
        vec->items=tmp;
        vec->cap=n;
    }
    vec->items[vec->len++] = def;
}

static void macro_vec_free(MacroVec *vec){
    if(!vec) return;
    for(size_t i=0;i<vec->len;i++) macro_def_free(&vec->items[i]);
    free(vec->items);
    vec->items=NULL;
    vec->len=vec->cap=0;
}

static MacroDef *macro_find(MacroVec *vec, const char *name){
    if(!name) return NULL;
    char *upper=strdup_upper(name);
    if(!upper) return NULL;
    MacroDef *result=NULL;
    for(size_t i=0;i<vec->len;i++){
        if(vec->items[i].name && strcasecmp(vec->items[i].name, upper)==0){
            result=&vec->items[i];
            break;
        }
    }
    free(upper);
    return result;
}

static char *collect_field(const char *start, size_t len){
    char *buf=malloc(len+1);
    if(!buf) die("oom");
    memcpy(buf,start,len);
    buf[len]='\0';
    trim(buf);
    return buf;
}

static bool parse_csv_line(const char *line, CsvRow *row){
    memset(row,0,sizeof(*row));
    if(!line) return false;
    const char *p=line;
    int col=0;
    while(*p && col<5){
        while(*p==' '||*p=='\t'||*p=='\r') p++;
        if(*p=='\0' || *p=='\n') break;
        if(*p==','){
            row->cols[col++]=dup_trimmed("");
            p++;
            continue;
        }
        bool quoted=false;
        const char *field_start=p;
        char *field=NULL;
        size_t alloc=64, len=0;
        char *buf=malloc(alloc);
        if(!buf) die("oom");
        if(*p=='"'){
            quoted=true;
            p++;
            while(*p){
                if(*p=='"'){
                    if(p[1]=='"'){ // escaped quote
                        if(len+1>=alloc){ alloc*=2; buf=realloc(buf,alloc); if(!buf) die("oom"); }
                        buf[len++]='"';
                        p+=2;
                        continue;
                    }else{
                        p++;
                        break;
                    }
                }
                if(len+1>=alloc){ alloc*=2; buf=realloc(buf,alloc); if(!buf) die("oom"); }
                buf[len++]=*p++;
            }
            buf[len]='\0';
            trim(buf);
            field=buf;
        }else{
            const char *start=p;
            while(*p && *p!=',' && *p!='\n' && *p!='\r') p++;
            size_t l=(size_t)(p-start);
            field=collect_field(start,l);
            free(buf);
        }
        row->cols[col++] = field;
        while(*p==' '||*p=='\t') p++;
        if(*p==','){ p++; continue; }
        while(*p && *p!='\n'){ if(*p==','){ p++; break; } p++; }
    }
    return col>0;
}

static bool load_sequence_rows(const char *path, CsvRowVec *rows, MacroVec *macros){
    FILE *fp=fopen(path,"r");
    if(!fp){
        fprintf(stderr,"konnte %s nicht öffnen: %s\n", path, strerror(errno));
        return false;
    }
    char *line=NULL;
    size_t cap=0;
    while(getline(&line,&cap,fp)!=-1){
        char *trim_line = dup_trimmed(line);
        if(!trim_line) continue;
        if(trim_line[0]=='\0' || trim_line[0]=='#' || strncmp(trim_line,"//",2)==0 || strncmp(trim_line,"--",2)==0){
            free(trim_line);
            continue;
        }
        if(trim_line[0]=='@'){
            char *brace=strchr(trim_line,'{');
            if(brace){
                *brace='\0';
                char *name=trim_line+1;
                trim(name);
                MacroDef def={0};
                def.name=strdup_upper(name);
                if(!def.name) die("oom");
                bool closed=false;
                while(getline(&line,&cap,fp)!=-1){
                    char *inner=dup_trimmed(line);
                    if(!inner) continue;
                    if(inner[0]=='\0' || inner[0]=='#' || strncmp(inner,"//",2)==0 || strncmp(inner,"--",2)==0){
                        free(inner);
                        continue;
                    }
                    if(inner[0]=='}'){
                        free(inner);
                        closed=true;
                        break;
                    }
                    CsvRow row;
                    if(parse_csv_line(line,&row)){
                        if(row.cols[0] && row.cols[0][0]!='\0'){
                            csv_rowvec_push(&def.rows, row);
                        }else{
                            csv_row_free(&row);
                        }
                    }
                    free(inner);
                }
                if(!closed){
                    fprintf(stderr,"macro %s missing closing brace in %s\n", name, path);
                    macro_def_free(&def);
                }else{
                    macro_vec_push(macros, def);
                }
                free(trim_line);
                continue;
            }
        }
        free(trim_line);
        CsvRow row;
        if(!parse_csv_line(line,&row)){
            continue;
        }
        if(!row.cols[0] || row.cols[0][0]=='\0' || row.cols[0][0]=='#'){
            csv_row_free(&row);
            continue;
        }
        csv_rowvec_push(rows, row);
    }
    free(line);
    fclose(fp);
    return true;
}

static bool row_is_repeat_marker(const CsvRow *row, int *span, int *reps){
    if(!row->cols[0] || row->cols[0][0]!='-') return false;
    int val=0;
    if(!parse_int_strict(row->cols[0], &val) || val>=0) return false;
    if(!row->cols[1]) return false;
    int rep=0;
    if(!parse_int_strict(row->cols[1], &rep) || rep<=0) return false;
    *span = -val;
    *reps = rep;
    return true;
}

static bool expand_repeats(const CsvRowVec *src, CsvRowVec *dst){
    size_t i=0;
    while(i<src->len){
        int span=0,reps=0;
        const CsvRow *row=&src->items[i];
        if(row_is_repeat_marker(row,&span,&reps)){
            if(span<=0 || reps<=0){ i++; continue; }
            // prefer forward-looking blocks; fallback to previous block if no future rows
            size_t start = i+1;
            size_t remaining = (start<src->len)? (src->len - start) : 0;
            if(remaining >= (size_t)span){
                CsvRowVec block={0}, block_expanded={0};
                for(size_t k=0;k<(size_t)span;k++){
                    csv_rowvec_push(&block, csv_row_clone(&src->items[start+k]));
                }
                expand_repeats(&block, &block_expanded);
                for(int r=0;r<reps;r++){
                    for(size_t k=0;k<block_expanded.len;k++){
                        csv_rowvec_push(dst, csv_row_clone(&block_expanded.items[k]));
                    }
                }
                csv_rowvec_free(&block_expanded);
                csv_rowvec_free(&block);
                i = start + (size_t)span;
                continue;
            }else if(dst->len>0){
                if((size_t)span>dst->len) span=(int)dst->len;
                size_t base = dst->len - (size_t)span;
                for(int r=0;r<reps;r++){
                    for(size_t k=0;k<(size_t)span;k++){
                        csv_rowvec_push(dst, csv_row_clone(&dst->items[base+k]));
                    }
                }
            }
            i++;
            continue;
        }
        csv_rowvec_push(dst, csv_row_clone(row));
        i++;
    }
    return true;
}

static bool expand_macro_rows(const MacroDef *macro, MacroVec *macros, CsvRowVec *dst, int depth){
    if(depth>16){
        fprintf(stderr,"macro recursion too deep for %s\n", macro->name);
        return false;
    }
    for(size_t i=0;i<macro->rows.len;i++){
        const CsvRow *row=&macro->rows.items[i];
        const char *tok=row->cols[0];
        if(tok && tok[0]=='@' && tok[1]!='\0'){
            MacroDef *inner = macro_find(macros, tok+1);
            if(!inner){
                fprintf(stderr,"unknown macro reference %s inside %s\n", tok, macro->name);
                return false;
            }
            if(!expand_macro_rows(inner, macros, dst, depth+1)) return false;
            continue;
        }
        csv_rowvec_push(dst, csv_row_clone(row));
    }
    return true;
}

static bool expand_macros(const CsvRowVec *src, MacroVec *macros, CsvRowVec *dst){
    for(size_t i=0;i<src->len;i++){
        const CsvRow *row=&src->items[i];
        const char *tok=row->cols[0];
        if(tok && tok[0]=='@' && tok[1]!='\0'){
            MacroDef *macro = macro_find(macros, tok+1);
            if(macro){
                if(!expand_macro_rows(macro, macros, dst, 1)) return false;
                continue;
            }
        }
        csv_rowvec_push(dst, csv_row_clone(row));
    }
    return true;
}

static int parse_gap_ms(const char *s){
    if(!s || !*s) return 0;
    if(strchr(s,'.')){
        float secs=0.f;
        if(parse_float(s,&secs)){
            int ms=(int)lrintf(secs*1000.f);
            if(ms<0) ms=0;
            return ms;
        }
    }
    int val=0;
    if(parse_int_strict(s,&val) && val>0) return val;
    return 0;
}

static char *extract_mode_token(const char *mode_in, bool *is_bg, bool *adv){
    if(!mode_in || !*mode_in) return NULL;
    char *tmp=strdup(mode_in);
    if(!tmp) die("oom");
    char *save=NULL;
    char *part=strtok_r(tmp,"|",&save);
    char *result=NULL;
    while(part){
        trim(part);
        if(*part!='\0'){
            if(strcasecmp(part,"BG")==0){ *is_bg=true; }
            else if(strcasecmp(part,"ADV")==0){ *adv=true; }
            else{
                if(!result) result=dup_trimmed(part);
                else {
                    size_t len=strlen(result);
                    size_t add=strlen(part);
                    char *n=realloc(result, len+add+2);
                    if(!n) die("oom");
                    result=n;
                    result[len]='|';
                    memcpy(result+len+1, part, add+1);
                }
            }
        }
        part=strtok_r(NULL,"|",&save);
    }
    free(tmp);
    return result;
}

static void parse_flag_string(const char *flags, bool *is_bg, bool *adv){
    if(!flags) return;
    char *tmp=strdup(flags);
    if(!tmp) die("oom");
    char *save=NULL;
    char *part=strtok_r(tmp,",|",&save);
    while(part){
        trim(part);
        if(*part!='\0'){
            if(strcasecmp(part,"BG")==0) *is_bg=true;
            else if(strcasecmp(part,"ADV")==0) *adv=true;
        }
        part=strtok_r(NULL,",|",&save);
    }
    free(tmp);
}

static void render_vec_push(RenderVec *vec, RenderEvent ev){
    if(vec->len==vec->cap){
        size_t n = vec->cap?vec->cap*2:64;
        RenderEvent *tmp = realloc(vec->items, n*sizeof(RenderEvent));
        if(!tmp) die("oom");
        vec->items=tmp;
        vec->cap=n;
    }
    vec->items[vec->len++] = ev;
}

static void render_vec_free(RenderVec *vec){
    if(!vec) return;
    free(vec->items);
    vec->items=NULL;
    vec->len=vec->cap=0;
}

static void speech_event_add_arg(SpeechEvent *ev, const char *arg){
    if(!arg || !*arg) return;
    char *dup=strdup(arg);
    if(!dup) die("oom");
    char **tmp=realloc(ev->args, sizeof(char*)*(ev->arg_count+1));
    if(!tmp) die("oom");
    ev->args=tmp;
    ev->args[ev->arg_count++]=dup;
}

static void speech_event_add_arg_pair(SpeechEvent *ev, const char *flag, const char *value){
    if(flag) speech_event_add_arg(ev, flag);
    if(value) speech_event_add_arg(ev, value);
}

static void speech_event_free(SpeechEvent *ev){
    if(!ev) return;
    free(ev->voice);
    free(ev->text);
    for(int i=0;i<ev->arg_count;i++) free(ev->args[i]);
    free(ev->args);
    ev->args=NULL;
    ev->arg_count=0;
}

static void speech_vec_push(SpeechVec *vec, SpeechEvent ev){
    if(vec->len==vec->cap){
        size_t n=vec->cap?vec->cap*2:16;
        SpeechEvent *tmp=realloc(vec->items, n*sizeof(SpeechEvent));
        if(!tmp) die("oom");
        vec->items=tmp;
        vec->cap=n;
    }
    vec->items[vec->len++]=ev;
}

static void speech_vec_free(SpeechVec *vec){
    if(!vec) return;
    for(size_t i=0;i<vec->len;i++) speech_event_free(&vec->items[i]);
    free(vec->items);
    vec->items=NULL;
    vec->len=vec->cap=0;
}

static void parse_say_options(const char *optstr, SpeechEvent *ev){
    if(!optstr || !*optstr) return;
    char *tmp=strdup(optstr);
    if(!tmp) die("oom");
    char *save=NULL;
    char *part=strtok_r(tmp,";",&save);
    while(part){
        trim(part);
        if(*part!='\0'){
            char *eq=strchr(part,'=');
            char *key=part;
            char *val=NULL;
            if(eq){
                *eq='\0';
                val=eq+1;
                trim(key); trim(val);
            }else{
                trim(key);
            }
            char lower[16]={0};
            size_t klen=strlen(key);
            for(size_t i=0;i<klen && i<sizeof(lower)-1;i++) lower[i]=(char)tolower((unsigned char)key[i]);
            const char *value = val && *val ? val : "1";
            if(strcmp(lower,"variant")==0){
                if(ev->voice && *value){
                    size_t len=strlen(ev->voice)+strlen(value)+1;
                    char *nv=malloc(len+1);
                    if(!nv) die("oom");
                    snprintf(nv,len+1,"%s%s",ev->voice,value);
                    free(ev->voice);
                    ev->voice=nv;
                }
            }else if(strcmp(lower,"s")==0){
                speech_event_add_arg_pair(ev,"-s",value);
            }else if(strcmp(lower,"p")==0){
                speech_event_add_arg_pair(ev,"-p",value);
            }else if(strcmp(lower,"a")==0){
                speech_event_add_arg_pair(ev,"-a",value);
            }else if(strcmp(lower,"g")==0){
                speech_event_add_arg_pair(ev,"-g",value);
            }else if(strcmp(lower,"k")==0){
                speech_event_add_arg_pair(ev,"-k",value);
            }else if(strcmp(lower,"punct")==0){
                if(strcmp(value,"1")==0) speech_event_add_arg(ev,"--punct");
                else{
                    char buf[64];
                    snprintf(buf,sizeof(buf),"--punct=%s",value);
                    speech_event_add_arg(ev,buf);
                }
            }else if(strcmp(lower,"ipa")==0){
                if(strcmp(value,"0")!=0) speech_event_add_arg(ev,"--ipa");
            }else if(strcmp(key,"X")==0){
                if(strcmp(value,"0")!=0) speech_event_add_arg(ev,"-X");
            }else if(strcmp(lower,"x")==0){
                if(strcmp(value,"0")!=0) speech_event_add_arg(ev,"-x");
            }else if(strcmp(lower,"m")==0){
                if(strcmp(value,"0")!=0) speech_event_add_arg(ev,"-m");
            }else if(strcmp(lower,"z")==0){
                if(strcmp(value,"0")!=0) speech_event_add_arg(ev,"-z");
            }else if(strcmp(lower,"q")==0){
                if(strcmp(value,"0")!=0) speech_event_add_arg(ev,"-q");
            }else if(strcmp(lower,"stdout")==0){
                if(strcmp(value,"0")!=0) speech_event_add_arg(ev,"--stdout");
            }else if(strcmp(lower,"w")==0){
                speech_event_add_arg_pair(ev,"-w",value);
            }else if(strcmp(lower,"path")==0){
                speech_event_add_arg_pair(ev,"--path",value);
            }
        }
        part=strtok_r(NULL,";",&save);
    }
    free(tmp);
}

static int64_t samples_to_ms(size_t samples, int sr){
    return (int64_t)(((long double)samples * 1000.0L) / (long double)sr);
}

static bool parse_say_event(const char *token, size_t start_samples, int sr, SpeechVec *speech){
    if(!token || strncasecmp(token,"SAY",3)!=0) return false;
    const char *p=token+3;
    while(*p==' '||*p=='\t') p++;
    char *voice=NULL;
    char *opts=NULL;
    if(*p=='@'){
        p++;
        const char *vstart=p;
        while(*p && *p!=':' && *p!=';') p++;
        voice=collect_field(vstart, (size_t)(p-vstart));
    }
    if(*p==';'){
        p++;
        const char *ostart=p;
        while(*p && *p!=':') p++;
        opts=collect_field(ostart,(size_t)(p-ostart));
    }
    if(*p==':') p++;
    const char *tstart=p;
    char *text=dup_trimmed(tstart);
    if(!text || text[0]=='\0'){
        free(voice);
        free(opts);
        free(text);
        return true;
    }
    SpeechEvent ev={0};
    ev.start_ms = samples_to_ms(start_samples, sr);
    ev.voice = voice;
    ev.text = text;
    parse_say_options(opts, &ev);
    speech_vec_push(speech, ev);
    free(opts);
    return true;
}

static void set_const_spec(Spec *sp, float freq){
    if(freq<=0.f){
        sp->type=SPEC_SILENCE;
        sp->f_const=0.f;
    }else{
        sp->type=SPEC_CONST;
        sp->f_const=freq;
    }
}

static void apply_mode_to_token(Token *tok, const char *mode){
    if(!mode || !*mode) return;
    const char *body=mode;
    float base = (tok->L.type==SPEC_CONST) ? tok->L.f_const : 0.f;
    if(strncasecmp(body,"GLIDE:",6)==0){
        body += 6;
        const char *arrow=strstr(body,"->");
        if(!arrow) return;
        char *left=collect_field(body,(size_t)(arrow-body));
        char *right=dup_trimmed(arrow+2);
        float f0=0,f1=0;
        if(parse_freq_value(left,&f0) && parse_freq_value(right,&f1)){
            tok->L.type=SPEC_GLIDE; tok->L.f0=f0; tok->L.f1=f1;
            tok->R=tok->L; tok->stereo=false;
        }
        free(left); free(right);
        return;
    }
    if(strncasecmp(body,"UPTO:",5)==0 && base>0.f){
        float target=0.f;
        if(parse_freq_value(body+5,&target)){
            tok->L.type=SPEC_GLIDE; tok->L.f0=base; tok->L.f1=target;
            tok->R=tok->L; tok->stereo=false;
        }
        return;
    }
    if(strncasecmp(body,"DOWNTO:",7)==0 && base>0.f){
        float target=0.f;
        if(parse_freq_value(body+7,&target)){
            tok->L.type=SPEC_GLIDE; tok->L.f0=base; tok->L.f1=target;
            tok->R=tok->L; tok->stereo=false;
        }
        return;
    }
    if(strncasecmp(body,"UPx:",4)==0 && base>0.f){
        float ratio=0.f;
        if(parse_float(body+4,&ratio) && ratio>0.f){
            tok->L.type=SPEC_GLIDE; tok->L.f0=base; tok->L.f1=base*ratio;
            tok->R=tok->L; tok->stereo=false;
        }
        return;
    }
    if(strncasecmp(body,"DOWNx:",6)==0 && base>0.f){
        float ratio=0.f;
        if(parse_float(body+6,&ratio) && ratio>0.f){
            tok->L.type=SPEC_GLIDE; tok->L.f0=base; tok->L.f1=base/ratio;
            tok->R=tok->L; tok->stereo=false;
        }
        return;
    }
    if(strncasecmp(body,"BINAURAL:",9)==0 && base>0.f){
        float delta=0.f;
        if(parse_float(body+9,&delta)){
            tok->stereo=true;
            set_const_spec(&tok->R, base + delta);
        }
        return;
    }
    float right=0.f;
    if(parse_freq_value(body,&right) && right>0.f){
        tok->stereo=true;
        set_const_spec(&tok->R, right);
    }
}

static bool build_sequence_events(const CsvRowVec *rows, int sr, int def_ms, RenderVec *tones, SpeechVec *speech, size_t *total_samples, int *max_event_samples){
    size_t timeline_samples=0;
    size_t max_end=0;
    int max_ev=0;
    for(size_t i=0;i<rows->len;i++){
        const CsvRow *row=&rows->items[i];
        const char *tok = row->cols[0] ? row->cols[0] : "";
        if(!tok || !*tok) continue;
        bool is_bg=false, adv=false;
        char *mode_clean = extract_mode_token(row->cols[3], &is_bg, &adv);
        parse_flag_string(row->cols[4], &is_bg, &adv);
        int dur_ms = def_ms;
        if(row->cols[1] && *row->cols[1]){
            if(parse_int_strict(row->cols[1], &dur_ms)==false || dur_ms<=0) dur_ms=def_ms;
        }
        int gap_ms = parse_gap_ms(row->cols[2]);
        int gap_samples = ms_to_samples_allow_zero(gap_ms, sr);
        size_t event_start = timeline_samples;
        if(parse_say_event(tok, timeline_samples, sr, speech)){
            bool advance = (!is_bg || adv);
            if(advance){
                int say_samples = ms_to_samples_allow_zero((dur_ms>0)?dur_ms:def_ms, sr);
                timeline_samples += say_samples;
                timeline_samples += gap_samples;
            }
            free(mode_clean);
            continue;
        }
        int effective_ms = (dur_ms>0)?dur_ms:def_ms;
        size_t need = strlen(tok)+32;
        char *token_with_dur = malloc(need);
        if(!token_with_dur) die("oom");
        snprintf(token_with_dur, need, "%s:%d", tok, effective_ms);
        Token t;
        if(!parse_token(token_with_dur, def_ms, &t)){
            free(token_with_dur);
            free(mode_clean);
            fprintf(stderr,"token parse error in sequence: %s\n", tok);
            return false;
        }
        free(token_with_dur);
        apply_mode_to_token(&t, mode_clean);
        int tone_samples = token_target_samples(&t, sr);
        bool advance = (!is_bg || adv);
        if(t.L.type==SPEC_SILENCE && t.R.type==SPEC_SILENCE){
            if(advance){
                int rest_samples = ms_to_samples_allow_zero(effective_ms, sr);
                timeline_samples += rest_samples;
                timeline_samples += gap_samples;
            }
            free(mode_clean);
            continue;
        }
        RenderEvent ev={0};
        ev.start = event_start;
        ev.samples = tone_samples;
        ev.L = t.L;
        ev.R = t.R;
        render_vec_push(tones, ev);
        if(tone_samples>max_ev) max_ev=tone_samples;
        size_t end = ev.start + (size_t)tone_samples;
        if(end>max_end) max_end=end;
        if(advance){
            timeline_samples += tone_samples;
            timeline_samples += gap_samples;
        }
        free(mode_clean);
    }
    if(max_end < timeline_samples) max_end = timeline_samples;
    *total_samples = max_end;
    *max_event_samples = max_ev;
    return true;
}

static int64_t now_ms(void){
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (int64_t)tv.tv_sec*1000 + tv.tv_usec/1000;
}

static void launch_espeak_event(const SpeechEvent *ev, const char *espeak_bin){
    if(!ev || !ev->text || !*ev->text) return;
    if(!espeak_bin || !*espeak_bin) return;
    pid_t pid=fork();
    if(pid<0) return;
    if(pid==0){
        int base = 1 + (ev->voice?2:0) + ev->arg_count + 1;
        char **argv=calloc((size_t)base+1, sizeof(char*));
        if(!argv) _exit(1);
        int idx=0;
        argv[idx++]=(char*)espeak_bin;
        if(ev->voice && *ev->voice){
            argv[idx++]="-v";
            argv[idx++]=ev->voice;
        }
        for(int i=0;i<ev->arg_count;i++) argv[idx++]=ev->args[i];
        argv[idx++]=ev->text;
        argv[idx]=NULL;
        execvp(espeak_bin, argv);
        _exit(127);
    }
}

static int play_sequence_file(const char *path, int sr, int def_ms, int fade_ms, float gain, const char *espeak_bin){
    CsvRowVec raw={0}, macro_applied={0}, expanded={0};
    MacroVec macros={0};
    if(!load_sequence_rows(path,&raw,&macros)){
        macro_vec_free(&macros);
        return 1;
    }
    if(!expand_macros(&raw,&macros,&macro_applied)){
        csv_rowvec_free(&raw);
        macro_vec_free(&macros);
        csv_rowvec_free(&macro_applied);
        return 1;
    }
    expand_repeats(&macro_applied,&expanded);
    csv_rowvec_free(&raw);
    csv_rowvec_free(&macro_applied);
    macro_vec_free(&macros);

    RenderVec tones={0};
    SpeechVec speech={0};
    size_t total_samples=0;
    int max_ev=0;
    if(!build_sequence_events(&expanded, sr, def_ms, &tones, &speech, &total_samples, &max_ev)){
        csv_rowvec_free(&expanded);
        render_vec_free(&tones);
        speech_vec_free(&speech);
        return 1;
    }
    csv_rowvec_free(&expanded);
    if(total_samples==0){
        render_vec_free(&tones);
        speech_vec_free(&speech);
        die("no audio to render");
    }
    if((size_t)max_ev==0) max_ev=1;
    float *L = calloc(total_samples, sizeof(float));
    float *R = calloc(total_samples, sizeof(float));
    float *tmpL = malloc((size_t)max_ev * sizeof(float));
    float *tmpR = malloc((size_t)max_ev * sizeof(float));
    if(!L||!R||!tmpL||!tmpR) die("oom");

    for(size_t i=0;i<tones.len;i++){
        RenderEvent *ev=&tones.items[i];
        if(ev->samples<=0) continue;
        synth_spec_into(&ev->L, tmpL, ev->samples, sr);
        synth_spec_into(&ev->R, tmpR, ev->samples, sr);
        apply_fade(tmpL, ev->samples, sr, fade_ms);
        apply_fade(tmpR, ev->samples, sr, fade_ms);
        for(int n=0;n<ev->samples;n++){
            size_t idx = ev->start + (size_t)n;
            if(idx>=total_samples) break;
            L[idx] += tmpL[n];
            R[idx] += tmpR[n];
        }
    }

    int rc = play_buffer_with_openal(L, R, total_samples, gain, sr, &speech, espeak_bin);

    free(tmpL); free(tmpR);
    free(L); free(R);
    render_vec_free(&tones);
    speech_vec_free(&speech);
    return rc;
}

static inline float sine(float ph){ return sinf(2.f*(float)M_PI*ph); }

static void synth_kick_into(float *dst, int n, int sr, float startHz, float endHz){
    if(startHz<=0.f) startHz=140.f;
    if(endHz<=0.f) endHz=40.f;
    float phase=0.f;
    for(int i=0;i<n;i++){
        float u = (float)i / (float)(n>1?n-1:1);
        float freq = startHz + (endHz - startHz) * u;
        if(freq < 10.f) freq = 10.f;
        phase += freq / (float)sr;
        if(phase>1.f) phase -= floorf(phase);
        float env = expf(-6.f*u);
        float body = sine(phase);
        float click = (i < sr/500 ? (1.f - (float)i / fmaxf(1.f, (float)(sr/500))) * 0.5f : 0.f);
        dst[i] = body * env * 1.3f + click;
    }
}

static void synth_snare_into(float *dst, int n, int sr, float toneHz){
    if(toneHz<=0.f) toneHz=200.f;
    float phase=0.f;
    float lp=0.f;
    for(int i=0;i<n;i++){
        float u = (float)i / (float)(n>1?n-1:1);
        float env = expf(-8.f*u);
        float noise = frand_unit();
        lp = lp + 0.2f*(noise - lp);
        float whiten = noise - lp;
        phase += toneHz / (float)sr;
        if(phase>1.f) phase -= floorf(phase);
        float ring = sine(phase);
        dst[i] = (whiten*0.85f + ring*0.35f) * env;
    }
}

static void synth_hat_into(float *dst, int n, int sr, float brightHz){
    if(brightHz<=0.f) brightHz=8000.f;
    float lp=0.f;
    float phase1=0.f, phase2=0.f;
    for(int i=0;i<n;i++){
        float u = (float)i / (float)(n>1?n-1:1);
        float env = expf(-16.f*u);
        float noise = frand_unit();
        lp = lp + 0.3f*(noise - lp);
        float hp = noise - lp;
        phase1 += brightHz / (float)sr;
        phase2 += (brightHz*1.5f) / (float)sr;
        if(phase1>1.f) phase1 -= floorf(phase1);
        if(phase2>1.f) phase2 -= floorf(phase2);
        float metal = (sine(phase1) + sine(phase2)) * 0.3f;
        dst[i] = (hp*0.8f + metal) * env;
    }
}

static void synth_bass_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=55.f;
    float phase=0.f, sub_phase=0.f, filt=0.f;
    int attack = (int)(0.005f * sr);
    if(attack<1) attack=1;
    int release = (int)(0.03f * sr);
    if(release<1) release=1;
    for(int i=0;i<n;i++){
        phase += freq / (float)sr;
        if(phase>=1.f) phase-=1.f;
        sub_phase += (freq*0.5f) / (float)sr;
        if(sub_phase>=1.f) sub_phase-=1.f;
        float saw = 2.f*phase - 1.f;
        float sub = sine(sub_phase);
        float mix = saw*0.7f + sub*0.3f;
        filt += 0.08f * (mix - filt);
        float env=1.f;
        if(i<attack) env = (float)i / (float)attack;
        else if(i > n - release){
            int rem = n - i;
            if(rem<0) rem=0;
            env = (float)rem / (float)release;
        }
        if(env<0.f) env=0.f;
        dst[i] = filt * env;
    }
}

static void synth_flute_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=523.25f;
    float phase=0.f, phase2=0.f;
    float breath=0.f;
    int attack = (int)(0.01f*sr);
    if(attack<1) attack=1;
    int release = (int)(0.08f*sr);
    if(release<1) release=1;
    for(int i=0;i<n;i++){
        phase += freq/(float)sr;
        phase2 += (freq*2.f)/(float)sr;
        if(phase>1.f) phase -= floorf(phase);
        if(phase2>1.f) phase2 -= floorf(phase2);
        float tone = 0.85f*sine(phase) + 0.15f*sine(phase2);
        breath += 0.25f*(frand_unit()-breath);
        float t_env;
        if(i<attack) t_env = (float)i/(float)attack;
        else if(i>n-release){
            int remain = n-i;
            t_env = remain>0 ? (float)remain/(float)release : 0.f;
        }else{
            float u=(float)i/(float)(n>1?n-1:1);
            t_env = expf(-1.8f*u);
        }
        dst[i] = (tone + 0.1f*breath)*t_env;
    }
}

static void synth_piano_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=440.f;
    float phase1=0.f, phase2=0.f, phase3=0.f;
    for(int i=0;i<n;i++){
        float t=(float)i/(float)sr;
        float env1=expf(-3.5f*t);
        float env2=expf(-6.0f*t);
        float env3=expf(-9.0f*t);
        phase1 += freq/(float)sr;
        phase2 += (freq*2.01f)/(float)sr;
        phase3 += (freq*3.01f)/(float)sr;
        if(phase1>1.f) phase1 -= floorf(phase1);
        if(phase2>1.f) phase2 -= floorf(phase2);
        if(phase3>1.f) phase3 -= floorf(phase3);
        float sample = 0.7f*sine(phase1)*env1 +
                       0.25f*sine(phase2)*env2 +
                       0.15f*sine(phase3)*env3;
        dst[i]=sample;
    }
}

static void synth_guitar_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=330.f;
    int period = (int)((float)sr / freq);
    if(period<2) period=2;
    float *buf = malloc((size_t)period * sizeof(float));
    if(!buf){ memset(dst,0,n*sizeof(float)); return; }
    for(int k=0;k<period;k++) buf[k]=frand_unit();
    int idx=0;
    for(int i=0;i<n;i++){
        int next = (idx+1)%period;
        float value = 0.5f*(buf[idx] + buf[next]);
        buf[idx] = value * 0.996f;
        float u = (float)i/(float)(n>1?n-1:1);
        float env = expf(-4.0f*u);
        dst[i]=value*env;
        idx = next;
    }
    free(buf);
}

static void synth_egtr_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=196.f;
    float phase=0.f;
    float vibr=0.f;
    for(int i=0;i<n;i++){
        float t=(float)i/(float)sr;
        vibr += 0.002f*(frand_unit()-vibr);
        phase += (freq*(1.f+0.01f*vibr))/(float)sr;
        if(phase>1.f) phase -= floorf(phase);
        float saw = 2.f*phase - 1.f;
        float square = saw >=0 ? 1.f : -1.f;
        float mix = 0.6f*saw + 0.4f*square;
        float driven = tanhf(mix*3.0f);
        float env = expf(-2.5f*t);
        dst[i]=driven*env;
    }
}

static void synth_sample_into(float *dst, int n, const SampleData *sd, int channel){
    if(!sd || !sd->chan[0] || sd->length<=0){
        memset(dst,0,(size_t)n*sizeof(float));
        return;
    }
    if(channel >= sd->channels) channel = 0;
    if(channel<0) channel=0;
    const float *src = sd->chan[channel];
    if(!src){
        memset(dst,0,(size_t)n*sizeof(float));
        return;
    }
    if(sd->length==1){
        for(int i=0;i<n;i++) dst[i]=src[0];
        return;
    }
    double step = (double)sd->length / (double)n;
    double pos = 0.0;
    for(int i=0;i<n;i++){
        int idx = (int)pos;
        if(idx >= sd->length-1){
            dst[i] = src[sd->length-1];
        }else{
            double frac = pos - idx;
            float a = src[idx];
            float b = src[idx+1];
            dst[i] = (float)(a + (b - a)*frac);
        }
        pos += step;
    }
}

static void synth_birds_into(float *dst, int n, int sr, float base){
    if(base<=0.f) base=6000.f;
    float freq=base;
    float phase=0.f;
    float noise=0.f;
    for(int i=0;i<n;i++){
        float u=(float)i/(float)(n>1?n-1:1);
        if(i%(sr/20)==0){
            freq = base + frand_unit()*800.f;
        }
        phase += freq/(float)sr;
        if(phase>1.f) phase -= floorf(phase);
        noise += 0.3f*(frand_unit()-noise);
        float chirp = sine(phase) * expf(-6.f*u);
        dst[i] = (chirp + 0.4f*noise)*0.7f;
    }
}

static void synth_strpad_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=440.f;
    float phase1=0.f, phase2=0.f, phase3=0.f;
    for(int i=0;i<n;i++){
        float t=(float)i/(float)sr;
        float env = (t<0.15f) ? (t/0.15f) : expf(-1.2f*(t-0.15f));
        phase1 += freq/(float)sr;
        phase2 += (freq*2.01f)/(float)sr;
        phase3 += (freq*0.5f)/(float)sr;
        if(phase1>1.f) phase1 -= floorf(phase1);
        if(phase2>1.f) phase2 -= floorf(phase2);
        if(phase3>1.f) phase3 -= floorf(phase3);
        float sample = 0.5f*sine(phase1) + 0.3f*sine(phase2) + 0.2f*sine(phase3);
        dst[i]=sample*env;
    }
}

static void synth_bell_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=880.f;
    float phase1=0.f, phase2=0.f, phase3=0.f;
    for(int i=0;i<n;i++){
        float t=(float)i/(float)sr;
        float env=expf(-4.0f*t);
        phase1 += freq/(float)sr;
        phase2 += (freq*2.7f)/(float)sr;
        phase3 += (freq*3.9f)/(float)sr;
        if(phase1>1.f) phase1-=floorf(phase1);
        if(phase2>1.f) phase2-=floorf(phase2);
        if(phase3>1.f) phase3-=floorf(phase3);
        float sample = 0.6f*sine(phase1)+0.25f*sine(phase2)+0.15f*sine(phase3);
        dst[i]=sample*env;
    }
}

static void synth_brass_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=330.f;
    float phase=0.f;
    float filt=0.f;
    for(int i=0;i<n;i++){
        float t=(float)i/(float)n;
        phase += freq/(float)sr;
        if(phase>1.f) phase-=floorf(phase);
        float saw = 2.f*phase - 1.f;
        filt += 0.05f*(saw - filt);
        float env = (t<0.1f)? (t/0.1f) : expf(-2.5f*(t-0.1f));
        dst[i] = tanhf(filt*3.f) * env;
    }
}

static void synth_kalimba_into(float *dst, int n, int sr, float freq){
    if(freq<=0.f) freq=392.f;
    int period = (int)((float)sr / freq);
    if(period<2) period=2;
    float *buf = malloc((size_t)period * sizeof(float));
    if(!buf){ memset(dst,0,n*sizeof(float)); return; }
    for(int i=0;i<period;i++) buf[i]=frand_unit();
    int idx=0;
    for(int i=0;i<n;i++){
        int next=(idx+1)%period;
        float value = 0.5f*(buf[idx]+buf[next]);
        buf[idx] = value * 0.998f;
        float env = expf(-3.5f*(float)i/(float)n);
        dst[i]=value*env;
        idx=next;
    }
    free(buf);
}

static void synth_spec_into(const Spec *sp, float *dst, int n, int sr){
    if(sp->type==SPEC_SILENCE){ memset(dst,0,n*sizeof(float)); return; }
    if(sp->type==SPEC_CONST){
        float f=sp->f_const;
        for(int i=0;i<n;i++){ float t=(float)i/(float)sr; dst[i]=sine(f*t); }
        return;
    }
    if(sp->type==SPEC_GLIDE){
        float f0=sp->f0, f1=sp->f1;
        float phase=0.f;
        for(int i=0;i<n;i++){
            float u=(float)i/(float)(n>1?n-1:1);
            float f = f0 + (f1-f0)*u;      // linear in freq
            phase += f/(float)sr;
            dst[i]=sine(phase);
            if(phase>1.f) phase -= floorf(phase);
        }
        return;
    }
    if(sp->type==SPEC_CHORD){
        int m=sp->n; if(m<1) { memset(dst,0,n*sizeof(float)); return; }
        float scale = 1.f/(float)m;
        for(int i=0;i<n;i++){
            float t=(float)i/(float)sr, acc=0.f;
            for(int k=0;k<m;k++) acc += sine(sp->chord[k]*t);
            dst[i]=acc*scale;
        }
        return;
    }
    if(sp->type==SPEC_KICK){
        synth_kick_into(dst, n, sr, sp->f0, sp->f1);
        return;
    }
    if(sp->type==SPEC_SNARE){
        synth_snare_into(dst, n, sr, sp->f0);
        return;
    }
    if(sp->type==SPEC_HIHAT){
        synth_hat_into(dst, n, sr, sp->f0);
        return;
    }
    if(sp->type==SPEC_BASS){
        synth_bass_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_FLUTE){
        synth_flute_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_PIANO){
        synth_piano_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_GUITAR){
        synth_guitar_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_EGTR){
        synth_egtr_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_SAMPLE){
        synth_sample_into(dst, n, sp->sample, sp->sample_channel);
        return;
    }
    if(sp->type==SPEC_BIRDS){
        synth_birds_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_STRPAD){
        synth_strpad_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_BELL){
        synth_bell_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_BRASS){
        synth_brass_into(dst, n, sr, sp->f_const);
        return;
    }
    if(sp->type==SPEC_KALIMBA){
        synth_kalimba_into(dst, n, sr, sp->f_const);
        return;
    }
}

static void apply_fade(float *x, int n, int sr, int fade_ms){
    int f = (int)((fade_ms/1000.0f)*sr);
    if(f<1) return;
    if(f*2>n) f = n/2;
    for(int i=0;i<f;i++){
        float g = (float)i/(float)f;
        x[i] *= g;
        x[n-1-i] *= g;
    }
}

static void clamp_and_interleave(int16_t *dst, const float *L, const float *R, int n, float gain){
    for(int i=0;i<n;i++){
        float l = L[i]*gain, r = R[i]*gain;
        if(l>1.f) l=1.f; if(l<-1.f) l=-1.f;
        if(r>1.f) r=1.f; if(r<-1.f) r=-1.f;
        dst[2*i+0] = (int16_t)lrintf(l*32767.f);
        dst[2*i+1] = (int16_t)lrintf(r*32767.f);
    }
}

static void die(const char *msg){ fprintf(stderr,"%s\n",msg); exit(1); }

static int play_buffer_with_openal(const float *L, const float *R, size_t total_samples, float gain, int sr, const SpeechVec *speech, const char *espeak_bin){
    if(total_samples==0) return 0;
    if(total_samples > (size_t)INT_MAX) die("sequence too long");
    int16_t *pcm = malloc(total_samples * 2 * sizeof(int16_t));
    if(!pcm) die("oom pcm");
    clamp_and_interleave(pcm, L, R, (int)total_samples, gain);

    ALCdevice *dev = alcOpenDevice(NULL);
    if(!dev){ free(pcm); die("alcOpenDevice failed"); }
    ALCcontext *ctx = alcCreateContext(dev, NULL);
    if(!ctx || !alcMakeContextCurrent(ctx)){
        if(ctx) alcDestroyContext(ctx);
        alcCloseDevice(dev);
        free(pcm);
        die("alcMakeContextCurrent failed");
    }

    ALuint buf=0, src=0;
    alGenBuffers(1,&buf);
    alBufferData(buf, AL_FORMAT_STEREO16, pcm, (ALsizei)(total_samples*2*sizeof(int16_t)), sr);
    alGenSources(1,&src);
    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(src, AL_ROLLOFF_FACTOR, 0.f);
    alSourcef(src, AL_GAIN, 1.f);
    const float pos[3]={0,0,0}; alSourcefv(src, AL_POSITION, pos);
    alSourcei(src, AL_BUFFER, buf);
    alSourcePlay(src);

    size_t speech_idx=0;
    size_t speech_count = speech ? speech->len : 0;
    int64_t start_ms = now_ms();
    while(true){
        if(speech && speech_idx<speech_count){
            int64_t elapsed = now_ms() - start_ms;
            while(speech_idx<speech_count && speech->items[speech_idx].start_ms <= elapsed){
                launch_espeak_event(&speech->items[speech_idx], espeak_bin);
                speech_idx++;
            }
        }
        ALint state=0;
        alGetSourcei(src, AL_SOURCE_STATE, &state);
        if(state!=AL_PLAYING && speech_idx>=speech_count) break;
        usleep(3000);
    }

    alDeleteSources(1,&src);
    alDeleteBuffers(1,&buf);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    free(pcm);
    return 0;
}

int main(int argc, char **argv){
    // defaults
    int sr=44100, def_ms=120, fade_ms=8;
    float gain=0.3f;
    const char *espeak_bin="espeak";
    const char *seq_file=NULL;
    // parse globals
    int i=1;
    for(; i<argc; ++i){
        if(strcmp(argv[i],"-sr")==0){
            if(i+1>=argc){ fprintf(stderr,"-sr requires value\n"); return 2; }
            int tmp=0;
            if(!parse_int_strict(argv[i+1], &tmp) || tmp<=0){ fprintf(stderr,"invalid samplerate: %s\n", argv[i+1]); return 2; }
            sr=tmp; ++i; continue;
        }
        if(strcmp(argv[i],"-g")==0){
            if(i+1>=argc){ fprintf(stderr,"-g requires value\n"); return 2; }
            float tmp=0.f;
            if(!parse_float(argv[i+1], &tmp) || tmp<0.f){ fprintf(stderr,"invalid gain: %s\n", argv[i+1]); return 2; }
            gain=tmp; ++i; continue;
        }
        if(strcmp(argv[i],"-l")==0){
            if(i+1>=argc){ fprintf(stderr,"-l requires value\n"); return 2; }
            int tmp=0;
            if(!parse_int_strict(argv[i+1], &tmp) || tmp<=0){ fprintf(stderr,"invalid default duration: %s\n", argv[i+1]); return 2; }
            def_ms=tmp; ++i; continue;
        }
        if(strcmp(argv[i],"-fade")==0){
            if(i+1>=argc){ fprintf(stderr,"-fade requires value\n"); return 2; }
            int tmp=0;
            if(!parse_int_strict(argv[i+1], &tmp) || tmp<0){ fprintf(stderr,"invalid fade: %s\n", argv[i+1]); return 2; }
            fade_ms=tmp; ++i; continue;
        }
        if(strcmp(argv[i],"-f")==0){
            if(i+1>=argc){ fprintf(stderr,"-f requires filename\n"); return 2; }
            seq_file = argv[++i];
            continue;
        }
        if(strcmp(argv[i],"-espeak")==0){
            if(i+1>=argc){ fprintf(stderr,"-espeak requires path\n"); return 2; }
            espeak_bin = argv[++i];
            continue;
        }
        if(argv[i][0]=='-'){ fprintf(stderr,"unknown opt: %s\n", argv[i]); return 2; }
        break;
    }
    if(seq_file){
        return play_sequence_file(seq_file, sr, def_ms, fade_ms, gain, espeak_bin);
    }
    if(i>=argc){
        fprintf(stderr,"oabeep usage:\n  oabeep [-g gain] [-sr rate] [-l ms] [-fade ms] tokens...\n"
                        "       oabeep -f sequence.txt [options]\n"
                        "tokens: F[:ms] | L,R[:ms] | A~B[:ms] | f1+f2+...[:ms] | r:ms | 0:ms\n"
                        "sequence.txt: CSV sampler mit Thunderstruck-Syntax inkl. SAY@ und BG\n");
        return 1;
    }

    // parse tokens
    int maxTok = argc - i;
    Token *tok = calloc(maxTok, sizeof(Token)); if(!tok) die("oom");
    int nt=0;
    size_t total_samples=0;
    for(; i<argc; ++i){
        if(!parse_token(argv[i], def_ms, &tok[nt])){ free(tok); die("parse error"); }
        tok[nt].samples = token_target_samples(&tok[nt], sr);
        if(total_samples > SIZE_MAX - (size_t)tok[nt].samples) die("sequence too long");
        total_samples += (size_t)tok[nt].samples;
        nt++;
    }

    if(total_samples==0) die("no audio to render");
    if(total_samples > (size_t)INT_MAX) die("sequence too long");

    // synth full sequence (stereo)
    float *L = calloc(total_samples, sizeof(float));
    float *R = calloc(total_samples, sizeof(float));
    if(!L||!R) die("oom");
    size_t off=0;
    for(int k=0;k<nt;k++){
        int n = tok[k].samples;
        synth_spec_into(&tok[k].L, L+off, n, sr);
        synth_spec_into(&tok[k].R, R+off, n, sr);
        apply_fade(L+off, n, sr, fade_ms);
        apply_fade(R+off, n, sr, fade_ms);
        off += n;
    }

    int rc = play_buffer_with_openal(L, R, total_samples, gain, sr, NULL, NULL);
    free(tok); free(L); free(R);
    return rc;
}
