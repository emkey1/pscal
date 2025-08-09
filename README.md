Pscal implements a substantial subset of the original Pascal language specification, along with a few extensions.  Notable things it does not support are nested functions/procedure.  It is also completely free of object oriented constructs by design.  Sorry, not a fan of that programming paradigm.

The most notable thing about pscal is that while I've been directing the development and doing the debugging, the large majority of the code has been written by various AI's. Anyone who has tried to work on a medium to large sized project with AI will know that at least as of the time I'm writing this that is no easy task.  Limited context windows and other limitations quickly lead to unpredictable results and constant code breakage.  Until recently the OpenAI models have been especially bad at this, I'm now using OpenAI's codex and I'm VERY impressed.  The state of the art in AI continues to move forward at a break neck pace.

Pscal uses cmake, but I've only just started learning that particular tool.  I do my development on an M1 MacBook Pro, primarily using (shudder) Xcode.

As the code was written primarily by AI's I'm releasing this to the public domain via the 'unlicense".

Install:

You will need cmake, curl, SDL2, and SDL2_ttf, though SDL graphics and sound support is optional.

Here's how to get them installed if needed...

For curl...

```
# Debian / Ubuntu
sudo apt-get update
sudo apt-get install libcurl4-openssl-dev

# RHEL / CentOS 7
sudo yum install libcurl-devel

# Fedora
sudo dnf install libcurl-devel

# SUSE / openSUSE
sudo zypper install libcurl-devel

# Alpine
sudo apk add curl-dev

# Arch Linux
sudo pacman -S curl

# MacOS (with Homebrew)
brew install curl

On macOS you may also need to tell your build where Homebrew’s curl lives:
export LDFLAGS="-L$(brew --prefix curl)/lib"
export CPPFLAGS="-I$(brew --prefix curl)/include"


```

Here’s how to get CMake itself installed if needed...


```
# Debian / Ubuntu
sudo apt-get update
sudo apt-get install cmake

# RHEL / CentOS
sudo yum install cmake

# Fedora
sudo dnf install cmake

# SUSE / openSUSE
sudo zypper install cmake

# Alpine
sudo apk add cmake

# Arch Linux
sudo pacman -S cmake

# macOS (with Homebrew)
brew update
brew install cmake
```
For SDL2 and SDL2_ttf...

```
Debian / Ubuntu
sudo apt-get update
sudo apt-get install libsdl2-dev libsdl2-ttf-dev

RHEL / CentOS (and similar, may require EPEL for older versions)
For CentOS 8+ / RHEL 8+ / AlmaLinux / Rocky Linux:
sudo dnf install SDL2-devel SDL2_ttf-devel

For CentOS 7 (these packages are often in EPEL repository):
sudo yum install epel-release # If EPEL is not already enabled
sudo yum install SDL2-devel SDL2_ttf-devel

Fedora
sudo dnf install SDL2-devel SDL2_ttf-devel

SUSE / openSUSE
sudo zypper install SDL2-devel SDL2_ttf-devel

Alpine
sudo apk add sdl2-dev sdl2_ttf-dev

Arch Linux
sudo pacman -S sdl2 sdl2_ttf

macOS (with Homebrew)
brew install sdl2 sdl2_ttf
```

After cloning the repo...

```
cd pscal
mkdir build
cd build
cmake ..
make
```

Binaries will be in ../build/bin

### Building without SDL

To build without SDL support, run:

```
mkdir build && cd build && cmake -DSDL=OFF .. && make
```

SDL-dependent examples and tests will be skipped when this flag is off.

You will also need to do something similar to the following to get units and the code in Examples/ and Tests/ to work...

```
# Run this from the root of your git repo
REPO_DIR="$(pwd)"

sudo mkdir -p /usr/local/Pscal

sudo ln -s "${REPO_DIR}/etc" /usr/local/Pscal/etc
sudo ln -s "${REPO_DIR}/lib" /usr/local/Pscal/lib
```

