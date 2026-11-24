# Street Rod 2 Enhanced GE para PSP

Port nativo de Street Rod 2 Enhanced para PlayStation Portable, basado en el motor de Krzysztof Kania. Usa el Graphics Engine (GE) de PSP para dibujar la pista y el entorno en ciudad y en la drag callejera. Conserva el tablero, los autos y el retrovisor del juego original.

## Descarga e instalación

Descargá [GE R7](https://github.com/pc-master-sw/street-rod-2-enhanced-psp/releases/tag/enhanced-ge-r7).

- **Instalación nueva.** Extraé la carpeta `STREETROD2E` del ZIP en `PSP/GAME`. Copiá los archivos de tu edición DOS a `STREETROD2E/Game`. El [manual](docs/instalacion-y-validacion.md#instalación) lista los archivos.
- **Actualización de Enhanced.** Hacé una copia del EBOOT anterior y reemplazalo por `EBOOT.PBP` de R7. Conservá las carpetas `Game` y `Save` de esa instalación.

Iniciá el juego desde el menú de la PSP. Usá la cruceta para dirigir y acelerar, L y R para cambiar de marcha y Cuadrado para abrir el teclado. Para salir y escribir el informe `PERFxx.TXT`, pulsá Start y Select juntos. El [manual de instalación y validación](docs/instalacion-y-validacion.md) describe los modos gráficos y los contadores de audio.

## Compilación

Con PSPSDK en el PATH, ejecutá desde la raíz del repositorio:

```bash
cd source
make -C build -f ../runtime/psp/psp.mk NATIVE_ROOT="$PWD" STREET_ROD_ENGINE=1 STREET_ROD_EDITION=2g
```

El ejecutable queda en `source/build/EBOOT.PBP`. Las fuentes preparadas para PSP se incluyen en `source/`. La [receta de compilación](source/BUILD.txt) registra la revisión del motor y [toolchain.txt](source/toolchain.txt) identifica la versión del compilador usada para R7.

## Estado de GE R7

R7 separa los contadores de audio de menús, carga, traslados y carrera. Pasó once pruebas de host. En PPSSPP se comprobó el guardado y la carga, se completaron Aqueduct y Mulholland con asistencia del motor y se verificaron Grudge Night y The King con resultados preparados. En PSP física, el cambio de pintura se conservó al guardar y cargar, y se completó Aqueduct. También se probó Mulholland Drive. El informe registró cero faltantes de audio durante las carreras. Quedan por verificar la meta de Mulholland, Grudge Night y la suspensión y reanudación. [Resultados de la prueba física](docs/instalacion-y-validacion.md#prueba-física-de-cierre).

## Créditos y licencias

Motor [streetrod2-enhanced](https://github.com/kylofon/streetrod2-enhanced) de Krzysztof Kania, bajo [licencia MIT](source/build/engine/LICENSE). Port PSP y runtime DOS2PSP Native bajo [GPL versión 2](LICENSE). El repositorio incluye el código del port. Para jugar, usá los datos de tu edición DOS.
