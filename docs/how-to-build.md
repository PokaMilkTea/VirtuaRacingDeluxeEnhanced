# How to build


The game itself is not included: you need your own ROM, `Virtua Racing Deluxe (USA).32x` (see "ROM" in the README).

## macOS

Apple Silicon (arm64), with the Xcode Command Line Tools and SDL2 from [Homebrew](https://brew.sh):

```sh
brew install sdl2
make                      # build/vrde (the game), build/vrdetrace (headless trace), build/sh2dis
./build/vrde "path/to/Virtua Racing Deluxe (USA).32x"
```

## Windows

`vrde.exe` is cross-compiled on a Mac:

```sh
make win                  # build/win/vrde.exe and SDL2.dll
```

- The first `make win` runs `make win-deps`, which downloads three things into `deps/`: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20260922 (UCRT, macOS universal), the SDL2 2.32.10 MinGW development files, and the zlib 1.3.1 source.
- Copy `vrde.exe` and `SDL2.dll` from `build/win/` into one folder. They run on 64-bit Windows 10 or later.

## Linux

Run `make` on the Linux machine itself, with a C compiler and the SDL2, OpenGL and zlib development files (on Ubuntu: `libsdl2-dev`, `libgl-dev` and `zlib1g-dev`):

```sh
make
./build/vrde "path/to/Virtua Racing Deluxe (USA).32x"
```

`make clean` removes `build/`.
