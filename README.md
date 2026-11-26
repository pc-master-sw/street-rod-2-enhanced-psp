# Street Rod 2 Enhanced GE para PSP

Port nativo de Street Rod 2 Enhanced para PlayStation Portable, basado en el motor de Krzysztof Kania. Usa el Graphics Engine (GE) de PSP para dibujar la pista y el entorno en ciudad y en la drag callejera. Conserva el tablero, los autos y el retrovisor del juego original.

## Descarga e instalación

**Candidato principal para PSP** · [Descargá GE R8](https://github.com/pc-master-sw/street-rod-2-enhanced-psp/releases/tag/enhanced-ge-r8).

- **Instalación nueva.** Extraé la carpeta `STREETROD2E` del ZIP en `PSP/GAME`. Copiá los archivos de tu edición DOS a `STREETROD2E/Game`. El [manual](docs/instalacion-y-validacion.md#instalación) lista los archivos.
- **Actualización de Enhanced.** Hacé una copia del EBOOT anterior y reemplazalo por `EBOOT.PBP` de R8. Conservá las carpetas `Game` y `Save` de esa instalación.

Iniciá el juego desde el menú de la PSP. Usá la cruceta para dirigir y acelerar, L y R para cambiar de marcha y Cuadrado para abrir el teclado. Para salir y escribir el informe `PERFxx.TXT`, pulsá Start y Select juntos. El [manual de instalación y validación](docs/instalacion-y-validacion.md) describe los modos gráficos y los contadores de audio.

## Compilación

Con PSPSDK en el PATH, ejecutá desde la raíz del repositorio:

```bash
cd source
make -C build -f ../runtime/psp/psp.mk NATIVE_ROOT="$PWD" STREET_ROD_ENGINE=1 STREET_ROD_EDITION=2g
```

El ejecutable queda en `source/build/EBOOT.PBP`. Las fuentes preparadas para PSP se incluyen en `source/`. La [receta de compilación](source/BUILD.txt) registra la revisión del motor y [toolchain.txt](source/toolchain.txt) identifica la versión del compilador usada para R8.

## Estado de GE R8

R8 reabre los archivos de datos de solo lectura después de reanudar la PSP. En PPSSPP, el periódico y los avisos de autos funcionaron tras dos reanudaciones simuladas. En PSP física se confirmó que el periódico y los avisos abren después de reanudar y que se puede volver al garaje. El informe registró una reapertura correcta y cero errores. [Resultado y procedimiento](docs/instalacion-y-validacion.md#prueba-de-reanudación-de-r8).

En PSP física con R7 se conservó la pintura al guardar y cargar, y se completaron Aqueduct y Grudge Night. También se corrió Mulholland Drive. Su llegada a la meta queda pendiente en la consola. [Resultados de la prueba física](docs/instalacion-y-validacion.md#prueba-física-de-cierre).

## Prueba panorámica

GE R9 es una prueba experimental de la vista de 480 × 272. [Ver la prueba GE R9](https://github.com/pc-master-sw/street-rod-2-enhanced-psp/releases/tag/enhanced-ge-r9).

## Créditos y licencias

Motor [streetrod2-enhanced](https://github.com/kylofon/streetrod2-enhanced) de Krzysztof Kania, bajo [licencia MIT](source/build/engine/LICENSE). Port PSP y runtime DOS2PSP Native bajo [GPL versión 2](LICENSE). El repositorio incluye el código del port. Para jugar, usá los datos de tu edición DOS.
