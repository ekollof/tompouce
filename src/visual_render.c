#include "terminal_graphics.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAU 6.2831853071795864769
static double clamp(double x, double a, double b) { return fmax(a,fmin(b,x)); }
static void dot(TerminalImage *im, int x, int y, uint32_t c, int radius) {
    radius = (int)clamp(radius,0,12);
    for (int j=-radius;j<=radius;j++) for (int i=-radius;i<=radius;i++)
        if (i*i+j*j <= radius*radius+1) terminal_graphics_pixel(im,x+i,y+j,c);
}
static void line(TerminalImage *im, double x, double y, double xx, double yy, uint32_t c, int radius) {
    if (!isfinite(x+y+xx+yy)) return;
    x=clamp(x,-im->width,2*im->width); xx=clamp(xx,-im->width,2*im->width);
    y=clamp(y,-im->height,2*im->height); yy=clamp(yy,-im->height,2*im->height);
    int n=(int)fmax(fabs(xx-x),fabs(yy-y))+1;
    for (int i=0;i<=n;i++) dot(im,(int)round(x+(xx-x)*i/n),(int)round(y+(yy-y)*i/n),c,radius);
}
static uint32_t opacity(uint32_t c, double gain) {
    return (c&0xffffff00u)|(unsigned)round((c&255)*clamp(gain,0,1));
}
static uint32_t ink(const VisualOptions *o, const VisualNote *n, int active) {
    uint32_t c=active?o->active:o->inactive;
    if ((active || o->colorize_inactive) && (n->flags&VISUAL_COLOR)) c=n->color;
    return opacity(c,n->gain);
}

/* Hann-windowed real input, iterative radix-2 FFT. UI thread only. */
void terminal_graphics_fft(const float *samples, int count, double rate, float bands[SPECTRUM_BANDS]) {
    double re[4096]={0}, imaginary[4096]={0}, sum=0;
    for (int i=0;i<SPECTRUM_BANDS;i++) bands[i]=-160;
    if (!samples || count<2 || !isfinite(rate) || rate<40) return;
    if (count>4096) { samples+=count-4096; count=4096; }
    int n=2; while (n*2<=count) n*=2;
    samples+=count-n;
    for (int i=0;i<n;i++) {
        double w=.5-.5*cos(TAU*i/(n-1)); sum+=w;
        re[i]=isfinite(samples[i])?samples[i]*w:0;
    }
    for (int i=1,j=0;i<n;i++) {
        int bit=n>>1; for (;j&bit;bit>>=1) j^=bit; j^=bit;
        if (i<j) { double a=re[i]; re[i]=re[j]; re[j]=a; }
    }
    for (int len=2;len<=n;len*=2) for (int start=0;start<n;start+=len)
        for (int j=0;j<len/2;j++) {
            double a=-TAU*j/len, c=cos(a), s=sin(a);
            int u=start+j,v=u+len/2;
            double r=c*re[v]-s*imaginary[v], q=s*re[v]+c*imaginary[v];
            re[v]=re[u]-r; imaginary[v]=imaginary[u]-q; re[u]+=r; imaginary[u]+=q;
        }
    double high=fmin(20000,rate/2), range=log(high/20);
    for (int i=1;i<=n/2;i++) {
        double f=(double)i*rate/n; if (f<20 || f>high) continue;
        int b=(int)(log(f/20)/range*SPECTRUM_BANDS);
        if (b>=SPECTRUM_BANDS) b=SPECTRUM_BANDS-1;
        double db=20*log10(fmax(1e-8,2*hypot(re[i],imaginary[i])/sum));
        if (db>bands[b]) bands[b]=(float)db;
    }
}

static void spectrum(TerminalImage *im, TerminalFeedbackState *state, const VisualOptions *o,
                     const float *samples, int count, double now, uint32_t color) {
    if (!state) return;
    if (!state->count || (o->speed>0 && now-state->last_time>=fmax(1.0/60,4/(o->speed*SPECTRUM_COLUMNS)))) {
        terminal_graphics_fft(samples,count,44100,state->levels[state->head]);
        state->times[state->head]=now; state->head=(state->head+1)%SPECTRUM_COLUMNS;
        if (state->count<SPECTRUM_COLUMNS) state->count++;
        state->last_time=now;
    }
    int left=terminal_graphics_plot_left(im), width=im->width-left;
    double end=o->speed>0?now:state->last_time, span=4/fmax(.125,o->speed);
    for (int x=left;x<im->width;x++) {
        double t=end-span+(x-left)*span/fmax(1,width-1);
        int best=-1; double distance=span;
        for (int j=0;j<state->count;j++) {
            int k=(state->head-1-j+SPECTRUM_COLUMNS)%SPECTRUM_COLUMNS;
            double d=fabs(t-state->times[k]); if (d<distance) { best=k; distance=d; }
        }
        if (best<0 || distance>fmin(.12,span/fmax(1,width-1)*o->thickness*.5)) continue;
        for (int y=0;y<im->height;y++) {
            int b=(im->height-1-y)*SPECTRUM_BANDS/im->height;
            double value=(state->levels[best][b]-o->min_db)/(o->max_db-o->min_db);
            terminal_graphics_pixel(im,x,y,opacity(color,value));
        }
    }
    if (left) {
        static const double hz[]={20,100,1000,10000,20000};
        static const char *text[]={"20","100","1K","10K","20K"};
        for (int i=0;i<5;i++) {
            int y=(int)((1-log(hz[i]/20)/log(1000))*(im->height-1));
            terminal_graphics_text(im,2,(int)clamp(y-2,0,im->height-5),text[i],1);
        }
    }
}

static void waveform(TerminalImage *im, const VisualOptions *o, const float *s, int n) {
    if (!s || n<2) return;
    double peak=0;
    for (int i=0;i<n;i++) if (isfinite(s[i])) peak=fmax(peak,fabs(s[i]));
    double scale=o->scale>0?o->scale:(peak>1e-5?.9/peak:1);
    int left=terminal_graphics_plot_left(im), width=im->width-left, begin=0;
    if (o->align) for (int i=1;i<n/2;i++)
        if (s[i-1]<=o->trigger && s[i]>o->trigger) { begin=i; break; }
    uint32_t color=o->has_color?o->color:o->active;
    double last=im->height*o->pos;
    for (int x=0;x<width;x++) {
        int a=begin+x*(n-begin)/width, b=begin+(x+1)*(n-begin)/width;
        if (b<=a) b=a+1;
        double lo=last,hi=last;
        for (int j=a;j<b && j<n;j++) {
            double y=(o->pos-(isfinite(s[j])?s[j]*scale:0)*.5)*(im->height-1);
            last=clamp(y,0,im->height-1); lo=fmin(lo,last); hi=fmax(hi,last);
        }
        line(im,left+x,lo,left+x,hi,color,(int)(o->thickness-1)/2);
    }
    if (left) {
        int text_scale=im->height>=30?2:1;
        char text[16]; snprintf(text,sizeof(text),"+%.2f",2*o->pos/scale);
        terminal_graphics_text(im,2,0,text,text_scale);
        terminal_graphics_text(im,2,(int)(o->pos*(im->height-5*text_scale)),"0",text_scale);
        snprintf(text,sizeof(text),"-%.2f",2*(1-o->pos)/scale);
        terminal_graphics_text(im,2,im->height-5*text_scale,text,text_scale);
    }
}

typedef struct { const VisualNote *note; int index; } RollOrder;
static int roll_order(const void *a, const void *b) {
    const VisualNote *x=((const RollOrder *)a)->note,*y=((const RollOrder *)b)->note;
    int sound=strcmp(x->sound,y->sound); if (sound) return sound;
    if (x->pitch!=y->pitch) return x->pitch<y->pitch?-1:1;
    return x->start<y->start?-1:x->start>y->start;
}
static void roll(TerminalImage *im, const VisualOptions *o, const VisualNote *notes, int count,
                 double now, double origin, double seconds) {
    double span=o->cycles*seconds, from=now-span*o->playhead;
    double low=o->min_midi,high=o->max_midi;
    int sound_rows[GRAPHICS_NOTES], sound_count=0, melodic=0;
    for (int i=0;i<count;i++) {
        if (notes[i].end<from || notes[i].start>from+span) continue;
        if (!*notes[i].sound) { melodic++; continue; }
        int j=0;
        while (j<sound_count && strcmp(notes[sound_rows[j]].sound,notes[i].sound)) j++;
        if (j==sound_count) sound_rows[sound_count++]=i;
    }
    for (int i=0;i<sound_count;i++) for (int j=i+1;j<sound_count;j++)
        if (strcmp(notes[sound_rows[i]].sound,notes[sound_rows[j]].sound)>0) {
            int index=sound_rows[i]; sound_rows[i]=sound_rows[j]; sound_rows[j]=index;
        }
    if (o->autorange) {
        low=128; high=-128;
        for (int i=0;i<count;i++) if (!*notes[i].sound && notes[i].end>=from && notes[i].start<=from+span) {
            low=fmin(low,notes[i].pitch); high=fmax(high,notes[i].pitch);
        }
        if (high<low) { low=48; high=72; }
        low=floor(low); high=ceil(high);
        if (high-low<12) { double mid=floor((high+low)/2); low=mid-6; high=mid+6; }
    }
    int left=o->vertical?0:terminal_graphics_plot_left(im), width=im->width-left;
    int top=o->vertical && !im->compact && im->height>=30?14:0, height=im->height-top;
    RollOrder order[GRAPHICS_NOTES]; int lane[GRAPHICS_NOTES],lanes[GRAPHICS_NOTES];
    double ends[GRAPHICS_NOTES];
    for (int i=0;i<count;i++) order[i]=(RollOrder){&notes[i],i};
    qsort(order,(size_t)count,sizeof(*order),roll_order);
    for (int first=0;first<count;) {
        int last=first,used=0;
        while (last<count && order[last].note->pitch==order[first].note->pitch &&
               !strcmp(order[last].note->sound,order[first].note->sound)) {
            int k=0; while (k<used && ends[k]>order[last].note->start) k++;
            if (k==used) used++;
            ends[k]=order[last].note->end; lane[order[last].index]=k; last++;
        }
        for (int j=first;j<last;j++) lanes[order[j].index]=used;
        first=last;
    }
    int onsets[GRAPHICS_NOTES][4],marked=0;
    double values[GRAPHICS_NOTES]; int rows=0;
    if (o->fold) for (int i=0;i<count;i++) if (!*notes[i].sound && notes[i].end>=from && notes[i].start<=from+span) {
        int j=0; while (j<rows && values[j]!=notes[i].pitch) j++;
        if (j==rows && rows<GRAPHICS_NOTES) values[rows++]=notes[i].pitch;
    }
    if (o->fold) for (int i=0;i<rows;i++) for (int j=i+1;j<rows;j++)
        if (values[j]<values[i]) { double v=values[i]; values[i]=values[j]; values[j]=v; }
    double pitch_rows=melodic?(o->fold?rows:ceil(high-low+1)):0;
    double total_rows=fmax(1,pitch_rows+sound_count);
    int axis_size=o->vertical?im->width:im->height;
    int label_step=(int)ceil(28*total_rows/fmax(1,axis_size));
    if (label_step<1) label_step=1;
    for (int row=0;row<(int)ceil(total_rows);row++) {
        double v=(row+.5)/total_rows;
        if (o->vertical?o->flip_values:!o->flip_values) v=1-v;
        int center=(int)(v*axis_size), half=(int)fmax(1,axis_size/total_rows/2);
        char label[16]; label[0]=0;
        int note=(int)round(o->fold && row<rows?values[row]:low+row);
        if (row<pitch_rows) {
            int pc=(note%12+12)%12,black=pc==1 || pc==3 || pc==6 || pc==8 || pc==10;
            uint32_t key=black?0x303d42ffu:0xa5b9beffu;
            if (o->vertical) {
                line(im,center,top,center,im->height-1,note%12?0x60708030u:0x60708080u,0);
                if (top) for (int x=center-half;x<center+half;x++) for (int y=top-4;y<top-1;y++)
                    terminal_graphics_pixel(im,x,y,key);
            } else {
                line(im,left,center,im->width-1,center,note%12?0x60708030u:0x60708080u,0);
                if (left) for (int y=center-half;y<center+half;y++) for (int x=left-7;x<left-1;x++)
                    terminal_graphics_pixel(im,x,y,key);
            }
            if (row%label_step==0) terminal_graphics_note_label(note,label,sizeof(label));
        } else {
            int index=row-(int)pitch_rows;
            if (index<sound_count) snprintf(label,sizeof(label),"%.8s",notes[sound_rows[index]].label);
        }
        if (*label && !im->compact) {
            if (top) {
                int cell_width=(int)(axis_size/total_rows)*(row<pitch_rows?label_step:1);
                int scale=(int)strlen(label)*8<=cell_width?2:1;
                int text_width=(int)strlen(label)*4*scale;
                if (text_width>cell_width) label[cell_width/(4*scale)]=0;
                text_width=(int)strlen(label)*4*scale;
                terminal_graphics_text(im,(int)clamp(center-text_width/2,0,im->width-1),0,label,scale);
            } else if (left) terminal_graphics_text(im,2,(int)clamp(center-5,0,im->height-10),label,2);
        }
    }
    for (int i=0;i<count;i++) {
        const VisualNote *n=&notes[i]; int active=n->start<=now && n->end>now;
        if (n->end<from || n->start>from+span || (o->hide_negative && n->start<origin) ||
            (o->hide_inactive && !active) || (!*n->sound && (n->pitch<low || n->pitch>high))) continue;
        double v=n->pitch-low+.5, cell=1/total_rows;
        if (o->fold && rows) {
            int j=0; while (j<rows && values[j]!=n->pitch) j++;
            v=j+.5;
        }
        if (*n->sound && sound_count) {
            int j=0; while (j<sound_count && strcmp(notes[sound_rows[j]].sound,n->sound)) j++;
            v=pitch_rows+j+.5;
        }
        v/=total_rows;
        v+=cell*((lane[i]+.5)/lanes[i]-.5);
        cell/=lanes[i];
        if (o->vertical?o->flip_values:!o->flip_values) v=1-v;
        double a=clamp((n->start-from)/span,0,1), b=clamp((n->end-from)/span,0,1);
        if (o->flip_time) { double t=a; a=1-b; b=1-t; }
        int x0,x1,y0,y1;
        if (o->vertical) {
            x0=(int)((v-cell*.4)*im->width); x1=(int)((v+cell*.4)*im->width);
            y0=top+(int)(a*(height-1)); y1=top+(int)(b*(height-1));
        } else {
            x0=left+(int)(a*(width-1)); x1=left+(int)(b*(width-1));
            y0=(int)((v-cell*.4)*im->height); y1=(int)((v+cell*.4)*im->height);
        }
        x0=(int)clamp(x0,0,im->width-1); x1=(int)clamp(x1,x0,im->width-1);
        y0=(int)clamp(y0,0,im->height-1); y1=(int)clamp(y1,y0,im->height-1);
        uint32_t c=ink(o,n,active);
        /* Braille has no opacity: preserve a quiet note's geometry. */
        if (im->compact && (c&255)) c=(c&0xffffff00u)|255u;
        int fill=active?o->fill_active:o->fill, stroke=active?o->stroke_active:o->stroke;
        for (int y=y0;y<=y1;y++) for (int x=x0;x<=x1;x++)
            if (fill || (stroke && (x==x0 || x==x1 || y==y0 || y==y1))) terminal_graphics_pixel(im,x,y,c);
        if (n->start>=from) {
            int onset_x=o->flip_time?x1:x0, onset_y=o->flip_time?y1:y0;
            onsets[marked][0]=o->vertical?x0:onset_x;
            onsets[marked][1]=o->vertical?onset_y:y0;
            onsets[marked][2]=o->vertical?x1:onset_x;
            onsets[marked++][3]=o->vertical?onset_y:y1;
        }
        if (o->labels && !im->compact && x1-x0>8 && y1-y0>=5) {
            char text[10];
            int limit=(x1-x0-1)/4; if (limit>9) limit=9;
            snprintf(text,sizeof(text),"%.*s",limit,active&&*n->active_label?n->active_label:n->label);
            terminal_graphics_text(im,x0+1,y0,text,1);
        }
    }
    for (int i=0;i<marked;i++) line(im,onsets[i][0],onsets[i][1],onsets[i][2],onsets[i][3],0xffbe50ffu,0);
    double p=o->flip_time?1-o->playhead:o->playhead;
    if (o->vertical) line(im,0,top+p*(height-1),im->width-1,top+p*(height-1),o->playhead_color,0);
    else line(im,left+p*(width-1),0,left+p*(width-1),im->height-1,o->playhead_color,0);
}

static void polar(TerminalImage *im, const VisualOptions *o, const VisualNote *notes, int count,
                  int kind, double now, double origin, double seconds) {
    double cx=im->width*.5,cy=im->height*.5;
    double radius=fmax(1,fmin(cx,cy)-fmin(o->margin,fmin(cx,cy)*.2));
    if (o->size>0) radius=fmin(radius,o->size);
    int thickness=im->compact?0:(int)(o->thickness-1)/2;
    if (kind==VISUAL_PITCHWHEEL) {
        for (int j=0;j<o->edo;j++) {
            double a=TAU*j/o->edo-TAU/4;
            line(im,cx+radius*.85*cos(a),cy+radius*.85*sin(a),cx+radius*cos(a),cy+radius*sin(a),0x60708080u,0);
        }
        if (o->circle) for (int j=0;j<180;j++) {
            double a=TAU*j/180,b=TAU*(j+1)/180;
            line(im,cx+radius*cos(a),cy+radius*sin(a),cx+radius*cos(b),cy+radius*sin(b),0x60708080u,0);
        }
        double angles[GRAPHICS_NOTES]; uint32_t colors[GRAPHICS_NOTES]; int used=0;
        double root=69+12*log2(o->root/440);
        for (int i=0;i<count && used<GRAPHICS_NOTES;i++) if (notes[i].start<=now && notes[i].end>now) {
            double p=(notes[i].pitch-root)/12; p-=floor(p);
            angles[used]=p*TAU-TAU/4; colors[used++]=ink(o,&notes[i],1);
        }
        for (int i=0;i<used;i++) for (int j=i+1;j<used;j++) if (angles[j]<angles[i]) {
            double a=angles[i]; angles[i]=angles[j]; angles[j]=a;
            uint32_t c=colors[i]; colors[i]=colors[j]; colors[j]=c;
        }
        for (int i=0;i<used;i++) {
            double x=cx+radius*cos(angles[i]),y=cy+radius*sin(angles[i]);
            int j=(i+1)%used;
            line(im,x,y,o->polygon?cx+radius*cos(angles[j]):cx,o->polygon?cy+radius*sin(angles[j]):cy,colors[i],thickness);
            if (o->hapcircles) dot(im,(int)x,(int)y,colors[i],im->compact?0:(int)o->hap_radius);
        }
    } else {
        double time=(now-origin)/seconds;
        for (int i=0;i<count;i++) {
            int active=notes[i].start<=now && notes[i].end>now;
            if (o->hide_inactive && !active) continue;
            double a=(notes[i].start-now)/seconds+o->inset;
            double b=(notes[i].end-now)/seconds+o->inset-o->padding;
            a=clamp(a,0,o->inset+o->cycles); b=clamp(b,0,o->inset+o->cycles);
            if (b<a) continue;
            uint32_t c=ink(o,&notes[i],active);
            if (o->fade) c=opacity(c,1-a/(o->inset+o->cycles)*.7);
            int steps=(int)clamp((b-a)*100*o->stretch,1,512);
            double px=0,py=0;
            for (int j=0;j<=steps;j++) {
                double t=a+(b-a)*j/steps, r=t/(o->inset+o->cycles);
                if (o->log_spiral) r=(exp(r)-1)/(exp(1)-1);
                double angle=TAU*(t*o->stretch+time*o->steady)-TAU/4;
                double x=cx+radius*r*cos(angle),y=cy+radius*r*sin(angle);
                if (j) line(im,px,py,x,y,c,thickness);
                px=x; py=y;
            }
        }
        double r=radius*o->inset/(o->inset+o->cycles);
        for (int j=0;j<32;j++) {
            double a=TAU*((o->inset+o->playhead_length*j/32)*o->stretch+time*o->steady)-TAU/4;
            double b=TAU*((o->inset+o->playhead_length*(j+1)/32)*o->stretch+time*o->steady)-TAU/4;
            line(im,cx+r*cos(a),cy+r*sin(a),cx+r*cos(b),cy+r*sin(b),o->playhead_color,
                 im->compact?0:(int)(o->playhead_thickness-1)/2);
        }
    }
}

void terminal_graphics_feedback(TerminalImage *im, TerminalFeedbackState *state, int kind,
                                const VisualOptions *o, const VisualNote *notes, int count,
                                const float *samples, int sample_count, double now, double origin, double seconds) {
    if (!im || !o || im->width<1 || im->width>GRAPHICS_WIDTH || im->height<1 || im->height>GRAPHICS_HEIGHT) return;
    if (count<0 || !notes) count=0;
    if (count>GRAPHICS_NOTES) count=GRAPHICS_NOTES;
    if (!isfinite(seconds) || seconds<=0) seconds=2;
    size_t bytes=(size_t)im->width*im->height*4;
    if (o->smear && state && state->previous_valid && state->width==im->width && state->height==im->height)
        memcpy(im->rgba,state->previous,bytes);
    else {
        memset(im->rgba,0,bytes);
        if (o->background) for (int y=0;y<im->height;y++) for (int x=0;x<im->width;x++)
            terminal_graphics_pixel(im,x,y,o->background);
    }
    if (kind==VISUAL_SCOPE) waveform(im,o,samples,sample_count);
    else if (kind==VISUAL_SPECTRUM) {
        uint32_t color=o->has_color?o->color:o->active;
        for (int i=0;i<count;i++) if (notes[i].start<=now && notes[i].end>now && (notes[i].flags&VISUAL_COLOR)) color=notes[i].color;
        spectrum(im,state,o,samples,sample_count,now,color);
    } else if (kind==VISUAL_SPIRAL || kind==VISUAL_PITCHWHEEL) polar(im,o,notes,count,kind,now,origin,seconds);
    else roll(im,o,notes,count,now,origin,seconds);
    if (o->smear && state) {
        memcpy(state->previous,im->rgba,bytes); state->width=im->width; state->height=im->height; state->previous_valid=1;
    }
}
