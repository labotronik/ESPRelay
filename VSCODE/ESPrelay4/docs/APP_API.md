# Documentation — ESPRelay4

## Vue d’ensemble
ESPRelay4 est un firmware ESP32‑S3 qui pilote des modules PCA9538 (I2C) et expose :
- une interface Web (LittleFS) ;
- une API HTTP JSON ;
- MQTT (avec auto‑découverte Home Assistant).

Fonctions principales :
- Entrées digitales (E*) via PCA9538 ;
- Relais (R*) via PCA9538 ;
- Règles logiques (FOLLOW/AND/OR/XOR/TOGGLE/PULSE) ;
- Volets (jusqu’à 2, 2 relais chacun, interlock + dead‑time) ;
- Réseau (DHCP ou statique) ;
- MQTT configurable ;
- Backup/restore complet ;
- Capteurs température DS18B20 (1‑Wire) et DHT22.

---

## Matériel & bus
### I2C / PCA9538
- PCA9538 adressés à partir de `0x70`.
- Scan automatique `0x70..0x73` (max 4 modules).
- Chaque module = 4 sorties + 4 entrées :
  - IO0..IO3 = relais (R)
  - IO4..IO7 = entrées (E)

### 1‑Wire / DS18B20
- Broche configurable : `PIN_ONEWIRE` (IO1 par défaut).

### DHT22
- Broche configurable : `PIN_DHT` (IO2 par défaut).

---

## Numérotation E/R
Les entrées et relais sont **globaux** :
- Module 1 : E1..E4, R1..R4
- Module 2 : E5..E8, R5..R8
- Module 3 : E9..E12, R9..R12
- Module 4 : E13..E16, R13..R16

L’UI propose une vue par module, mais les règles utilisent l’index global.

---

## Règles (rules.json)
Structure (résumé) :
```json
{
  "version": 2,
  "relays": [
    {
      "expr": { "op": "FOLLOW", "in": 1 },
      "invert": false,
      "onDelay": 0,
      "offDelay": 0,
      "pulseMs": 200
    }
  ],
  "shutters": []
}
```
### Opérateurs
- `FOLLOW` : suit une entrée
- `AND`, `OR`, `XOR` : logique multi‑entrées
- `TOGGLE_RISE` : bascule sur front montant
- `PULSE_RISE` : pulse sur front montant

### Volets (shutters[])
Jusqu’à 2 volets :
```json
{
  "name": "Volet 1",
  "up_in": 1,
  "down_in": 2,
  "up_relay": 1,
  "down_relay": 2,
  "mode": "hold",
  "priority": "stop",
  "deadtime_ms": 400,
  "max_run_ms": 25000
}
```
Contraintes :
- `up_relay` ≠ `down_relay`
- Les deux volets ne doivent pas partager de relais

---

## API HTTP
### GET /
Servir la page Web depuis LittleFS.

### GET /api/state
État global (entrées, relais, overrides, volets, capteurs, réseau).
```json
{
  "inputs": [0,1,...],
  "relays": [0,1,...],
  "override": [-1,0,1,...],
  "reserved": [0,1,...],
  "eth": { "link": 1, "ip": "192.168.1.50" },
  "shutter": { "enabled": 1, "name": "Volet 1", "up_relay": 1, "down_relay": 2, "move": "up", "cooldown_ms": 0 },
  "shutters": [
    { "enabled": 1, "name": "Volet 1", "up_relay": 1, "down_relay": 2, "move": "up", "cooldown_ms": 0 },
    { "enabled": 0 }
  ],
  "temps": [
    { "addr": "28FF...", "c": 21.45 },
    { "addr": "DHT22", "c": 22.10, "h": 48.2 }
  ],
  "modules": 2,
  "relays_per": 4,
  "inputs_per": 4,
  "total_relays": 8,
  "total_inputs": 8,
  "uptime_ms": 12345678
}
```

### GET /api/rules
Retourne le JSON complet des règles (relays + shutters).

### PUT /api/rules
Remplace toutes les règles.
- Validation taille `relays` = `total_relays`.
- Validation volets + conflits de relais.

### POST /api/override
```json
{ "relay": 1, "mode": "AUTO|FORCE_ON|FORCE_OFF" }
```
Refus si relais réservé par volet.

### POST /api/shutter
```json
{ "id": 1, "cmd": "UP|DOWN|STOP|AUTO" }
```
`id` ∈ {1,2}

### GET /api/net
Config réseau (inclut MAC Ethernet).

### PUT /api/net
```json
{
  "mode": "dhcp|static",
  "ip": "192.168.1.50",
  "gw": "192.168.1.1",
  "sn": "255.255.255.0",
  "dns": "192.168.1.1"
}
```

### GET /api/mqtt
Config MQTT (inclut état connecté).

### PUT /api/mqtt
```json
{
  "enabled": 1,
  "host": "192.168.1.43",
  "port": 1883,
  "user": "",
  "pass": "",
  "client_id": "ESPRelay4",
  "base": "esprelay4",
  "discovery_prefix": "homeassistant",
  "retain": 1
}
```

### GET /api/backup
Retourne un backup complet :
```json
{ "rules": { ... }, "net": { ... }, "mqtt": { ... } }
```

### PUT /api/backup
Applique et redémarre la carte.

---

## MQTT (Home Assistant)
### Disponibilité
`<base>/status` = `online|offline`

### Relais
- `base/relay/<n>/set` (ON/OFF/AUTO)
- `base/relay/<n>/state`

### Entrées
- `base/input/<n>/state`

### Volets
2 covers :
- `base/shutter/1/set` / `base/shutter/1/state`
- `base/shutter/2/set` / `base/shutter/2/state`
Payloads : OPEN / CLOSE / STOP

### Températures
DS18B20 :
- `base/temp/<n>/state`

DHT22 :
- Température : `base/temp/dht/state`
- Humidité : `base/hum/dht/state`

---

## UI Web
Onglets :
- Dashboard : état entrées/relais/volets + capteurs si détectés
- Programmation : règles + volets 1/2
- JSON : vue JSON complet
- Réseau : IP + MQTT
- Backup : export / import

---

## Notes
- `millis()` overflow ~49,7 jours → uptime repart à 0.
- Les relais réservés par volet ignorent overrides et règles simples.
