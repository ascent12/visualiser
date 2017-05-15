A visualiser for audio using OpenGL.

![screenshot](https://raw.githubusercontent.com/ascent12/visualiser/master/screenshot.png)

Note: this is not high-quality code intended for other people to use.
It was mainly written as a learning experience for audio programming and OpenGL.
I might improve it in the future, but for the time being, it remains as a hack-job.

# Dependencies:
- GLFW
- FFTW
- ALSA

# Usage:
Compile with
```sh
make
```

Run with
```sh
./main some_file.wav
```

# Controls:
- `ESC`: Quit
- `Up`: Scale +10%
- `Down`: Scale -10%
- `Left`: Double FFT size
- `Right`: Half FFT size
- `L`: Switch between Logarithmic/Linear views (Default: Logarithmic)

# FFT size:
The FFT size has a direct impact on the horizontal resolution of the output, and how long a signal will stay on the display.  
Setting the FFT size to be too large will have strange results, where the audio signals stay on the screen for far too long.  
Setting the FFT too low will also have strange results, with the audio signals appearing 'jittery'.  
Staying reasonably close to the default value has the best results.  
