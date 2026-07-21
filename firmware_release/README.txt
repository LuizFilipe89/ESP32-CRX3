Hydra-ESP — Firmware compilado (ESP32-WROOM-32 DevKit, 30 pinos, USB-C)
=========================================================================

Arquivos nesta pasta (todos necessários — grave os 4 juntos):

  bootloader.bin        -> endereço 0x1000
  partition-table.bin    -> endereço 0x8000
  hydra-esp.bin          -> endereço 0x10000   (firmware principal)
  storage.bin            -> endereço 0x190000  (SPIFFS: interface web + páginas do captive portal)

Compilado com ESP-IDF v5.3, target esp32.


1) Instalar o esptool (uma vez só)
-----------------------------------
    pip install esptool


2) Descobrir a porta COM da placa
-----------------------------------
Conecte o ESP32 via USB-C e veja em:
  Gerenciador de Dispositivos > Portas (COM & LPT)
Normalmente aparece como "Silicon Labs CP210x" ou "USB-SERIAL CH340" (ex: COM5).


3) (Opcional, mas recomendado se já tinha outro firmware) Apagar a flash
-----------------------------------
    python -m esptool --chip esp32 --port COM5 erase_flash


4) Gravar o firmware
-----------------------------------
Rode este comando dentro desta pasta (troque COM5 pela porta correta):

    python -m esptool --chip esp32 --port COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size detect --flash_freq 80m ^
        0x1000    bootloader.bin ^
        0x8000    partition-table.bin ^
        0x10000   hydra-esp.bin ^
        0x190000  storage.bin

(No PowerShell, troque o "^" de continuação de linha por um único comando em uma linha só,
ou use "`" no final de cada linha.)


5) Após gravar
-----------------------------------
- O ESP32 reinicia e cria o Access Point de gerenciamento (mesmo SSID/senha configurados no firmware).
- Conecte-se a ele e acesse http://192.168.4.1 no navegador.
- A nova aba "Log" mostra as credenciais capturadas pelo Evil Twin (persistidas em
  /spiffs/eviltwin_log.txt, sobrevive a reboot/queda de energia).


Notas
-----------------------------------
- Esta build já inclui: build/ atualizado a partir do código-fonte em main/ e components/,
  com as mudanças pedidas (log persistente do captive portal + aba "Log" na interface).
- Se regravar depois de já ter uma versão anterior instalada, não é obrigatório apagar a flash
  (passo 3) a menos que o particionamento (partitions.csv) tenha mudado.
