## Configuration

### Configuration Erase
For erase all configuration create wifi with SSID: "esp-erase-cfg", if connected to network
Simply use HTTP API for write new SSID, "http://hostname/api/v1/config/wifi

With CURL:
```url -X POST http://yourname.local/api/v1/config/wifi -H 'Content-Type: application/json' -d '{"ssid":"yourssid","password":"pass", "token":"telegramtoken", "chatid":-xxxxxx}'```

### Mdns
mdns can be set with related key default name: "door-lock.local"


# Telegram Bot

## Create BOT and set credential inside ESP32

## Send BotCommands
```curl -X POST https://api.telegram.org/bot-xxxxxxx/setMyCommands ```

# OTA Via HTTPD

```curl -X POST name.local/ota --data-binary "@build/Apri-cancello.bin"```
