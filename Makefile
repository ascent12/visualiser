LIBS = glfw3 glew gl fftw3f alsa libavcodec libswresample libavformat libavutil

CFLAGS = $(shell pkg-config --cflags $(LIBS)) -Og -Wall -Wextra -g -flto
LDLIBS = $(shell pkg-config --libs $(LIBS)) -lm -lrt -flto -g

all: main

main: main.o gl.o

clean:
	$(RM) main main.o
