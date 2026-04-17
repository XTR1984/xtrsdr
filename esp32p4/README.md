
Trying to connect rtlsdr dongle and esp32-p4

Попытка подключить свисток rtlsdr к esp32-p4

### Что получилось:
- Подключить RTLSDR к модулю esp32-p4-eth c полным samplerate 2000000   (NEW!!)
- Успешное подключение из SDR++ к esp32-p4-eth по сети 

### Прошивка
- сборка и прошивка из среды Platformio (например в VS Code + Platformio extension)
- web-flasher (экспериментально, резет руками) - https://xtr1984.github.io/xtrsdr/



### Известные проблемы
- SDR++ не работает стабильно при подключении со смартфона через wifi роутера

### Примечания
- Для подключения usb-устройств нужно спаять переходник USB-гнездо в разъем JST 1.25мм 4pin male


