# Xash3D FWGS Engine (Wii port) <img align="right" width="128" height="128" src="https://github.com/FWGS/xash3d-fwgs/raw/master/game_launch/icon-xash-material.png" alt="Xash3D FWGS icon" />

Xash3D ([pronounced](https://ipa-reader.com/?text=ks%C9%91%CA%82) `[ksɑʂ]`) FWGS Es un motor de juego diseñado para brindar compatibilidad con Half-Life Engine y ampliarlo, además de ofrecer a los desarrolladores de juegos un flujo de trabajo conocido. Esta es la versión para Wii/Gamecube del motor.

Xash3D FWGS is a heavily modified fork of an original [Xash3D Engine](https://www.moddb.com/engines/xash3d-engine) by Unkle Mike.

## Instalación y Ejecución 

0) Renombrar `xash.dol` a `boot.dol` una vez compilado.
1) Mueve el archivo `boot.dol` a algún directorio dentro de la carpeta `apps`.
2) Pega tu copia legal `valve` Dentro de la carpeta `xash3d` que deberás crear en la raíz de tu SD.
3) Ejecútalo por medio de Homebrew Channel.

## Controles

La mayoría de controles son soportados.
Todo lo que aparece a continuación se puede volver a enlazar desde Option-> Controls.

### Wii remote + nunchuk

El Nunshuck es necesario en este esquema "Debido a que su stick es el que usarás para moverte"

| Botón | Acción |
| --- | --- |
| **B** (Activar) | Fuego |
| **A** | Saltar |
| **Nunchuk C** | Usar / interactuar, y recargar |
| **Nunchuk Z** | Agacharse |
| **1** | Linterna |
| **2** | Arma secundaria |
| **D-pad up** | Recargar |
| **D-pad down** | Última arma usada |
| **D-pad left / right** | Previo / Siguiente arma|
| **-** | Caminar (Mantén) |
| **+** | Pausa |
| **Home** | Menu |

Para apuntar se usa el puntero del mando. Apunta cerca del centro de la pantalla y solo se moverá el arma, lo que mantiene estable el apuntado de precisión; apunta hacia un borde y la vista girará, más rápido cuanto más te alejes. Los disparos siguen al puntero en lugar de a la cámara.

### Classic controller

| Botón | Acción |
| --- | --- |
| **R** | Fuego |
| **ZR** | Fuego secundaria |
| **L** | Agacharse |
| **ZL** | Caminar (mantenido) |
| **a** | Saltado |
| **b** | Usar / interactuar |
| **x** | Recargar |
| **y** | Linterna |
| **D-pad left / right** | Previo / Siguiente Arma |
| **D-pad down** | Última arma usada|
| **+** | Pausa |
| **Home** | Menu |

El stick izquierdo mueve, el stick derecho mira. No hay apuntado por puntero en este esquema, así que `wii_ir 0` Vale la pena configurarlo si lo usas.

### GameCube controller

| Botón | Acción |
| --- | --- |
| **R** | Fuego |
| **Z** | Fuego secundario |
| **L** | Agacharse |
| **A** | Saltar |
| **B** | Usar / interactuar |
| **X** | Recargar |
| **Y** | Linterna |
| **D-pad left / right** | Previo / Siguiente arma|
| **D-pad down** | Última arma usada |
| **Start** | Menu |

Con el stick izquierdo te mueves, con el C-stick mueves la cámara.

El esquema que está activo se decide según lo que esté conectado, de modo que un mando clásico y un mando de GameCube pueden estar conectados al mismo tiempo sin presentar conflictos entre sí. `wii_buttons 0` Desactiva la lectura directa y recurre a lo que sea que SDL interprete del mando (o controlador).

El menú reproduce media/gamestartup.mp3. Las versiones de Steam incluyen la banda sonora como media/Half-Life01.mp3 en adelante, sin ningún archivo gamestartup.mp3, por lo que el menú se inicia en silencio hasta que copies uno de ellos con ese nombre.

## Instrucciones de la Build 
El puerto de Wii utiliza cmake para compilar sus binarios.

## De preferencia, compile este motor por medio de una distribución Linux, es más rápido y no presenta problemas de enlace

**NOTE: NUNCA USE LOS ARCHIVOS ZIP DE GitHub. GitHub no incluye las dependencias externas que estamos utilizando!**

### Prerrequisitos

*  Instala CMake
*  Instala [devkitPro](https://devkitpro.org/wiki/Getting_Started)
*  Instala devkitPPC y las siguientes librerías faltantes
 `sudo (dkp-)pacman -S wii-dev wii-sdl2 wii-opengx ppc-bzip2 ppc-freetype ppc-zlib`
*  Crea una dirección de desarrollo(Ubicada en un almacenamiento el cual no tenga espacios en su nombre, al momento de compilar no se suele encontrar archivos por este problema)
*  Clona los siguientes repositorios en el mismo directorio.
```
git clone --recursive https://github.com/Gerardo-Hub17/xash3d-fwgs
git clone --recursive https://github.com/Gerardo-Hub17/mainui_cpp
git clone --recursive https://github.com/Gerardo-Hub17/hlsdk-portable
```

### Building
1) Configura build `cmake -S. -Bbuild -DCMAKE_TOOLCHAIN_FILE="/opt/devkitpro/cmake/Wii.cmake"`
2) Compila `make -C build`

O simplemente ./build_wii.sh, que envuelve ambos pasos y toma devkitPro de $DEVKITPRO. En CMake 4, el paso de configuración necesita adicionalmente -DCMAKE_POLICY_VERSION_MINIMUM=3.5, porque el opus empaquetado (vendored) todavía declara un mínimo anterior a 3.5; el script de ayuda lo pasa por ti.

This will build:
- the filesystem
-  hlsdk (game libraries)
-  mainui
-  the engine itself

### Nota: 
-Este es un proyecto en desarrollo. 
-Es posible muy probable toparse con errores e inestabilidad 

### Build options
| option | default | meaning |
| --- | --- | --- |
| `XASH_RENDERER` | `soft` | `soft` for the software rasteriser, `gl` for ref_gl on opengx. Only one can be linked - both compile `ref/common` and both export `GetRefAPI`. |
| `XASH_LOW_MEMORY` | `2` | Engine memory profile. `0` currently crashes on map load - see below. It no longer affects the menu artwork, which is controlled separately by `ui_lowmemory`. |
| `XASH_OGC_TRACE` | `OFF` | Report model loads and texture uploads over the gecko. |

The heap lives in MEM2 (`MALLOC_MEM2` in `sys_ogc.c`). MEM1 holds the
executable, and a GL build leaves only a few MB of it - not enough to load a
map.

### Performance notes

The devkitPro toolchain puts `-O2 -DNDEBUG` on every target, but the `xash`
target used to append `-Og -g3 -fno-omit-frame-pointer`, and later flags win in
GCC. So the engine core, meaning the frame loop, the client, the server, model
and sound loading, was the only part of the build compiled unoptimised while
the renderer and game code were not.

That is fixed, and `engine/common/host.c` is no longer an exception. It was
pinned to `-Og` for a long time because switching it to `-O2` let the engine
come up while almost nothing reached the screen, a symptom matching
`Host_FilterTime` never returning true so `Host_Frame` bails before rendering.
Retested at `-O2` across the full map set and the hang does not return, so
whatever it depended on has been fixed since. If it ever comes back, that is
the first thing to suspect.

Measured on the c0a0 intro, counting dumped frames over a fixed wall-clock
window, software renderer:

| change | avg fps |
| --- | --- |
| 640x480, whole engine -Og | 25.3 |
| 320x240 | 47.2 |
| 320x240, engine -O2 except host.c | 49.8 |
| 320x240, everything -O2 | pin removed, worth about 5% |
| 320x240, 3D rendering disabled (ceiling) | 55.0 |

`vid_scale` is not the way to lower the render resolution, `ref_soft` does
not implement it and silently ignores the request. Change the video mode
(`width`/`height`) instead. 512x384 is not a real mode here and falls back to
640x480.

### Frame pacing

Both renderers wait for vsync before presenting, so the console's refresh rate
is the frame rate ceiling: 60fps on a 60Hz console, 50fps on a 50Hz PAL mode.
The GPU renderer goes through `SDL_GL_SetSwapInterval`, and the software
renderer through `SDL_OGC_UpdateWindowFramebuffer`, which asks the flip to
wait. The engine prints the mode it found at startup.

`fps_max` defaults to 60 and is a fallback, not the pacing. `Host_CalcFPS`
returns zero in single player whenever `gl_vsync` is set, which skips the
software cap entirely, so `fps_max` only takes over once vsync is off.

Default resolution is per renderer: 640x480 for the GPU, 320x240 for the
software rasteriser, since resolution costs the CPU renderer far more.

### Known limits

`XASH_LOW_MEMORY=0` crashes on map load with a NULL callback in libogc's tick
task. The menu artwork no longer depends on it, `ui_lowmemory` controls that
on its own, so profile 0 is now only about the engine's own limits.

An engine error on real hardware is still under investigation. The error
itself is not identified; what is known is that the red exception screen
players used to see was the shutdown crashing after the error, not the error.
`sd:/xash3d/engine.log` now carries the reason.

### Debugging

The Wii has nowhere to print to, so the engine can route stdout to a USB Gecko
in memory card slot B (`-DXASH_OGC_GECKO=1`, on by default in this tree). That
covers everything from the first line of `main()` onwards, which is where the
interesting crashes still are.

Dolphin emulates the gecko as a TCP socket, so a headless boot can be captured
end to end:

```
./test_wii.sh [seconds]
```

It launches Dolphin in batch mode with a USB Gecko in slot B, attaches to the
socket and prints the engine's trace. It reads the SD image from
`~/Library/Application Support/Dolphin/Load/WiiSD.raw` (Dolphin ignores
`WiiSDCardPath`), which needs `xash3d/valve` in its root - the same layout as a
real card. `SDIMG=`, `DOL=` and `DOLPHIN=` override the paths.

To drive it without input, put commands in `valve/userconfig.cfg` on the card -
it is exec'd last, after the menu is up, so `map c0a0` boots straight into the
game. Dolphin can capture what that looks like with
`-C Dolphin.Movie.DumpFrames=True`, which writes PNGs to `<userdir>/Dump/Frames`.

Note that Dolphin's SD emulation is roughly a thousand times slower than real
hardware - every read is a full emulated IOS round trip - so booting under the
emulator takes minutes where a real Wii takes seconds. Don't optimise for it.

### Note
- This is a work in progress
- Expect crashes and instability

### Créditos 
- Uncle Mike for the original Xash3D Engine
- FWGS team for Xash3D FWGS fork
- mardy for the SDL2 port and OpenGX
- devkitPro team

