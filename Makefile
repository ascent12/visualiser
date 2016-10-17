CFLAGS = $(shell pkg-config --cflags glfw3 glew gl fftw3f alsa) -Og -Wall -Wextra -g 
LDLIBS = $(shell pkg-config --libs glfw3 glew gl fftw3f alsa) -lm -lrt

all: main

clean:
	$(RM) main main.o
