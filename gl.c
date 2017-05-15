#include "gl.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <tgmath.h>

struct {
	const GLchar *srcs_vert[SCALE_MAX];

	const GLchar *src_geom;
	const GLchar *src_frag;

	GLuint progs[SCALE_MAX];

	GLuint vao;
	GLuint vbo;
} gl = {
	.srcs_vert = {
		[SCALE_LIN] =
			"#version 330 core\n"
			"#extension GL_ARB_explicit_uniform_location : require\n"
			"layout(location = 0) uniform float in_width;\n"
			"layout(location = 0) in float pos;\n"
			"out float width;\n"
			"void main() {\n"
			"	width = in_width;\n"
			"	gl_Position = vec4(width * gl_VertexID - 1.0, pos, 0.0, 1.0);\n"
			"}\n",
		[SCALE_LOG] =
			"#version 330 core\n"
			"#extension GL_ARB_explicit_uniform_location : require\n"
			"layout(location = 0) uniform float scale;\n"
			"layout(location = 0) in float pos;\n"
			"out float width;\n"
			"void main() {\n"
			"	int i = (gl_VertexID == 0) ? 1 : gl_VertexID;\n"
			"	float x = log(i) * scale;\n"
			"	width = log(i + 1) * scale - x;\n"
			"	gl_Position = vec4(x - 1.0, pos, 0.0, 1.0);\n"
			"}\n",
	},

	.src_geom =
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
		"}",

	.src_frag =
		"#version 330 core\n"
		"out vec4 color;\n"
		"void main() {\n"
		"	color = vec4(1.0, 0.0, 0.0, 1.0);\n"
		"}\n",
};

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

static GLuint create_prog(const GLchar *src_vert, GLuint geom, GLuint frag)
{
	GLuint prog = glCreateProgram();
	GLuint vert = create_shader(GL_VERTEX_SHADER, src_vert);

	glAttachShader(prog, vert);
	glAttachShader(prog, geom);
	glAttachShader(prog, frag);

	glLinkProgram(prog);

	GLint ret;
	glGetProgramiv(prog, GL_LINK_STATUS, &ret);

	if (ret == GL_FALSE) {
		GLint loglen;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &loglen);

		if (loglen != 0) {
			GLchar msg[loglen];
			glGetProgramInfoLog(prog, loglen, &loglen, msg);

			puts(msg);
		}

		exit(EXIT_FAILURE);
	}

	glDetachShader(prog, vert);
	glDetachShader(prog, geom);
	glDetachShader(prog, frag);

	glDeleteShader(vert);

	return prog;
}

void gl_init(void)
{
	GLuint geom = create_shader(GL_GEOMETRY_SHADER, gl.src_geom);
	GLuint frag = create_shader(GL_FRAGMENT_SHADER, gl.src_frag);

	for (int i = 0; i < SCALE_MAX; ++i) {
		gl.progs[i] = create_prog(gl.srcs_vert[i], geom, frag);
	}

	glDeleteShader(geom);
	glDeleteShader(frag);

	glGenVertexArrays(1, &gl.vao);
	glGenBuffers(1, &gl.vbo);

	glBindVertexArray(gl.vao);
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, sizeof(GLfloat), NULL);
}

void gl_destroy(void)
{
	for (int i = 0; i < SCALE_MAX; ++i)
		glDeleteProgram(gl.progs[i]);

	glDeleteVertexArrays(1, &gl.vao);
	glDeleteBuffers(1, &gl.vbo);
}

void gl_render(GLFWwindow *win, size_t n, float arr[static restrict n], enum scale s)
{
	int width, height;
	glfwGetFramebufferSize(win, &width, &height);
	glViewport(0, 0, width, height);

	switch (s) {
	case SCALE_LIN:
		// in_width
		glUniform1f(0, 4.0 / n);
		break;
	case SCALE_LOG:
		// scale
		glUniform1f(0, 2.0 / log(n));
		break;
	default:
		exit(EXIT_FAILURE);
	}

	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, n * sizeof arr[0], arr, GL_STATIC_DRAW);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindVertexArray(gl.vao);

	glUseProgram(gl.progs[s]);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glDrawArrays(GL_POINTS, 0, n);

	glfwSwapBuffers(win);
}
