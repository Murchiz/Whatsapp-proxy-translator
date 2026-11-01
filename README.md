# WhatsApp Proxy SOCKS5/HTTP Translator (Standalone)

This is a highly optimized, asynchronous, standalone C++ proxy translator for Windows and Linux. It completely replaces the official WhatsApp Proxy (HAProxy) and acts as the bridging link to an upstream SOCKS5 or HTTP proxy. 
It uses standalone `asio` for non-blocking networking and `OpenSSL` for SSL termination.

## Features

1. **Cross-Platform:** Full native support for both **Linux** (systemd service) and **Windows** (Windows Service Control Manager).
2. **HAProxy Independent:** You no longer need to run or compile HAProxy or Docker. This tool implements the HAProxy PROXY Protocol v1 internally and bridges traffic natively.
3. **Auto Certificates:** Automatically generates the required `proxy.whatsapp.net.pem` via OpenSSL if it doesn't already exist.
4. **SSL Termination:** Intercepts and decrypts port 443 WhatsApp HTTPS traffic locally before tunneling to `g.whatsapp.net:5222` via your proxy.
5. **High Performance:** Multithreaded and asynchronous coroutine design based on C++23 and standalone Asio.
6. **Native Background Services:** Can be installed, configured, updated, and managed as a background system service on both Linux (systemd) and Windows.
7. **Dynamic Proxy Configuration:** Easily configure the upstream proxy at service installation, and update it later with a single command without reinstalling or manually editing registry/unit files.

---

## Requirements

### Windows
1. **CMake** (3.14+)
2. **MSVC** (Visual Studio 2022+ / C++23) or MinGW
3. **OpenSSL**:
   ```powershell
   scoop install openssl
   ```

### Linux (Ubuntu/Debian)
1. **CMake** (3.14+)
2. **GCC 13+** or **Clang 16+** with C++23 support
3. **OpenSSL development libraries**:
   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential cmake libssl-dev openssl
   ```

---

## Compilation

### On Windows
```powershell
cd translator
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### On Linux
```bash
cd translator
mkdir build
cd build
cmake ..
make -j$(nproc)
```

### Running Automated Tests
```bash
ctest --test-dir build -C Release --output-on-failure
```

---

## Usage

### 1. Console Mode

Run directly in your terminal:

**SOCKS5 Proxy:**
```bash
# Windows
translator.exe --proxy 127.0.0.1:1080 --type socks5

# Linux
./translator --proxy 127.0.0.1:1080 --type socks5
```

**HTTP Proxy:**
```bash
# Windows
translator.exe --proxy 192.168.1.100:3128 --type http

# Linux
./translator --proxy 192.168.1.100:3128 --type http
```

To stop console mode, press `Ctrl+C` for a graceful shutdown.

---

### 2. Service Mode (Background System Service)

#### A. Installing the Service with a Proxy

You can install the translator as a system service configured with your proxy in one command:

**Windows (run PowerShell or Command Prompt as Administrator):**
```powershell
translator.exe --install --proxy 127.0.0.1:1080 --type socks5
```

**Linux (run with sudo):**
```bash
sudo ./translator --install --proxy 127.0.0.1:1080 --type socks5
```

The service will be configured to start automatically on system boot, and will start immediately.

---

#### B. Changing the Proxy After Installing the Service

You can change the upstream proxy at any time without reinstalling the service:

**Windows (Administrator):**
```powershell
translator.exe --change-proxy 192.168.1.50:9050
```
Or switch both proxy and proxy type:
```powershell
translator.exe --change-proxy 192.168.1.50:3128 --type http
```

**Linux (sudo):**
```bash
sudo ./translator --change-proxy 192.168.1.50:9050
```
Or with type:
```bash
sudo ./translator --change-proxy 192.168.1.50:3128 --type http
```

*Aliases: `--set-proxy` and `--update-proxy` can also be used.*
The service manager will automatically reload configuration and restart the service with the new settings.

---

#### C. Checking Service Status

Check whether the service is installed, its current running state, and the active upstream proxy:

```bash
# Windows
translator.exe --status

# Linux
./translator --status
```

---

#### D. Controlling the Service

Start, stop, or restart the background service:

```bash
# Windows (Administrator)
translator.exe --stop
translator.exe --start
translator.exe --restart

# Linux (sudo)
sudo ./translator --stop
sudo ./translator --start
sudo ./translator --restart
```

---

#### E. Uninstalling the Service

To stop and completely remove the service:

```bash
# Windows (Administrator)
translator.exe --uninstall

# Linux (sudo)
sudo ./translator --uninstall
```

---

## Command Reference Summary

| Option | Description |
| :--- | :--- |
| `--proxy <host:port>` | Specifies the upstream proxy address (e.g., `127.0.0.1:1080`) |
| `--type <socks5\|http>` | Proxy protocol type (`socks5` by default, or `http`) |
| `--cert <path>` | Custom path to the SSL certificate (`proxy.whatsapp.net.pem`) |
| `--install` | Installs the background system service (requires `--proxy`) |
| `--change-proxy <addr>` | Updates the service's upstream proxy and restarts the service |
| `--status` | Displays service status, running state, and configured proxy |
| `--start` / `--stop` / `--restart` | Controls the background service |
| `--uninstall` | Removes the background service |
| `--service` | Internal flag used by service managers (SCM / systemd) |
| `--help`, `-h` | Displays usage and help |
