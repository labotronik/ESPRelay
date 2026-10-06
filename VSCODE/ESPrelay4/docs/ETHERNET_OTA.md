# Compiler et mettre à jour par Ethernet avec PlatformIO

La carte doit déjà exécuter un firmware proposant `/api/ota`, être accessible
par Ethernet et disposer de partitions OTA. La configuration `default.csv`
du projet prévoit ces partitions. Le premier flash d'une carte vierge se fait
par USB.

1. Copier `platformio.local.ini.example` vers `platformio.local.ini` à la racine.
2. Renseigner `upload_port` avec l'adresse IP de la carte et les identifiants
   de son interface web dans `custom_ota_user` et `custom_ota_password`.
   Le fichier local est ignoré par Git. Les variables d'environnement
   `ESPRELAY_OTA_USER` et `ESPRELAY_OTA_PASSWORD` peuvent remplacer ces valeurs.
3. Dans VS Code, ouvrir **PlatformIO → Project Tasks → esp32s3_custom_n4_ethernet**.
4. Utiliser **General → Build** pour compiler, puis **General → Upload** pour
   compiler et envoyer le firmware par Ethernet. Après son redémarrage, PlatformIO
   sauvegarde les réglages, génère une image LittleFS avec la nouvelle interface
   et ces réglages, puis l'envoie automatiquement. La carte redémarre une seconde fois.

L'environnement `esp32s3_custom_n4` conserve le téléversement USB habituel.
Sélectionner l'environnement Ethernet dans la barre PlatformIO pour utiliser
les boutons Build et Upload de la barre d'état avec cette destination.

Depuis un terminal PlatformIO :

```sh
pio run -e esp32s3_custom_n4_ethernet
pio run -e esp32s3_custom_n4_ethernet -t upload
```

Pour mettre à jour l'interface web, utiliser **Platform → Build Filesystem Image**,
puis **Platform → Upload Filesystem Image** dans ce même environnement Ethernet.
La commande correspondante compile l'image LittleFS et l'envoie :

```sh
pio run -e esp32s3_custom_n4_ethernet -t uploadfs
```

L'envoi Ethernet conserve réseau, MQTT, règles, WiFi, BLE et identifiants :
le script récupère `/api/backup` et ajoute ces fichiers à une copie temporaire
de `data/` avant de créer l'image à envoyer. Les fichiers du projet restent
inchangés. Si la sauvegarde est inaccessible ou incomplète, l'envoi LittleFS
est annulé. Le firmware doit inclure les champs WiFi et BLE dans sa sauvegarde ;
un **Upload** de cette version l'installe avant l'envoi automatique de LittleFS.
Les autres fichiers personnalisés présents uniquement sur la carte ne sont pas
conservés. Recharger la page avec Ctrl + F5 après la mise à jour.

Pour envoyer uniquement le firmware, ajouter `custom_upload_web = no` dans
`platformio.local.ini`, sous `[env:esp32s3_custom_n4_ethernet]`.
**Build Filesystem Image** génère une image avec les seuls fichiers du projet,
sans contacter la carte ; la conservation des réglages intervient lors de
**Upload Filesystem Image** par Ethernet. Un envoi manuel de l'image brute depuis
le navigateur remplace toujours les réglages.

Le script envoie le firmware à `/api/ota` et LittleFS à `/api/otafs`, avec
authentification et contrôle SHA-256. Le protocole est HTTP, comme l'interface
de la carte.

L'intégration utilise le [protocole custom de PlatformIO](https://docs.platformio.org/en/latest/projectconf/sections/env/options/upload/upload_command.html).
