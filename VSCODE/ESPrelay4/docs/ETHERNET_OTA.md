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
   compiler, sauvegarder les réglages actuels puis envoyer le firmware par Ethernet.
   Après vérification du redémarrage et de la version, PlatformIO génère une image
   LittleFS avec la nouvelle interface et les réglages sauvegardés avant l’envoi,
   puis l’envoie automatiquement. La carte redémarre une seconde fois et les
   réglages restaurés sont vérifiés.

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
le script récupère `/api/backup` avant le premier upload et ajoute ces fichiers à une copie temporaire
de `data/` avant de créer l'image à envoyer. Les fichiers du projet restent
inchangés. Si la sauvegarde est inaccessible ou incomplète, l'envoi LittleFS
est annulé. Le firmware doit inclure les champs WiFi et BLE dans sa sauvegarde ;
si le firmware installé ne fournit pas WiFi/BLE dans l’export, la procédure
s’arrête avant tout upload. Installer d’abord un firmware compatible par USB,
après avoir sauvegardé les réglages existants.
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

Les sauvegardes durables sont stockées dans `.pio/ota-backups/`, avec un nom
unique et des permissions `0600` sur Unix (répertoire `0700`). Elles contiennent
les mots de passe. Elles restent disponibles même si l’upload ou sa vérification
échoue. Pour restaurer, importer la section `settings` du fichier dans l’interface
web : l’enveloppe complète contient également destination et identité de la carte.
Un upload filesystem seul vérifie la version déjà installée, indépendamment de
la version du code local. Le transfert automatique vérifie la version du firmware
envoyé. Aucune erreur de vérification n’est assimilée à une réussite.

Les paramètres sont désormais enregistrés dans un document atomique
`/settings.json` (schéma interne 1), distinct des fichiers JSON historiques.
L’image Ethernet contient les deux formats pour assurer la compatibilité.
L’import/export HTTP utilise le schéma 2 et couvre règles, réseau, MQTT, WiFi, BLE
et identifiants HTTP. Un import historique sans schéma conserve les sections
auxiliaires absentes ; un import de schéma 2 exige les six sections.
