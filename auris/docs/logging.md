# Auris — Système de Logs

## Vue d'ensemble

Le daemon de log Auris (`auris_log`) est le composant transversal du firmware.
Il démarre au plus tôt après le montage du stockage et centralise tous les
événements du système : BLE, audio, UI, erreurs.

Chaque module du firmware utilise les macros `ALOG_I`, `ALOG_W`, `ALOG_E` pour
émettre des entrées structurées, thread-safe, écrites sur le stockage flash
et mirrored sur la console série.

## Architecture

```
┌──────────┐  ┌──────────┐  ┌──────────┐
│ BLE task │  │Audio task│  │  UI task  │
└────┬─────┘  └────┬─────┘  └────┬─────┘
     │             │              │
     └──── ALOG_I / ALOG_W / ALOG_E ────┐
                                         │
                              ┌──────────▼──────────┐
                              │   FreeRTOS Queue     │
                              │   (32 entrées max)   │
                              └──────────┬──────────┘
                                         │
                              ┌──────────▼──────────┐
                              │  Writer Task (prio 1)│
                              │  - Écriture fichier  │
                              │  - Flush par batch   │
                              │  - Rotation auto     │
                              │  - Compression gzip  │
                              └─────────────────────┘
```

## Fichiers de log

| Fichier | Description |
|---------|-------------|
| `/sdcard/logs/auris.log` | Log courant (append across reboots) |
| `/sdcard/logs/auris_NNNN.log.gz` | Archive compressée (rotation N) |

### Format des entrées

```
[uptime_sec.ms][MODULE][LEVEL] message
```

Exemple :
```
[00000.001][SYS ][I] ======== BOOT #12 ========
[00000.050][BLE ][I] Starting BLE daemon...
[00000.120][BLE ][I] AURIS advertising started
[00032.100][BLE ][I] Connected handle=1 peer=AA:BB:CC:DD:EE:FF type=public
[00032.250][BLE ][I] Encrypted auth=yes bonded=yes keysize=16
[00032.350][GATT][I] Peer subscribed to notifications
[00032.351][GATT][I] AURIS:OK sent to conn 1
[01234.567][BLE ][I] Disconnected peer=AA:BB:CC:DD:EE:FF reason=0x13
```

### Modules

| Code | Source |
|------|--------|
| `SYS` | Système (boot, init, erreurs générales) |
| `BLE` | Bluetooth Low Energy (connexion, pairing, advertising) |
| `GATT` | Service GATT (notifications, subscribe, validation) |
| `AUD` | Audio (décodage, I2S, playback) — à venir |
| `UI` | Interface utilisateur (display, rendu) — à venir |

### Niveaux

| Niveau | Macro | Usage |
|--------|-------|-------|
| `I` | `ALOG_I(mod, fmt, ...)` | Information — événement normal |
| `W` | `ALOG_W(mod, fmt, ...)` | Warning — situation anormale mais récupérable |
| `E` | `ALOG_E(mod, fmt, ...)` | Erreur — échec d'opération |

## Rotation et compression

### Rotation automatique

Le log courant (`auris.log`) est rotaté quand il dépasse **32 Ko**.

Séquence de rotation :
1. Fermeture du fichier courant
2. Compression gzip via `tdefl_compress_mem_to_mem()` (ROM miniz)
3. Sauvegarde en `auris_NNNN.log.gz` (compteur incrémental en NVS)
4. Suppression du fichier non compressé
5. Purge des archives au-delà de **8 fichiers** (les plus anciennes)
6. Ouverture d'un nouveau `auris.log`

### Compression

Format : **gzip** (RFC 1952) — compatible avec `gunzip`, `zcat`, 7-Zip, etc.

L'algorithme deflate est exécuté par la ROM de l'ESP32-S3 (`tdefl_compress_mem_to_mem`),
sans allocation dynamique côté firmware. Le fichier `.gz` contient :
- Header gzip (10 octets)
- Données compressées (deflate)
- CRC-32 (4 octets)
- Taille originale (4 octets)

En cas d'échec de compression (mémoire insuffisante), le fichier est conservé
en clair sous `auris_NNNN.log`.

### Rétention

- **8 archives** maximum conservées sur le stockage flash
- Les plus anciennes sont supprimées automatiquement lors de chaque rotation
- Le stockage total des logs ne dépasse jamais ~256 Ko (8 × 32 Ko compressés)

## Séquence de boot

```
app_main()
  ├── 1. nvs_flash_init()          ← NVS pour boot counter + rotation counter
  ├── 2. mount_virtual_storage()   ← FATFS sur SPI flash
  ├── 3. auris_log_init()          ← Daemon de log (le plus tôt possible)
  │       ├── Création /sdcard/logs/
  │       ├── Incrémentation boot counter (NVS)
  │       ├── Ouverture auris.log (append)
  │       ├── Démarrage writer task (prio 1)
  │       └── Log: "======== BOOT #N ========"
  ├── 4. BLE daemon                ← Tout est loggé à partir d'ici
  └── 5. Player (display + audio)
```

## API C

```c
#include "log/auris_log.h"

// Initialisation (une seule fois, dans app_main après mount storage)
auris_log_init();

// Logging depuis n'importe quelle task
ALOG_I("MOD", "Connexion établie: handle=%d", handle);
ALOG_W("MOD", "Timeout après %d ms", elapsed);
ALOG_E("MOD", "Échec allocation mémoire");
```

### Comportement thread-safe

- Les appels `ALOG_*` sont non-bloquants (queue FreeRTOS)
- Si la queue est pleine (burst de 32+ messages), les messages en excès sont
  silencieusement ignorés — aucune task n'est jamais bloquée
- L'écriture disque est faite par batch par la writer task dédiée

## Ajouter un nouveau module

1. Inclure `"log/auris_log.h"` dans le fichier source
2. Utiliser un code module de 2-4 caractères (ex: `"AUD"`, `"UI"`, `"NET"`)
3. Ajouter le module dans le tableau ci-dessus
4. Utiliser `ALOG_I`/`ALOG_W`/`ALOG_E` pour les événements importants

## Récupération des logs

### Via console série

Les entrées sont automatiquement dupliquées sur la sortie série (UART/USB).
Utiliser `idf.py monitor` pour les voir en temps réel.

### Via le stockage

Les fichiers de log sont sur la partition FATFS montée à `/sdcard/logs/`.
Ils peuvent être lus via :
- Un outil de lecture flash (`esptool.py read_flash`)
- À terme, le transfert BLE vers l'app Android
- Montage USB si le firmware expose un mass storage

### Décompression des archives

```bash
# Décompresser une archive
gunzip auris_0042.log.gz

# Lire sans décompresser
zcat auris_0042.log.gz

# Décompresser toutes les archives
gunzip auris_*.log.gz
```
