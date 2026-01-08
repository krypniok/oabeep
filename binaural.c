// oabeep.c - OpenAL Realtime Streaming Synth (Atomsirenen-Edition)
// gcc -o oabeep binaural.c -lopenal -lm

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

#define NUM_BUFFERS 4
#define CHUNK_SIZE 2048 

typedef enum { SPEC_SILENCE=0, SPEC_CONST, SPEC_GLIDE, SPEC_CHORD_GLIDE } SpecType;

typedef struct {
    SpecType type;
    int n;                      // Anzahl der Töne im Akkord
    float f0[16], f1[16];       // Start- und Zielfrequenzen
    double phase_acc[16];       // Individuelle Phasen für lückenlosen Sound
} Spec;

typedef struct {
    Spec L, R;
    int dur_ms;
} Token;

// --- Parser-Werkstatt ---

static void trim(char *s){
    size_t n=strlen(s), i=0, j=n;
    while(i<n && isspace((unsigned char)s[i])) i++;
    while(j>i && isspace((unsigned char)s[j-1])) j--;
    memmove(s, s+i, j-i); s[j-i]='\0';
}

static bool parse_float(const char *s, float *out){
    char *e=NULL; float v=strtof(s,&e);
    if(e==s || (e && *e!='\0')) return false;
    *out=v; return true;
}

// Zerlegt einen Part (z.B. "200~600" oder "440")
static void parse_single_glide(const char *s, float *f0, float *f1) {
    const char *tilde = strchr(s, '~');
    if (tilde) {
        char a[64], b[64];
        size_t la = (size_t)(tilde - s);
        if(la >= sizeof(a)) la = sizeof(a)-1;
        memcpy(a, s, la); a[la] = '\0';
        strncpy(b, tilde + 1, sizeof(b)-1);
        trim(a); trim(b);
        parse_float(a, f0); parse_float(b, f1);
    } else {
        parse_float(s, f0); *f1 = *f0;
    }
}

static Spec parse_spec(const char *s){
    Spec sp = {.type = SPEC_CHORD_GLIDE, .n = 0};
    for(int k=0; k<16; k++) sp.phase_acc[k] = 0.0;

    if(!s || !*s || s[0]=='r' || s[0]=='0') { sp.type = SPEC_SILENCE; return sp; }

    char *dup = strdup(s);
    char *saveptr;
    char *part = strtok_r(dup, "+", &saveptr);
    while(part && sp.n < 16) {
        parse_single_glide(part, &sp.f0[sp.n], &sp.f1[sp.n]);
        sp.n++;
        part = strtok_r(NULL, "+", &saveptr);
    }
    free(dup);
    if(sp.n == 0) sp.type = SPEC_SILENCE;
    return sp;
}

static bool parse_token(const char *arg, int def_ms, Token *out){
    char *dup = strdup(arg);
    char *col = strrchr(dup, ':');
    int dur = def_ms;
    if(col) { *col = '\0'; dur = atoi(col+1); if(dur <= 0) dur = def_ms; }
    
    char *comma = strchr(dup, ',');
    if(comma) {
        *comma = '\0';
        out->L = parse_spec(dup);
        out->R = parse_spec(comma + 1);
    } else {
        out->L = parse_spec(dup);
        out->R = out->L;
    }
    out->dur_ms = dur;
    free(dup);
    return true;
}

// --- Synthese-Reaktor ---

static void synth_spec_chunk(Spec *sp, float *dst, int n, int sr, int64_t t_off, int total_n) {
    if(sp->type == SPEC_SILENCE) { memset(dst, 0, n * sizeof(float)); return; }
    
    double dt = 1.0 / (double)sr;
    for(int i = 0; i < n; i++) {
        float u = (float)(t_off + i) / (float)(total_n > 1 ? total_n - 1 : 1);
        float sample_acc = 0.0f;
        
        for(int k = 0; k < sp->n; k++) {
            float freq = sp->f0[k] + (sp->f1[k] - sp->f0[k]) * u;
            sp->phase_acc[k] += 2.0 * M_PI * (double)freq * dt;
            sample_acc += (float)sin(sp->phase_acc[k]);
            if(sp->phase_acc[k] > 2.0 * M_PI) sp->phase_acc[k] -= 2.0 * M_PI;
        }
        dst[i] = sample_acc / (float)sp->n;
    }
}

static void apply_fade_chunk(float *x, int n, int sr, int fade_ms, int64_t t_off, int total_n) {
    int f = (int)((fade_ms/1000.0f)*sr);
    if(f < 1) return;
    for(int i = 0; i < n; i++) {
        int64_t pos = t_off + i;
        float g = 1.0f;
        if(pos < f) g = (float)pos/(float)f;
        else if(pos > (int64_t)total_n - f) g = (float)(total_n - pos)/(float)f;
        x[i] *= (g < 0 ? 0 : g);
    }
}

int main(int argc, char **argv){
    int sr=44100, def_ms=120, fade_ms=8, repeat_count=1;
    float gain=0.3f;
    int i=1;
    for(; i<argc; ++i){
        if(strcmp(argv[i],"-sr")==0 && i+1<argc) sr=atoi(argv[++i]);
        else if(strcmp(argv[i],"-g")==0 && i+1<argc) gain=strtof(argv[++i],NULL);
        else if(strcmp(argv[i],"-l")==0 && i+1<argc) def_ms=atoi(argv[++i]);
        else if(strcmp(argv[i],"-fade")==0 && i+1<argc) fade_ms=atoi(argv[++i]);
        else if(strcmp(argv[i],"-c")==0 && i+1<argc) repeat_count=atoi(argv[++i]);
        else if(argv[i][0]=='-') continue;
        else break;
    }
    if(i>=argc) return 1;

    int nt=0; Token *tok = calloc(argc-i, sizeof(Token));
    for(int k=i; k<argc; ++k) parse_token(argv[k], def_ms, &tok[nt++]);

    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);

    ALuint source, buffers[NUM_BUFFERS];
    alGenSources(1, &source);
    alGenBuffers(NUM_BUFFERS, buffers);

    int16_t *pcm = malloc(CHUNK_SIZE*2*sizeof(int16_t));
    float *L = malloc(CHUNK_SIZE*sizeof(float)), *R = malloc(CHUNK_SIZE*sizeof(float));
    int rep=0, t_idx=0; int64_t t_off=0;

    while(rep < repeat_count) {
        ALint proc, queued;
        alGetSourcei(source, AL_BUFFERS_PROCESSED, &proc);
        alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
        if(queued < NUM_BUFFERS) proc = NUM_BUFFERS - queued;

        while(proc-- > 0 && rep < repeat_count) {
            ALuint b;
            if(queued < NUM_BUFFERS) b = buffers[queued++];
            else alSourceUnqueueBuffers(source, 1, &b);

            int filled = 0;
            while(filled < CHUNK_SIZE && rep < repeat_count) {
                int total_s = (int)((int64_t)sr * tok[t_idx].dur_ms / 1000);
                int rem = total_s - (int)t_off;
                int take = (CHUNK_SIZE - filled < rem) ? (CHUNK_SIZE - filled) : rem;

                synth_spec_chunk(&tok[t_idx].L, L+filled, take, sr, t_off, total_s);
                synth_spec_chunk(&tok[t_idx].R, R+filled, take, sr, t_off, total_s);
                apply_fade_chunk(L+filled, take, sr, fade_ms, t_off, total_s);
                apply_fade_chunk(R+filled, take, sr, fade_ms, t_off, total_s);

                filled += take; t_off += take;
                if(t_off >= total_s) { t_off = 0; t_idx++; if(t_idx >= nt) { t_idx = 0; rep++; } }
            }
            if(filled > 0) {
                for(int j=0; j<filled; j++) {
                    pcm[2*j]   = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, L[j]*gain)) * 32767.f);
                    pcm[2*j+1] = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, R[j]*gain)) * 32767.f);
                }
                alBufferData(b, AL_FORMAT_STEREO16, pcm, filled*2*sizeof(int16_t), sr);
                alSourceQueueBuffers(source, 1, &b);
                ALint state; alGetSourcei(source, AL_SOURCE_STATE, &state);
                if(state != AL_PLAYING) alSourcePlay(source);
            }
        }
        usleep(1000);
    }
    ALint state; do { alGetSourcei(source, AL_SOURCE_STATE, &state); usleep(5000); } while(state == AL_PLAYING);
    return 0;
}
