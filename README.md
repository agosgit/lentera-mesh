# Lentera Mesh

**Lentera Mesh** adalah sistem komunikasi **LoRa mesh berbasis ESP32** untuk pengiriman data telemetri kapal berbentuk **AIS-like payload**, dengan dukungan **gateway**, **node**, **MQTT**, **SIM800L**, **OTA update**, **web configuration**, dan **logging**.

Project ini terdiri dari dua bagian utama:

- **Node**: perangkat di kapal atau titik sensor yang membaca data GPS, IMU, dan compass, lalu mengirimkan payload melalui LoRa mesh dan/atau GSM MQTT.
- **Gateway**: perangkat penerima LoRa yang meneruskan payload ke server MQTT melalui WiFi atau modem SIM800L.

---

## Features

### Node
- ESP32 based
- LoRa mesh relay
- GPS position reading
- IMU reading using GY-521 / MPU6050
- Compass heading using QMC5883L
- AIS Message 18-like payload encoding
- MQTT publish via SIM800L
- SD card logging
- Offline backlog storage to `temp.csv`
- OTA firmware update
- Web configuration page
- SOS flag support
- Route learning from mesh ping (`PINK`)

### Gateway
- ESP32 based
- LoRa packet receiver
- Mesh route monitoring
- AIS payload forwarding to MQTT
- MQTT connectivity via:
  - WiFi
  - SIM800L modem
- SPIFFS-based configuration storage
- OTA firmware update
- Web configuration page
- Heartbeat publish to MQTT
- Automatic connection recovery

---

## Repository Structure

```text
lentera-mesh/
├── LORA_NODE_V2/        # Firmware for LoRa mesh node
├── LORA_GW_V2/          # Firmware for LoRa mesh gateway
├── .gitignore
└── README.md
```

---

## Folder Description

### `LORA_NODE_V2/`
Firmware untuk **node LoRa mesh**.

Fungsi utama:
- membaca data GPS
- membaca IMU dan heading
- meng-encode data ke format AIS-like payload
- mengirim payload via LoRa mesh
- mengirim payload via MQTT menggunakan SIM800L
- menyimpan log dan backlog ke SD card
- menyediakan OTA dan halaman konfigurasi web

### `LORA_GW_V2/`
Firmware untuk **gateway LoRa mesh**.

Fungsi utama:
- menerima paket LoRa dari node
- membaca route mesh
- mengubah payload menjadi format AIVDM
- meneruskan data ke broker MQTT
- menyediakan mode koneksi WiFi atau modem
- menyediakan OTA dan halaman konfigurasi web
- menyimpan konfigurasi pada SPIFFS

---

## System Overview

Secara umum alur sistem adalah sebagai berikut:

1. **Node** membaca:
   - GPS
   - IMU
   - compass
2. Node membentuk payload AIS-like
3. Payload dikirim:
   - melalui **LoRa mesh**
   - dan/atau melalui **MQTT GSM**
4. **Gateway** menerima paket LoRa
5. Gateway meneruskan data ke **MQTT broker**
6. Data dapat diteruskan ke server monitoring atau dashboard

---

## Communication Flow

### LoRa Mesh Route Discovery
Sistem menggunakan paket `PINK:` untuk mendeteksi jalur mesh menuju gateway.

Contoh:
```text
PINK:GATE6D9FE8
PINK:GATE6D9FE8-525000100
PINK:GATE6D9FE8-525000100-525000101
```

Node akan:
- menyimpan route terpendek
- memilih route dengan RSSI lebih baik jika panjang route sama
- menghindari loop
- melakukan relay route ke node lain

### Payload Message
Data AIS-like dikirim menggunakan format:

```text
MSG:<route>:<payload>
```

Contoh:
```text
MSG:GATE6D9FE8-525000100,13aG?P0P00PD;88MD5MTDwwt0@E>
```

atau saat belum menemukan gateway:

```text
MSG:BROADCAST-525000100:<payload>
```

---

## Main Technologies Used

- **ESP32**
- **LoRa**
- **TinyGPSPlus**
- **GY521 / MPU6050**
- **QMC5883LCompass**
- **TinyGSM**
- **PubSubClient**
- **WebServer**
- **ArduinoJson**
- **SPIFFS**
- **SD Card**
- **OTA Update**

---

## Hardware Requirements

### Node
- ESP32
- LoRa module
- GPS module
- GY-521 / MPU6050
- QMC5883L compass
- SIM800L
- SD card module
- push button for config / SOS
- LEDs for status indication

### Gateway
- ESP32
- LoRa module
- SIM800L or WiFi connection
- push button for config mode
- LEDs for TX/RX/status indication

---

## Configuration Summary

### Node Configuration
Konfigurasi node disimpan di SD card dalam file:

```text
/config.json
```

Field utama:
- `mmsi`
- `shipName`
- `apn`
- `simUser`
- `simPass`
- `mqttHost`
- `mqttPort`
- `mqttUser`
- `mqttPass`
- `mqttTopic`
- `loraInterval`
- `gsmInterval`
- `enableLoRa`
- `enableGSM`

### Gateway Configuration
Konfigurasi gateway disimpan di SPIFFS dalam file:

```text
/config.json
```

Field utama:
- `mmsi`
- `shipName`
- `apn`
- `simUser`
- `simPass`
- `koneksi`
- `wifiSSID`
- `wifiPass`
- `mqttHost`
- `mqttPort`
- `mqttUser`
- `mqttPass`
- `mqttTopic`

---

## OTA and Web Config Mode

Baik node maupun gateway mendukung mode konfigurasi berbasis Access Point.

### Node AP Mode
- SSID: `ESP32-OTA-NODE`
- Password: `mesh12345`

### Gateway AP Mode
- SSID: `ESP32-OTA-GATE`
- Password: `mesh12345`

Mode ini aktif ketika tombol mode ditekan saat boot selama beberapa detik.

Fitur pada halaman web:
- upload firmware OTA
- ubah konfigurasi koneksi
- ubah MQTT config
- ubah parameter pengiriman
- download log untuk node

---

## Logging

### Node
Node menyimpan:
- log sensor dan LoRa ke `log.csv`
- backlog offline MQTT ke `temp.csv`

### Gateway
Gateway menyimpan konfigurasi pada SPIFFS dan menampilkan status via serial monitor.

---

## MQTT

Contoh parameter default MQTT yang digunakan:

- Host: `aisport.ppns.ac.id`
- Port: `1883`
- Username: `mosquitto`
- Password: `mosquitto`

Contoh topic:
```text
lentera/GATE6D9FE8
```

Heartbeat gateway:
```text
lentera/HB
```

Status node:
```text
lentera/status/<mmsi>
```

---

## AIS Payload

Project ini menggunakan format payload yang menyerupai **AIS Message Type 18**.

Informasi yang dibawa antara lain:
- MMSI
- latitude
- longitude
- speed
- heading
- pitch
- roll
- date/time info
- SOS flag

Payload kemudian dapat dikonversi menjadi kalimat:

```text
!AIVDM,1,1,,A,<payload>,0*hh
```

---

## Notes About Security

Beberapa nilai default di dalam source code masih berisi:
- AP password
- MQTT username/password
- APN
- WiFi credentials placeholder

Sebelum repo dijadikan public, sebaiknya:
- hapus credential sensitif
- ganti dengan placeholder
- gunakan file konfigurasi lokal jika diperlukan

Contoh placeholder:
```text
your_apn_here
your_mqtt_user_here
your_mqtt_pass_here
your_wifi_ssid_here
your_wifi_pass_here
```

---

## Suggested Improvements

Beberapa pengembangan yang bisa dilakukan selanjutnya:

- memisahkan shared utility antara node dan gateway
- menambahkan dokumentasi wiring
- menambahkan diagram arsitektur sistem
- menambahkan contoh `config.json`
- menambahkan dashboard monitoring
- menambahkan validasi konfigurasi web
- menambahkan autentikasi untuk OTA/web config
- menambahkan enkripsi atau autentikasi payload
- menambahkan retry dan QoS yang lebih baik pada MQTT

---

## Recommended Future Structure

Jika project berkembang lebih besar, struktur repo dapat dirapikan menjadi:

```text
lentera-mesh/
├── node/
│   ├── src/
│   ├── include/
│   ├── lib/
│   └── platformio.ini
├── gateway/
│   ├── src/
│   ├── include/
│   ├── lib/
│   └── platformio.ini
├── docs/
├── hardware/
├── examples/
├── .gitignore
└── README.md
```

---

## Getting Started

### 1. Clone repository
```bash
git clone https://github.com/your-username/lentera-mesh.git
```

### 2. Open project
Buka folder repository di:
- Visual Studio Code
- PlatformIO

### 3. Choose firmware
Pilih salah satu:
- `LORA_NODE_V2`
- `LORA_GW_V2`

### 4. Build and upload
Compile dan upload firmware ke board ESP32 yang sesuai.

### 5. Configure device
Masuk ke mode AP dan buka halaman konfigurasi untuk mengisi:
- MMSI
- APN
- MQTT host
- topic
- interval
- dan parameter lain

---

## Suggested Repository Naming

Repository ini menggunakan nama:

```text
lentera-mesh
```

Karena project ini mencakup:
- node
- gateway
- routing mesh
- AIS-like telemetry
- LoRa + MQTT bridge

---

## Author

**XinnThink**

---

## License

Belum ditentukan.

Jika ingin, kamu bisa menambahkan license seperti:
- MIT
- Apache-2.0
- GPL-3.0

---

## Disclaimer

Project ini masih dalam tahap pengembangan dan eksperimen.  
Gunakan dengan pengujian yang cukup sebelum dipakai pada sistem operasional nyata.