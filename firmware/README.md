# Firmware para la pantalla redonda

Placa: ESP32-2424S012 (ESP32-C3, pantalla redonda GC9A01 de 240×240 y táctil CST816).

- `centauri/`: la mascota con Wi‑Fi. Recibe las misiones desde la app de escritorio y enseña una cada vez; desliza o toca para cambiar de misión.
- `prueba-idf/`: primera prueba. Enseña al centauro, cambia de estado al tocar o deslizar y comprueba colores y táctil.

## Conectar la pantalla a tu Wi‑Fi

1. La primera vez, la pantalla crea su propia red Wi‑Fi llamada **Centauri**. Conéctate a ella con el móvil.
2. Abre http://192.168.4.1, elige tu red (solo 2,4 GHz), escribe la contraseña y pulsa **Guardar y conectar**.
3. La pantalla se reinicia, se conecta y enseña su dirección. La app de escritorio la busca sola como `centauri.local`; si no la encuentra, clic derecho en la mascota → **Pantalla redonda** y escribe la dirección que sale en la pantalla.

Para girar la imagen 90°, mantén el dedo quieto sobre la pantalla unos 3 segundos y suelta. Para olvidar la red guardada, mantenlo 6 segundos.

## Cargarlo en la placa (sin instalar nada)

La placa lleva un chip USB CH340: en Windows instala su controlador (https://www.wch-ic.com/downloads/CH341SER_EXE.html) y usa un cable USB‑A a USB‑C que pase datos (muchos solo cargan).

1. Conecta la placa y abre https://web.esphome.io en Chrome o Edge.
2. Pulsa **Connect** y elige el puerto (en Windows, `USB-SERIAL CH340 (COMx)`).
3. Pulsa **Install** y elige el archivo `.bin` completo (se graba en la dirección `0x0`).
4. Cuando termine, pulsa RESET (el botón de la derecha).

Si no conecta, mantén pulsado BOOT (izquierda), pulsa y suelta RESET y luego suelta BOOT.

## Compilar

Con ESP-IDF 5.3:

```bash
cd firmware/centauri
idf.py set-target esp32c3 build
cd build && python -m esptool --chip esp32c3 merge_bin -o centauri-completo.bin @flash_args
```

`components/esp_lcd_gc9a01` es el controlador de pantalla de [esp-bsp](https://github.com/espressif/esp-bsp) (Apache 2.0).

`components/mdns` es el componente mDNS de [esp-protocols](https://github.com/espressif/esp-protocols) v1.8.2 (Apache 2.0).
