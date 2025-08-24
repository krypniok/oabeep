// oabeep.c - OpenAL "beep"-Ersatz mit Mono/Stereo, Glide, Chords, Rests.
// Usage: oabeep [global opts] token [token...]
// Tokens: mono: F[:ms]  | stereo: L,R[:ms]  | glide: A~B[:ms]  | chord: f1+f2+...[:ms] | rest: r:ms / 0:ms
// Global: -g gain(0..1) -sr samplerate -l default_ms -fade fadems

#define _GNU_SOURCE
#include <AL/al.h>
#include <AL/alc.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef enum { SPEC_SILENCE=0, SPEC_CONST, SPEC_GLIDE, SPEC_CHORD } SpecType;

typedef struct {
    SpecType type;
    float f_const;           // CONST
    float f0, f1;            // GLIDE
    float chord[16]; int n;  // CHORD (max 16 partials)
} Spec;

typedef struct {
    Spec L, R;
    bool stereo;
    int dur_ms;
} Token;

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

static Spec parse_spec(const char *s){
    Spec sp={.type=SPEC_CONST,.f_const=0};
    if(!s || !*s){ sp.type=SPEC_SILENCE; return sp; }
    if ((s[0]=='r' || s[0]=='R') && (s[1]==0 || s[1]==':')){ sp.type=SPEC_SILENCE; return sp; }
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
        if(!parse_float(a,&f0) || !parse_float(b,&f1)){ sp.type=SPEC_SILENCE; return sp; }
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
            float f=0; if(parse_float(tmp,&f)) sp.chord[sp.n++]=f;
            if(!q) break; p=q+1;
        }
        if(sp.n==0) { sp.type=SPEC_SILENCE; }
        return sp;
    }
    // const freq or 0 (rest)
    float f=0;
    if(!parse_float(s,&f) || f<=0){ sp.type=SPEC_SILENCE; }
    else { sp.type=SPEC_CONST; sp.f_const=f; }
    return sp;
}

static bool parse_token(const char *arg, int def_ms, Token *out){
    char *dup=strdup(arg); if(!dup) return false;
    char *col=strrchr(dup,':'); // last ':' as duration sep
    int dur = def_ms;
    if(col){ *col='\0'; char *d=col+1; trim(d); dur = atoi(d); if(dur<=0) dur=def_ms; }
    char *body=dup; trim(body);
    // rest token can be "r:ms" or "0:ms"
    if( (body[0]=='r'||body[0]=='R'||body[0]=='0') && (body[1]==0) ){
        out->L.type=SPEC_SILENCE; out->R.type=SPEC_SILENCE; out->stereo=false; out->dur_ms=dur; free(dup); return true;
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
    }
    out->dur_ms = dur;
    free(dup); return true;
}

static inline float sine(float ph){ return sinf(2.f*(float)M_PI*ph); }

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

int main(int argc, char **argv){
    // defaults
    int sr=44100, def_ms=120, fade_ms=8;
    float gain=0.3f;
    // parse globals
    int i=1;
    for(; i<argc; ++i){
        if(strcmp(argv[i],"-sr")==0 && i+1<argc){ sr=atoi(argv[++i]); continue; }
        if(strcmp(argv[i],"-g")==0 && i+1<argc){ gain=strtof(argv[++i],NULL); continue; }
        if(strcmp(argv[i],"-l")==0 && i+1<argc){ def_ms=atoi(argv[++i]); continue; }
        if(strcmp(argv[i],"-fade")==0 && i+1<argc){ fade_ms=atoi(argv[++i]); continue; }
        if(argv[i][0]=='-'){ fprintf(stderr,"unknown opt: %s\n", argv[i]); return 2; }
        break;
    }
    if(i>=argc){
        fprintf(stderr,"oabeep usage:\n  oabeep [-g gain] [-sr rate] [-l ms] [-fade ms] tokens...\n"
                        "tokens: F[:ms] | L,R[:ms] | A~B[:ms] | f1+f2+...[:ms] | r:ms | 0:ms\n");
        return 1;
    }

    // parse tokens
    int maxTok = argc - i;
    Token *tok = calloc(maxTok, sizeof(Token)); if(!tok) die("oom");
    int nt=0; int64_t total_samp=0;
    for(; i<argc; ++i){
        Token t;
        if(!parse_token(argv[i], def_ms, &t)){ free(tok); die("parse error"); }
        int n = (int)((int64_t)sr * t.dur_ms / 1000);
        if(n<1) n=1;
        total_samp += n;
        tok[nt++] = t;
    }

    // synth full sequence (stereo)
    float *L = calloc(total_samp, sizeof(float));
    float *R = calloc(total_samp, sizeof(float));
    if(!L||!R) die("oom");
    int64_t off=0;
    for(int k=0;k<nt;k++){
        int n = (int)((int64_t)sr * tok[k].dur_ms / 1000);
        if(n<1) n=1;
        synth_spec_into(&tok[k].L, L+off, n, sr);
        synth_spec_into(&tok[k].R, R+off, n, sr);
        apply_fade(L+off, n, sr, fade_ms);
        apply_fade(R+off, n, sr, fade_ms);
        off += n;
    }

    // interleave to STEREO16
    int16_t *pcm = malloc((size_t)total_samp * 2 * sizeof(int16_t));
    if(!pcm) die("oom pcm");
    clamp_and_interleave(pcm, L, R, (int)total_samp, gain);

    // OpenAL init
    ALCdevice *dev = alcOpenDevice(NULL);
    if(!dev) die("alcOpenDevice failed");
    ALCcontext *ctx = alcCreateContext(dev, NULL);
    if(!ctx || !alcMakeContextCurrent(ctx)) die("alcMakeContextCurrent failed");

    ALuint buf=0, src=0;
    alGenBuffers(1,&buf);
    alBufferData(buf, AL_FORMAT_STEREO16, pcm, (ALsizei)(total_samp*2*sizeof(int16_t)), sr);
    alGenSources(1,&src);
    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(src, AL_ROLLOFF_FACTOR, 0.f);
    alSourcef(src, AL_GAIN, 1.f);
    const float pos[3]={0,0,0}; alSourcefv(src, AL_POSITION, pos);
    alSourcei(src, AL_BUFFER, buf);
    alSourcePlay(src);

    // wait until done
    ALint state=0;
    do { alGetSourcei(src, AL_SOURCE_STATE, &state); usleep(1000*5); } while(state==AL_PLAYING);

    alDeleteSources(1,&src);
    alDeleteBuffers(1,&buf);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    free(tok); free(L); free(R); free(pcm);
    return 0;
}
