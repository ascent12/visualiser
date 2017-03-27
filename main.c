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

#include <alsa/asoundlib.h>

#include <fftw3.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

// Global variables wrapped up in a struct for neatness.
// TODO: Get rid of all of these
struct {
	GLFWwindow *win;

	struct {
		GLuint prog;
		GLuint vert;
		GLuint frag;
		GLuint geom;
	} gl_prog;

	GLuint vao;
	GLuint vbo;

	snd_pcm_t *pcm_handle;

	bool linear;
	float scale;
	unsigned fft_size;
	bool fft_recalculate;
} glob = {
	// Default values
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
			glob.linear = !glob.linear;
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

static GLuint create_shader(GLenum type, const GLchar *src)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ret;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ret);

	if (ret == GL_FALSE) {
		GLint loglen;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &loglen);

		if (loglen != 0) {
			GLchar msg[loglen];
			glGetShaderInfoLog(shader, loglen, &loglen, msg);

			puts(msg);
		}

		exit(EXIT_FAILURE);
	}

	return shader;
}

// Linear scaling
/*
static const GLchar vertex_shader[] =
"#version 330 core\n"
"uniform int max;\n"
"layout(location = 0) in float pos;\n"
"out float width;\n"
"void main() {\n"
"	width = 4.0 / max;\n"
"	gl_Position = vec4(width * gl_VertexID - 1.0, pos, 0.0, 1.0);\n"
"}\n";
*/

// Logarithmic scaling
static const GLchar vertex_shader[] =
"#version 330 core\n"
"uniform float scale;\n"
"layout(location = 0) in float pos;\n"
"out float width;\n"
"void main() {\n"
"	int i = (gl_VertexID == 0) ? 1 : gl_VertexID;\n"
"	float x = log(i) * scale;\n"
"	width = log(i + 1) * scale - x;\n"
"	gl_Position = vec4(x - 1.0, pos, 0.0, 1.0);\n"
"}\n";

static const GLchar fragment_shader[] =
"#version 330 core\n"
"out vec4 color;\n"
"void main() {\n"
"	color = vec4(1.0, 0.0, 0.0, 1.0);\n"
"}\n";

static const GLchar geometry_shader[] =
"#version 330 core\n"
"layout(points) in;\n"
"layout(triangle_strip, max_vertices = 4) out;\n"
"in float width[];\n"
"void main() {\n"
"	vec4 v = gl_in[0].gl_Position;\n"

"	gl_Position = v;\n"
"	EmitVertex();\n"

"	gl_Position = vec4(v.x + width[0], v.yzw);\n"
"	EmitVertex();\n"

"	gl_Position = vec4(v.x, -1.0, v.zw);\n"
"	EmitVertex();\n"

"	gl_Position = vec4(v.x + width[0], -1.0, v.zw);\n"
"	EmitVertex();\n"

"	EndPrimitive();\n"
"}";

static void init_opengl()
{
	glob.gl_prog.vert = create_shader(GL_VERTEX_SHADER, vertex_shader);
	glob.gl_prog.frag = create_shader(GL_FRAGMENT_SHADER, fragment_shader);
	glob.gl_prog.geom = create_shader(GL_GEOMETRY_SHADER, geometry_shader);

	glob.gl_prog.prog = glCreateProgram();
	glAttachShader(glob.gl_prog.prog, glob.gl_prog.vert);
	glAttachShader(glob.gl_prog.prog, glob.gl_prog.geom);
	glAttachShader(glob.gl_prog.prog, glob.gl_prog.frag);
	glLinkProgram(glob.gl_prog.prog);

	GLint ret;
	glGetProgramiv(glob.gl_prog.prog, GL_LINK_STATUS, &ret);

	if (ret == GL_FALSE) {
		GLint loglen;
		glGetProgramiv(glob.gl_prog.prog, GL_INFO_LOG_LENGTH, &loglen);

		if (loglen != 0) {
			GLchar msg[loglen];
			glGetProgramInfoLog(glob.gl_prog.prog, loglen, &loglen, msg);

			puts(msg);
		}

		exit(EXIT_FAILURE);
	}

	glGenVertexArrays(1, &glob.vao);
	glGenBuffers(1, &glob.vbo);

	glUseProgram(glob.gl_prog.prog);
	glBindVertexArray(glob.vao);
	glBindBuffer(GL_ARRAY_BUFFER, glob.vbo);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, sizeof(GLfloat), NULL);
}

static void destroy_opengl()
{
	glDeleteProgram(glob.gl_prog.vert);
	glDeleteProgram(glob.gl_prog.frag);
	glDeleteProgram(glob.gl_prog.geom);
	glDeleteProgram(glob.gl_prog.prog);
}

void render(GLfloat arr[], GLsizeiptr arr_size, GLsizei count)
{
	int width, height;
	glfwGetFramebufferSize(glob.win, &width, &height);
	glViewport(0, 0, width, height);

	// Linear
	//GLint max = glGetUniformLocation(glob.gl_prog.prog, "max");
	//glUniform1i(max, count);

	// Logarithmic
	GLint scale = glGetUniformLocation(glob.gl_prog.prog, "scale");
	glUniform1f(scale, 2.0 / log(count));

	glBufferData(GL_ARRAY_BUFFER, arr_size, arr, GL_STATIC_DRAW);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindVertexArray(glob.vao);
	glUseProgram(glob.gl_prog.prog);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glDrawArrays(GL_POINTS, 0, count);

	glfwSwapBuffers(glob.win);
}

struct wave {
	struct {
		uint8_t id[4];
		uint32_t size;
		uint8_t format;
	} riff;
	struct chunk {
		uint8_t id[4];
		uint32_t size;
	} fmt_hdr;
	struct {
		uint16_t format;
		uint16_t channels;
		uint32_t sample_rate;
		uint32_t byte_rate;
		uint16_t block_align;
		uint16_t bit_depth;
	} fmt;
	struct chunk data_hdr;
	int16_t data[][2];
};

struct wave *read_wave(const char *filename)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		perror("");
		exit(EXIT_FAILURE);
	}

	uint32_t buf[2];
	fread(buf, sizeof buf[0], 2, fp);
	fseek(fp, 0, SEEK_SET);

	struct wave *wav = malloc(buf[1] + 8);
	fread(wav, buf[1] + 8, 1, fp);

	return wav;
}

void init_alsa(struct wave *wav)
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

	ret = snd_pcm_hw_params_set_channels(glob.pcm_handle, params, wav->fmt.channels);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_hw_params_set_channels: %s\n", snd_strerror(ret));
		exit(EXIT_FAILURE);
	}

	unsigned rate = wav->fmt.sample_rate;
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

#define min(a, b) ((a) < (b) ? (a) : (b))

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s song_name\n", argv[0]);
		return 1;
	}

	struct wave *wav = read_wave(argv[1]);

	// I'm going to make these modifiable at runtime at some point,
	// but until then, they can just be const

	const unsigned frames_per_sec = 60;
	const size_t frame_offset = wav->fmt.sample_rate / frames_per_sec;
	const size_t audio_offset = frame_offset * 32;

	size_t num_samples = wav->data_hdr.size / wav->fmt.channels / sizeof(int16_t);

	init_window();
	init_opengl();
	init_alsa(wav);

	struct timespec time_now, time_next;
	clock_gettime(CLOCK_MONOTONIC, &time_now);
	time_next = time_now;

	// FFT related variables.
	// These should be initalised properly the first time we run the loop

	fftwf_plan plan = NULL;
	float *real = NULL;
	complex float *cmplx = NULL;
	GLfloat *arr = NULL;

	snd_pcm_writei(glob.pcm_handle, &wav->data[0][0], audio_offset);
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
			int16_t mono = (wav->data[i + j][0] + wav->data[i + j][1]) / 2;
			float sample = (float)mono / INT16_MAX;

			real[j] = sample;
			avg += sample / glob.fft_size;
		}

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
							 &wav->data[frame_start][0],
							 min(audio_offset, num_samples - frame_start));
				if (ret < 0) {
					fprintf(stderr, "snd_pcm_writei: %s\n", snd_strerror(ret));
					snd_pcm_recover(glob.pcm_handle, ret, 0);
				}
			}
		}

		render(arr, sizeof *arr * (glob.fft_size / 2), glob.fft_size / 2);

		// Wait to start the next frame
		do {
			sched_yield();
			clock_gettime(CLOCK_MONOTONIC, &time_now);
		} while (time_next.tv_sec > time_now.tv_sec && time_next.tv_nsec > time_now.tv_nsec);

		time_next.tv_nsec += (1e9 / wav->fmt.sample_rate) * frame_offset;
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
	destroy_opengl();
	destroy_window();

	fftwf_free(real);
	fftwf_free(cmplx);
	fftwf_destroy_plan(plan);
	fftwf_cleanup();

	free(arr);
	free(wav);
}
