# flutter_mqtt_relay

Application Flutter MQTT pour piloter la carte ESPRelay4.

## Lancement

```bash
cd flutter_mqtt_relay
flutter pub get
flutter run
```

## Configuration MQTT

Dans l'onglet `MQTT`:

- `Broker host`: IP ou DNS du broker.
- `Broker port`: port MQTT (1883 par defaut).
- `Username` / `Password`: optionnel.
- `Base topic`: ex. `esprelay4`.
- `Client ID`: identifiant MQTT du client Flutter.

Puis cliquer `Save + Connect`.

## Setup relais

Dans l'onglet `Setup`, vous pouvez renommer les relais `R1..R4`.
Les noms sont sauvegardes localement dans l'application.

## Topics utilises

Commandes relais:

- `<base>/relay/1/set` payload `ON|OFF|AUTO`

Etats ecoutes:

- `<base>/status` (`online/offline`)
- `<base>/net/ip`
- `<base>/relay/<n>/state`
- `<base>/input/<n>/state`
- `<base>/vin/<n>/state`

L'interface reprend la presentation de la page Ethernet:

- bandeau de statuts (pills),
- onglet `Input` dedie aux etats des entrees `E1..E4` et `VIN`,
- cartes modules (dashboard),
- lignes relais avec etat/entree et actions `ON/OFF/AUTO`.
