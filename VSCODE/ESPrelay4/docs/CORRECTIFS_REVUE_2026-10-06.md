# Correctifs de la revue du 6 octobre 2026

État initial sauvegardé dans le commit `aa53362`. Ce document décrit les
18 recommandations demandées et leur implémentation. L’audit original reste
consultable dans [REVUE_CODE_2026-10-06.md](REVUE_CODE_2026-10-06.md).

| Recommandation | Changement |
| --- | --- |
| R06 | PCA : charger le registre de sortie OFF avant de passer les broches en sorties. |
| R08 | Réinitialiser les PCA retrouvés, contrôler leurs registres chaque seconde, préserver le nombre d’E/S sauvegardé même si un module manque. Un nouveau module au-delà de cette topologie exige un redémarrage pour être ajouté aux règles. |
| R09 | Comparer les échéances par différence signée ; séparer échéance et activation pour les impulsions, temps morts et mode MQTT rapide. La période de grâce MQTT ne se répète pas après le débordement. Le debounce et les sondages DHT utilisent également un état explicite. |
| R10 | Valider les expressions, opérateurs, entrées, tableaux et durées. Valider les indices de volets avant réduction en `uint8_t`. Une configuration de relais invalide au démarrage désactive les règles et conserve le fichier pour récupération. |
| R11 | Sauvegarde atomique avec contrôle JSON, longueur, relecture et renommage LittleFS. Un document `/settings.json` remplace atomiquement l’ensemble des paramètres. Les requêtes HTTP et les commandes de configuration MQTT appliquent leurs valeurs après l’écriture réussie. |
| R12 | Déconnecter les sessions MQTT existantes lors d’un changement de configuration, puis configurer les serveurs et réinitialiser les annonces/reconnexions. |
| R13 | Publier les commandes relais et entrées virtuelles de `flutter_espr4` avec `retain: false`. |
| R14 | Supprimer les Last Will des applications sur le topic de disponibilité de la carte. |
| R16 | Tester les constantes `SIM_READY` et `SIM_LOCKED` de TinyGSM ; retenter la vérification si la SIM n’est pas prête. Un échec de déverrouillage bloque les tentatives automatiques de PIN jusqu’au redémarrage pour ne pas épuiser leur nombre. |
| R17 | Appeler périodiquement `Ethernet.maintain()` en DHCP ; journaliser renouvellement/échec et fermer la connexion MQTT Ethernet si l’adresse change. |
| R18 | Export/import schéma 2 : règles, réseau, MQTT, WiFi, BLE et accès HTTP, dans une transaction unique. Les imports historiques sans schéma conservent les sections auxiliaires absentes. Les exports contiennent les mots de passe ; cette politique est indiquée dans l’interface. |
| R19 | Sauvegarde locale privée avant tout upload, transmise au second upload sans reprendre les paramètres après migration. Vérification de l’identité, version et redémarrage, puis des réglages après LittleFS. Arrêt en cas de sauvegarde incomplète ou de vérification échouée. |
| R22 | Bloquer les écritures après échec du GET des règles, garder la dernière lecture et refuser les réponses incomplètes. Une révision SHA-256 accompagne GET/PUT ; le serveur refuse une révision obsolète avec HTTP 409. |
| R23 | Échapper les noms de volets avant insertion HTML. |
| R25 | Nettoyage BLE sans mise à jour après destruction ; invalidation des opérations asynchrones, contrôle du widget après attentes, lectures périodiques sans chevauchement. |
| R26 | Remplacer le test de compteur inexistant par des tests de l’écran BLE, de fermeture et de reconstruction des trames. Les trames sont tamponnées en octets pour supporter la fragmentation UTF-8 et les trames consécutives. |
| R28 | Séparer demande de conversion DS18B20 et lecture ; attendre selon la résolution. Mesures invalides/nulles au lieu de -127, validité/âge/statut dans l’API et disponibilité du capteur dans la découverte MQTT. |
| R29 | Préserver le runtime des règles/volets inchangés ; réinitialiser les mémoires des règles modifiées ou changeant de propriétaire. Arrêter les anciennes sorties d’un volet recâblé et appliquer un temps mort au nouveau câblage. |

## Compatibilité et vérifications

Les anciens fichiers de paramètres restent lisibles. La première sauvegarde
réussie les migre vers le document atomique sans les supprimer. Le document
canonique devient ensuite la référence. Une image LittleFS Ethernet injecte
les deux formats ; un flash USB brut du filesystem ne préserve pas ces paramètres.

`PUT /api/rules` doit reprendre le champ `_revision` obtenu par le dernier
`GET /api/rules`. Après HTTP 409, recharger les règles avant de modifier.
`PUT /api/backup` reste une restauration complète explicite et n’utilise pas
cette révision. Pour restaurer un fichier de l’uploader, importer sa section
`settings`, comme décrit dans [ETHERNET_OTA.md](ETHERNET_OTA.md).

Vérifications automatisées :

- `python3 test/test_firmware_safety.py` : code firmware réel avec ArduinoJson,
  matériel simulé, écritures courtes/renommage refusé, PCA, schéma, débordement,
  runtime sélectif, conversion de température, renouvellement DHCP et états SIM.
- `python3 test/test_input_loop.py` : ordre des entrées/commandes et fronts.
- `python3 test/test_mqtt_reconnect.py` : nettoyage avant abonnement MQTT.
- `python3 -m unittest discover -s test -p test_ethernet_upload.py` : sauvegarde
  avant POST, permissions, copie temporaire, transmission entre uploads et contrôles.
- `node test/test_web_rules.js` : lecture échouée, blocage des écritures,
  conflit de révision et échappement HTML.
- Analyse Dart des trois applications et tests Flutter BLE.
- Compilation des environnements USB/Ethernet et de leurs images LittleFS.

Aucun firmware n’a été envoyé à la carte pendant ces vérifications. Les garanties
électriques réelles, les coupures d’alimentation LittleFS, le broker HA,
le DHCP réel et le modem doivent encore être vérifiés sur matériel.
Les recommandations non sélectionnées restent celles de l’audit original,
notamment les appels réseau bloquants, certains cas d’inversion/arrêt de volet,
les défauts d’écriture matériels et la limite de quatre E/S du client Flutter principal.

## Investigation VInput2 → R2 après la revue

La configuration lue sur la carte est VInput2 momentané 500 ms et R2
`PULSE_RISE` sur l’entrée 2, durée 500 ms, en AUTO, sans réservation par un volet.
L’observation en lecture seule a capturé la commande MQTT `PRESS`, puis les
transitions de `/api/state` : VInput2=ON et R2=ON, puis tous deux OFF environ
500 ms plus tard. Ces valeurs décrivent la demande logicielle ; elles ne
prouvent pas l’alimentation physique de la bobine du relais.

Le défaut R15 est confirmé : les publications MQTT étant suspendues pendant
700 ms après une commande, l’impulsion entière restait invisible dans HA.
Les changements d’entrée, VInput, relais, modes et volets sont désormais
publiés après application des sorties, indépendamment de cette pause.
Une publication échouée ne met plus à jour la mémoire des états publiés.

Pour vérifier le problème physique signalé, les écritures PCA sont contrôlées
et le registre de sortie est relu lors d’un changement ou après un échec.
La direction des broches est contrôlée avant une nouvelle commande : un PCA
réinitialisé est remis en configuration immédiatement, pour qu’une impulsion
plus courte que le sondage d’une seconde ne soit pas perdue.
L’API expose `relay_outputs` (registre de sortie confirmé, sinon null),
`modules_output_ok` et `modules_output_fail`. L’interface indique
« Sorties non confirmées » en cas d’erreur. Ce contrôle du registre ne
constitue pas une mesure du contact ni de la bobine du relais.

Le test firmware utilise désormais les vraies fonctions `evalSimpleRules`,
`buildFinalRelays`, `pcaApplyRelays` et `mqttPublishControlStates`. Il vérifie
VInput2/R2 à 500 ms, les deux messages ON/OFF malgré la pause MQTT,
les erreurs d’écriture/relecture, la récupération après reset du PCA
et la reprise des publications après un échec. Aucun upload ni commande
de relais n’a été effectué pendant l’investigation ; les appuis ont été
réalisés par l’utilisateur dans HA.
