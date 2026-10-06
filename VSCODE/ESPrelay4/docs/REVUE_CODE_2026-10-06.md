**Revue du code — 6 octobre 2026**

Le code compile, mais plusieurs défauts de commande et de reprise après incident
restent présents. Les corrections prioritaires concernent les volets, les
blocages de la boucle de contrôle et l'initialisation des sorties.
Cette revue inclut les ajouts récents à MQTT et à l'upload Ethernet.
Aucun correctif ni téléversement sur la carte n'a été effectué pendant la revue.

**Périmètre et niveau de preuve**

- Lecture du firmware actif `src/main.cpp`, de l'interface `data/index.html`,
  des traductions, des scripts PlatformIO et des tests existants.
- Inspection des chemins de connexion, de publication MQTT, de BLE et de
  stockage des trois applications : `flutter_espr4`, `flutter_mqtt_relay`,
  `flutter_app`. Analyse statique Dart de leurs sources et tests.
- `ESPHome.yaml` est une configuration alternative, non utilisée par cette
  compilation. `src/main.scan` est un brouillon de diagnostic, non compilé.
  Les fichiers générés des plateformes Flutter et les schémas électroniques
  n'ont pas fait l'objet d'un audit complet.
- Les reproductions utilisent des fonctions extraites du firmware actuel,
  avec horloge et matériel simulés. Elles établissent des défauts de logique,
  sans mesurer les temps électriques ni les contacts des relais.

Priorités : **P1** = commande ou sécurité des sorties, ou perte de configuration
avec un scénario concret ; **P2** = défaut fonctionnel ou de robustesse ;
**P3** = entretien ou optimisation. Le niveau n'indique pas que tous les
scénarios surviennent avec la configuration actuelle de la carte.

**Résultats des vérifications**

| Vérification | Résultat |
| --- | --- |
| Compilation PlatformIO, environnement Ethernet | Réussie |
| Occupation application | 1 147 733 / 1 310 720 octets, soit 87,6 % |
| RAM statique annoncée par le compilateur | 57 384 / 327 680 octets, soit 17,5 % ; le tas à l'exécution n'est pas mesuré |
| Tests Python de l'uploader | 4 tests réussis |
| Tests existants de la boucle d'entrées et de reconnexion MQTT | Réussis |
| Syntaxe JavaScript, `node --check` | Valide |
| Analyse Dart des trois dossiers `lib` | Aucune erreur ; 3 champs inutilisés et 1 remarque de style dans `flutter_app` |
| Analyse Dart des tests | Une erreur : `MyApp` inexistant dans `flutter_app/test/widget_test.dart:16` |
| Reproductions supplémentaires | 3 défauts de volet, 1 blocage OTA et 1 erreur de temporisation reproduits |

Les tests existants passent malgré ces défauts : ils ne constituent donc pas
encore une couverture suffisante de la commande des sorties.

**Défauts prioritaires**

**R01 — P1 — Une coupure pendant l'OTA peut bloquer la carte indéfiniment.**

Emplacement : `src/main.cpp:3652`, `handleOtaStream`.
Quand `c.read()` renvoie zéro ou une valeur négative, le code attend puis
recommence, sans vérifier la connexion ni un délai maximal. La quantité restante
ne diminue jamais. Reproduction : 10 000 lectures vides, aucune sortie de la
boucle et aucun appel à `Update.abort()`.
La boucle principale ne revient plus à la commande des relais.
Ajouter une limite d'inactivité, une durée totale, une vérification de connexion
et un abandon propre. Avant une OTA, mettre les sorties concernées dans un état
de maintenance défini et refuser un mouvement de volet actif.

**R02 — P1 — Le réseau peut suspendre les sécurités temporelles des relais.**

Emplacements : `src/main.cpp:1558`, `gsmEnsureData` ; `src/main.cpp:4425`, `loop`.
HTTP et MQTT s'exécutent avant `shutterTick` et `pcaApplyRelays`.
Une initialisation modem attend explicitement 12 secondes ; l'attachement réseau
peut attendre 10 secondes ; les commandes AT, les connexions et les transferts
HTTP ajoutent leurs propres attentes. Pendant ces opérations, un relais déjà
actif conserve sa sortie, même si sa durée d'impulsion ou `max_run_ms` expire.
Ce point dépasse le seul défaut OTA de R01. Isoler le contrôle périodique des
E/S des opérations réseau lentes, avec une file de commandes et un propriétaire
unique du bus I2C. Mesurer et borner la latence maximale du contrôle.

**R03 — P1 — STOP puis sens opposé contourne le temps mort d'un volet.**

Emplacement : `src/main.cpp:2844`, `shutterCommand`.
STOP efface le mouvement courant sans enregistrer le dernier sens ni l'heure
de coupure. Une commande opposée immédiatement après STOP repart aussitôt.
Reproduction : UP à 100 ms, STOP à 110 ms, DOWN à 111 ms ; DOWN est autorisé
alors que `deadtime_ms` vaut 400 ms.
Mémoriser le dernier sens réellement alimenté et imposer le temps mort depuis
sa coupure, y compris après STOP explicite ou une modification des règles.

**R04 — P1 — Le temps maximal d'un volet n'empêche pas le redémarrage immédiat.**

Emplacement : `src/main.cpp:2894`, `shutterTickOne`.
En mode `hold`, l'expiration appelle `shutterForceStop`. Si le bouton physique
ou virtuel reste actif, la boucle suivante recrée la même demande et redémarre.
Reproduction avec `max_run_ms = 100` : STOP à 200 ms, UP à 201 ms sans nouvel
appui. Le moteur peut donc répéter des cycles sans relâchement du bouton.
Verrouiller l'arrêt jusqu'au relâchement des entrées ou jusqu'à une nouvelle
commande explicitement autorisée.

**R05 — P2 — Une inversion en mode toggle perd la demande du nouveau sens.**

Emplacements : `src/main.cpp:2857` et `src/main.cpp:2928`.
Le front opposé demande DOWN ; `shutterCommand` passe à STOP pour le temps
mort. Au tour suivant, le front a disparu et la demande devient le mouvement
courant, donc STOP. Le nouveau sens n'est pas mémorisé.
Reproduction : après 401 ms d'attente pour un temps mort de 400 ms, le volet
reste arrêté au lieu de repartir dans l'autre sens.
Ajouter une direction demandée persistante, distincte du mouvement effectif.

**R06 — P1 — L'initialisation du PCA9538 active les sorties avant de charger OFF.**

Emplacement : `src/main.cpp:904`, `pcaInitModule`.
`REG_CFG` configure IO0..IO3 en sorties avant l'écriture du registre de sortie.
Après une remise sous tension du PCA9538, son registre de sortie vaut `0xFF`.
Avec `RELAY_ACTIVE_LOW = false`, les broches sont donc initialement commandées
à HIGH avant l'écriture OFF. Le risque électrique est établi par l'ordre du
code et la fiche constructeur ; l'activation mécanique dépend du circuit et
de la durée. Ce cas se distingue des anciens forçages MQTT de R1/R2.
Écrire d'abord le registre de sortie à OFF, puis activer les directions.
Valider les broches à l'oscilloscope, notamment les paires de relais de volet.
Source : [fiche PCA9538, registres Output et Configuration](https://www.ti.com/lit/ds/symlink/pca9538.pdf).

**R07 — P1 — Un arrêt de relais qui échoue sur I2C n'est pas signalé.**

Emplacement : `src/main.cpp:990`, `pcaApplyRelays`.
Le résultat de `i2cWriteReg8` est ignoré ; l'API et MQTT décrivent le tableau
logique `relays`, pas une sortie dont l'écriture a été confirmée.
Une écriture OFF peut échouer alors que le relais reste alimenté. Le code
réessaie aux boucles suivantes, mais ne signale pas l'échec de la sortie et ne
garantit pas le temps mort entre modules si l'une des écritures échoue.
Distinguer demande, valeur écrite et état matériel connu ; compter les échecs
d'écriture, exposer le défaut et définir la stratégie de mise en sécurité.
Une garantie d'arrêt physique nécessite aussi un dispositif matériel adapté.

**R08 — P2 — Un module PCA retrouvé n'est pas réinitialisé.**

Emplacement : `src/main.cpp:950`, `pcaReadInputs`.
Un module absent au scan devient `pcaPresent = true` dès qu'une lecture réussit,
sans repasser par `pcaInitModule`. Après un reset du PCA, les broches peuvent
donc rester en entrées et la polarité ne correspond plus à celle attendue.
De plus, `totalInputs` et `totalRelays` restent ceux du scan initial.
Réinitialiser un module retrouvé, confirmer ses registres et définir une
politique explicite pour les changements du nombre de modules.

**R09 — P1 — Les comparaisons de deadlines échouent au débordement de `millis()`.**

Emplacements : `src/main.cpp:2851`, `2977`, `3028`, ainsi que plusieurs deadlines
MQTT et DHT. Le compteur 32 bits boucle après environ 49,7 jours.
Reproduction dans `applyDelays` : à `4294967195` ms, un délai ON de 200 ms
produit une deadline de 99 ms ; `millis() >= deadline` autorise immédiatement
ON. Le temps mort et les impulsions présentent le même type de comparaison.
Employer des durées écoulées avec soustraction non signée, ou des différences
signées pour des deadlines bornées, et un indicateur d'activation distinct.
Éteindre/effacer une impulsion expirée évite également son rejeu après un tour
complet du compteur.

**R10 — P1 — Les règles de relais ne sont pas réellement validées côté firmware.**

Emplacement : `src/main.cpp:3770`, `validateAndApplyRulesDoc`.
La validation vérifie la taille de `relays`, les volets et les VInputs, mais
pas le type de chaque règle, son opérateur, ses entrées ni ses durées.
Par exemple, quatre objets vides sont acceptables pour quatre relais ; leurs
expressions absentes prennent ensuite le défaut `FOLLOW` sur E1. Un document
incomplet peut donc commander des relais au lieu d'être refusé.
Les durées négatives/non bornées et les conversions d'indices en `uint8_t`
avant validation doivent aussi être traitées.
Valider un schéma complet et utiliser un comportement OFF explicite pour une
expression absente ou invalide. Les bornes de l'interface ne protègent pas l'API.

**R11 — P1 — L'enregistrement d'une configuration n'est pas transactionnel.**

Emplacements : `src/main.cpp:428`, `writeFile` ; `src/main.cpp:4190`, PUT rules.
`writeFile` ouvre directement le fichier existant en écriture, puis retourne
true sans contrôler le nombre d'octets écrits. Un fichier tronqué peut donc
être annoncé comme sauvegardé.
Le PUT rules modifie en outre les règles et le runtime avant l'enregistrement,
sans retour arrière si celui-ci échoue. Auth, WiFi et certaines branches réseau
présentent aussi des changements en RAM avant une réussite durable.
Écrire un fichier temporaire, vérifier l'écriture complète et le JSON, puis
remplacer atomiquement le fichier. Appliquer la nouvelle configuration après
réussite, ou restaurer un snapshot cohérent en cas d'échec.

**MQTT, réseau et sauvegardes**

**R12 — P2 — Changer le broker MQTT ne déconnecte pas le client existant.**

Emplacement : `src/main.cpp:3590`, `mqttOnConfigApplied`.
La configuration et `setServer` sont modifiés, mais les clients actifs ne sont
pas déconnectés. `mqttTryConnectTransport` retourne immédiatement si le client
est déjà connecté. La connexion peut rester sur l'ancien broker, avec les
anciens abonnements, tout en publiant les topics de la nouvelle configuration.
Déconnecter explicitement les transports affectés et remettre à zéro leur
session et leurs abonnements avant la reconnexion.

**R13 — P1 — L'application `flutter_espr4` conserve encore les commandes.**

Emplacements : `flutter_espr4/lib/main.dart:237`, `2998`, `3026`.
Le défaut `retain = true` est réutilisé pour publier ON/OFF et les commandes
de VInput. Cela réintroduit des commandes conservées sur le broker après le
nettoyage du firmware. Le nettoyage récent limite le rejeu lors des reconnexions
de ce firmware, mais les clients plus anciens et les autres consommateurs
restent exposés.
Séparer la conservation des états de celle des commandes ; publier toutes les
actions avec `retain: false`, sans reprendre le paramètre des états de la carte.

**R14 — P2 — Les applications publient leur Last Will sur la disponibilité de la carte.**

Emplacements : `flutter_espr4/lib/main.dart:2814` et
`flutter_mqtt_relay/lib/main.dart:528`.
Leur message de déconnexion vaut `offline` sur le topic `/status` de la carte.
Une perte de réseau du téléphone peut donc faire apparaître une carte fonctionnelle
comme indisponible dans HA. Utiliser un topic propre au client, ou aucun Last Will
pour ces applications de commande. Le topic de disponibilité matériel doit être
publié uniquement par la carte concernée.

**R15 — P2 — HA peut manquer complètement une impulsion de relais.**

Emplacements : `src/main.cpp:2370`, `2619` et `2680`.
Chaque commande MQTT suspend les publications d'état pendant 700 ms.
Une impulsion de 200 ou 500 ms commence et finit pendant cette suspension.
À la reprise, l'état est déjà OFF et égale le dernier état publié : HA peut ne
recevoir aucun ON, alors que le relais a été actionné.
Publier les transitions des sorties indépendamment du délai de télémétrie,
avec une file d'événements si nécessaire. Réserver le ralentissement aux
mesures non prioritaires, à la découverte et aux diagnostics.

**R16 — P2 — Le statut SIM est comparé à la mauvaise constante.**

Emplacements : `src/main.cpp:1617` et `1635`.
Le code traite 3 comme l'état READY. TinyGSM 0.12.0 définit `SIM_READY = 1`
et `SIM_ANTITHEFT_LOCKED = 3`. Avec une SIM prête, le journal annonce donc
incorrectement « not ready » ; si un PIN est configuré, un déverrouillage est
tenté même quand il n'est pas nécessaire et peut interrompre cette tentative
de connexion. Employer les valeurs symboliques et ne marquer le contrôle PIN
terminé qu'après un résultat exploitable.
Source : [énumération TinyGSM 0.12.0](https://raw.githubusercontent.com/vshymanskyy/TinyGSM/v0.12.0/src/TinyGsmGPRS.tpp).

**R17 — P2 — Le DHCP n'est pas entretenu.**

Emplacement : `src/main.cpp:851`, `applyNetCfg`, et absence d'appel dans `loop`.
Le projet démarre le DHCP avec `Ethernet.begin(mac)`, mais n'appelle jamais
`Ethernet.maintain()`. Le renouvellement/rebinding du bail n'est pas assuré.
Appeler l'entretien périodiquement en mode DHCP et gérer ses résultats, en
gardant les opérations lentes hors de la commande critique des relais.

**R18 — P2 — La sauvegarde exporte WiFi/BLE, mais l'import HTTP les ignore.**

Emplacements : `src/main.cpp:3417`, GET backup ; `src/main.cpp:4087`, PUT backup.
Les champs WiFi et BLE ajoutés à l'export servent à l'uploader, mais le chemin
de restauration HTTP ne rétablit toujours que règles, réseau et MQTT.
Un export puis import n'est donc pas symétrique. Valider et restaurer tous les
champs pris en charge dans une transaction, avec une version de schéma et une
politique explicite pour les identifiants.

**R19 — P2 — L'uploader récupère les réglages seulement après le premier redémarrage.**

Emplacements : `scripts/ethernet_upload.py:50`, `src/main.cpp:1140`.
L'envoi automatique installe le firmware, attend son redémarrage, puis prend
la sauvegarde. Si le nouveau firmware migre, normalise ou tronque la configuration
au boot — notamment si un module manque et réduit le nombre de relais — la copie
obtenue n'est déjà plus celle d'avant l'upload.
L'image avec les réglages est temporaire et il n'existe pas de sauvegarde durable
automatique de ces six fichiers. Une coupure pendant le flash de l'unique
partition LittleFS reste difficile à récupérer.
Prendre une sauvegarde avant le firmware, la conserver avec des permissions
restreintes, vérifier le schéma et la compatibilité, puis contrôler le résultat
après chaque redémarrage. À terme, séparer paramètres persistants et fichiers web.

**R20 — P2 — Le nettoyage MQTT récent n'est pas une vérification côté broker.**

Emplacement : `src/main.cpp:2395`, `mqttClearRetainedCommands`.
Les publications PubSubClient sont en QoS 0 : leur retour confirme l'envoi du
paquet, pas l'effacement accepté par le broker. Une ACL peut laisser s'abonner
aux commandes tout en refusant les publications de nettoyage. Le firmware ne
dispose alors d'aucune preuve d'effacement.
Le correctif actuel reste utile dans la configuration testée, mais un mécanisme
plus robuste doit pouvoir distinguer et ignorer un message reçu avec le drapeau
RETAIN ; l'API de callback de cette bibliothèque ne le fournit pas directement.
Prévoir l'accès à cette métadonnée dans le client, des droits broker cohérents
et un test avec refus de publication.
Source : [API PubSubClient](https://pubsubclient.knolleary.net/api.html).

**Interface et applications**

**R21 — P2 — « Tous AUTO » s'arrête sur les relais réservés aux volets.**

Emplacement : `data/index.html:1415`, `setAllAuto`.
La boucle envoie AUTO à tous les relais. L'API refuse un relais réservé et
l'exception arrête la boucle : les relais non réservés suivants ne sont pas
traités. Filtrer les relais réservés à partir de l'état confirmé par la carte
et signaler les éventuels échecs individuellement.

**R22 — P2 — Un échec de chargement des règles crée un formulaire par défaut enregistrable.**

Emplacement : `data/index.html:1928`, `loadRules`.
En cas d'erreur GET, l'interface remplace les règles par des valeurs par défaut.
Les actions d'enregistrement envoient le document entier : après une simple
coupure réseau, modifier/appliquer une règle peut écraser les autres règles
réelles avec les valeurs par défaut. Désactiver l'enregistrement jusqu'à une
lecture valide, ou demander explicitement la création d'une nouvelle configuration.
Ajouter une révision côté serveur pour éviter les écritures concurrentes obsolètes.

**R23 — P2 — Le nom d'un volet est inséré dans du HTML sans échappement.**

Emplacement : `data/index.html:1434`, `renderShutterDash`.
Un nom importé contenant une balise est interprété par `innerHTML` ; un attribut
d'événement peut exécuter du JavaScript. La création/importation nécessite les
droits de configuration, mais une sauvegarde importée peut introduire ce contenu.
Les identifiants stockés dans `localStorage` augmentent la conséquence d'une telle
injection. Utiliser `textContent`/des nœuds DOM, ou un échappement systématique des
champs de configuration avant construction HTML.

**R24 — P2 — La désactivation BLE peut relancer l'annonce.**

Emplacements : `src/main.cpp:577`, `onDisconnect` ; `src/main.cpp:643`,
`setBleEnabled`.
La désactivation arrête l'annonce puis déconnecte le client ; le callback de
déconnexion redémarre l'annonce sans tester `bleEnabled`.
Condition : un client BLE était connecté lors de la désactivation.
Conditionner le redémarrage à l'état voulu et vérifier la valeur annoncée
après une déconnexion normale et une désactivation depuis MQTT.

**R25 — P2 — L'application BLE appelle `setState` après `dispose`.**

Emplacements : `flutter_app/lib/main.dart:84`, `124`, `674`.
`dispose` appelle la fonction asynchrone `_disconnect`, qui appelle ensuite
`setState`. Le scan et plusieurs opérations BLE font également des mises à jour
d'interface après un `await` sans vérifier `mounted`.
Une fermeture de la page pendant ces opérations peut produire une exception.
Séparer la libération des ressources de la mise à jour de l'interface, vérifier
`mounted` après les attentes et empêcher le chevauchement des lectures périodiques.

**R26 — P2 — Le test Flutter BLE ne compile plus.**

Emplacement : `flutter_app/test/widget_test.dart:16`.
Le test de compteur généré par Flutter instancie `MyApp`, alors que l'application
expose `EspRelayApp` et ne contient pas ce compteur.
L'analyseur Dart confirme l'erreur. Remplacer le test par des tests utiles de
connexion/fermeture et de reconstitution des trames BLE. Le test de l'autre
application MQTT ne vérifie actuellement que `true == true`.

**R27 — P2 — L'application principale reste limitée à quatre entrées/relais.**

Emplacements : `flutter_espr4/lib/main.dart:2154`, `2235`, `2823`.
Les boucles de présentation et d'abonnement sont bornées à quatre, alors que
le firmware gère jusqu'à seize E/S. Les modules supplémentaires ne sont donc
pas entièrement contrôlables avec ce client. Déduire le nombre d'E/S de l'état
de la carte et centraliser ces capacités dans un modèle partagé.

**R28 — P2 — Les températures DS18B20 sont lues avant la fin de conversion.**

Emplacements : `src/main.cpp:4353`, `setWaitForConversion(false)` ;
`src/main.cpp:4471`, demande puis lecture immédiate.
Avec le mode asynchrone, la demande ne garantit pas une conversion terminée.
La lecture peut présenter la conversion précédente, soit une mesure en retard
d'un cycle de cinq secondes, et une valeur de démarrage lors du premier passage.
Séparer demande et lecture dans deux états avec l'attente requise par la
résolution, filtrer les valeurs d'erreur et exposer l'âge/validité des mesures.

**R29 — P2 — Les paramètres de règles changent sans réinitialisation sélective du runtime.**

Emplacements : `src/main.cpp:3830`, `rebuildRuntimeFromRules` ; `377`,
`toggleState`, `pulseUntilMs`, `pendingDeadlineMs`.
Le runtime des volets est réinitialisé à chaque sauvegarde globale, même si
seule une règle indépendante est modifiée. À l'inverse, les mémoires des règles
simples ne sont pas remises à zéro quand leur opérateur/entrée change.
Un toggle peut retrouver un état ancien, une impulsion reprendre une ancienne
deadline ; un volet en mouvement perd ses informations de sens et de temps mort.
Comparer ancienne et nouvelle configuration, préserver les moteurs inchangés
et arrêter/neutraliser explicitement ceux dont le câblage logique change.

**Sécurité et optimisations proposées**

Ces pistes complètent les défauts précédents ; elles ne sont pas toutes des
corrections nécessaires au fonctionnement de la configuration actuelle.

| Piste | Gain attendu | Travail / limite |
| --- | --- | --- |
| Séparer paramètres persistants et fichiers web, par exemple avec NVS pour les paramètres | Une mise à jour de l'interface ne remplace plus les règles, mots de passe et réglages réseau | Migration versionnée et procédure de récupération à prévoir |
| Refactoriser `main.cpp` en modules E/S, règles, volets, réseau, config, OTA | Contrats plus clairs, tests des fonctions réelles et évolutions plus faciles | Préserver d'abord le comportement ; éviter une réécriture globale simultanée |
| Convertir les règles JSON en structures validées à l'enregistrement | Moins de recherches JSON, allocations et comparaisons de chaînes dans la boucle | Garder JSON comme format d'échange, avec une seule validation |
| Espacer les sondages des modules absents | Moins d'essais I2C inutiles et de délais cumulés à chaque boucle | Conserver une reprise automatique, mesurable et bornée |
| Écrire les sorties uniquement quand la valeur change, avec surveillance périodique | Moins de trafic I2C | Ne pas supprimer les retries après échec ni la resynchronisation après reset d'un PCA |
| Publier les changements de relais en priorité et limiter le reste | HA reflète les impulsions ; moins de trafic GSM | Conserver une file bornée et un snapshot de resynchronisation |
| Précharger les identités/topics et réserver les buffers récurrents | Moins d'allocations temporaires et de fragmentation potentielle | Mesurer le tas libre/minimal et la taille du plus grand bloc avant optimisation |
| Mettre à jour seulement les éléments DOM qui changent | Moins de reconstruction du tableau toutes les 1,5 s | Garder un affichage explicite de l'ancienneté de l'état en cas de perte réseau |
| Compresser la page web et réduire les transferts | Lecture HTTP plus courte, occupation LittleFS réduite | Gérer `Content-Encoding` et le cache versionné correctement |
| Mettre les identifiants locaux dans le fichier ignoré par Git et un stockage sécurisé côté application | Réduit les copies de secrets en clair | Le projet a actuellement des valeurs `admin/admin` dans `platformio.ini`, des identifiants web dans `localStorage` et des secrets dans SharedPreferences |
| Ne plus journaliser le mot de passe WiFi (`src/main.cpp:510`) | Évite sa divulgation dans les logs | Correction locale de faible coût |
| Authentification réseau plus forte et images OTA authentifiées | Protège les commandes et l'origine du firmware | HTTP Basic et MQTT actuel sont non chiffrés ; SHA-256 seul vérifie l'intégrité, pas l'identité du producteur |
| Borner les lignes/en-têtes HTTP, JSON et durées de requête | Empêche une requête trop volumineuse ou trop lente d'épuiser RAM et disponibilité | `readLine` ne borne pas la longueur et `readBody` réserve la longueur annoncée |
| Versionner et figer les dépendances PlatformIO | Compilations reproductibles | La plateforme et plusieurs bibliothèques évoluent actuellement sans verrou explicite du projet |
| Ajouter journal d'événements et compteurs de défauts | Permet de distinguer règle, override, commande MQTT, erreur I2C, redémarrage et OTA | Ne pas stocker un log dans la flash à chaque tour de boucle |
| Clarifier le mode MQTT AUTO | Évite de confondre secours et fonctionnement simultané | L'interface annonce `ETH->GSM`, le code peut activer les deux transports avec un broker GSM dédié |

La flash application dispose d'environ **159 Kio** de marge. Les fichiers web
actuels totalisent environ **95 Ko**, alors que la partition LittleFS fait
`0x160000` octets. Un partitionnement plus adapté peut augmenter les deux slots
OTA, mais changer la table de partitions doit être une migration dédiée avec
sauvegarde et généralement reflash de la table, pas un upload OTA applicatif
ordinaire. L'utilisation de la RAM annoncée à la compilation ne mesure pas les
allocations dynamiques d'ArduinoJson, des chaînes, du BLE et des connexions.

**Ordre conseillé pour les corrections**

1. Corriger R01–R04, R06–R07 et R09 : arrêt des sorties, temps morts,
   durée maximale, erreurs I2C et timers. Ajouter les reproductions aux tests.
2. Corriger R10–R11 et R29 : validation complète et application durable des
   configurations, sans perturber les volets inchangés.
3. Corriger R12–R16 et R20 : reconnexion MQTT, clients Flutter, publication des
   impulsions, état SIM et traitement robuste des commandes conservées.
4. Corriger R18–R19 : sauvegarde avant upload et restauration symétrique ;
   vérifier le contenu et l'état réel après chaque étape de mise à jour.
5. Corriger interface et BLE, puis optimiser à partir de mesures de latence,
   mémoire et trafic. Réserver le changement de partitionnement à une étape séparée.

**Tests à ajouter en priorité**

- UP → STOP → DOWN avant la fin du temps mort ; inversion toggle pendant le
  mouvement ; bouton maintenu après expiration du temps maximal.
- Coupure Ethernet pendant firmware/LittleFS OTA ; corps HTTP incomplet ;
  broker/modem indisponible pendant une impulsion ou un mouvement.
- Horloge proche de `0xFFFFFFFF`, deadlines expirées et impulsion inactive.
- Échec d'écriture I2C, reset/reprise d'un PCA et validation des états exposés.
- Règles incomplètes, opérateur inconnu, indices trop grands et durées négatives.
- Sauvegarde avec panne d'écriture, export/import complet, migration avec
  module absent et absence de fichier web lors d'une récupération.
- Commandes MQTT non conservées dans tous les clients ; disponibilité de la
  carte après perte réseau d'un téléphone ; transition ON/OFF d'une impulsion courte.
- Fermeture de l'application pendant un scan BLE, une connexion et une lecture.

Les tests de boucle actuels remplacent notamment l'évaluation des règles et
les volets par des stubs. Ils vérifient l'ordre d'exécution, mais ne valident
pas les machines d'état réelles. La prochaine étape doit faire porter les tests
sur ces fonctions réelles ou sur des modules extraits, avec injection d'horloge
et d'erreurs matérielles.
