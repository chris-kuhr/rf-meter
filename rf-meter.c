/*
 * rf-meter is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * rf-meter is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <inttypes.h>

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <sys/time.h>
#include "pink.h"

static char              *device      = "hw:0,0";       /* playback device */
static snd_pcm_format_t   format      = SND_PCM_FORMAT_S16; /* sample format */
static unsigned int       rate        = 48000;	            /* stream rate */
static unsigned int       channels    = 2;	            /* count of channels */
static unsigned int       buffer_time = 500000;	            /* ring buffer length in us */
static unsigned int       period_time = 100000;	            /* period time in us */
#define PERIODS 4
static pink_noise_t pink;
static snd_output_t      *output      = NULL;
static snd_pcm_uframes_t  buffer_size;
static snd_pcm_uframes_t  period_size;


static int set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *params, snd_pcm_access_t access) {
  unsigned int rrate;
  int          err, dir;
  snd_pcm_uframes_t     period_size_min;
  snd_pcm_uframes_t     period_size_max;
  snd_pcm_uframes_t     buffer_size_min;
  snd_pcm_uframes_t     buffer_size_max;
  snd_pcm_uframes_t     buffer_time_to_size;

  /* choose all parameters */
  err = snd_pcm_hw_params_any(handle, params);
  if (err < 0) {
    printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
    return err;
  }

  /* set the interleaved read/write format */
  err = snd_pcm_hw_params_set_access(handle, params, access);
  if (err < 0) {
    printf("Access type not available for playback: %s\n", snd_strerror(err));
    return err;
  }

  /* set the sample format */
  err = snd_pcm_hw_params_set_format(handle, params, format);
  if (err < 0) {
    printf("Sample format not available for playback: %s\n", snd_strerror(err));
    return err;
  }

  /* set the count of channels */
  err = snd_pcm_hw_params_set_channels(handle, params, channels);
  if (err < 0) {
    printf("Channels count (%i) not available for playbacks: %s\n", channels, snd_strerror(err));
    return err;
  }

  /* set the stream rate */
  rrate = rate;
  err = snd_pcm_hw_params_set_rate(handle, params, rate, 0);
  if (err < 0) {
    printf("Rate %iHz not available for playback: %s\n", rate, snd_strerror(err));
    return err;
  }

  if (rrate != rate) {
    printf("Rate doesn't match (requested %iHz, get %iHz, err %d)\n", rate, rrate, err);
    return -EINVAL;
  }

  /* set the buffer time */
  buffer_time_to_size = ( (snd_pcm_uframes_t)buffer_time * rate) / 1000000;
  err = snd_pcm_hw_params_get_buffer_size_min(params, &buffer_size_min);
  err = snd_pcm_hw_params_get_buffer_size_max(params, &buffer_size_max);
  dir=0;
  err = snd_pcm_hw_params_get_period_size_min(params, &period_size_min,&dir);
  dir=0;
  err = snd_pcm_hw_params_get_period_size_max(params, &period_size_max,&dir);
  printf("Buffer size range from %lu to %lu\n",buffer_size_min, buffer_size_max);
  printf("Period size range from %lu to %lu\n",period_size_min, period_size_max);
  printf("Periods = %d\n", PERIODS);
  printf("Buffer time size %lu\n",buffer_time_to_size);

  buffer_size = buffer_time_to_size;
  //buffer_size=8096;
  buffer_size=15052;
  if (buffer_size_max < buffer_size) buffer_size = buffer_size_max;
  if (buffer_size_min > buffer_size) buffer_size = buffer_size_min;
  //buffer_size=0x800;
  period_size = buffer_size/PERIODS;
  buffer_size = period_size*PERIODS;
  //period_size = 510;
  printf("To choose buffer_size = %lu\n",buffer_size);
  printf("To choose period_size = %lu\n",period_size);
  dir=0;
  err = snd_pcm_hw_params_set_period_size_near(handle, params, &period_size, &dir);
  if (err < 0) {
    printf("Unable to set period size %lu for playback: %s\n", period_size, snd_strerror(err));
    return err;
  }
  dir=0;
  err = snd_pcm_hw_params_get_period_size(params, &period_size, &dir);
  if (err < 0)  printf("Unable to get period size for playback: %s\n", snd_strerror(err));
                                                                                                                             
  dir=0;
  err = snd_pcm_hw_params_set_buffer_size_near(handle, params, &buffer_size);
  if (err < 0) {
    printf("Unable to set buffer size %lu for playback: %s\n", buffer_size, snd_strerror(err));
    return err;
  }
  err = snd_pcm_hw_params_get_buffer_size(params, &buffer_size);
  printf("was set period_size = %lu\n",period_size);
  printf("was set buffer_size = %lu\n",buffer_size);
  if (2*period_size > buffer_size) {
    printf("buffer to small, could not use\n");
    return err;
  }


  /* write the parameters to device */
  err = snd_pcm_hw_params(handle, params);
  if (err < 0) {
    printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
    return err;
  }

  return 0;
}

static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams) {
  int err;

  /* get the current swparams */
  err = snd_pcm_sw_params_current(handle, swparams);
  if (err < 0) {
    printf("Unable to determine current swparams for playback: %s\n", snd_strerror(err));
    return err;
  }

  /* start the transfer when a buffer is full */
  err = snd_pcm_sw_params_set_start_threshold(handle, swparams, buffer_size);
  if (err < 0) {
    printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
    return err;
  }

  /* allow the transfer when at least period_size frames can be processed */
  err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size);
  if (err < 0) {
    printf("Unable to set avail min for playback: %s\n", snd_strerror(err));
    return err;
  }

  /* align all transfers to 1 sample */
  err = snd_pcm_sw_params_set_xfer_align(handle, swparams, 1);
  if (err < 0) {
    printf("Unable to set transfer align for playback: %s\n", snd_strerror(err));
    return err;
  }

  /* write the parameters to the playback device */
  err = snd_pcm_sw_params(handle, swparams);
  if (err < 0) {
    printf("Unable to set sw params for playback: %s\n", snd_strerror(err));
    return err;
  }

  return 0;
}

/*
 *   Underrun and suspend recovery
 */

static int xrun_recovery(snd_pcm_t *handle, int err) {
  if (err == -EPIPE) {	/* under-run */
    err = snd_pcm_prepare(handle);
    if (err < 0)
      printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
    return 0;
  } 
  else if (err == -ESTRPIPE) {

    while ((err = snd_pcm_resume(handle)) == -EAGAIN)
      sleep(1);	/* wait until the suspend flag is released */

    if (err < 0) {
      err = snd_pcm_prepare(handle);
      if (err < 0)
        printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
    }

    return 0;
  }

  return err;
}


unsigned int read_rf_level(){
        int rf_read, i=0;
        FILE *rf_line;
        char line[200], rf_read_char[3];

        //      Read RF Level from /proc/net/wireless
        if((rf_line = fopen("/proc/net/wireless","r")) == NULL) printf("fopen error\n");
        while(fgets(line, 200, rf_line)!=NULL){
                if(i==2){
                        rf_read_char[0] = line[21];
                        rf_read_char[1] = line[22];
                        rf_read_char[2] = '\0';
                }
                i++;
        }
        if(fclose(rf_line)==-1) printf("fclose error");

//	if(i<2) return 0;

	rf_read = atoi(rf_read_char);
//        printf("%d\n", rf_read);

//      return (0xA3D70A * rf_read);   // 0x3FFFFFFF / 100 = 0xA3D70A
//	return (0x147AE14 * rf_read);   // 0x7FFFFFFF / 100 = 0x147AE14
	return (0x28F5C28 * rf_read);   // 0xFFFFFFFF / 100 = 0x28F5C28

}


static void generate_pink_noise( uint8_t *frames, unsigned int rf_level, int count, int adjusting) {
        double res;
        int32_t  ires;
        int16_t *samp16 = (int16_t*) frames;
	int i=0;

        while (count-- > 0) {
		if(adjusting==0) res = generate_pink_noise_sample(&pink) * rf_level;
		else if(adjusting==1) res = generate_pink_noise_sample(&pink) * 0xFFFFFFFF;
       	 	ires = res;
               	*samp16++ = ires >>16;
               	*samp16++ = ires >>16;
	}
}


/*
 *   Transfer method - write only
 */
static int write_loop(snd_pcm_t *handle, int periods, uint8_t *frames, int adjusting) {
	uint8_t *ptr;
	int err, cptr, n;
	int bytes_per_frame=snd_pcm_frames_to_bytes(handle, 1);

	for(n = 0; n < periods; n++) {
		unsigned int rf_level = read_rf_level();
//		printf("%x\n", rf_level);
		generate_pink_noise(frames, rf_level, period_size, adjusting);
      
		ptr = frames;
		cptr = period_size;

		while (cptr > 0) {

			err = snd_pcm_writei(handle, ptr, cptr);

			if (err == -EAGAIN)
				continue;

			if (err < 0) {
				printf("Write error: %d,%s\n", err, snd_strerror(err));
				if (xrun_recovery(handle, err) < 0) {
					printf("xrun_recovery failed: %d,%s\n", err, snd_strerror(err));
					return -1;
				}
				break;	/* skip one period */
			}

			ptr += (err * bytes_per_frame);
			cptr -= err;
		}
	}
	return 0;
}


int main(int argc, char *argv[]) {
  snd_pcm_t            *handle;
  int err, adjusting=0;
  snd_pcm_hw_params_t  *hwparams;
  snd_pcm_sw_params_t  *swparams;
  uint8_t              *frames;


	int i=0;
	char line[200];
	FILE *fd;
	// Check for wlan0, if not available terminate program
        //      Read RF Level from /proc/net/wireless
        if((fd = popen("ifconfig | grep wlan0","r")) == NULL) printf("init fopen error\n");
        if(fgets(line, 200, fd)==NULL){
		printf("no wlan0 available, exiting...\n");
		exit(EXIT_FAILURE);
	}
        if(pclose(fd)==-1) printf("init fclose error");

  snd_pcm_hw_params_alloca(&hwparams);
  snd_pcm_sw_params_alloca(&swparams);

  err = snd_output_stdio_attach(&output, stdout, 0);
  if (err < 0) {
    printf("Output failed: %s\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  printf("Stream parameters are %iHz, %s, %i channels\n", rate, snd_pcm_format_name(format), channels);

loop:
  while ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
    printf("Playback open error: %d,%s\n", err,snd_strerror(err));
    sleep(1);
  }

  if ((err = set_hwparams(handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    printf("Setting of hwparams failed: %s\n", snd_strerror(err));
    snd_pcm_close(handle);
    goto loop;
    exit(EXIT_FAILURE);
  }
  //getchar();
  if ((err = set_swparams(handle, swparams)) < 0) {
    printf("Setting of swparams failed: %s\n", snd_strerror(err));
    snd_pcm_close(handle);
    goto loop;
    exit(EXIT_FAILURE);
  }

  frames = malloc((period_size * channels * snd_pcm_format_width(format)) / 8);
  initialize_pink_noise( &pink, 16);
  
  if (frames == NULL) {
    printf("No enough memory\n");
    exit(EXIT_FAILURE);
  }
//printf("%d \n", argc);
if(argc==2) {
adjusting = atoi(argv[1]);
//printf("%d\n", adjusting);
}
while (1) {
err = write_loop(handle, ((rate*3)/period_size), frames, adjusting);

if (err < 0) {
printf("Transfer failed: %s\n", snd_strerror(err));
free(frames);
snd_pcm_close(handle);
printf("Pausing\n");
goto loop ;
//pause();
//printf("Done Pausing\n");
exit(EXIT_SUCCESS);
goto loop ;
}
}


free(frames);
snd_pcm_close(handle);

exit(EXIT_SUCCESS);
}
