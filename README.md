
# xtrsdr
Trying to connect rtlsdr dongle and esp32s2  and esp32p4 (NEW!!)

!["pic 1"](pictures/pic1.jpg?raw=true )
!["pic 2"](pictures/pic2.jpg?raw=true )

### Что получилось:
- Адаптировать librtlsdr под библиотеку esp32 usb host
- Опрашивается и конфигурируется RTLSDR v3 (Чипы RTL2832U + R820T2) 
- esp32s2: Переписать rtl_tcp для вещания  по Wifi с samplerate 240000
- esp32s2: Подключение GQRX, SDRSharp, SDR++ к rtl_tcp, успешно демодулируется iq-поток
- esp32s2: Подключиться cо смартфона из SDR++
- esp32s2: Адаптировать rtl_fm для демодуляции и воспроизведения через i2s DAC модуль MAX98357A, проверено на вещательной FM-радиостанции
- esp32s2: Подключить сетевой модуль w5500 по SPI и вещать rtl_tcp с samplerate 300000 
- eps32p4: Подключить RTLSDR к модулю esp32-p4-eth c полным samplerate 2000000   (NEW!!)

### Не получилось:
- esp32s2: добиться стабильности потока через wifi на расстоянии


### Примечания
- поддержка USB и USB host имеется не во всех esp32 модулях
- по модулю esp32s2 смотрите соответствующий readme
- нужно поправить путь к librtlsdr в platformio.ini
