# ESP32-webcam-control-Mirroring-ESP-NOW-
This project turns an ESP32-CAM (or ESP32-based camera board) into a dual-output video system:

Live MJPEG video streaming over HTTP for browser access.

Wireless video mirroring via ESP-NOW to another ESP32, enabling remote display or processing &  controlling .

The system ensures synchronized streaming without blocking calls and handles WiFi reconnection automatically.

Features

HTTP MJPEG Streaming:
Access live camera feed in any web browser using the ESP32’s IP address.

ESP-NOW Video Mirroring:
Mirror frames to another ESP32 device over ESP-NOW. Frames are sent directly in JPEG format (≤60 KB) for efficiency.

Non-blocking Synchronization:
The HTTP stream and ESP-NOW transmission run concurrently without interfering, maintaining smooth video output.

WiFi Auto-Reconnect:
If the WiFi connection drops, the ESP32 automatically reconnects while continuing ESP-NOW streaming.

LED Flash Support (Optional):
Integrated control of a flash LED for low-light conditions.

PSRAM Optimized:
High-resolution frames (UXGA) with JPEG compression for boards with PSRAM.

How It Works

Camera Initialization:

Configured for JPEG format, optionally UXGA resolution.

Frame buffer in PSRAM for high frame rates.

HTTP MJPEG Streaming:

stream_handler() serves continuous MJPEG chunks.

Frames are sent in real-time to connected browsers.

ESP-NOW Mirroring:

sendPacketData() sends frames ≤60 KB directly to a receiver.

Non-blocking send: uses sendingEspNow flag and callback onDataSent() to track delivery.

Synchronization:

Both HTTP and ESP-NOW share the same camera frame but operate independently.

A single frame is captured per iteration and streamed over both channels without blocking each other.

WiFi Management:

Loop continuously checks WiFi status.

On disconnect, ESP32 attempts reconnect while optionally sending frames via ESP-NOW.

Wiring & Board Config

The project uses board_config.h to define camera pins for various ESP32 camera boards.
Ensure you select the correct board and define your flash LED pin if needed.

Setup

Clone this repository.

Open in Arduino IDE.

Configure your WiFi SSID and password:

const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";


Verify camera model in board_config.h.

Upload to your ESP32-CAM.

Access

HTTP Stream: Open http://<ESP32_IP> in a browser.

ESP-NOW Receiver: Another ESP32 board running the receiver sketch can get mirrored frames in real-time.

Notes

Frames >60 KB are not sent via ESP-NOW due to packet size limitations. Adjust resolution or JPEG quality as needed.

Non-blocking design ensures MJPEG streaming never pauses due to ESP-NOW transmission.

For high FPS, PSRAM boards are recommended.

License

Apache 2.0 License – see LICENSE
 
