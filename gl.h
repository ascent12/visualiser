#ifndef GL_H
#define GL_H

#include <stddef.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

void gl_init(void);
void gl_destroy(void);

enum scale {
	SCALE_LIN,
	SCALE_LOG,
	SCALE_MAX,
};

void gl_render(GLFWwindow *win, size_t n, float arr[static restrict n], enum scale s);

#endif
