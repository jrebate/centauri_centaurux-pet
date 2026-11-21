# Firmware para la pantalla redonda

Placa: ESP32-2424S012 (ESP32-C3, pantalla redonda GC9A01 de 240×240 y táctil CST816).

- `prueba-idf/`: primera prueba. Enseña al centauro, cambia de estado al tocar o deslizar y comprueba colores y táctil.

## Cargarlo en la placa (sin instalar nada)

1. Conecta la placa por USB-C y abre https://espressif.github.io/esptool-js/ en Chrome o Edge.
2. Pulsa **Connect** y elige el puerto de la placa.
3. En **Flash Address** deja `0x0`, elige el archivo `.bin` y pulsa **Program**.
4. Cuando termine, desconecta y vuelve a conectar la placa.

Si no conecta, desenchufa la placa, mantén pulsado el botón BOOT, enchúfala y suéltalo.

## Compilar

Con ESP-IDF 5.3:

```bash
cd firmware/prueba-idf
idf.py set-target esp32c3 build
cd build && python -m esptool --chip esp32c3 merge_bin -o centauri-prueba.bin @flash_args
```

`components/esp_lcd_gc9a01` es el controlador de pantalla de [esp-bsp](https://github.com/espressif/esp-bsp) (Apache 2.0).
