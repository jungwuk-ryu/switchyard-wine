<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Otros idiomas](../README.md#translations)

Switchyard Wine es el runtime de Wine detrás de
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), una app de macOS para
mantener juegos y lanzadores de Windows en contenedores administrados. La app
controla la interfaz, el estado del contenedor y la selección de runtime; este
repositorio contiene el código de Wine, la canalización de build y el trabajo de
compatibilidad debajo de él.

El proyecto hace una elección deliberada: se mantiene en una base Wine conocida
hasta verificar las cargas de trabajo establecidas de Switchyard, en lugar de
actualizar solo porque exista un Wine más nuevo. Los cambios aguas abajo siguen
siendo commits de Git revisables y convencionales sobre una revisión fijada de
WineHQ.

## Por qué existe esta rama

- **Trabajo de macOS en la runtime.** La rama incluye correcciones para rutas de
  fallo reales vistas en Switchyard, incluyendo callback y puenteo de recursos D3DMetal,
  sincronización macOS MSync, renderizado Chromium/CEF, selección de proveedor de
  gráficos, reproducción de medios y fallback tipográfico multilingüe.
- **Una runtime con identidad verificable.** Las builds fijan y hashean entradas
  externas, se ensamblan fuera de la runtime en vivo y solo se publican tras
  verificación. Cada runtime registra revisión de origen, digests de dependencias,
  arquitectura y hashes de binarios principales.
- **Pruebas de regresión junto a las correcciones.** El repositorio cubre D3DMetal,
  callbacks nativos, MSync, hotpatching de Steam overlay, TLS, medios, OpenGL y
  rutas de seguridad de runtime en lugar de aceptar solo compilaciones exitosas.
- **Registros de compatibilidad con contexto.** Los resultados nombran revisión de
  runtime exacta, host macOS, ruta de gráficos, fecha y limitación conocida.
  Un resultado en una configuración no se presenta como soporte universal.

## MSIX y apps de escritorio empaquetadas

Switchyard Wine incluye `wineappx` y `appxsvc.dll` para paquetes MSIX/AppX de
desktop full-trust firmados y sin cifrar. El ciclo implementado cubre inspección,
extracción verificada, instalación y actualización por prefijo, eliminación,
recuperación, recolección de basura y lanzamiento de apps Win32 o WinUI 3
declaradas con identidad de paquete y dependencias estáticas.

Ese alcance es intencionalmente más estrecho que en Windows. Esto no es un cliente
de Microsoft Store, no es compatibilidad UWP/AppContainer, no hay forma de omitir la
verificación de paquetes sin firma, ni promesa de que cada API de Windows App SDK
funcione. Los requisitos
exactos del paquete, comandos, modelo de durabilidad y límites actuales están en la
[guía MSIX](../msix.md).

## Verlo en acción

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Biblioteca de contenedores de Switchyard mostrando juegos y lanzadores de Windows en macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced y Rockstar Games Launcher ejecutándose en Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced y Rockstar Games Launcher en una sesión de Wine administrada.</sub>
</p>

## Obtener o compilar la runtime

Los usuarios de Switchyard normalmente dejan que la app gestione contenedores y
selección de runtime. Los archivos Wine-only firmados y notarizados están
disponibles en
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
Apple Game Porting Toolkit es software provisto por el usuario y nunca se incluye
en este repositorio ni en sus releases.

Para compilar desde código fuente en Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Lea la [guía de compilación](../building.md) antes de publicar o reemplazar una
runtime. Incluye las herramientas necesarias, las dependencias verificadas, la
preparación, la firma, la notarización y las comprobaciones de regresión disponibles.

## Documentación

- [Índice de documentación](../README.md)
- [Arquitectura y límites de repositorio](../architecture.md)
- [Construcción y lanzamiento de runtime](../building.md)
- [Soporte MSIX y de escritorio empaquetado](../msix.md)
- [Compatibilidad de aplicaciones registrada](../compatibility.md)
- [Procedencia del código y dependencias](../provenance.md)
- [Resolución de problemas de juegos Unity](../troubleshooting-unity-games.md)

Para uso general de Wine y desarrollo, utilice
[documentación WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/home) y
[código fuente upstream](https://gitlab.winehq.org/wine/wine). Esta README describe
Switchyard Wine y no duplica el manual de Wine upstream.

## Comunidad y licencia

Únase a
[Switchyard Discord](https://discord.gg/USNfzUza7B) para pruebas de runtime,
informes de compatibilidad y conversación de desarrollo.

Los cambios en Wine y Switchyard Wine están licenciados bajo LGPL-2.1-or-later;
vea `LICENSE` y `COPYING.LIB`. Switchyard Wine es independiente y no está
apoyado por WineHQ, Apple, Microsoft ni por los productos mostrados.
