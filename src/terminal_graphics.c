#include "terminal_graphics.h"
#include <math.h>
#include <string.h>
#include <ctype.h>

static uint32_t palette[18]; /* ANSI 0..15, default foreground/background */
static unsigned palette_known, palette_generation;
void terminal_graphics_palette_reset(void) { palette_known=0; palette_generation++; }
unsigned terminal_graphics_palette_generation(void) { return palette_generation; }
int terminal_graphics_palette_probe(FILE *out) {
    if (!out || fputs("\033]10;?\033\\\033]11;?\033\\",out)==EOF) return 0;
    for (int i=0;i<16;i++) if (fprintf(out,"\033]4;%d;?\033\\",i)<0) return 0;
    return 1;
}
static int hex_component(const char **text, unsigned *value) {
    unsigned v=0; int digits=0;
    while (isxdigit((unsigned char)**text)) {
        if (++digits>4) return 0;
        int c=tolower((unsigned char)*(*text)++);
        v=(v<<4)|(unsigned)(c<='9'?c-'0':c-'a'+10);
    }
    if (!digits) return 0;
    unsigned max=(1u<<(digits*4))-1;
    *value=(v*255+max/2)/max; return 1;
}
int terminal_graphics_palette_reply(const char *reply) {
    if (!reply || strlen(reply)>96) return 0;
    const char *p=reply; int index=-1;
    if (!strncmp(p,"10;",3)) { index=16; p+=3; }
    else if (!strncmp(p,"11;",3)) { index=17; p+=3; }
    else if (!strncmp(p,"4;",2)) {
        p+=2; int value=0,digits=0;
        while (*p>='0' && *p<='9') { if (++digits>2) return 0; value=value*10+*p++-'0'; }
        if (!digits || value>15 || *p++!=';') return 0;
        index=value;
    }
    if (index<0) return 0;
    uint32_t color;
    if (!strncmp(p,"rgb:",4)) {
        p+=4; unsigned r,g,b;
        if (!hex_component(&p,&r) || *p++!='/' || !hex_component(&p,&g) || *p++!='/' ||
            !hex_component(&p,&b) || *p) return 0;
        color=(r<<24)|(g<<16)|(b<<8)|255u;
    } else {
        size_t n=strlen(p);
        if (*p!='#' || (n!=4 && n!=7) || !visual_color(p,&color)) return 0;
    }
    unsigned bit=1u<<index;
    if (!(palette_known&bit) || palette[index]!=color) {
        palette[index]=color; palette_known|=bit; palette_generation++;
    }
    return 1;
}
uint32_t terminal_graphics_theme(int role, unsigned alpha) {
    static const uint32_t fallback[]={0xbed2e1ffu,0x607080ffu,0x5ad7f5ffu,0x6eebaaffu,
        0xffbe50ffu,0xa5b9beffu,0x303d42ffu,0xe6ebf5ffu,0x5062d4ffu};
    if (role<0 || role>GRAPHICS_SPECTRUM_LOW) return 0;
    uint32_t color=fallback[role];
    int index=role==GRAPHICS_ACTIVE?14:role==GRAPHICS_INACTIVE?10:role==GRAPHICS_ONSET?3:
        role==GRAPHICS_SPECTRUM_LOW?4:16;
    if (palette_known&(1u<<index)) color=palette[index];
    else if (palette_known&(1u<<16)) color=palette[16];
    if (role==GRAPHICS_KEY_DARK && (palette_known&(1u<<17))) {
        uint32_t fg=palette_known&(1u<<16)?palette[16]:fallback[GRAPHICS_TEXT],bg=palette[17];
        color=0;
        for (int shift=8;shift<=24;shift+=8) color|=((((fg>>shift)&255)+3*((bg>>shift)&255))/4)<<shift;
    }
    return (color&0xffffff00u)|(alpha>255?255:alpha);
}

int terminal_graphics_probe(FILE *out) {
    return out && fprintf(out,"\033_Gi=%u,s=1,v=1,a=q,t=d,f=24;AAAA\033\\",GRAPHICS_QUERY_ID) > 0;
}

int terminal_graphics_reply(const char *reply) {
    char expected[48];
    snprintf(expected,sizeof(expected),"i=%u;OK",GRAPHICS_QUERY_ID);
    return reply && !strcmp(reply,expected);
}

static int valid(const TerminalImage *im) {
    return im && im->width > 0 && im->width <= GRAPHICS_WIDTH &&
        im->height > 0 && im->height <= GRAPHICS_HEIGHT;
}

static void pixel(TerminalImage *im, int x, int y, unsigned r, unsigned g, unsigned b, unsigned a) {
    if (x < 0 || x >= im->width || y < 0 || y >= im->height) return;
    unsigned char *p = im->rgba+((size_t)y*im->width+x)*4;
    p[0] = (unsigned char)r; p[1] = (unsigned char)g;
    p[2] = (unsigned char)b; p[3] = (unsigned char)a;
}

/* Tiny raster type stays legible after the terminal scales the image. */
static unsigned glyph_row(int c, int y) {
    static const unsigned char digits[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
    static const unsigned char letters[26][5] = {
        {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},
        {7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},
        {7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},
        {5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},{6,5,6,4,4},
        {2,5,5,7,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
        {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},
        {5,5,2,2,2},{7,1,2,4,7}};
    c = toupper((unsigned char)c);
    if (c >= '0' && c <= '9') return digits[c-'0'][y];
    if (c >= 'A' && c <= 'Z') return letters[c-'A'][y];
    if (c == '#') return y%2 ? 7 : 5;
    if (c == '-') return y == 2 ? 7 : 0;
    if (c == '+') return y == 2 ? 7 : (y == 1 || y == 3 ? 2 : 0);
    if (c == '.') return y == 4 ? 2 : 0;
    if (c == ':') return y == 1 || y == 3 ? 2 : 0;
    return 0;
}

static void label(TerminalImage *im, int x, int y, const char *text, int scale) {
    for (int i = 0; text && text[i] && i < 9; i++)
        for (int dy = 0; dy < 5; dy++) for (int dx = 0; dx < 3; dx++)
            if (glyph_row((unsigned char)text[i],dy)&(1u<<(2-dx)))
                for (int py = 0; py < scale; py++) for (int px = 0; px < scale; px++)
                    terminal_graphics_pixel(im,x+i*4*scale+dx*scale+px,y+dy*scale+py,terminal_graphics_theme(GRAPHICS_TEXT,255));
}

int terminal_graphics_note_label(int midi, char *out, size_t size) {
    static const char *names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (!out || !size) return 0;
    out[0] = 0;
    if (midi < 0 || midi > 127) return 0;
    int n = snprintf(out,size,"%s%d",names[midi%12],midi/12-1);
    if (n < 0 || (size_t)n >= size) { out[0] = 0; return 0; }
    return 1;
}

static int plot_left(const TerminalImage *im) { return !im->compact && im->width >= 96 ? GRAPHICS_AXIS_WIDTH : 0; }

static void background(TerminalImage *im) {
    int left = plot_left(im);
    memset(im->rgba,0,(size_t)im->width*im->height*4);
    for (int x = left; x < im->width; x++)
        pixel(im,x,im->height/2,100,150,150,75);
    for (int x = left; x < im->width; x += (im->width-left)/8+1)
        for (int y = 0; y < im->height; y++) pixel(im,x,y,100,150,150,35);
    if (left) for (int y = 0; y < im->height; y++) pixel(im,left-1,y,100,150,150,180);
}

static void amplitude_axis(TerminalImage *im, double limit) {
    int left = plot_left(im);
    int scale = im->height >= 30 ? 2 : 1, font_height = 5*scale;
    if (!left || im->height < 2*font_height) return;
    int ticks = im->height >= 6*font_height ? 4 : (im->height >= 3*font_height ? 2 : 1);
    for (int i = 0; i <= ticks; i++) {
        int y = i*(im->height-1)/ticks, text_y = y-(font_height-1)/2;
        if (text_y < 0) text_y = 0;
        if (text_y > im->height-font_height) text_y = im->height-font_height;
        double value = (1-2.0*i/ticks)*limit;
        char text[16];
        if (value == 0) snprintf(text,sizeof(text),"0");
        else snprintf(text,sizeof(text),"%+.2g",value);
        label(im,2,text_y,text,scale);
        for (int x = left; x < im->width; x++) pixel(im,x,y,100,150,150,55);
    }
}

void terminal_graphics_scope(TerminalImage *im, const float *samples, int n) {
    if (!valid(im)) return;
    background(im);
    if (!samples || n < 1) return;
    double peak = 0;
    for (int i = 0; i < n; i++)
        if (isfinite(samples[i]) && fabs(samples[i]) > peak) peak = fabs(samples[i]);
    double scale = peak > 1e-5 ? .9/peak : 1;
    amplitude_axis(im,1/scale);
    int left = plot_left(im), width = im->width-left;
    int previous = -1;
    for (int x = left; x < im->width; x++) {
        int begin = (int)((size_t)(x-left)*n/width), end = (int)((size_t)(x-left+1)*n/width);
        if (end <= begin) end = begin+1;
        int low = im->height-1, high = 0, last = im->height/2;
        for (int i = begin; i < end && i < n; i++) {
            double v = isfinite(samples[i]) ? fmax(-1,fmin(1,samples[i]*scale)) : 0;
            int y = (int)((1-v)*.5*(im->height-1));
            if (i == begin && previous >= 0) {
                if (previous < low) low = previous;
                if (previous > high) high = previous;
            }
            if (y < low) low = y;
            if (y > high) high = y;
            last = y;
        }
        for (int y = low; y <= high; y++) {
            pixel(im,x,y,110,235,170,255);
            pixel(im,x,y+1,110,235,170,100);
        }
        previous = last;
    }
}

typedef struct { const GraphicsNote *note; int lane; } RollNote;

static int roll_collect(const GraphicsNote *notes, int n, double now, RollNote *ordered) {
    int count = 0;
    if (n > GRAPHICS_NOTES) n = GRAPHICS_NOTES;
    for (int i = 0; i < n; i++) {
        const GraphicsNote *note = notes+i;
        if (!isfinite(note->start) || !isfinite(note->end) || note->start > now ||
            note->end < now-4 || note->end < note->start || note->pitch < 0 || note->pitch > 127) continue;
        int at = count;
        while (at > 0 && (ordered[at-1].note->pitch > note->pitch ||
               (ordered[at-1].note->pitch == note->pitch && ordered[at-1].note->start > note->start))) {
            ordered[at] = ordered[at-1]; at--;
        }
        ordered[at] = (RollNote){note,0}; count++;
    }
    return count;
}

static void roll_lanes(RollNote *ordered, int count, int lanes[128]) {
    double ends[GRAPHICS_NOTES];
    int pitch = -1, used = 0;
    for (int i = 0; i < count; i++) {
        const GraphicsNote *note = ordered[i].note;
        if (pitch != note->pitch) { pitch = note->pitch; used = 0; }
        int lane = 0;
        while (lane < used && ends[lane] > note->start) lane++;
        if (lane == used) used++;
        ends[lane] = note->end;
        ordered[i].lane = lane; lanes[pitch] = used;
    }
}

static void roll_bounds(const TerminalImage *im, const RollNote *item, int low, int high,
                        int lanes, int *y0, int *y1) {
    int range = high-low+1;
    int top = (high-item->note->pitch)*im->height/range;
    int bottom = (high-item->note->pitch+1)*im->height/range-1;
    if (bottom < top) bottom = top;
    int height = bottom-top+1, gap = height >= 2*lanes-1;
    int lane_height = (height-gap*(lanes-1))/lanes;
    if (lane_height < 1) lane_height = 1;
    *y0 = top+item->lane*(lane_height+gap);
    if (*y0 > bottom) *y0 = bottom;
    *y1 = *y0+lane_height-1;
    if (*y1 > bottom) *y1 = bottom;
}

void terminal_graphics_roll(TerminalImage *im, const GraphicsNote *notes, int n, double now) {
    if (!valid(im)) return;
    background(im);
    if (!notes || n < 0 || !isfinite(now)) return;
    RollNote ordered[GRAPHICS_NOTES]; int lanes[128] = {0};
    int count = roll_collect(notes,n,now,ordered);
    roll_lanes(ordered,count,lanes);
    int low = count ? ordered[0].note->pitch : 54;
    int high = count ? ordered[count-1].note->pitch : 66;
    if (high-low < 12) { int center = (low+high)/2; low = center-6; high = center+6; }
    if (low < 0) { high -= low; low = 0; }
    if (high > 127) { low -= high-127; high = 127; }
    int range = high-low+1;
    int left = plot_left(im);
    int minimum = (12*range+im->height-1)/im->height, step = 12;
    const int steps[] = {1,2,3,4,6,12};
    for (int i = 0; i < 6; i++) if (steps[i] >= minimum) { step = steps[i]; break; }
    if (minimum > 12) step = ((minimum+11)/12)*12;
    for (int p = low; p <= high; p++) {
        int y = (high-p)*im->height/range;
        for (int x = left; x < im->width; x++)
            pixel(im,x,y,100,150,150,p%12 == 0 ? 120 : 35);
        if (left) {
            int pc = (p%12+12)%12;
            int black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            int end = (high-p+1)*im->height/range;
            for (int ky = y; ky < end; ky++) for (int x = left-7; x < left-1; x++)
                pixel(im,x,ky,black ? 45 : 165,black ? 60 : 185,black ? 65 : 190,180);
            if (im->height >= 10 && p%step == 0) {
                char text[8];
                if (terminal_graphics_note_label(p,text,sizeof(text))) {
                    int ty = (y+end)/2-5;
                    if (ty < 0) ty = 0;
                    if (ty > im->height-10) ty = im->height-10;
                    int tx = left-10-(int)strlen(text)*8;
                    if (tx < 2) tx = 2;
                    label(im,tx,ty,text,2);
                }
            }
        }
    }
    for (int y = 0; y < im->height; y++) pixel(im,im->width-1,y,230,235,245,150);
    /* Paint onset markers last: another sustained note must not erase them. */
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < count; i++) {
        const GraphicsNote *note = ordered[i].note;
        int x0 = left+(int)(fmax(0,fmin(1,(note->start-(now-4))/4))*(im->width-left-1));
        int x1 = left+(int)(fmax(0,fmin(1,(note->end-(now-4))/4))*(im->width-left-1));
        int y0, y1; roll_bounds(im,&ordered[i],low,high,lanes[note->pitch],&y0,&y1);
        int active = note->end >= now;
        if (pass == 0) {
            for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++)
                pixel(im,x,y,active ? 90 : 110,active ? 215 : 235,active ? 245 : 170,y == y0 ? 255 : 190);
        } else if (note->start >= now-4) {
            for (int y = y0; y <= y1; y++) {
                pixel(im,x0,y,245,205,105,255);
                if (im->width > 400 && x0 < x1) pixel(im,x0+1,y,245,205,105,255);
            }
        }
    }
}

/* One 3072-byte raw chunk encodes to the protocol's 4096-byte limit. */
static size_t encode(const unsigned char *src, size_t n, char out[4096]) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)src[i]<<16;
        if (i+1 < n) v |= (unsigned)src[i+1]<<8;
        if (i+2 < n) v |= src[i+2];
        out[at++] = alphabet[v>>18]; out[at++] = alphabet[(v>>12)&63];
        out[at++] = i+1 < n ? alphabet[(v>>6)&63] : '=';
        out[at++] = i+2 < n ? alphabet[v&63] : '=';
    }
    return at;
}

int terminal_graphics_send(FILE *out, uint32_t id, const TerminalImage *im,
                           int row, int col, int columns, int rows, int first, int height) {
    if (!out || !id || !valid(im) || row < 0 || row >= 65535 || col < 0 || col >= 65535 ||
        columns < 1 || columns > 65535 || rows < 1 || rows > 65535 ||
        first < 0 || height < 1 || first > im->height-height) return 0;
    if (fprintf(out,"\033[%d;%dH",row+1,col+1) < 0) return 0;
    size_t total = (size_t)im->width*height*4, offset = (size_t)im->width*first*4;
    for (size_t at = 0; at < total;) {
        size_t n = total-at;
        if (n > 3072) n = 3072;
        int more = at+n < total;
        if (at == 0) {
            if (fprintf(out,"\033_Ga=T,t=d,f=32,s=%d,v=%d,i=%u,p=1,c=%d,r=%d,C=1,q=2,m=%d;",
                        im->width,height,id,columns,rows,more) < 0) return 0;
        } else if (fprintf(out,"\033_Gq=2,m=%d;",more) < 0) return 0;
        char encoded[4096]; size_t length = encode(im->rgba+offset+at,n,encoded);
        if (fwrite(encoded,1,length,out) != length || fputs("\033\\",out) == EOF) return 0;
        at += n;
    }
    return !ferror(out);
}

int terminal_graphics_delete(FILE *out, uint32_t id) {
    return out && id && fprintf(out,"\033_Ga=d,d=I,i=%u,q=2\033\\",id) > 0;
}

void terminal_graphics_pixel(TerminalImage *im, int x, int y, uint32_t color) {
    pixel(im,x,y,color>>24,(color>>16)&255,(color>>8)&255,color&255);
}
void terminal_graphics_text(TerminalImage *im, int x, int y, const char *text, int scale) {
    if (scale >= 1 && scale <= 2) label(im,x,y,text,scale);
}
void terminal_graphics_background(TerminalImage *im) { if (valid(im)) background(im); }
int terminal_graphics_plot_left(const TerminalImage *im) { return plot_left(im); }
