#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tgmath.h>
#include <time.h>

#include <alsa/asoundlib.h>

#include <fftw3.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

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

	_Bool linear;
	float scale;
} glob = { .scale = 25.0f };

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
			glob.scale += 5.0f;
			break;
		case GLFW_KEY_DOWN:
			glob.scale -= 5.0f;
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
	//(void)glGetError(); // Throw out possible GL_INVALID_ENUM
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

static const GLchar vertex_shader[] =
"#version 330 core\n"
"layout(location = 0) in vec2 pos;\n"
"layout(location = 2) in float next_x;\n"
"out float v_next_x;\n"
"void main() {\n"
"	v_next_x = next_x;\n"
"	gl_Position = vec4(pos, 0.0, 1.0);\n"
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
"in float v_next_x[];\n"
"void main() {\n"
"	vec4 v = gl_in[0].gl_Position;\n"

"	gl_Position = v;\n"
"	EmitVertex();\n"

"	gl_Position = vec4(v_next_x[0], v.yzw);\n"
"	EmitVertex();\n"

"	gl_Position = vec4(v.x, -1.0, v.zw);\n"
"	EmitVertex();\n"

"	gl_Position = vec4(v_next_x[0], -1.0, v.zw);\n"
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
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, NULL);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, (void *)(sizeof(GLfloat) * 2));
}

static void destroy_opengl()
{
	glDeleteProgram(glob.gl_prog.vert);
	glDeleteProgram(glob.gl_prog.frag);
	glDeleteProgram(glob.gl_prog.geom);
	glDeleteProgram(glob.gl_prog.prog);
}

void render(GLfloat arr[][2], GLsizeiptr arr_size, GLsizei count)
{
	int width, height;
	glfwGetFramebufferSize(glob.win, &width, &height);
	glViewport(0, 0, width, height);

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

#define N 0x1000

#define FRAME_SIZE (N / 4)
#define AUDIO_SIZE 500
#define AUDIO_FRAME (FRAME_SIZE * AUDIO_SIZE)

volatile sig_atomic_t should_run = 0;

void handler(int num)
{
	(void)num;
	should_run = 1;
}

void init_timer(uint32_t sample_rate)
{
	struct sigaction sa = {
		.sa_handler = handler,
	};

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sigaction(SIGUSR1, &sa, NULL);

	timer_t timer;
	struct sigevent se = {
		.sigev_notify = SIGEV_SIGNAL,
		.sigev_signo = SIGUSR1,
	};

	timer_create(CLOCK_MONOTONIC, &se, &timer);

	struct itimerspec spec = {
		.it_interval = {
			.tv_sec = 0,
			.tv_nsec = (1.0f / sample_rate) * FRAME_SIZE * 1000000000,
		},
		.it_value = spec.it_interval,
	};

	timer_settime(timer, 0, &spec, NULL);
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
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s song_name\n", argv[0]);
		return 1;
	}

	struct wave *wav = read_wave(argv[1]);

	float *real = fftwf_alloc_real(N);
	complex float *cmplx = fftwf_alloc_complex(N);

	fftwf_plan plan = fftwf_plan_dft_r2c_1d(N, real, cmplx, FFTW_MEASURE);

	size_t data_len = wav->data_hdr.size / wav->fmt.channels;
	static GLfloat arr[N / 2 + 1][2] = { [N / 2] = {1.0f, 0.0f} };

	init_window();
	init_opengl();
	init_alsa(wav);
	init_timer(wav->fmt.sample_rate);

	int should_write = 0;
	int16_t (*prev_audio)[2] = &wav->data[0];

	snd_pcm_writei(glob.pcm_handle, prev_audio, AUDIO_FRAME);
	prev_audio += AUDIO_FRAME;

	float log_scale = 2.0f / log10(N / 2.0f);

	for (size_t i = 0; i < data_len && !glfwWindowShouldClose(glob.win); i += FRAME_SIZE) {
	//while (!glfwWindowShouldClose(glob.win) && data < wav->data + data_len - N) {
		float avg = 0.0f;
		for (size_t j = 0; j < N; ++j) {
			int16_t mono = (wav->data[i + j][0] + wav->data[i + j][1]) / 2;
			float sample = (float)mono / INT16_MAX;

			real[j] = sample;
			avg += sample / N;
		}

		for (size_t j = 0; j < N; ++j)
			real[j] -= avg;

		fftwf_execute(plan);

		for (size_t j = 0; j < N / 2; ++j) {
			if (glob.linear)
				arr[j][0] = 2.0f / (N / 2) * j - 1.0f;
			else
				arr[j][0] = log10((float)j) * log_scale - 1.0f;

			arr[j][1] = fabs(cmplx[j] / N) * glob.scale - 1.0f;
		}

		if (should_write == 0) {
			printf("write\n");
			int nwrite;
			if (prev_audio + AUDIO_FRAME > wav->data + data_len)
				nwrite = wav->data - prev_audio;
			else
				nwrite = AUDIO_FRAME;

			int ret = snd_pcm_writei(glob.pcm_handle, &prev_audio[0], nwrite);
			if (ret < 0) {
				fprintf(stderr, "snd_pcm_writei: %s\n", snd_strerror(ret));
				snd_pcm_recover(glob.pcm_handle, ret, 0);
			}

			prev_audio += nwrite;
		}

		should_write = (should_write + 1) % AUDIO_SIZE;

		while (!should_run);
		should_run = 0;

		render(arr, sizeof arr, N / 2);
		glfwPollEvents();
	}

	destroy_alsa();
	destroy_opengl();
	destroy_window();

	free(wav);
}
