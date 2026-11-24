# Street Rod 2 Enhanced GE para PSP

## Actualización GE R8

En R7 se observó un error al abrir el periódico después de suspender y
reanudar desde el garaje. El periódico carga imágenes de `LIB2`. R8
reabre los archivos de datos de solo lectura al primer acceso después de
reanudar y restaura su posición. Las partidas mantienen su ruta de
lectura y escritura.

`PERFxx.TXT` agrega `RESUME_COUNT`, `RESOURCE_REOPENS` y
`RESOURCE_REOPEN_ERRORS`. Son el número de notificaciones de
reanudación completa, reaperturas correctas y fallos de reapertura
durante la sesión. Si hubo una reapertura correcta, el contador de
errores debería seguir en cero.

La prueba de host verificó la reapertura, la posición de lectura y el
guardado. En PPSSPP se simuló dos veces el aviso de reanudación y se
comprobó que el periódico y los avisos de autos abrieran y permitieran
volver al garaje. Se registraron dos reaperturas y cero errores.
En PSP física se registró una reanudación y una reapertura correcta.

## Actualización GE R7

El informe agrega contadores de audio por fase. `MENU_PCM_*` cuenta
menús y escenas fuera de la conducción. `LOAD_PCM_*` cubre lectura y
escritura de archivos y preparación o cierre de la vista de conducción.
`DRIVE_PCM_*` identifica los traslados y `RACE_VIEW_PCM_*` el bucle de
la carrera. `RACE_PCM_*` conserva la ventana de carrera de R6. El
informe también se genera al salir desde el menú sin haber corrido.

## Audio en GE R6

El modo Enhanced GE reúne tres bloques PCM antes de reproducir y
vuelve a llenar la cola tras un bloque incompleto. Son 3.072 cuadros
estéreo a 44,1 kHz, unos 70 ms de audio en la cola existente. Se
conservan el ritmo original de la carrera y el límite visual de 24
cuadros por segundo.

Un archivo vacío `AUDIOLOW.TXT` junto al EBOOT recupera la lectura de
audio anterior y mantiene el informe de carrera. Reiniciá el juego
para aplicar el cambio.

## Cambios de versiones anteriores

GE R5 acelera el dibujo con máscara del capó, el pilar del parabrisas
y otros elementos que usan la misma operación. Escribe directamente
el plano EGA activo cuando los registros VGA permiten reproducir
la operación byte por byte. Conserva la transparencia y el estado
final de la VGA. Los demás modos usan el recorrido original.

GE R4 convierte los cuatro planos EGA en grupos de ocho píxeles.
También escribe por bloques los tramos de color sólido de la pista
cuando la configuración VGA permite el mismo resultado. Los extremos
con máscara y las escrituras que necesitan comparar colores mantienen
el recorrido byte por byte. Estos cambios conservan la paleta, los
píxeles de cobertura y el valor final de los latches VGA.

Desde GE R3 se usa un hilo de presentación que espera el fin del dibujo y hace
el cambio de búfer al inicio del refresco. El juego puede seguir con
los controles y la lógica mientras tanto. El búfer, las texturas y la
lista de dibujo se reutilizan una vez que el hilo confirma el cambio.
El mismo mecanismo presenta los cuadros indexados de Grudge Night.
La escritura en los planos EGA acumula los píxeles cambiados en el
mismo recorrido que los escribe. Se conserva la máscara que separa
la imagen original de los polígonos del GE.
[API de pantalla de PSPSDK](https://github.com/pspdev/pspsdk/blob/master/src/display/pspdisplay.h)

Un archivo vacío `POLL.TXT` junto al EBOOT recupera el mecanismo de
presentación de GE R2. Si la PSP no puede iniciar el nuevo hilo, el
juego también vuelve a ese mecanismo.

Mientras está activo Enhanced GE, el juego solicita 333 MHz de CPU y
166 MHz de bus si la CPU arrancó a una frecuencia menor. Al salir
restaura la frecuencia anterior. Un archivo vacío `STOCK.TXT` junto al
EBOOT conserva la frecuencia de inicio para comparar. Los tiempos de
la simulación y los controles de la carrera siguen el juego original.
[API de reloj de PSPSDK](https://pspdev.github.io/pspsdk/psppower_8h.html)

## Instalación

Si ya tenés Enhanced, copiá este `EBOOT.PBP` a `PSP/GAME/STREETROD2E`.
La build usa los archivos que ya están en `Game` y las partidas de `Save`.
Podés conservar una copia del EBOOT anterior para volver a esa build.

Para una instalación nueva, extraé `STREETROD2E` en `PSP/GAME` y copiá
desde tu edición DOS `SR.EXE`, `LIB1`, `LIB2`, `LIB3`, `SR2_DATA`,
`SR2_SHP`, `SR2_END`, `MSR.SND`, `MSR2.MUS`, `MSR20.MUS`, `MSR21.MUS`,
`MSR24.MUS`, `MSRBEG.MUS` y `PAUSE.MUS` a `STREETROD2E/Game`.

| Archivo junto al EBOOT | Modo al arrancar |
|---|---|
| Carpeta sin archivos de selección | Enhanced GE |
| `SOFTWARE.TXT` | Enhanced R2 por software |
| `CLASSIC.TXT` | Gráficos clásicos. Tiene prioridad si ambos archivos existen. |

Los archivos de selección pueden estar vacíos. Reiniciá el juego para
aplicar el cambio. Los controles son los de R2. La cruceta dirige y
acelera, L y R cambian marcha, Cuadrado abre el teclado y Start más
Select sale del juego.

## Medición en PSP

Después de correr en modo Enhanced GE, salí con Start más Select para
guardar un informe `PERF00.TXT` junto al EBOOT. Cada salida usa el
siguiente nombre disponible. `ROAD_MODES` identifica el tipo de carrera
(`0x1` para calle, `0x2` y `0x4` para Grudge Night, `0x8` para
Aqueduct y `0x10` para Mulholland). `ROAD_STEP_US` suma
el tiempo de los pasos de la carrera, incluida su espera original.
`ORIGINAL_FRONT_US`, `ORIGINAL_MIRROR_US` y `ORIGINAL_COCKPIT_US`
desglosan el dibujo original. `PREPARE_US` mide la composición en CPU.
En `PRESENT_PATH=worker`, `GE_UNTIL_POLL_US` termina cuando el hilo
recibe la finalización de la lista y `DISPLAY_LATCH_US` mide desde ese
momento hasta el cambio de búfer en el refresco. En `PRESENT_PATH=poll`,
los dos campos conservan los puntos de medición de GE R2. Los tiempos
están en microsegundos y vienen con sus contadores.
`SESSION_FAST_MASKED_PLANE_BLITS` cuenta las operaciones por plano
que usaron la ruta de R5 en toda la sesión.

`AUDIO_PREBUFFER_FRAMES` indica la reserva al arrancar y recuperar la
reproducción. `RACE_PCM_BLOCKS` cuenta los bloques de 1.024 cuadros de
audio preparados durante las carreras. `RACE_PCM_UNDERRUNS` cuenta los
bloques incompletos, incluidos los silencios durante el llenado de la
cola. `RACE_PCM_MISSING_FRAMES` suma los cuadros de silencio debidos a
esos faltantes y `RACE_PCM_DROPPED_FRAMES` cuenta los descartes por cola
llena. El contador de sesión de R6 excluye el llenado inicial previsto
cuando se usa la reserva.

## Qué dibuja el GE

En ciudad y en aceleración callejera, la CPU proyecta y recorta los
polígonos de la pista. El GE rellena los triángulos de la carretera, el
terreno y las construcciones. El cielo es una textura indexada.
El tablero, el retrovisor, los autos y los objetos del juego original
se superponen con una máscara. Las demás pruebas usan la vista clásica.

La build usa `sceGuDrawArray` con `GU_TRIANGLES` y coordenadas de pantalla
para los polígonos. El GE también aplica el recorte transparente de la
superposición. La profundidad y el orden visual siguen el recorrido de
dibujo existente de Enhanced.
[API GU de PSPSDK](https://pspdev.github.io/pspsdk/group__GU.html)

## Compilación

En el árbol DOS2PSP con PSPSDK, ejecutá
`bash native/scripts/build_psp.sh street-rod-2-ge`. El ZIP trae el
código preparado y la receta de compilación en `source/BUILD.txt`.

## Validación

- **T0**. Once pruebas host pasaron con ASan y UBSan, incluida la
  cobertura del recorte, la triangulación, el índice transparente y el
  desborde del lote de vértices. Una comparación adicional ejecutó
  250.000 escrituras VGA con ambos algoritmos y verificó que produjeran
  los mismos planos EGA y las mismas máscaras de cobertura en R3.
  En R4 se compararon 12.000 tramos de relleno y 160 pantallas con R3.
  Coincidieron los planos, latches, máscaras y píxeles de salida.
  R5 pasó una comparación con R4 de 20.000 dibujos con máscara,
  incluidos destinos solapados, cruces del límite de 64 KiB y
  configuraciones VGA que usan la ruta original.
  R6 agregó cobertura para el llenado inicial, la recuperación tras
  un bloque parcial y la lectura inmediata con `AUDIOLOW.TXT`.
- **T1**. PPSSPP 1.20.4 con perfil PSP-1000 cargó una partida del port
  clásico. Se comprobó la vista GE en ciudad y en la drag callejera,
  el avance tras la luz verde en la drag callejera y en la clasificatoria
  de Grudge Night, y la vista Enhanced por software con `SOFTWARE.TXT`.
  Con R7 se creó una partida, se compró un auto desde el periódico y se
  volvió al garaje. Otra prueba repintó un Ford, escribió una ranura
  libre conservando la original y recuperó el color y el saldo tras
  reiniciar. Aqueduct y Mulholland llegaron a la meta con la asistencia
  del motor. Para completar Mulholland se usaron las reglas del modo
  demostración después de la salida, tras una rotura de transmisión
  en la primera pasada. Las cuatro rondas de Grudge Night, las tres etapas de The
  King y el salón de la fama se verificaron con resultados preparados en
  perfiles privados del emulador. Los perfiles de prueba quedan fuera
  del ZIP. En la pasada de Mulholland, el informe registró tres bloques
  incompletos en menús, dos en carga y cero en conducción o carrera.
- **T2**. GE R2 a R6 se probaron en PSP física en la drag callejera
  (`ROAD_MODES=0x1`) a 333/166 MHz. R3 a R6 usaron el hilo de presentación
  (`PRESENT_PATH=worker`). R5 presentó los 960 cuadros que envió y
  registró 4.448 operaciones por plano en la ruta de dibujo con máscara.
  Con la reserva de 3.072 cuadros PCM, R6 procesó 3.760 bloques de
  audio en dos carreras, con cero faltantes y cero descartes durante
  las carreras. Presentó los 1.887 cuadros de video enviados.

| Medición en PSP física | GE R2 | GE R3 | GE R4 | GE R5 | GE R6 |
|---|---:|---:|---:|---:|---:|
| Tiempo por paso de carrera | 126,1 ms | 111,4 ms | 109,6 ms | 109,6 ms | 109,5 ms |
| Dibujo original por paso | 60,6 ms | 34,1 ms | 22,1 ms | 13,8 ms | 14,0 ms |
| Vista frontal original por paso | | 20,85 ms | 10,04 ms | 10,20 ms | 10,43 ms |
| Retrovisor original por paso | | 2,23 ms | 1,95 ms | 2,00 ms | 2,04 ms |
| Tablero y superposiciones por paso | | 11,04 ms | 10,10 ms | 1,55 ms | 1,57 ms |
| Preparación por cuadro | 24,2 ms | 17,5 ms | 15,8 ms | 15,0 ms | 15,2 ms |
| Cuadros por segundo de tiempo acumulado en pasos | 7,7 | 18,0 | 19,3 | 22,9 | 22,2 |
| `DISPLAY_LATCH_US` por cuadro | 83,5 ms | 8,6 ms | 8,3 ms | 8,0 ms | 8,2 ms |

Hay una sesión informada por versión. La de R6 contiene dos carreras.
Los cuadros por segundo se estiman con
`DISPLAYED` y `ROAD_STEP_US`. `DISPLAY_LATCH_US` en R2 incluye la demora
hasta que el juego vuelve a consultar el refresco. Desde R3 mide hasta
el cambio de búfer desde el hilo de presentación. Los 61 bloques
incompletos de la sesión de R6 ocurrieron fuera de las carreras.
Las versiones anteriores registraban solo faltantes de sesión, con
83, 83, 84 y 117 respectivamente.

### Prueba física de cierre

**R7 en PSP física, 23 de noviembre de 2026.** El color nuevo del auto
se conservó al guardar y cargar la partida. Se completó una carrera de
Aqueduct sin choques. Se jugaron varias carreras de Mulholland Drive
que terminaron en choques durante la conducción. Queda por comprobar
la llegada a la meta en Mulholland en PSP física.

La sesión registró ambos circuitos (`ROAD_MODES=0x18`) a 333/166 MHz,
con `PRESENT_PATH=worker`. El hilo de presentación mostró los 3.647
cuadros enviados al GE.

| Fase | Bloques PCM | Bloques incompletos |
|---|---:|---:|
| Menús | 16.920 | 98 |
| Carga | 806 | 8 |
| Traslados | 809 | 0 |
| Vista de carrera | 17.097 | 0 |

La ventana de carrera `RACE_PCM_*` registró 17.391 bloques, con cero
faltantes, cuadros silenciados por faltantes o descartes. Los 106
bloques incompletos de la sesión se repartieron entre menús y carga.

En otra sesión se completó Grudge Night con R7. El informe registró
`ROAD_MODES=0x6` y presentó los 1.161 cuadros enviados. En carrera
hubo cero faltantes de audio y 12 cuadros PCM descartados. Después de
completar el evento, se volvió al garaje, se suspendió la PSP y se
reanudó. Al abrir el periódico apareció un mensaje de formato de
archivo y la sesión terminó al salir por HOME.

#### Prueba de reanudación de R8

La sesión en PSP física se ejecutó a 333/166 MHz. El informe registró
`RESUME_COUNT=1`, `RESOURCE_REOPENS=1` y `RESOURCE_REOPEN_ERRORS=0`.
El contador confirma que se reabrió un recurso con su posición de
lectura restaurada. Se confirmó en PSP física que, tras reanudar,
se pudieron abrir el periódico, consultar los avisos y volver al garaje.

La sesión se limitó a los menús y la carga. Registró 55 bloques PCM
incompletos en menús y 7 en carga.

#### Procedimiento

1. Iniciá R8 y entrá al garaje con una partida.
2. Suspendé y reanudá la PSP. Abrí el periódico, entrá a los avisos y
   volvé al garaje.
3. Salí con Start más Select y guardá el informe `PERFxx.TXT`.
