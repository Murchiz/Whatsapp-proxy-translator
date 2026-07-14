# WhatsApp Proxy SOCKS5/HTTP Translator (Standalone)

This is a highly optimized, asynchronous, standalone C++ proxy translator. It completely replaces the official WhatsApp Proxy (HAProxy) and acts as the bridging link to an upstream SOCKS5 or HTTP proxy. 
It uses standalone `asio` for non-blocking networking and `OpenSSL` for SSL termination.

## Features

1. **HAProxy Independent:** You no longer need to run or compile HAProxy. This tool implements the HAProxy PROXY Protocol v1 internally and bridges traffic natively.
2. **Auto Certificates:** The tool automatically generates the required `proxy.whatsapp.net.pem` via OpenSSL if it doesn't exist.
3. **SSL Termination:** Intercepts and decrypts port 443 WhatsApp HTTPS traffic locally before tunneling to `g.whatsapp.net:5222` via your proxy.
4. **Optimized:** Multithreaded and asynchronous design.
5. **Windows Service:** Can be run natively as a background Windows Service.

## Requirements

1. **CMake** (3.14+)
2. **OpenSSL** must be installed on your system. If you use Scoop, you can install it via:
   ```bash
   scoop install openssl
   ```

## Compilation

```bash
cd E:\Tyt\repos\whatsapp_proxy\translator
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## Usage

### Console Mode

```bash
translator.exe --proxy 127.0.0.1:1080 --type socks5
```

For HTTP proxies:
```bash
translator.exe --proxy 192.168.1.100:3128 --type http
```

### Windows Service Mode

You can install this translator as a background Windows service. Run this command with Administrator privileges:

```bash
translator.exe --install
```

After installing, open `services.msc`, find **WhatsApp Proxy Translator**, configure the launch parameters (e.g., in Registry under `ImagePath` by appending your `--proxy` arguments), and start the service.
