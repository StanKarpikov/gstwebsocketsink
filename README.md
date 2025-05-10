# GStreamer WebSocket Sink Plugin

GStreamer sink element that sends data over WebSocket.

It accepts any data format (video, audio, or raw data). It was designed to send binary data such as JSON, Protobuf, or Flatbuffers, but should also work with other formats.

## Dependencies

Make sure you have all required dependencies installed (Example for Linux Debian/Ubuntu):

```bash
sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libwebsockets-dev cmake
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
```

## Example

Open `example/web_client.html` in browser and run the pipeline:

```bash
gst-launch-1.0 videotestsrc ! websocketsink host=127.0.0.1 port=8080
```
