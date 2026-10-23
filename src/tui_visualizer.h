/* Foreground-only, bounded fullscreen feedback. No source evaluation. */
#define FULLSCREEN_IMAGE_ID (GRAPHICS_IMAGE_ID+GRAPHICS_IMAGES)
static int fullscreen_images;
static TerminalImage fullscreen_image[2];

static void fullscreen_clear(void) {
    if (fullscreen_images) {
        for (unsigned i=0;i<2;i++)
            terminal_graphics_delete(terminal_output,FULLSCREEN_IMAGE_ID+i);
        fullscreen_images=0;
    }
}

static void fullscreen_text(const TerminalImage *im,int top,int height,int width,int role) {
    static const unsigned dots[4][2]={{1,8},{2,16},{4,32},{64,128}};
    terminal_use_role(role);
    for (int y=0;y<height;y++) {
        terminal_move(top+y,0);
        for (int x=0;x<width;x++) {
            unsigned bits=0;
            for (int dy=0;dy<4;dy++) for (int dx=0;dx<2;dx++) {
                int px=(int)((long long)(x*2+dx)*im->width/(width*2));
                int py=(int)((long long)(y*4+dy)*im->height/(height*4));
                if (im->rgba[((size_t)py*im->width+px)*4+3]>100) bits|=dots[dy][dx];
            }
            terminal_putc(viz_utf8?0x2800+bits:(bits?'#':' '));
        }
    }
    terminal_style_reset();
}

static void draw_fullscreen_viz(void) {
    int height=rows-1, upper=height*2/3, lower=height-upper;
    for (int y=0;y<rows;y++) blank_inner(y,0,cols);
    draw_text(0,0,"Visualizer  |  F12 / Esc: back  |  Ctrl+P: play / stop",cols,6);
    if (cols<2 || upper<1 || lower<1) {
        fullscreen_clear();
        notcurses_cursor_disable(terminal_nc);
        terminal_present(); fflush(terminal_output); return;
    }
    NoteHist hist[NOTE_HIST_N];
    GraphicsNote notes[GRAPHICS_NOTES];
    int n=note_hist_copy(hist,NOTE_HIST_N), count=0;
    for (int i=0;i<n && count<GRAPHICS_NOTES;i++)
        notes[count++]=(GraphicsNote){hist[i].t0,hist[i].t0+hist[i].dur,hist[i].midi};
    float samples[2048];
    audio_scope_copy(samples,2048);
    for (int i=0;i<2;i++) {
        TerminalImage *im=&fullscreen_image[i];
        im->width=cols>GRAPHICS_WIDTH/2?GRAPHICS_WIDTH:cols*2;
        int h=i?lower:upper;
        im->height=h>GRAPHICS_HEIGHT/4?GRAPHICS_HEIGHT:h*4;
        im->compact=1;
    }
    terminal_graphics_roll(&fullscreen_image[0],notes,count,audio_time_now());
    terminal_graphics_scope(&fullscreen_image[1],samples,2048);
    for (int i=0;i<2;i++) {
        TerminalImage *im=&fullscreen_image[i];
        uint32_t color=ncchannels_fg_rgb(terminal_colors[i?4:3]);
        for (int p=0;p<im->width*im->height;p++) {
            unsigned char *pixel=im->rgba+(size_t)p*4;
            pixel[0]=(unsigned char)(color>>16);
            pixel[1]=(unsigned char)(color>>8);
            pixel[2]=(unsigned char)color;
        }
    }
    int kitty=graphics_supported && !graphics_disabled;
    if (!kitty) {
        fullscreen_clear();
        fullscreen_text(&fullscreen_image[0],1,upper,cols,3);
        fullscreen_text(&fullscreen_image[1],1+upper,lower,cols,4);
    }
    notcurses_cursor_disable(terminal_nc);
    terminal_present();
    if (kitty) {
        for (unsigned i=0;i<2;i++) {
            fullscreen_images=1;
            if (!terminal_graphics_send(terminal_output,FULLSCREEN_IMAGE_ID+i,
                    &fullscreen_image[i],i?1+upper:1,0,cols,i?lower:upper,0,
                    fullscreen_image[i].height)) {
                graphics_supported=0;
                fullscreen_clear();
                break;
            }
        }
    }
    fflush(terminal_output);
}
