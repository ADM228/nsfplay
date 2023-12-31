/* demo application for
 * 1. loading an NSF/NSFe or playlist line
 * 2. probing track/time info
 * 3. converting to WAV
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <string.h>

#include <algorithm>
#include <memory>

#include "../xgm/xgm.h"

namespace {

struct Nsf2WavOptions;
void pack_int16le(uint8_t *d, int16_t n);
void pack_uint16le(uint8_t *d, uint16_t n);
void pack_uint32le(uint8_t *d, uint32_t n);
void pack_frames(uint8_t *d, int16_t *s, unsigned int frameCount, int channels);
int write_wav_header(FILE *f, uint64_t totalFrames, const Nsf2WavOptions &options);
int write_frames(FILE *f, uint8_t *d, unsigned int frameCount, int channels);

const char *progname;

constexpr const uint64_t kFramesToBuffer = 4096;

struct Nsf2WavOptions {
    Nsf2WavOptions(const xgm::NSF &nsf)
        : length_ms(nsf.default_playtime),
          fade_ms(nsf.default_fadetime)
    {}

    int32_t length_ms;
    int32_t fade_ms;
    int channels = 1;
    double samplerate = xgm::DEFAULT_RATE;
    int track = 1;
    bool quiet = false;
    uint64_t mask = 0;
	uint64_t mute = 0;
    bool trigger = false;
	bool lengthForce = false;
};

void Usage(FILE *output, int exit_code, const xgm::NSF &nsf) {
    Nsf2WavOptions defaults(nsf);
    fprintf(
        output,
        R"(Usage: %s [options] (/path/to/nsf[e] | nez m3u entry) [out.wav]
Convert an NSF[e] file to WAV.

The file to convert can either be a path to an NSF or NSFe file, or can be a
Nez M3U playlist entry. See http://www.vgmpf.com/Wiki/index.php/NEZ_Plug for
the Nez M3U format.

If no output file is specified, nsf2wav will print information about the NSF
to the screen and then exit without performing any conversion.

Options:
 -c, --channels=%-8d The number of audio channels to output.
 -f, --fade_ms=%-9d The length of time in milliseconds to fade out at the
                         end of the song.
 -h, --help              Show this help message.
 -l, --length_ms=%-7d The length in milliseconds to output. The final file
                         may be shorter than specified if the NSF program
                         terminates before outputting the specified amount of
                         audio.
 -y, --length_force	 Force the NSF to output the specified amount of audio
			 even if it loops/ends earlier.
 -q, --quiet             Suppress all non-error output.
 -s, --samplerate=%-6.0f The audio sample rate.
 -t, --track=%-11d Track number, starting with 1.
 -m, --mask=<number>	 Mute a certain channel (starting with 2A03 Pulse 1 = 0) by masking.
 -r, --mask_reverse	 Invert channel masking options to be soloing channels instead.
 -u, --mute		 Use the masking settings set so far as muting, reset masking options.
 -w, --trigger		 Output trigger waves instead of normal output.
)",
        progname, defaults.channels, defaults.fade_ms, defaults.length_ms,
        defaults.samplerate, defaults.track);
    exit(exit_code);
}

Nsf2WavOptions ParseOptions(int *argc, char ***argv, const xgm::NSF &nsf) {
    static constexpr struct option longopts[] = {
        { "help", no_argument, nullptr, 'h' },
        { "length_ms", required_argument, nullptr, 'l' },
		{ "length_force", no_argument, nullptr, 'y' },
        { "fade_ms", required_argument, nullptr, 'f' },
        { "track", required_argument, nullptr, 't' },
        { "samplerate", required_argument, nullptr, 's' },
        { "channels", required_argument, nullptr, 'c' },
        { "quiet", no_argument, nullptr, 'q' },
        { "mask", required_argument, nullptr, 'm' },
        { "mask_reverse", no_argument, nullptr, 'r' },
		{ "mute", no_argument, nullptr, 'u' },
        { "trigger", no_argument, nullptr, 'w' },
        { nullptr, 0, nullptr, 0 }
    };
    Nsf2WavOptions options(nsf);
    int ch = 0;
    while ((ch = getopt_long(*argc, *argv, "hl:s:f:c:", longopts, NULL)) != -1) {
        switch (ch) {
        case 'q':
            options.quiet = true;
            break;
        case 'l':
            options.length_ms = std::stoi(optarg);
            break;
        case 't':
            options.track = std::stoi(optarg);
            break;
		case 'y':
			options.lengthForce = true;
			break;
        case 'f':
            options.fade_ms = std::stoi(optarg);
            break;
        case 's':
            options.samplerate = std::stod(optarg);
            break;
        case 'c':
            options.channels = std::stoi(optarg);
            break;
        case 'm':
            options.mask |= 1<<std::stoi(optarg);
            break;
        case 'r':
            options.mask ^= 0xFFFFFFFF; // will be 0x7FFFFFFFFFFF when EPSM gets implemented
            break;
        case 'w':
            options.trigger = true;
            break;
		case 'u':
            options.mute = options.mask;
			options.mask = 0;
            break;
        case 'h':
            Usage(stdout, EXIT_SUCCESS, nsf);
        default:
            Usage(stderr, EX_USAGE, nsf);
        }
    }
    *argc -= optind;
    *argv += optind;
    return options;
}

void pack_int16le(uint8_t *d, int16_t n) {
    d[0] = (uint8_t)(n      );
    d[1] = (uint8_t)(n >> 8 );
}

void pack_uint16le(uint8_t *d, uint16_t n) {
    d[0] = (uint8_t)(n      );
    d[1] = (uint8_t)(n >> 8 );
}

void pack_uint32le(uint8_t *d, uint32_t n) {
    d[0] = (uint8_t)(n      );
    d[1] = (uint8_t)(n >> 8 );
    d[2] = (uint8_t)(n >> 16);
    d[3] = (uint8_t)(n >> 24);
}

int write_wav_header(FILE *f, uint64_t totalFrames, const Nsf2WavOptions &options) {
    unsigned int dataSize = totalFrames * sizeof(int16_t) * options.channels;
    uint8_t tmp[4];
    if(fwrite("RIFF",1,4,f) != 4) return 0;
    pack_uint32le(tmp,dataSize + 44 - 8);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    if(fwrite("WAVE",1,4,f) != 4) return 0;
    if(fwrite("fmt ",1,4,f) != 4) return 0;

    pack_uint32le(tmp,16); /*fmtSize */
    if(fwrite(tmp,1,4,f) != 4) return 0;

    pack_uint16le(tmp,1); /* audioFormat */
    if(fwrite(tmp,1,2,f) != 2) return 0;

    pack_uint16le(tmp,options.channels); /* numChannels */
    if(fwrite(tmp,1,2,f) != 2) return 0;

    pack_uint32le(tmp,options.samplerate);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    pack_uint32le(tmp,options.samplerate * options.channels * sizeof(int16_t));
    if(fwrite(tmp,1,4,f) != 4) return 0;

    pack_uint16le(tmp,options.channels * sizeof(int16_t));
    if(fwrite(tmp,1,2,f) != 2) return 0;

    pack_uint16le(tmp,sizeof(int16_t) * 8);
    if(fwrite(tmp,1,2,f) != 2) return 0;

    if(fwrite("data",1,4,f) != 4) return 0;

    pack_uint32le(tmp,dataSize);
    if(fwrite(tmp,1,4,f) != 4) return 0;

    return 1;
}

void pack_frames(uint8_t *d, int16_t *s, unsigned int frameCount, int channels) {
    unsigned int i = 0;
    while(i<frameCount) {
        pack_int16le(&d[0],s[(i*channels)+0]);
        if (channels == 2) {
            pack_int16le(&d[sizeof(int16_t)],s[(i*channels)+1]);
        }
        i++;
        d += (sizeof(int16_t) * channels);
    }
}

int write_frames(FILE *f, uint8_t *d, unsigned int frameCount, int channels) {
    return fwrite(d,sizeof(int16_t) * channels,frameCount,f) == frameCount;
}

}  // namespace

int main(int argc, char *argv[]) {
    int i;
    int m; /* max track counter */
    uint8_t nsfe_i; /* nsfe playlist track number */
    int fc; /* current # of frames to decode */
    FILE *f;
    uint64_t frames; /* total pcm frames */

    progname = argv[0];

    xgm::NSF nsf;
    xgm::NSFPlayerConfig config;
    xgm::NSFPlayer player;

    Nsf2WavOptions options = ParseOptions(&argc, &argv, nsf);
    nsf.SetDefaults(options.length_ms, options.fade_ms, nsf.default_loopnum);

    if(argc < 1 || argc > 2) Usage(stderr, EX_USAGE, nsf);

    // audio samples, native machine format
    std::unique_ptr<int16_t[]> buf(new int16_t[kFramesToBuffer * options.channels]);
    // audio samples, little-endian format
    std::unique_ptr<uint8_t[]> pac(new uint8_t[kFramesToBuffer * options.channels * (sizeof(uint16_t) / sizeof(uint8_t))]);

    if(!nsf.LoadFile(argv[0])) {
        fprintf(stderr,"Error loading NSF: %s\n",nsf.LoadError());
        return 1;
    }

    if(argc == 1) {
        /* dump info */
        /* use playlist order, if available */

        if (options.quiet) return EXIT_SUCCESS;

        printf("Title: %s\n",nsf.title);
        printf("Artist: %s\n",nsf.artist);
        printf("Copyright: %s\n",nsf.copyright);
        printf("Ripper: %s\n",nsf.ripper);

        if(nsf.playlist_mode) {
            printf("Track %03d: %s\n",nsf.song+1,nsf.GetTitleString());
            return EXIT_SUCCESS;
        }

        if(nsf.nsfe_plst_size > 0) {
            m = nsf.nsfe_plst_size;
        } else {
            m = nsf.total_songs;
        }
        for(i=0;i<m;i++) {
            if(nsf.nsfe_plst) {
                nsfe_i = nsf.nsfe_plst[i];
            } else {
                nsfe_i = i;
            }
            printf("Track %03d: %s\n",i+1,
              nsf.nsfe_entry[nsfe_i].tlbl);
        }
        return EXIT_SUCCESS;
    }


    if(nsf.playlist_mode) {
        i = nsf.song;
    } else {
        if (options.track <= 0) {
            fprintf(stderr,"error: use 1-based track number\n");
            return EXIT_FAILURE;
        }
        i = options.track - 1;
    }

    /* dump info for single track */
    if (!nsf.playlist_mode && nsf.nsfe_plst) {
        nsfe_i = nsf.nsfe_plst[i];
    } else {
        nsfe_i = i;
    }

    if (!nsf.playlist_mode && nsf.nsfe_entry[nsfe_i].time >= 0) {
        options.length_ms = nsf.nsfe_entry[nsfe_i].time;
    } else if (nsf.time_in_ms >= 0) {
        options.length_ms = nsf.time_in_ms;
    } else if (!options.lengthForce){
      fprintf(
          stderr,
          "Warning: Could not detect track length, will use default of %" PRId32
              " ms.\n",
          options.length_ms);
    }

    if (!nsf.playlist_mode && nsf.nsfe_entry[nsfe_i].fade >= 0) {
        options.fade_ms = nsf.nsfe_entry[nsfe_i].fade;
    } else if (nsf.fade_in_ms >= 0) {
        options.fade_ms = nsf.fade_in_ms;
    } else {
      fprintf(
          stderr,
          "Warning: Could not detect fade time, will use default of %" PRId32
              " ms.\n",
          options.fade_ms);
    }

    if (!options.quiet) {
        printf("Track %03d: %s\n",i+1,
          (!nsf.playlist_mode && nsf.nsfe_entry[nsfe_i].tlbl[0] != '\0') ?
          nsf.nsfe_entry[nsfe_i].tlbl :
          nsf.GetTitleString("%L",i));
        printf("  length: %" PRId32 " ms\n", options.length_ms);
        printf("    fade: %" PRId32 " ms\n", options.fade_ms);
    }

    config["MASTER_VOLUME"] = 256; /* default volume = 128 */
    config["APU2_OPTION5"] = 0; /* disable randomized noise phase at reset */
    config["APU2_OPTION7"] = 0; /* disable randomized tri phase at reset */

	if (!options.lengthForce) {
	  config["AUTO_DETECT"] = 1;
	  config["LOOP_NUM"] = 2;
	  nsf.loop_num = 2;
	  config["DETECT_INT"] = 1000;
	}


    player.SetConfig(&config);

    if(!player.Load(&nsf)) {
        fprintf(stderr,"Error with player load\n");
        return EXIT_FAILURE;
    }


    player.SetPlayFreq(options.samplerate);
    player.SetChannels(options.channels);
    player.SetSong(i);
    player.Reset();


    frames  = (uint64_t)options.length_ms * options.samplerate;
    frames += (uint64_t)options.fade_ms * options.samplerate;
    constexpr uint64_t kMillisPerSecond = 1000;
    frames /= kMillisPerSecond;

	if (!options.lengthForce) {
    	uint64_t framesbackup = frames;
		bool notYetFading = true;

		while(frames && !player.fader.IsFading()) {
		    fc = std::min(frames, kFramesToBuffer);
		    player.Skip(fc);
		    frames -= fc;
		}

		if (player.playtime_detected){
	  		config["AUTO_DETECT"] = 0;
	  config["LOOP_NUM"] = 0;
		    frames = player.total_render + ((uint64_t)nsf.GetFadeTime()*options.samplerate/kMillisPerSecond);
		    printf("Detected loop time successfully, it's %lu\n", frames);
		    if (options.trigger){
		        nsf.time_in_ms += nsf.GetFadeTime();
		        nsf.fade_in_ms = 0;
		    }
		} else frames = framesbackup;

		config["STOP_SEC"] = std::ceil((player.total_render + ((uint64_t)nsf.GetFadeTime()/kMillisPerSecond))/options.samplerate);
	}	// else frames stays the same
 
	config["MASK"] = options.mask; /* channel mask that shit */


    config["TRIGGER"] = options.trigger ? 1 : 0;

	if (options.trigger) {
		config["APU1_OPTION2"] = 0;	/* Disable nonlinear mixing */
		config["APU2_OPTION4"] = 0; /* Disable nonlinear mixing */
		config["MMC5_OPTION0"] = 0; /* Disable nonlinear mixing */
		config["N163_OPTION0"] = 0; /* Enable normal mixing */
		config["FDS_OPTION0"] = 96000; /* practically disable LPF */
	}

	for (int i = 0; i < 32; i++){
		uint64_t mask = 1<<i;
		config.GetChannelConfig(i, "VOL") = (options.mute&mask)?0:128;
	}
	config.Notify(-1);
    player.SetConfig(&config);
    player.Reset();

    f = fopen(argv[1],"wb");
    if(f == NULL) {
        fprintf(stderr, "Error opening %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    write_wav_header(f, frames, options);

    while(frames) {
        fc = std::min(frames, kFramesToBuffer);
		printf("%lu, %lu\n", frames+player.total_render, frames);
        player.Render(buf.get(), fc);
        pack_frames(pac.get(), buf.get(), fc, options.channels);
        write_frames(f, pac.get(), fc, options.channels);
        frames -= fc;
    }

    fclose(f);

    return EXIT_SUCCESS;
}
