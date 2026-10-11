# Commandes BLE dans espR4

La détection d'une carte ne suffit pas à commander les relais. Le firmware et
l'application doivent tous deux prendre en charge les commandes BLE.

Installer le nouvel APK `flutter_espr4/build/app/outputs/flutter-apk/app-release.apk`
et mettre à jour le firmware de la carte avec
`.pio/build/esp32s3_custom_n4/firmware.bin` (fichier application pour l'OTA).
L'ajout par BLE récupère les informations et sélectionne le mode BLE dans
l'application, même si la carte utilise Ethernet. L'enregistrement conserve la
fiche sur le téléphone sans requête HTTP ni modification du réseau de la carte.
L'import HTTP et l'application des réglages MQTT sont des actions facultatives,
accessibles lorsqu'un transport réseau est choisi.

Appuyer ensuite sur Connecter : en mode BLE, aucune connexion MQTT, Ethernet
ou Wi-Fi n'est tentée. Les notifications affichent les entrées, les entrées
virtuelles, les sorties et leurs modes, ainsi que l'IP et le firmware de la carte.
Le même bouton devient **Déconnecter** lorsque la connexion est établie.

Un ancien firmware reste lisible, mais l'application affiche
« BLE en lecture seule : mettre à jour le firmware de la carte. »

## Identifier une carte

Le bouton **Identifier la carte** dans espR4 demande un clignotement rapide
(LED basculée toutes les 100 ms) pendant 10 secondes, puis la LED reprend
son rythme normal (basculement toutes les secondes). Un nouveau clic relance
les 10 secondes. Les relais et les règles continuent à fonctionner.
La commande BLE est `IDENTIFY`. En MQTT, espR4 publie `ON` sans rétention
sur `<baseTopic>/identify/set`, auquel la carte est abonnée sur les deux transports.
Le firmware doit être mis à jour pour prendre en charge cette commande.

## Protocole

Le service est `6E400001-B5A3-F393-E0A9-<ID_CARTE>`.
La caractéristique `6E400002-B5A3-F393-E0A9-<ID_CARTE>` prend en charge
WRITE avec réponse et READ pour consulter le résultat.

- Relais : `R1:ON`, `R1:OFF`, `R1:AUTO`, `R1:TOGGLE`.
- Entrée virtuelle : `V1:ON`, `V1:OFF`, `V1:TOGGLE`.
- Indices : de 1 à 16, limités aux ressources configurées sur la carte.
- Résultats : `PENDING`, puis `OK` ou `ERROR:<raison>`.

Les messages tiennent dans le MTU BLE minimal. L'application n'envoie qu'une
commande à la fois et attend sa confirmation pendant quatre secondes.
`OK` confirme l'acceptation de la commande ; l'état réel vient des notifications
`6E400003-...` et de la lecture `6E400004-...`.

Le callback BLE valide et met la commande en file. La boucle principale la
traite avant l'évaluation des règles, avec les mêmes actions que MQTT.
Les relais réservés à un volet refusent les commandes directes. Les entrées
virtuelles momentanées conservent leur durée d'impulsion configurée.
Une déconnexion ferme la session Flutter et désactive les commandes BLE.

## Vérifications

`flutter analyze` et `flutter test` dans `flutter_espr4` vérifient l'application,
les commandes et les notifications fragmentées. Les tests hôtes
`test/test_ble_commands.py`, `test/test_input_loop.py` et
`test/test_firmware_safety.py` couvrent le protocole, les limites, les relais
réservés et l'évaluation des impulsions. La validation radio et électrique
nécessite un téléphone et la carte.
