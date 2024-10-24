# Dolphin - A GameCube and Wii Emulator

This repo is a fork of [vladfi1's fork of Slippi](https://github.com/vladfi1/slippi-Ishiiruka/tree/exi-ai-rebase), which supports FFW, headless emulation, and is to be used with a [specific version of libmelee](https://github.com/vladfi1/libmelee). Please refer to those repos for more information. Existing instructions were incompatible with Ubuntu 20.04 or running on dockerized containers, so I've included my own here.

# How to build on Ubuntu 20.04 LTS in Docker

```shell
# Install deps
sudo apt-get update
sudo apt-get install libasound2-dev pkg-config libegl-dev libusb-1.0-0-dev libavcodec-dev libavformat-dev libswscale-dev libavutil-dev libxi-dev libevdev-dev libwxgtk3.0-gtk3-dev libasound2 libegl1 libgl1 libusb-1.0-0 libglib2.0-0 libgdk-pixbuf2.0-0 libpangocairo-1.0-0 libudev-dev libegl1-mesa-dev libwxgtk3.0-gtk3-dev

# Install Rust and Cargo
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
export PATH="$HOME/.cargo/bin:$PATH"

# Clone repo
git clone https://github.com/ericyuegu/slippi-Ishiiruka
cd slippi-Ishiiruka
git submodule update --init --recursive

# Build
./build-linux.sh playback
APPIMAGE_EXTRACT_AND_RUN=1 ./build-appimage.sh playback

# Extract for use with libmelee
./Slippi_Playback-x86_64.AppImage --appimage-extract
```