<div align="center">

[🇺🇸 English](README.md) | **🇧🇷 Português (Brasil)**

<img src="resources/hydra_logo.png" alt="Logo CRX3" width="320"/>

# CRX3

**Um firmware de pesquisa em segurança Wi-Fi para o ESP32 — 100% open source, sem pegadinha.**

[![Stars](https://img.shields.io/github/stars/LuizFilipe89/ESP32-CRX3?style=for-the-badge&color=yellow)](https://github.com/LuizFilipe89/ESP32-CRX3/stargazers)
[![Forks](https://img.shields.io/github/forks/LuizFilipe89/ESP32-CRX3?style=for-the-badge&color=orange)](https://github.com/LuizFilipe89/ESP32-CRX3/network/members)
[![Issues](https://img.shields.io/github/issues/LuizFilipe89/ESP32-CRX3?style=for-the-badge&color=red)](https://github.com/LuizFilipe89/ESP32-CRX3/issues)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue?style=for-the-badge)](LICENSE)
[![Last Commit](https://img.shields.io/github/last-commit/LuizFilipe89/ESP32-CRX3?style=for-the-badge&color=brightgreen)](https://github.com/LuizFilipe89/ESP32-CRX3/commits)
[![Open Source](https://img.shields.io/badge/100%25-open%20source-success?style=for-the-badge)](LICENSE)

</div>

---

O **CRX3** transforma uma DevKit V1 de ESP32 (uns R$30) num kit completo de teste de invasão Wi-Fi, controlado inteiramente pelo navegador do celular ou notebook em `http://192.168.4.1` — sem precisar instalar nenhum app. O painel web tem **dois idiomas: Inglês (padrão) e Português (Brasil)**, trocáveis a qualquer momento em Configurações. É um fork do [Hydra-ESP / ProjectHydraOS de Sameer Al Sahab](https://github.com/SameerAlSahab/ESP32-Deauther), que por sua vez é construído sobre a ferramenta original de teste de invasão Wi-Fi do [risinek](https://github.com/risinek/esp32-wifi-penetration-tool).

O foco deste fork: um **Evil Twin totalmente personalizável** (seu próprio SSID, sua própria página de portal cativo), tetos de intensidade de deauth e beacon spam mais altos, e uma rodada de correções reais na lógica de parada/timeout dos ataques — tudo isso mantendo o código o mais simples possível de ler, compilar e expandir.

> **Feito com IA, às claras.** Toda mudança neste fork foi escrita por [LuizFilipe89](https://github.com/LuizFilipe89) trabalhando com o Claude Sonnet (Anthropic) — **sem experiência prévia em programação**. Se você também está aprendendo, o histórico de commits conta a história toda. Veja [Legal & Créditos](#legal--créditos) mais abaixo.

---

<div align="center">

| Escanear | Configurar Ataque |
|:---:|:---:|
| <img src="resources/screenshots/scan.jpg" width="260"/> | <img src="resources/screenshots/attack-config.jpg" width="260"/> |
| **Evil Twin** | **Configurações — interface bilíngue** |
| <img src="resources/screenshots/evil-twin.jpg" width="260"/> | <img src="resources/screenshots/settings.jpg" width="260"/> |
| **Impressora (em desenvolvimento)** | |
| <img src="resources/screenshots/printer.jpg" width="260"/> | |

</div>

---

## Sumário

- [Visão Geral Rápida dos Recursos](#visão-geral-rápida-dos-recursos)
- [Hardware](#hardware)
- [Instalação](#instalação)
- [Como Funciona](#como-funciona)
- [Ataques](#ataques)
  - [Desautenticação](#-desautenticação)
  - [Captura de Handshake WPA](#-captura-de-handshake-wpa)
  - [Beacon Spam](#-beacon-spam)
  - [Modo Fantasma](#-modo-fantasma-spam-de-probe-request)
  - [Evil Twin — totalmente personalizável](#-evil-twin--totalmente-personalizável)
  - [Detector de Ataque de Deauth](#-detector-de-ataque-de-deauth)
  - [Ataque à Impressora de Rede (em desenvolvimento)](#-ataque-à-impressora-de-rede--em-desenvolvimento)
- [Tour pela Interface Web](#tour-pela-interface-web)
- [Credenciais Padrão](#credenciais-padrão)
- [Dependências](#dependências)
- [Legal & Créditos](#legal--créditos)

---

## Visão Geral Rápida dos Recursos

| Recurso | O que faz | Limites |
|---|---|---|
| 🎯 Desautenticação | 4 métodos, incluindo um que vence o 802.11w (PMF) | Até 16 alvos, intensidade até **50** quadros/rajada |
| 🤝 Captura de Handshake | Captura o handshake WPA2 de 4 vias → `.pcap` / `.hccapx` | Pra quebrar offline com Hashcat / aircrack-ng |
| 📡 Beacon Spam | Inunda o ar com redes falsas | Até **250** SSIDs falsos, 4 modos de nomeação |
| 👻 Modo Fantasma | Espelha os probes de rede salva de dispositivos próximos | Não precisa de alvo |
| 🎭 Evil Twin | Clone uma rede real **ou** rode seu próprio SSID customizado, com sua própria página de portal | Totalmente personalizável, log de credenciais persiste entre reboots |
| 🚨 Detector de Deauth | Monitor passivo que sinaliza ataques de deauth por perto | Log de alertas em tempo real |
| 🖨️ Ataque à Impressora de Rede | Encontra e imprime em impressoras de rede abertas | 🚧 em desenvolvimento |

---

## Hardware

**Obrigatório**

- ESP32 DevKit V1, ou qualquer placa usando o mesmo chip ESP32 (Xtensa LX6, dual-core). Desenvolvido e testado na DevKit V1 padrão de 38 pinos; **placas DevKit V1 de 30 pinos funcionam igualzinho** — é o mesmo módulo ESP32-WROOM-32 numa PCB mais estreita com menos GPIOs expostos, e esse firmware não precisa de nenhum dos pinos que faltam. Módulos ESP32-WROOM-32 / WROVER em outros formatos de placa devem funcionar também. **ESP32-S2, S3, C3, C6, H2 e outras variantes não são suportadas** — arquitetura de rádio diferente, sem injeção de pacotes brutos.
- Se você tem uma placa ESP32-S3, o projeto original tem uma branch dedicada: [`s3-N16R8`](https://github.com/SameerAlSahab/ESP32-Deauther/tree/s3-N16R8).

> **🛒 O que comprar de verdade:** procure por **"ESP32 DevKit V1"** ou **"ESP32 DevKitC"** — uma placa construída em cima do módulo **ESP32-WROOM-32**, com **pelo menos 4 MB de flash** (padrão em praticamente toda placa vendida hoje). **Tanto a versão de 30 pinos quanto a de 38 pinos funcionam** — a quantidade de pinos só muda quantos GPIOs ficam expostos no header, não o chip em si, e esse firmware não usa os pinos extras. Ignore qualquer anúncio que tenha **S2, S3, C2, C3, C6 ou H2** em qualquer parte do nome — são chips diferentes que esse firmware não suporta. O chip USB-serial da placa (CP2102 ou CH340, não importa qual) só afeta a gravação pelo PC; não tem nenhum efeito em como o firmware roda, já que o controle é 100% por Wi-Fi. As placas custam em torno de R$25–40 no AliExpress/Mercado Livre.

**Opcional**

- Tela OLED SSD1306 (128×64, I2C). Mostra cronômetros de ataque ao vivo, status e senhas capturadas pelo Evil Twin direto no dispositivo. Detectada automaticamente na inicialização — se não tiver uma conectada, o firmware simplesmente pula essa parte em silêncio e a interface web continua com 100% da funcionalidade.

---

## Instalação

1. Pegue os binários mais recentes na **[página de Releases](https://github.com/LuizFilipe89/ESP32-CRX3/releases)**, ou compile a partir do código-fonte com o ESP-IDF v5.3.2 (`idf.py build`).
2. Grave o firmware:
   ```bash
   idf.py -p <PORTA> flash
   ```
   …ou direto com o `esptool.py`:
   ```bash
   esptool.py --chip esp32 -p <PORTA> -b 460800 write_flash \
     --flash_mode dio --flash_freq 80m --flash_size detect \
     0x1000   bootloader.bin \
     0x8000   partition-table.bin \
     0x10000  projecthydra-32.bin \
     0x190000 storage.bin
   ```
3. Ligue a placa, conecte na rede Wi-Fi `crx3` (veja [Credenciais Padrão](#credenciais-padrão)), e abra **`http://192.168.4.1`** em qualquer navegador.

---

## Como Funciona

Ao ligar, o ESP32 sobe seu próprio ponto de acesso Wi-Fi (o "AP de gerência"). Conecte um celular ou notebook nele e abra `http://192.168.4.1` — o painel de controle inteiro é servido direto da memória flash do dispositivo, sem precisar de internet.

A partir daí você escaneia redes próximas, toca numa pra selecioná-la como alvo, escolhe um ataque, configura e lança — tudo com status ao vivo e explicações "o que isso faz?" embutidas ao lado de cada opção.

**Uma coisa importante de saber de antemão:** ataques que precisam de uso exclusivo do rádio (Deauth, Evil Twin, Multi-Clone, Ghost Mode) desligam temporariamente o AP de gerência, então **você vai perder a conexão com a interface web enquanto o ataque roda.** Isso é normal, não é uma falha — não existe um botão de "parar" ao vivo enquanto um ataque desses está rodando, já que a própria interface web é o que cai. Um timeout configurável traz o AP de volta automaticamente; sem um configurado, um ciclo de energia (desligar e ligar) é o que restaura o acesso (o modo de Evil Twin com SSID customizado é a única exceção, com sua própria rota de parada dedicada — veja abaixo). O Beacon Spam é o único ataque de uso exclusivo do rádio que **não** derruba o AP de gerência — veja abaixo o motivo disso ser proposital.

---

## Ataques

### 🎯 Desautenticação

Envia quadros brutos de desautenticação 802.11 pra desconectar clientes de um ponto de acesso alvo. Até **16 alvos simultâneos**, com intensidade ajustável de 1 a **50 quadros por rajada**.

| Método | Como funciona | Melhor contra |
|---|---|---|
| **Deauth Normal** | Deauth clássico em broadcast mirando o AP inteiro | A maioria das redes abertas/WPA2 |
| **Clone de BSSID (Agressivo)** | Sobe um AP falso clonando o BSSID do alvo pra confundir clientes | Dispositivos com 802.11w (Proteção de Quadros de Gerência), que ignoram deauth broadcast puro |
| **Deauth Multi-Clone** | AP falso + uma enxurrada de clones de SSID preenchidos com espaços do alvo | Sobrecarregar tanto a visibilidade quanto a conectividade de uma rede |
| **Clientes Mirados** | Fareja quais estações estão realmente conectadas e desautentica cada uma diretamente (com fallback em broadcast) | Dispositivos teimosos que ignoram o deauth em broadcast |

---

### 🤝 Captura de Handshake WPA

Empurra os clientes conectados a se reautenticar (via deauth), e captura o handshake WPA2 de 4 vias resultante — salvo como **`.pcap`** e **`.hccapx`**, prontos pra jogar direto no Hashcat ou aircrack-ng pra auditoria offline de senha contra uma wordlist.

Métodos: **Clone de BSSID**, **Deauth Normal**, ou **Captura Silenciosa** (nenhum deauth é enviado — só espera pacientemente por um handshake natural, mais discreto porém mais lento). Precisa de pelo menos um cliente conectado na rede alvo; roda até um handshake ser capturado, o timeout configurado se esgotar, ou você desligar e ligar o dispositivo.

---

### 📡 Beacon Spam

Inunda o ar local com quadros de beacon 802.11 falsos — polui a lista de redes Wi-Fi de todo dispositivo próximo. Escolha quantas redes falsas transmitir, **até 250**, e um estilo de nomeação:

- **Nomes Comuns** — SSIDs cotidianos e críveis (`TP-Link_5G`, `Casa-WiFi`…)
- **Textos Aleatórios** — puro ruído
- **Rick Roll** — os SSIDs soletram a letra da música, uma palavra por rede
- **Temática de Segurança** — nomes alarmantes pra um caos básico ("Van de Vigilância FBI", etc.)

Diferente de todo outro ataque de uso exclusivo do rádio, esse **mantém o AP de gerência no ar** em vez de derrubá-lo — de propósito. Um celular conectado ao AP já está ancorado no canal dele e capta a inundação passivamente, sem precisar escanear ativamente; uma versão anterior que alternava entre os 13 canais (e precisava derrubar o AP pra liberar o rádio pra isso) na verdade piorou a visibilidade na prática, já que o celular perdia a conexão e caía num escaneamento em segundo plano mais lento em vez de simplesmente ficar ali captando os beacons. Os envios são feitos em lotes rotativos por rede configurada — calibrados do mesmo jeito que o teto de intensidade do Deauth — então o pool inteiro continua ciclando de forma confiável mesmo no limite de 250 redes, com uma checagem de segurança embutida que monitora (e avisa, pelo log serial) qualquer lote que comece a consumir tempo demais do seu orçamento.

---

### 👻 Modo Fantasma (Spam de Probe Request)

A maioria dos celulares e notebooks transmite constantemente probe requests por toda rede Wi-Fi que já salvaram. O Modo Fantasma escuta esses probes, extrai os SSIDs, e começa a anunciar esses mesmos nomes de volta — fazendo os dispositivos por perto tentarem se conectar automaticamente ao seu ESP32 em vez da rede salva de verdade. Não precisa selecionar alvo nenhum.

---

### 🎭 Evil Twin — totalmente personalizável

Esse é o ataque principal, e o que mais recebeu atenção neste fork. Existem **duas formas de rodá-lo**, e as duas deixam você personalizar o que a vítima realmente vê.

#### Modo 1 — Clonar uma rede real

Escolha um AP escaneado como alvo. O CRX3 sobe um **clone aberto (sem senha)** usando o mesmo SSID, enquanto desautentica simultaneamente o AP real pra que os clientes próximos se desconectem. Quando eles saem procurando uma rede, veem o clone aberto, se conectam a ele, e caem num portal cativo pedindo a senha do Wi-Fi.

- Toda submissão é conferida contra a rede **real** antes de ser aceita — senhas erradas são registradas e o portal mostra um erro, as corretas são reportadas como capturadas.
- Um timeout de segurança (5 minutos sem nenhuma vítima se conectar) encerra o ataque automaticamente e restaura o AP de gerência sozinho — você nunca fica permanentemente travado esperando.
- Não existe parada manual pra esse modo: assim como todo ataque que derruba o AP de gerência, ele termina sozinho (senha capturada ou o timeout de 5 minutos) ou via ciclo de energia.

#### Modo 2 — Lance seu próprio AP falso com nome customizado

Não quer clonar ninguém? Digite **qualquer SSID que quiser** na aba Devil Twin e lance uma rede aberta independente. Não tem AP real pra desautenticar e nenhuma senha pra verificar — ele simplesmente fica ali, transmitindo o nome que você escolheu, registrando toda credencial que alguém submeter, pelo tempo que você quiser.

- Por ser um modo feito pra rodar indefinidamente (coletando credenciais ao longo de horas, digamos, num espaço público que você tem autorização pra testar), ele **não tem timeout automático**.
- **Como parar:** já que o AP de gerência cai enquanto isso roda, conecte um dispositivo no seu **próprio AP falso** e abra `http://192.168.4.1/crx3-admin-stop` — uma rota de administração dedicada que fica registrada mesmo com a rede falsa no ar. Esse é o *único* ataque no CRX3 com uma forma de parar manualmente no meio da execução; todos os outros terminam pelo timeout ou por um ciclo de energia (veja [Como Funciona](#como-funciona)).

#### Deixando realista: páginas de portal cativo personalizadas

Os dois modos servem uma página HTML de portal cativo. Em **Configurações → Página do Portal** você pode:

- **Enviar (upload)** seu próprio arquivo HTML (até 100 KB) pra substituir o portal padrão — veja **[docs/custom-captive-portal-guide.md](docs/custom-captive-portal-guide.md)** pra saber exatamente o que a página precisa ter pra funcionar (nomes de campo do formulário, endpoints obrigatórios).
- **Pré-visualizar** o portal ativo no momento antes de rodar um ataque.
- **Restaurar** o portal padrão de fábrica a qualquer momento.

#### O log de credenciais

Toda submissão — certa ou errada, de qualquer um dos dois modos — é adicionada a um log persistente no dispositivo (sobrevive a reboots), visível e limpável na aba **Devil Twin**: tempo ligado, SSID, BSSID, usuário, senha e status, tudo de relance.

---

### 🚨 Detector de Ataque de Deauth

Vira a ideia do avesso: coloca o ESP32 em modo de monitor promíscuo passivo e fica de olho em ataques de deauth *de outra pessoa*. Sinaliza um BSSID assim que ele manda mais de 10 quadros de deauth em menos de um segundo — a assinatura clássica de um ataque ativo — incluindo deauths em broadcast de uma origem falsificada `00:00:00:00:00:00`. Os alertas aparecem numa tabela ao vivo na interface web. Não transmite nada; é puramente um monitor.

---

### 🖨️ Ataque à Impressora de Rede (em desenvolvimento)

Entra numa rede Wi-Fi escolhida como estação e escaneia a subrede local por impressoras nas portas 9100 (raw/JetDirect), 631 (IPP) ou 515 (LPR) — não só a 9100, já que a maioria das multifuncionais domésticas (Epson incluída) nunca abre uma porta JetDirect e só entende IPP. A impressão tenta primeiro PJL bruto e cai pra uma requisição IPP `Print-Job` mínima quando a 9100 não está aberta, que é o que de fato faz uma impressora da classe IPP/AirPrint aceitar o trabalho. A aba já está presente e funcional na interface, mas esse recurso ainda está sendo refinado — espere arestas soltas se for testar.

---

## Tour pela Interface Web

Tudo mora em `http://192.168.4.1`. O painel tem **dois idiomas: Inglês (padrão) e Português (Brasil)** — troque a qualquer momento na aba Configurações, sem precisar reiniciar.

| Aba | O que tem lá |
|---|---|
| **Escanear** | Redes próximas com SSID / BSSID / força do sinal — toque numa linha pra mirar nela |
| **Ataque** | Configure e lance qualquer ataque acima, com status ao vivo, tempo decorrido e resultados |
| **Devil Twin** | Controles do Evil Twin (os dois modos), upload/preview/restauração de portal customizado, log de credenciais |
| **Impressora** | O ataque de impressora ainda em desenvolvimento |
| **Detector** | Iniciar/parar o monitor de deauth, ver o log de alertas ao vivo |
| **Configurações** | Troca de idioma, mudar o SSID/senha do AP de gerência (reinicia pra aplicar) |
| **Sobre** | Versão do firmware, créditos, aviso legal |

---

## Credenciais Padrão

| Campo    | Padrão        |
|----------|---------------|
| SSID     | `crx3`        |
| Senha    | `notforfun`   |
| Web UI   | `192.168.4.1` |

Altere a qualquer momento em **Configurações** — salvo na flash NVS, sobrevive a reboots.

---

## Dependências

| Biblioteca | Licença | Observações |
|---|---|---|
| [u8g2-hal-esp-idf](https://github.com/mkfrey/u8g2-hal-esp-idf) | Ver repositório | Controla a tela OLED opcional |
| [u8g2](https://github.com/olikraus/u8g2) | BSD 2-Clause | Controla a tela OLED opcional |
| [esp-nimble-cpp](https://github.com/h2zero/esp-nimble-cpp) | Apache 2.0 | Incluída no repositório, mas não usada atualmente — reservada pra um possível módulo BLE futuro |
| [ESP32-BLE-Keyboard](https://github.com/T-vK/ESP32-BLE-Keyboard) | Ver repositório | Incluída no repositório, mas não usada atualmente — reservada pra um possível módulo BLE futuro |

---

## Legal & Créditos

**O CRX3 é somente para fins educacionais.** Use apenas em redes e dispositivos que você possui, ou tenha autorização explícita por escrito pra testar. Uso não autorizado contra redes que não são suas é ilegal na maioria das jurisdições — incluindo sob o Computer Fraud and Abuse Act (EUA), Computer Misuse Act (Reino Unido), e a Lei de Crimes Cibernéticos / Marco Civil da Internet (Brasil), entre outras. **Os autores — original e deste fork — não se responsabilizam por uso indevido ou qualquer dano resultante. Você é o único responsável por como usa este software.**

O código-fonte completo, módulos de ataque incluídos, é **100% open source sob a licença GPL-3.0** — leia, aprenda com ele, audite.

| Papel | Nome |
|---|---|
| Este fork (CRX3) | [LuizFilipe89](https://github.com/LuizFilipe89) — construído com Claude Sonnet (Anthropic), sem experiência prévia em programação |
| Desenvolvedor Principal, Hydra-ESP / ProjectHydraOS | [Sameer Al Sahab](https://github.com/SameerAlSahab) |
| Código-base Original | [risinek](https://github.com/risinek/esp32-wifi-penetration-tool) |
| Inspiração | [spacehuhn](https://github.com/SpacehuhnTech/esp8266_deauther) |
