/*
 * main.c: entry point for biteye
 * Creation date: Mon Apr 14 08:27:48 2014
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>

#include <biteye.h>

/* TODO: mark bits to detect holes */
#define APPNAME        "biteye"
#define APPVERSION     "0.1"
#define FONT_SIZE      8

#define SCROLL_FIRST_SHOT   200000
#define SCROLL_PERIODICALLY  10000

#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT 768

#define INFO_TEXT_COLOR  ARGB (0xff, 0xff, 0xa5, 0x00)
#define WINDOW_BGCOLOR   ARGB (0xff, 0x00, 0x00, 0x00)
#define DATA_FGCOLOR     ARGB (0xff, 0x00, 0xff, 0x00)

int display_width  = DEFAULT_WIDTH;
int display_height = DEFAULT_HEIGHT;

int bit_frame_cols = 32;
int bit_frame_rows = 500;

size_t offset = 0;
int    dirty  = 1;
int    need_read = 1;

int    buf_offset;

int    advance_locked = 0; /* CHAPUZA */
int    repeating_action = 0;

char *bit_frame_buf;

FILE *fp;

int  timer_enabled;

void
setup_timer (void)
{
  struct itimerval scroll_itimer;
 
  if (timer_enabled)
    return;
     
  scroll_itimer.it_value.tv_sec  = SCROLL_FIRST_SHOT / 1000000;
  scroll_itimer.it_value.tv_usec = SCROLL_FIRST_SHOT % 1000000;
  
  scroll_itimer.it_interval.tv_sec  = SCROLL_PERIODICALLY / 1000000;
  scroll_itimer.it_interval.tv_usec = SCROLL_PERIODICALLY % 1000000;
  
  setitimer (ITIMER_REAL, &scroll_itimer, NULL);
  
  timer_enabled++;
}

void
disable_timer (void)
{
  struct itimerval scroll_itimer;
  
  if (!timer_enabled)
    return;
    
  scroll_itimer.it_value.tv_sec  = 0;
  scroll_itimer.it_value.tv_usec = 0;
  
  scroll_itimer.it_interval.tv_sec  = 0;
  scroll_itimer.it_interval.tv_usec = 0;
  
  setitimer (ITIMER_REAL, &scroll_itimer, NULL);
  
  timer_enabled = 0;
}

static inline const char *
filetype (mode_t mode)
{
  if (S_ISREG (mode))
    return "regular file";
  else if (S_ISDIR (mode))
    return "directory file";
  else if (S_ISCHR (mode))
    return "character device";
  else if (S_ISBLK (mode))
    return "block device";
  else if (S_ISFIFO (mode))
    return "pipe";
  else if (S_ISLNK (mode))
    return "symbolic link";
  else if (S_ISSOCK (mode))
    return "socket";
}

void
display_bit_frame (display_t *disp, int x, int y, int rows, int cols, int hl_start, int hl_end, const char *buf)
{
  int i, j, b, p;
  int bright;
  
  for (j = 0; j < rows; j++)
    for (i = 0; i < cols; i++)
    {
      p = i + j * cols;
      
      for (b = 7; b >= 0; b--)
      {
        bright = (((8 - b) << 4) + 63) * !! (buf[p] & (1 << b));
        
        pset_abs (disp, x + (i << 3) + b, y + j, 
          (p >= hl_start && p <= hl_end) ? ARGB (0xff, bright, 0, 0) : ARGB (0xff, bright, bright * 165 / 255, 0x00));
      }
    }
}


void
display_bit_frame_vert (display_t *disp, int x, int y, int rows, int cols, int hl_start, int hl_end, const char *buf)
{
  int i, j, b, p;
  int bright;
  
  for (j = 0; j < rows; j++)
    for (i = 0; i < cols; i++)
    {
      p = i * rows + j;
      
      for (b = 7; b >= 0; b--)
      {
        bright = (((8 - b) << 4) + 63) * !! (buf[p] & (1 << b));
        
        pset_abs (disp, x + (i << 3) + b, y + j, 
          (p >= hl_start && p <= hl_end) ? ARGB (0xff, bright, 0, 0) : ARGB (0xff, 0, bright, 0));
      }
    }
}

void
display_hd_frame (display_t *disp, int x, int y, int rows, int cols, const char *buf, off_t offset, off_t buf_offset)
{
  int i, j;
  int bright;
  
  for (j = 0; j < rows; j++)
  {
    display_printf (disp, x, y + j * FONT_SIZE, 
      INFO_TEXT_COLOR, WINDOW_BGCOLOR, 
      "%04x:%04x", 
      0xffff & ((offset + buf_offset + j * cols) >> 16),
      0xffff & (offset + buf_offset + j * cols));
      
    for (i = 0; i < cols; i++)
    {
      bright = (unsigned char) buf[buf_offset + i + j * cols] / 2 + 128;
      display_printf (disp, x + i * 24 + 8 * 10, y + j * FONT_SIZE, 
                      ARGB (0xff, bright, bright * 165 / 255, 0x00),
                      WINDOW_BGCOLOR, "%02x", 
                      (unsigned char) buf[buf_offset + i + j * cols]);
    
    }   
  } 
}


void
display_ascii_frame (display_t *disp, int x, int y, int rows, int cols, const char *buf)
{
  int i, j;
  int bright;
  
  for (j = 0; j < rows; j++)
    for (i = 0; i < cols; i++)
    {
      bright = (unsigned char) buf[buf_offset + i + j * cols] / 2 + 128;
      display_printf (disp, x + i * 8, y + j * FONT_SIZE, 
                      ARGB (0xff, bright, bright * 165 / 255, 0x00),
                      WINDOW_BGCOLOR, "%c", 
                      (unsigned char) buf[buf_offset + i + j * cols]);
    
    }   
    
}


void
repaint_layout (display_t *disp)
{
  clear (disp, OPAQUE (0));
  
  display_bit_frame (disp, 0, 48, bit_frame_rows, bit_frame_cols, buf_offset, buf_offset + 64 * 16 - 1, bit_frame_buf);
  display_hd_frame (disp, 260, 48, 64, 16, bit_frame_buf, offset, buf_offset);
  display_ascii_frame (disp, 720, 48, 64, 16, bit_frame_buf);
  
  display_bit_frame_vert (disp, 0, 570, 128, 128, buf_offset, buf_offset + 64 * 16 - 1, bit_frame_buf);
  
  display_refresh (disp);
}

void
parse_keysym (int sym)
{
  if (sym == SDLK_DOWN)
  {
    if (buf_offset >= (bit_frame_cols * bit_frame_rows - 64 * 16))
    {
      if (!advance_locked)
      {
        need_read++;
        offset += 4 * bit_frame_cols;
      }
    }
    else
      buf_offset += 4 * bit_frame_cols;
  }
  else if (sym == SDLK_UP)
  {
    if (buf_offset > 0)
      buf_offset -= 4 * bit_frame_cols;
    else
    {
      if (offset >= 4 * bit_frame_cols)
        offset -= 4 * bit_frame_cols;
      else
        offset = 0;
      
      need_read++;
      
    }
  }
  else if (sym == SDLK_PAGEDOWN && !advance_locked)
  {
    offset += bit_frame_cols * bit_frame_rows;
    need_read++;
  }
  else if (sym == SDLK_PAGEUP)
  {
    if (offset >= bit_frame_cols * bit_frame_rows)
      offset -= bit_frame_cols * bit_frame_rows;
    else
      offset = 0;
      
    need_read++;
  }
  else
    return;
    
  setup_timer ();
  dirty++;
}

int 
scroll_handler (int sym, struct display_info *di, struct event_info *ei)
{
  if (!ei->state)
  {
    disable_timer ();
    return;
  }
  
  repeating_action = sym;
  
  parse_keysym (sym);
}


void
break_wait (void)
{
  SDL_Event event;
  
  event.type = SDL_USEREVENT;
  event.user.code = 0;
  event.user.data1 = 0;
  event.user.data2 = 0;
  
  SDL_PushEvent (&event); 
}

void
sigalarm (int sig)
{
  parse_keysym (repeating_action);
  
  break_wait ();
}


void
display_file (display_t *disp, textarea_t *text, const char *path)
{
  
  struct stat sbuf;
  
  clear (disp, WINDOW_BGCOLOR);
  
  
  textarea_gotoxy (text, 0, 0);
  
  cputs (text, APPNAME " " APPVERSION " for SDL\n");
  
  if ((fp = fopen (path, "rb")) == NULL)
  {
    cprintf (text, "Can't open %s: %s\n", path, strerror (errno));
    cprintf (text, "Hit ENTER to continue . . .\n");
    
    return;
  }
  
  (void) fstat (fileno (fp), &sbuf); /* Can this even fail? */
  
  cprintf (text, "%s: %s (%d bytes in disk) - mode 0%o\n", 
    path, filetype (sbuf.st_mode), sbuf.st_size, sbuf.st_mode & 0x777);
    
  
  
  for (;;)
  {
    if (dirty)
    {
      dirty = 0;

      
      
      if (need_read)
      {
        memset (bit_frame_buf, 0, bit_frame_cols * bit_frame_rows);
        fseek (fp, offset, SEEK_SET);
        advance_locked = fread (bit_frame_buf, bit_frame_cols * bit_frame_rows, 1, fp) < 1;
        need_read = 0;
      }
      
      repaint_layout (disp);
    }
    
    display_wait_events (disp);
  }
}

void
usage (void)
{
  fprintf (stderr, "Usage:\n");
  fprintf (stderr, "\t" APPNAME " [OPTIONS] FILE\n\n");
  fprintf (stderr, APPNAME " displays a binary dump, hexdump and ASCII\n");
  fprintf (stderr, "dump of a given file inside a SDL window.\n\n");
  fprintf (stderr, "The main aim of this application is to fastly recognize\n");
  fprintf (stderr, "the structure and enthropy of a file by just looking to\n");
  fprintf (stderr, "the shapes of its bits.\n\n");
  fprintf (stderr, "OPTIONS:\n");
  fprintf (stderr, "  -W <width>        Set SDL window to <width>\n");
  fprintf (stderr, "  -H <height>       Set SDL window to <height>\n");
  fprintf (stderr, "  -b <num>          Set the bytes per bit row to <num>.\n");
  fprintf (stderr, "                    Note that this option overrides -W\n");
  fprintf (stderr, "  -f <font.cpi>     Set the window font to <font.cpi>\n");
  fprintf (stderr, "                    Only DOS CPI fonts are allowed\n");
  fprintf (stderr, "  -h                This help\n");
  fprintf (stderr, "\n");
  
  fprintf (stderr, "(c) 2011 BatchDrake <BatchDrake@gmail.com>\n");
  fprintf (stderr, 
    "Copyrighted but free, under the terms of the GPLv3 license\n");
}

int
main (int argc, char *argv[], char *envp[])
{
  int c;
  int err = 0;
  
  extern char *optarg;
  extern int   optind, optopt;
  
  char *cpifile = NULL;
  
  display_t *disp;
  textarea_t *text;
  
  while (!err && ((c = getopt (argc, argv, ":W:H:b:f:h")) != -1))
  {
    switch (c)
    {
      case ':':
        error ("option `-%c' requires an argument\n", optopt);
        err++;
        break;
        
      case '?':
        error ("unrecognized option `-%c'\n", optopt);
        err++;
        break;
        
      case 'h':
        usage ();
        return 0;
        
      case 'W':
        if (!sscanf (optarg, "%u", &display_width))
        {
          error ("invalid width\n");
          err++;
        }
        
        break;
        
      case 'H':
        if (!sscanf (optarg, "%u", &display_height))
        {
          error ("invalid height\n");
          err++;
        }
        
        break;
      
      case 'f':
        cpifile = optarg;
        break;
    }
    
    if (err)
    {
      usage ();
      exit (1);
    }
  }
  
  if (optind == argc)
  {
    error ("no file given\n");
    usage ();
    exit (1);
  }
  
  if ((disp = display_new (display_width, display_height)) == NULL)
  {
    error ("display_new failed :(\n");
    exit (1);
  }
  
  if (display_select_cpi (disp, cpifile) == -1)
    exit (1);
    
  /* Need options for this */
  if (display_select_font (disp, 850, FONT_SIZE) == -1) 
    exit (1);
  
  if ((text = display_textarea_new (disp, 0, 0, display_width / 8, 10, cpifile, 850, FONT_SIZE)) == NULL)
  {
    perror ("display_textarea_new");
    return -1;
  }
  
  textarea_set_fore_color (text, INFO_TEXT_COLOR);
  
  bit_frame_buf = xmalloc (bit_frame_cols * bit_frame_rows);
  
  display_register_key_handler (disp, SDLK_DOWN, scroll_handler);
  display_register_key_handler (disp, SDLK_UP,   scroll_handler);
  display_register_key_handler (disp, SDLK_PAGEDOWN, scroll_handler);
  display_register_key_handler (disp, SDLK_PAGEUP,   scroll_handler);
  
  signal (SIGALRM, sigalarm);
  
  display_file (disp, text, argv[optind]);
  
  display_end (disp);
  
  return 0;
}

