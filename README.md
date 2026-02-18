# ESP32-S3 Camera Web Server - On-Demand WiFi

This project is a high-performance Camera Web Server for the ESP32-S3 (specifically designed for AI Thinker ESP32-S3-CAM or similar modules with OV2640/OV3660 sensors).

## Key Features

*   **On-Demand Station Mode**: The device always boots into **Access Point (AP)** mode for reliability. It only connects to your router (Station Mode) when you explicitly tell it to via the Web Interface.
*   **Fail-Safe Connection**: If the router connection fails or is lost (after 12 retries), it automatically reverts to AP mode so you never lose access.
*   **DHCP Support**: Automatically obtains an IP address from your router. No manual IP configuration required.
*   **Power Efficient**: WiFi Transmission Power is optimized (Level 70 ~17.5dBm) to prevent brownouts and reduce heat while maintaining good range.
*   **Physical Button Control**: Use the BOOT button (GPIO 0) or a custom button on **GPIO 19** to toggle between AP and Station modes manually.
*   **Persistent Settings**: Saves your WiFi credentials in NVS (Non-Volatile Storage) so you don't have to re-enter them after a reboot.

## Hardware Required

*   **ESP32-S3 Development Board** (e.g., ESP32-S3-CAM)
*   **Camera Module** (OV2640 or OV3660)
*   **Button** (Optional, connected to GPIO 19 and GND)

## Installation

### Prerequisites

*   **ESP-IDF v5.x** installed and configured.
*   Visual Studio Code with Espressif IDF Extension.

### Build & Flash

1.  **Clone the repository**:
    ```bash
    git clone <your-repo-url>
    cd esp32-cam-web-server
    ```

2.  **Configure the Project** (Optional):
    If you need to change pin definitions, edit `main/camera_pins.h`.
    The default AP SSID is "ESP32-CAM" and password "12345678".

3.  **Build**:
    Run the command: `idf.py build`

4.  **Flash**:
    Run the command: `idf.py -p COMx flash monitor` (Replace `COMx` with your device port).

## Usage Guide

### 1. Initial Access (AP Mode)
*   Power on the device.
*   Connect your phone or laptop to the WiFi network: **ESP32-CAM**
*   Password: **12345678**
*   Open your browser and go to: `http://192.168.4.1`

### 2. Connect to Router (Station Mode)
*   In the Web Interface, scroll down to **Network Settings**.
*   Enter your Router's **SSID** and **Password**.
*   Click **Connect to Router**.
*   The device will restart and attempt to connect.
*   **Access**: Once connected, use `http://esp32-cam.local` to access the camera stream.

### 3. Fail-Safe
*   If the router is unreachable or the password is wrong, the device will retry 12 times (approx. 45 seconds).
*   If it still fails, it will automatically switch back to **AP Mode** so you can fix the settings.
