/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tgmath.h>
#include <time.h>
#include <sched.h>
#include <stdint.h>

#include <alsa/asoundlib.h>

#include <fftw3.h>

#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include "gl.h"

// Global variables wrapped up in a struct for neatness.
// TODO: Get rid of all of these
struct {
	GLFWwindow *win;

	snd_pcm_t *pcm_handle;

	enum scale horiz_scale;
	float scale;
	unsigned fft_size;
	bool fft_recalculate;
} glob = {
	// Default values
	.horiz_scale = SCALE_LOG,
	.scale = 15.0f,
	.fft_size = 0x1000,
};

static void error_callback(int error, const char *desc)
{
	(void)error;

	fprintf(stderr, "%s\n", desc);
}

static void key_callback(GLFWwindow *win, int key, int scancode, int action, int mods)
{
	(void)scancode;
	(void)mods;

	if (action == GLFW_PRESS) {
		switch (key) {
		case GLFW_KEY_ESCAPE:
			glfwSetWindowShouldClose(win, GL_TRUE);
			break;
		case GLFW_KEY_L:
			glob.horiz_scale = (glob.horiz_scale + 1) % SCALE_MAX;
			break;
		case GLFW_KEY_UP:
			glob.scale *= 1.2f;
			break;
		case GLFW_KEY_DOWN:
			glob.scale *= 0.8f;
			break;
		case GLFW_KEY_LEFT:
			if (glob.fft_size != 0x80000000) {
				glob.fft_size <<= 1;
				glob.fft_recalculate = true;
			}
			break;
		case GLFW_KEY_RIGHT:
			if (glob.fft_size != 0x00000001) {
				glob.fft_size >>= 1;
				glob.fft_recalculate = true;
			}
			break;
		default:
			break;
		}
	}
}

static void init_window()
{
	if (!glfwInit())
		exit(EXIT_FAILURE);

	glfwSetErrorCallback(error_callback);

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_FLOATING, GL_TRUE);

	glob.win = glfwCreateWindow(800, 600, "Visualiser", NULL, NULL);
	if (!glob.win)
		exit(EXIT_FAILURE);

	glfwMakeContextCurrent(glob.win);
	glfwSwapInterval(1);

	glfwSetKeyCallback(glob.win, key_callback);

	glewExperimental = GL_TRUE;
	glewInit();
}

static void destroy_window()
{
	glfwDestroyWindow(glob.win);
	glfwTerminate();
}

struct audio_info {
	int sample_rate;
	int channels;

	size_t num_samples;

	int16_t (*playback)[2];
	float *data;
};

void init_alsa(struct audio_info *info)
{
	int ret;

	ret = snd_pcm_open(&glob.pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(ret));
		exit(EXIT_FAILURE);
	}

	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	snd_pcm_hw_params_any(glob.pcm_handle, params);

	ret = snd_pcm_hw_params_set_access(glob.pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_hw_params_set_access: %s\n", snd_strerror(ret));
		exit(EXIT_FAILURE);
	}

	ret = snd_pcm_hw_params_set_format(glob.pcm_handle, params, SND_PCM_FORMAT_S16_LE);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_hw_params_set_format: %s\n", snd_strerror(ret));
		exit(EXIT_FAILURE);
	}

	ret = snd_pcm_hw_params_set_channels(glob.pcm_handle, params, info->channels);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_hw_params_set_channels: %s\n", snd_strerror(ret));
		exit(EXIT_FAILURE);
	}

	unsigned rate = info->sample_rate;
	ret = snd_pcm_hw_params_set_rate_near(glob.pcm_handle, params, &rate, 0);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_hw_params_set_rate_near: %s\n", snd_strerror(ret));
		exit(EXIT_FAILURE);
	}

	ret = snd_pcm_hw_params(glob.pcm_handle, params);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_hw_params: %s\n", snd_strerror(ret));
		exit(EXIT_FAILURE);
	}
}

void destroy_alsa()
{
	snd_pcm_close(glob.pcm_handle);
	snd_config_update_free_global();
}

void free_audio_file(struct audio_info *info)
{
	free(info->playback);
	free(info->data);
}

void get_audio_file(const char *path, int sample_rate, struct audio_info *info)
{
	av_register_all();

	AVFormatContext *format = NULL;
	if (avformat_open_input(&format, path, NULL, NULL) != 0) {
		fprintf(stderr, "Could not open %s\n", path);
		exit(EXIT_FAILURE);
	}

	if (avformat_find_stream_info(format, NULL) < 0) {
		fprintf(stderr, "Could not retrieve stream info for %s\n", path);
		exit(EXIT_FAILURE);
	}

	int stream_index = -1;
	for (unsigned i = 0; i < format->nb_streams; ++i) {
		if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			stream_index = i;
			break;
		}
	}

	if (stream_index == -1) {
		fprintf(stderr, "Could not retrieve audio stream from %s\n", path);
		exit(EXIT_FAILURE);
	}

	AVStream *stream = format->streams[stream_index];

	AVCodecParameters *codec = stream->codecpar;
	AVCodecContext *cntxt = avcodec_alloc_context3(NULL);

	if (avcodec_parameters_to_context(cntxt, codec) < 0) {
		fprintf(stderr, "Unable to converts pamaters to context\n");
		exit(EXIT_FAILURE);
	}

	if (avcodec_open2(cntxt, avcodec_find_decoder(codec->codec_id), NULL) < 0) {
		fprintf(stderr, "Failed to open stream %u in %s\n", stream_index, path);
		exit(EXIT_FAILURE);
	}

	struct SwrContext *swr1 = swr_alloc();
	av_opt_set_int(swr1, "in_channel_count",  codec->channels, 0);
	av_opt_set_int(swr1, "out_channel_count", 1, 0);
	av_opt_set_int(swr1, "in_channel_layout",  codec->channel_layout, 0);
	av_opt_set_int(swr1, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
	av_opt_set_int(swr1, "in_sample_rate", codec->sample_rate, 0);
	av_opt_set_int(swr1, "out_sample_rate", sample_rate, 0);
	av_opt_set_sample_fmt(swr1, "in_sample_fmt",  codec->format, 0);
	av_opt_set_sample_fmt(swr1, "out_sample_fmt", AV_SAMPLE_FMT_FLT,  0);

	swr_init(swr1);
	if (!swr_is_initialized(swr1)) {
		fprintf(stderr, "Resampler has not been properly initialized\n");
		exit(EXIT_FAILURE);
	}

	struct SwrContext *swr2 = swr_alloc();
	av_opt_set_int(swr2, "in_channel_count",  codec->channels, 0);
	av_opt_set_int(swr2, "out_channel_count", 2, 0);
	av_opt_set_int(swr2, "in_channel_layout",  codec->channel_layout, 0);
	av_opt_set_int(swr2, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
	av_opt_set_int(swr2, "in_sample_rate", codec->sample_rate, 0);
	av_opt_set_int(swr2, "out_sample_rate", sample_rate, 0);
	av_opt_set_sample_fmt(swr2, "in_sample_fmt",  codec->format, 0);
	av_opt_set_sample_fmt(swr2, "out_sample_fmt", AV_SAMPLE_FMT_S16,  0);

	swr_init(swr2);
	if (!swr_is_initialized(swr2)) {
		fprintf(stderr, "Resampler has not been properly initialized\n");
		exit(EXIT_FAILURE);
	}

	AVPacket packet;
	av_init_packet(&packet);

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		fprintf(stderr, "Unable to allocate frame\n");
		exit(EXIT_FAILURE);
	}

	size_t size = 0;

	int16_t (*playback)[2] = NULL;
	float *data = NULL;

	uint8_t *buffer1 = NULL;
	uint8_t *buffer2 = NULL;

	while (av_read_frame(format, &packet) >= 0) {
		avcodec_send_packet(cntxt, &packet);

		while (avcodec_receive_frame(cntxt, frame) == 0) {
			if (!buffer1)
				av_samples_alloc(&buffer1, NULL, 1, frame->nb_samples, AV_SAMPLE_FMT_FLT, 0);
			if (!buffer2)
				av_samples_alloc(&buffer2, NULL, 2, frame->nb_samples, AV_SAMPLE_FMT_S16, 0);

			int frame_count = swr_convert(swr1, &buffer1, frame->nb_samples,
						      (const uint8_t**)frame->data, frame->nb_samples);
			swr_convert(swr2, &buffer2, frame->nb_samples,
					      (const uint8_t**)frame->data, frame->nb_samples);

			data = realloc(data, (size + frame_count) * sizeof *data);
			memcpy(data + size, buffer1, frame_count * sizeof *data);

			playback = realloc(playback, (size + frame_count) * sizeof *playback);
			memcpy(playback + size, buffer2, frame_count * sizeof *playback);

			size += frame_count;

			av_frame_unref(frame);
		}

		av_packet_unref(&packet);
	}

	av_freep(&buffer1);
	av_freep(&buffer2);

	av_frame_free(&frame);

	swr_free(&swr1);
	swr_free(&swr2);

	avcodec_close(cntxt);
	avcodec_free_context(&cntxt);

	avformat_close_input(&format);

	info->sample_rate = sample_rate;
	info->channels = 2;
	info->num_samples = size;
	info->playback = playback;
	info->data = data;
}

#define min(a, b) ((a) < (b) ? (a) : (b))

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s song_name\n", argv[0]);
		return 1;
	}

	struct audio_info info;
	// TODO: This works with 44100, but not 48000.
	// It ends early. This needs to be inverstigated.
	get_audio_file(argv[1], 44100, &info);

	// I'm going to make these modifiable at runtime at some point,
	// but until then, they can just be const

	const unsigned frames_per_sec = 60;
	const size_t frame_offset = info.sample_rate / frames_per_sec;
	const size_t audio_offset = frame_offset * 32;

	size_t num_samples = info.num_samples;

	init_window();
	gl_init();
	init_alsa(&info);

	struct timespec time_now, time_next;
	clock_gettime(CLOCK_MONOTONIC, &time_now);
	time_next = time_now;

	// FFT related variables.
	// These should be initalised properly the first time we run the loop

	fftwf_plan plan = NULL;
	float *real = NULL;
	complex float *cmplx = NULL;
	GLfloat *arr = NULL;

	snd_pcm_writei(glob.pcm_handle, info.playback, audio_offset);
	snd_pcm_pause(glob.pcm_handle, 1);

	glob.fft_recalculate = true;

	for (size_t i = 0; i < num_samples && !glfwWindowShouldClose(glob.win); i += frame_offset) {
		if (glob.fft_recalculate) {
			// Pause the audio, because computing a new FFT is expensive
			// and can desync the audio
			snd_pcm_pause(glob.pcm_handle, 1);

			// fftw doesn't seem to have a realloc() function
			fftwf_free(real);
			fftwf_free(cmplx);

			real = fftwf_alloc_real(glob.fft_size);
			cmplx = fftwf_alloc_complex(glob.fft_size);

			fftwf_destroy_plan(plan);
			plan = fftwf_plan_dft_r2c_1d(glob.fft_size, real, cmplx, FFTW_MEASURE);

			arr = realloc(arr, sizeof *arr * (glob.fft_size / 2));

			glob.fft_recalculate = false;
			snd_pcm_pause(glob.pcm_handle, 0);
		}

		float avg = 0.0f;
		for (size_t j = 0; j < min(glob.fft_size, num_samples - i - 1); ++j) {
			real[j] = info.data[i + j];
			avg += real[j] / glob.fft_size;
		}

		// Zero out data past end of read in samples
		// i.e. we're at the end of the file
		for (size_t j = num_samples - i - 1; j < glob.fft_size; ++j)
			real[j] = 0.0f;

		for (size_t j = 0; j < glob.fft_size; ++j)
			real[j] -= avg;

		fftwf_execute(plan);

		for (size_t j = 0; j < glob.fft_size / 2; ++j) {
			arr[j] = fabs(cmplx[j] / glob.fft_size) * glob.scale - 1.0f;
		}

		if (i % audio_offset == 0) {
			// We always start 1 frame ahead, so the buffer never fully drains
			size_t frame_start = i + audio_offset;

			if (frame_start < num_samples) {
				int ret = snd_pcm_writei(glob.pcm_handle,
							 info.playback + frame_start,
							 min(audio_offset, num_samples - frame_start));
				if (ret < 0) {
					fprintf(stderr, "snd_pcm_writei: %s\n", snd_strerror(ret));
					snd_pcm_recover(glob.pcm_handle, ret, 0);
				}
			}
		}

		gl_render(glob.win, glob.fft_size / 2, arr, glob.horiz_scale);

		// Wait to start the next frame
		do {
			sched_yield();
			clock_gettime(CLOCK_MONOTONIC, &time_now);
		} while (time_next.tv_sec > time_now.tv_sec && time_next.tv_nsec > time_now.tv_nsec);

		time_next.tv_nsec += (1e9 / info.sample_rate) * frame_offset;
		if (time_next.tv_nsec > 1e9) {
			time_next.tv_nsec -= 1e9;
			++time_next.tv_sec;
		};

		glfwPollEvents();
	}

	if (glfwWindowShouldClose(glob.win))
		snd_pcm_drop(glob.pcm_handle);
	else
		snd_pcm_drain(glob.pcm_handle);

	destroy_alsa();
	gl_destroy();
	destroy_window();

	fftwf_free(real);
	fftwf_free(cmplx);
	fftwf_destroy_plan(plan);
	fftwf_cleanup();

	free(info.playback);
	free(info.data);

	free(arr);
}
