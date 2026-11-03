# OmniGuard - Sistema di Videosorveglianza per 3 Telecamere IP Zosi

Applicazione desktop C++/Qt6 per il monitoraggio in tempo reale di esattamente 3 telecamere IP Zosi tramite flussi RTSP. Progettata per la **stabilità assoluta** con architettura multithread, recupero automatico e gestione robusta delle risorse.

## Caratteristiche Principali

- **Architettura Multithread**: Un thread dedicato per ciascuna telecamera (3 thread totali)
- **Isolamento Completo**: Guasto di una telecamera non influenza le altre
- **Auto-Riconnessione**: Loop di tentativi ogni 3 secondi (configurabile) con backoff esponenziale
- **Thread-Safe GUI**: Comunicazione via Qt Signals/Slots (QueuedConnection)
- **Rendering Ottimizzato**: QImage con scaling bilineare, doppio buffering
- **Gestione Memoria RAII**: Smart pointers, cleanup automatico FFmpeg
- **Graceful Shutdown**: Gestione segnali SIGINT/SIGTERM
- **Configurazione JSON**: File esterno per URL RTSP, credenziali, timeout

## Requisiti di Sistema

- Linux (X11 o Wayland)
- Qt 6.2+
- FFmpeg 4.4+ (libavformat, libavcodec, libavutil, libswscale)
- CMake 3.16+
- Compilatore C++20 (GCC 10+, Clang 12+)
## Installazione Dipendenze (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    qt6-base-dev \
    qt6-base-dev-tools \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libswscale-dev \
    pkg-config
```

### Verifica Installazione

```bash
# Verifica Qt6
qmake6 --version

# Verifica FFmpeg
pkg-config --modversion libavformat libavcodec libavutil libswscale
```

## Compilazione

```bash
cd /home/admin/vscode/OmniGuard
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Opzioni CMake

```bash
# Build tipo Release (default)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build tipo Debug
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Installazione system-wide (opzionale)
sudo make install
```

## Esecuzione

```bash
# Esecuzione standard
./OmniGuard

# Con file configurazione personalizzato
./OmniGuard -c /percorso/cameras.json

# Schermo intero
./OmniGuard -f

# Aiuto
./OmniGuard --help
```

## Configurazione

Il file di configurazione predefinito si trova in:
- Linux: `~/.config/OmniGuard/cameras.json`

Copia l'esempio e modifica:
```bash
cp cameras.json.example ~/.config/OmniGuard/cameras.json
```

### Parametri Configurabili per Camera

| Parametro | Default | Descrizione |
|-----------|---------|-------------|
| `id` | 0-2 | Indice camera (0, 1, 2) |
| `name` | "Camera N" | Nome visualizzato |
| `rtspUrl` | - | URL RTSP completo |
| `username` | "admin" | Username RTSP |
| `password` | "admin" | Password RTSP |
| `reconnectIntervalMs` | 3000 | Intervallo base riconnessione (ms) |
| `connectionTimeoutMs` | 10000 | Timeout connessione (µs) |
| `receiveTimeoutMs` | 5000 | Timeout ricezione dati (µs) |
| `enableTcp` | true | Usa TCP per RTSP (più affidabile) |
| `enableBuffering` | true | Abilita buffer di rete |

### Esempio URL RTSP Zosi

Formati comuni per telecamere Zosi:
```
rtsp://user:pass@192.168.1.100:554/stream1     # Stream principale
rtsp://user:pass@192.168.1.100:554/stream2     # Stream secondario
rtsp://192.168.1.100:554/h264Preview_01_main   # Alternativo
```

## Architettura

```
Main Thread (GUI)          Thread Cam 0          Thread Cam 1          Thread Cam 2
┌─────────────────┐       ┌──────────────┐      ┌──────────────┐      ┌──────────────┐
│ VideoWidget x3  │◄─────►│ FFmpeg       │      │ FFmpeg       │      │ FFmpeg       │
│ Qt Signals/Slots│       │ avformat     │      │ avformat     │      │ avformat     │
│ (QueuedConn)    │       │ avcodec      │      │ avcodec      │      │ avcodec      │
└─────────────────┘       │ swscale      │      │ swscale      │      │ swscale      │
## Scorciatoie Tastiera

| Tasto | Azione |
|-------|--------|
| `F11` | Toggle schermo intero |
| `Ctrl+Q` | Esci |
| `Ctrl+P` | Preferenze (TODO) |

## Risoluzione Problemi

### Camera non connette
1. Verifica URL RTSP con VLC: `vlc rtsp://...`
2. Controlla firewall/porte (554 TCP/UDP)
3. Prova `enableTcp: false` per UDP
4. Verifica credenziali nel file config

### FPS basso / frame persi
1. Aumenta `buffer_size` (es. 2MB)
2. Riduci risoluzione stream da camera
3. Verifica carico CPU (`htop`)
4. Controlla latenza rete (`ping`)

### Crash all'avvio
1. Verifica librerie: `ldd OmniGuard | grep -i ffmpeg`
2. Controlla versioni Qt/FFmpeg compatibili
3. Esegui con `gdb ./OmniGuard` per backtrace

### Memory leak
- Usa `valgrind --leak-check=full ./OmniGuard`
- Verifica distruttori `StreamWorker::~StreamWorker()`

## Struttura Progetto

```
OmniGuard/
├── CMakeLists.txt           # Build system
├── README.md                # Questa documentazione
├── cameras.json.example     # Configurazione esempio
├── include/
│   ├── Common.h             # Tipi condivisi, enum, struct
│   ├── CameraConfig.h       # Gestione configurazione JSON
│   ├── StreamWorker.h       # Worker thread FFmpeg
│   └── MainWindow.h         # Finestra principale + VideoWidget
└── src/
    ├── main.cpp             # Entry point, signal handling
    ├── CameraConfig.cpp     # Implementazione config manager
    ├── StreamWorker.cpp     # Loop FFmpeg, decodifica, riconnessione
    └── MainWindow.cpp       # GUI, layout, status bar
```

## Licenza

Proprietario - OmniGuard v1.0

## Supporto

Per problemi di compilazione o runtime, verificare:
1. Log applicazione (stdout/stderr)
2. `journalctl -f` per log di sistema
3. Versione FFmpeg: `ffmpeg -version`
4. Versione Qt: `qmake6 --version`
                          │ Auto-Reconnect│      │ Auto-Reconnect│      │ Auto-Reconnect│
                          └──────────────┘      └──────────────┘      └──────────────┘
```

### Flusso Dati per Camera

1. **Thread Worker** apre connessione RTSP via `avformat_open_input()`
2. **Demuxing**: `av_read_frame()` estrae pacchetti video
3. **Decodifica**: `avcodec_send_packet()` / `avcodec_receive_frame()`
4. **Conversione**: `sws_scale()` YUV → RGB24
5. **Copia Thread-Safe**: `QImage::copy()` per proprietà esclusiva
6. **Signal**: `frameReady(FrameData)` → Main Thread (QueuedConnection)
7. **GUI**: `VideoWidget::setImage()` → `update()` → `paintEvent()`

## Gestione Errori e Robustezza

### Riconnessione Automatica
- Tentativo ogni 3 secondi (configurabile)
- Backoff esponenziale dopo 5 fallimenti (max 60s)
- Massimo 10 errori decodifica consecutivi prima di forzar riconnessione

### Timeout Configurati
- `stimeout`: Timeout connessione TCP (10s default)
- `rw_timeout`: Timeout lettura/scrittura (5s default)
- `reorder_queue_size`: Buffer riordino pacchetti (100)

### Pulizia Risorse
- `avformat_close_input()` chiude connessione rete
- `avcodec_free_context()` libera decoder
- `sws_freeContext()` libera scaler
- `av_frame_free()`, `av_packet_free()` liberano frame/pacchetti
- `av_free()` per buffer RGB
- Distruttori RAII per tutto