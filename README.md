# GStreamer WebSocket Sink Plugin

GStreamer sink element that sends data over WebSocket.

It accepts any data format (video, audio, or raw data). It was designed to send binary data such as JSON, Protobuf, or Flatbuffers, but should also work with other formats.

It can be used to stream MJPEG to a website, see the `example` folder.

## Dependencies

Make sure you have all required dependencies installed (Example for Linux Debian/Ubuntu):

```bash
sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libboost-system-dev cmake
```

## Build from Source

From the source directory run:

```bash
mkdir build
cd build
cmake ..
cmake --build .

# If you want to install it system-wide
sudo make install

# Or if you want to use the library from the build folder
export GST_PLUGIN_PATH="$(pwd)"
```

## Example

Open `example/web_client.html` in browser and run the pipeline:

```bash
GST_DEBUG="websocketsink:4" gst-launch-1.0 videotestsrc pattern=ball ! jpegenc ! websocketsink host=127.0.0.1 port=8080
```

## Known Issues

It may fault with error: "Websocket initialisation error: Underlying Transport Error" and the "address already in use". Probably it is not closed correctly in some situatons.