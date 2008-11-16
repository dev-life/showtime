/*
 *  Showtime mediacenter
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <sys/time.h>
#include <sys/resource.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>

#include <libhts/htsprop.h>

#include <libglw/glw.h>

#include "showtime.h"
#include "event.h"

#include "video/video_decoder.h"
#include "display/display.h"
#include "audio/audio.h"
#include "layout/layout.h"
#include "fileaccess/fileaccess.h"
#include "fileaccess/fa_imageloader.h"
#include "fileaccess/fa_rawloader.h"
#include "navigator.h"
#include "settings.h"

pthread_mutex_t ffmutex;

int frame_duration;

int64_t wallclock;
time_t walltime;
int concurrency;
extern char *htsversion;
hts_prop_t *prop_ui_scale;
int showtime_running;
static int stopcode;

const char *themepath = HTS_CONTENT_PATH "/showtime/themes/new";

static int main_event_handler(glw_event_t *ge, void *opaque);


static void
ffmpeglockmgr(int lock)
{
  if(lock)
    fflock();
  else
    ffunlock();
}


hts_prop_t *prop_sec;
hts_prop_t *prop_hour;
hts_prop_t *prop_minute;
hts_prop_t *prop_weekday;
hts_prop_t *prop_month;
hts_prop_t *prop_date;
hts_prop_t *prop_day;

/**
 *
 */
static void *
propupdater(void *aux)
{
  char buf[30];

  time_t now;
  struct tm tm;

  while(1) {

    time(&now);

    localtime_r(&now, &tm);

    hts_prop_set_int(prop_sec, tm.tm_sec);
    hts_prop_set_int(prop_hour, tm.tm_hour);
    hts_prop_set_int(prop_minute, tm.tm_min);
    hts_prop_set_int(prop_day, tm.tm_mday);

    strftime(buf, sizeof(buf), "%A", &tm);
    hts_prop_set_string(prop_weekday, buf);

    strftime(buf, sizeof(buf), "%B", &tm);
    hts_prop_set_string(prop_month, buf);

    strftime(buf, sizeof(buf), "%x", &tm);
    hts_prop_set_string(prop_date, buf);

    sleep(1);
  }
}



/**
 *
 */
hts_prop_t *prop_cloner;
hts_prop_t *prop_cloner2;
static void *
clonertest(void *aux)
{
  hts_prop_t *a, *b, *c;

  while(1) {

    a = hts_prop_create(prop_cloner, "hej");
    hts_prop_set_string(hts_prop_create(a, "name"), "jag e a");

    sleep(1);

    b = hts_prop_create(prop_cloner, "hej2");
    hts_prop_set_string(hts_prop_create(b, "name"), "jag e b");

    sleep(1);

    c = hts_prop_create(prop_cloner, "hej3");
    hts_prop_set_string(hts_prop_create(c, "name"), "jag e c");

    sleep(1);
    hts_prop_destroy(b);

    sleep(1);
    hts_prop_destroy(a);

    if(prop_cloner2 == NULL) {
      prop_cloner2 = hts_prop_create(hts_prop_get_global(), "clonertest2");
      hts_prop_link(prop_cloner, prop_cloner2);
    }

    sleep(1);
    hts_prop_destroy(c);

    sleep(1);
  }
}


/**
 *
 */
static void
global_prop_init(void)
{
  hts_prop_t *p;
  hts_prop_t *cpu;
  hts_thread_t tid;

  prop_ui_scale = hts_prop_create(hts_prop_get_global(), "uiscale");

  prop_cloner = hts_prop_create(hts_prop_get_global(), "clonertest");
  //  prop_cloner2 = hts_prop_create(hts_prop_get_global(), "clonertest2");




  p = hts_prop_create(hts_prop_get_global(), "version");
  hts_prop_set_string(p, htsversion);

  cpu = hts_prop_create(hts_prop_get_global(), "cpu");
  p = hts_prop_create(cpu, "cores");
  hts_prop_set_float(p, concurrency);


  /* */
  p = hts_prop_create(hts_prop_get_global(), "clock");

  prop_sec = hts_prop_create(p, "sec");
  prop_hour = hts_prop_create(p, "hour");
  prop_minute = hts_prop_create(p, "minute");
  prop_weekday = hts_prop_create(p, "weekday");
  prop_month = hts_prop_create(p, "month");
  prop_date = hts_prop_create(p, "date");
  prop_day = hts_prop_create(p, "day");

  hts_thread_create_detached(&tid, propupdater, NULL);

  if(0)hts_thread_create_detached(&tid, clonertest, NULL);

}


/*
 *
 */
int
main(int argc, char **argv)
{
  int c;
  struct timeval tv;
  const char *settingspath = NULL;
  struct rlimit rlim;

#ifdef RLIMIT_AS
  getrlimit(RLIMIT_AS, &rlim);
  rlim.rlim_cur = 512 * 1024 * 1024;
  setrlimit(RLIMIT_AS, &rlim);
#endif

#ifdef RLIMIT_DATA
  getrlimit(RLIMIT_DATA, &rlim);
  rlim.rlim_cur = 512 * 1024 * 1024;
  setrlimit(RLIMIT_DATA, &rlim);
#endif

  gettimeofday(&tv, NULL);
  srand(tv.tv_usec);

  while((c = getopt(argc, argv, "s:t:")) != -1) {
    switch(c) {
    case 's':
      settingspath = optarg;
      break;
    case 't':
      themepath = optarg;
      break;
    }
  }


  concurrency = hts_get_system_concurrency();

  hts_prop_init();

  global_prop_init();

  hts_settings_init("showtime", settingspath);

  settings_init();

  event_init();

  hts_mutex_init(&ffmutex);
  av_log_set_level(AV_LOG_ERROR);
  av_register_all();

  fileaccess_init();

  gl_sysglue_init(argc, argv);

  if(glw_init(ffmpeglockmgr, fa_imageloader, fa_rawloader, fa_rawunload,
	      concurrency)) {
    fprintf(stderr, "libglw user interface failed to initialize, exiting\n");
    exit(0);
  }

  nav_init();

  audio_init();

  vd_init();

  layout_create();

  event_handler_register("main", main_event_handler, EVENTPRI_MAIN, NULL);

  //  apps_load();

  showtime_running = 1;

  if(optind < argc)
    nav_open(argv[optind]);
  else
    nav_open("mainmenu://");

  gl_sysglue_mainloop();

  return stopcode;
}

/**
 * Catch buttons used for exiting
 */
static int
main_event_handler(glw_event_t *ge, void *opaque)
{
  switch(ge->ge_type) {
  default:
    return 0;

  case EVENT_KEY_QUIT:
    stopcode = 0;
    break;

  case EVENT_KEY_POWER:
    stopcode = 10;
    break;
  }
  showtime_running = 0;
  return 1;
}
