#include "os-update.h"
#include "minui/minui.h"

#include <stdio.h>
#include <assert.h>

#define MARGIN 10

const char logo_filename[] = "test";

gr_surface logo;

int osUpdateScreenInit(){
  if ( gr_init() ) {
    printf("Failed gr_init!\n");
    return -1;
  }

  // Clear the screen
  gr_color(0,0,0,255);
  gr_clear();

  return 0;
}

int loadLogo(const char* filename){
  if (logo) {
    res_free_surface(logo);
  }

  int ret = res_create_display_surface(filename, &logo);
  if (ret < 0) {
    printf("Error while trying to load %s, retval: %i.\n", filename, ret);
    return -1;
  }
  return 0;
}

int showLogo()
{
  int fbw = gr_fb_width();
  int fbh = gr_fb_height();

  /* draw logo to middle of the screen */
  if (logo) {
    int logow = gr_get_width(logo);
    int logoh = gr_get_height(logo);
    int dx = (fbw - logow) / 2;
    int dy = (fbh - logoh) / 2;

    gr_blit(logo, 0, 0, logow, logoh, dx, dy);
    gr_flip();
  } else {
    printf("No logo loaded\n");
    return -1;
  }

  return 0;
}

void osUpdateScreenShowProgress(int percentage){

  int fbw = gr_fb_width();
  int fbh = gr_fb_height();

  int splitpoint = (fbw-2*MARGIN)*percentage/100;

  assert( splitpoint >= 0 );
  assert( splitpoint <= fbw );

  int x1 = MARGIN;
  int y1 = fbh/2+MARGIN;
  int x2 = MARGIN+splitpoint;
  int y2 = fbh/2+20;

  /* white color for the beginning of the progressbar */
  gr_color(255,255,255,255);

  gr_fill(x1,y1,x2,y2);

  /* Grey color for the end part of the progressbar */
  gr_color(84,84,84,255);

  x1 = MARGIN+splitpoint;
  x2 = fbw-MARGIN;

  gr_fill(x1,y1,x2,y2);
  
  /* draw logo on the top of the progress bar if it is loaded */
  if (logo) {
    int logow = gr_get_width(logo);
    int logoh = gr_get_height(logo);
    int dx = (fbw - logow)/2;
    int dy = (fbh/2 - logoh - 2 * MARGIN);
#ifdef DEBUG
    printf("width: %i, height: %i, row_bytes: %i, pixel_bytes: %i\n", logo->width, logo->height, logo->row_bytes, logo->pixel_bytes);
#endif
    gr_blit(logo, 0, 0, logow, logoh, dx, dy);
  }

  /* And finally draw everything */
  gr_flip();
}

void osUpdateScreenExit() {
  if (logo) {
    res_free_surface(logo);
  }
  gr_exit();
}

