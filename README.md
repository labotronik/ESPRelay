# ESPRelay4

Documentation rapide d'utilisation de la carte relais (Ethernet, MQTT, Wi-Fi AP fallback, BLE) d'après le firmware actuel.

## 1) Démarrage et accès

- Le serveur HTTP écoute sur le port `80`.
- La page web principale est servie sur `/`.
- Réseau Ethernet W5500 actif au boot.

Configuration réseau par défaut (si aucun fichier de config):
- IP: `192.168.1.50`
- GW: `192.168.1.1`
- Mask: `255.255.255.0`
- DNS: `192.168.1.1`

## 2) API HTTP disponible

Endpoints principaux:
- `GET /api/state` -> état courant (inputs, relays, overrides, modules, réseau, volets, etc.)
- `GET /api/rules` -> règles simples + volets
- `GET /api/net` -> config réseau
- `GET /api/wifi` -> config/status Wi-Fi AP
- `GET /api/mqtt` -> config/status MQTT (transport actif, état GSM)
- `GET /api/backup` -> backup global
- `PUT /api/rules` -> applique des règles
- `PUT /api/net` -> applique réseau
- `PUT /api/wifi` -> active/désactive AP Wi-Fi
- `PUT /api/mqtt` -> applique config MQTT
- `POST /api/override` -> force un relais (`AUTO|FORCE_ON|FORCE_OFF`)
- `POST /api/shutter` -> commande volet (`UP|DOWN|STOP|AUTO`)
- `POST /api/ota` -> OTA firmware binaire
- `POST /api/otafs` -> OTA LittleFS binaire

Authentification:
- Défaut: `admin / admin`
- API de changement: `PUT /api/auth`

## 3) Utilisation MQTT (Ethernet + fallback GSM)

### 3.1 Principe de transport

Le firmware peut utiliser:
- broker local (Ethernet)
- broker GSM (A7670 + 1NCE)

Comportement actuel recommandé:
- tentative sur broker local d'abord
- si échec local, fallback sur broker GSM public (`gsm_mqtt_host`)

### 3.2 Paramètres MQTT (via `PUT /api/mqtt`)

Champs utiles:
- `enabled`: `1` pour activer MQTT
- `transport`: `ethernet`, `auto` ou `gsm`
- `host`, `port`, `user`, `pass`: broker principal (local)
- `gsm_mqtt_host`, `gsm_mqtt_port`, `gsm_mqtt_user`, `gsm_mqtt_pass`: broker GSM
- `client_id`: identifiant MQTT
- `base`: base topic (ex: `esprelay4`)
- `retain`: `1` recommandé pour récupérer un état immédiatement après subscribe
- `apn`, `gsm_user`, `gsm_pass`: paramètres data opérateur GSM

Exemple minimal:
```json
{
  "enabled": 1,
  "transport": "ethernet",
  "host": "192.168.1.43",
  "port": 1883,
  "gsm_mqtt_host": "82.64.24.196",
  "gsm_mqtt_port": 1883,
  "client_id": "ESPRelay4",
  "base": "esprelay4",
  "retain": 1,
  "apn": "iot.1nce.net",
  "gsm_user": "",
  "gsm_pass": ""
}
```

### 3.3 Commandes MQTT relais

Base topic par défaut: `esprelay4`

Relais 1:
- commande: `esprelay4/relay/1/set`
- payload: `ON`, `OFF`, `AUTO`, `TOGGLE`
- état: `esprelay4/relay/1/state`

Exemples:
```bash
mosquitto_sub -h 82.64.24.196 -p 1883 -t esprelay4/relay/1/state -v
mosquitto_pub -h 82.64.24.196 -p 1883 -t esprelay4/relay/1/set -m ON
```

Important:
- Si un relais est réservé à un volet, les overrides directs sont refusés/ignorés.
- `set ON/OFF` force le relais (override) tant que `AUTO` n'est pas envoyé.

### 3.4 Keepalive MQTT

- Paramètre firmware: `MQTT_KEEPALIVE_SECONDS`
- Valeur actuelle: `1800` secondes (30 min)

### 3.5 Mode GSM économie data

Quand transport actif = GSM:
- publication limitée aux états de pilotage utiles
- pas de discovery Home Assistant en boucle
- pas de télémétrie non essentielle en continu

## 4) Wi-Fi (AP fallback)

Le Wi-Fi est utilisé ici en mode AP local de maintenance.

- AP SoftAP activable/désactivable via `/api/wifi` ou MQTT:
  - `esprelay4/wifi/ap/set` avec payload `ON` ou `OFF`
- SSID par défaut: basé sur l'identifiant de la carte (`ESPRelay4-XXXXXX`)
- Mot de passe par défaut: `esprelay4`
- Captive portal simplifié (redirections vers l'IP AP)

## 5) BLE

Le firmware expose l'état en BLE (NimBLE):
- Device name: similaire au SSID (`ESPRelay4-XXXXXX`)
- Service UUID dynamique basé sur MAC:
  - `6E400001-B5A3-F393-E0A9-XXXXXXXXXXXX`
- Characteristic notify/read:
  - `6E400003-B5A3-F393-E0A9-XXXXXXXXXXXX` (READ + NOTIFY)
- Characteristic read:
  - `6E400004-B5A3-F393-E0A9-XXXXXXXXXXXX` (READ)

Particularités flux notify:
- cadence ~1 Hz
- tramage: `0x1E 0x1E + payload JSON + 0x1F`
- chunk BLE max ~160 bytes

Activation BLE:
- via MQTT: `esprelay4/ble/set` avec `ON`/`OFF`
- via API config BLE (`/ble.json`)

## 6) Règles / Volets / Priorités

Ordre de priorité des sorties:
1. Règles simples
2. Logique volet (si relais réservés)
3. Override manuel (MQTT/API) sur relais non réservés
4. Sécurité finale interlock volet

Conséquence:
- un `relay/x/set ON` écrase la règle tant que `AUTO` n'est pas renvoyé
- sur relais réservé volet: override interdit

## 7) Factory reset

- Maintenir le bouton factory (`IO0`) pendant ~10 secondes au boot
- Supprime les fichiers config: `/net.json`, `/mqtt.json`, `/rules.json`, `/auth.json`, `/wifi.json`, `/ble.json`
- Redémarrage automatique

## 8) Estimation conso data GSM

Hypothèses:
- Forfait: 500 Mo sur 10 ans
- Conso idle estimée: 10 Ko/jour
- 1 activation relais + lecture d'état MQTT: ~0,3 Ko

Seuil moyen:
- 500 Mo / 10 ans ~= 140 Ko/jour
- Après idle (10 Ko/jour), il reste ~130 Ko/jour pour les actions

Cas d'usage pouvant vider 500 Mo:
- ~430 activations/jour avec lecture d'état, sur la durée
- ou moins si réseau instable (reconnexions MQTT fréquentes)
- OTA via GSM: plusieurs Mo ponctuels

Exemple à risque:
- ~150 activations/jour + ~10 à 15 reconnexions MQTT/jour sur la durée
