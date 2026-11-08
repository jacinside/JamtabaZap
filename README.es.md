# JamtabaZap

Un fork de [JamTaba](https://github.com/elieserdejesus/JamTaba), basado en [la versión de defterai](https://github.com/defterai/JamTaba), que envía y recibe video sincronizado en el [formato de video de NinjamZap](https://github.com/jacinside/ninjamzap-core/blob/main/docs/VIDEO_SYNC.md). Los músicos en JamtabaZap y en las apps de NinjamZap se ven en la misma sala.

**Estado: beta.** Se descarga desde [Releases](https://github.com/jacinside/JamtabaZap/releases).

[Read in English](README.md)

## Qué agrega

- Video H.264 sincronizado con el intervalo de audio de cada músico, en los servidores que transmiten video. En los demás usa el formato de video anterior de JamTaba, en SD.
- Calidad de video en Preferencias > Video: SD, VGA o HD. Ahí mismo se puede dejar de recibir video.
- Calidad de audio en Preferencias > Audio: Económica, Estándar, Alta o Estudio (64, 128, 192 o 256 kbps).
- Video en sincronía con el audio en el chat de voz.

## Instalación

- **Windows:** descomprimí el zip y abrí `JamtabaZap.exe`. Necesita un driver ASIO; si tu interfaz no tiene, sirve [ASIO4ALL](https://www.asio4all.org/). El programa no está firmado, así que SmartScreen muestra un aviso: elegí "Más información" y después "Ejecutar de todas formas".
- **macOS:** versión para Intel, que también corre en Apple Silicon con Rosetta. No está firmada: la primera vez abrila con clic derecho > Abrir.
- **Linux:** un solo archivo, `JamtabaZap-x86_64.AppImage`, para distribuciones de 64 bits. Trae las librerías que necesita; no hay que instalar nada.
  1. Hacelo ejecutable: clic derecho, Propiedades > Permisos, y marcá "Permitir ejecutar el archivo como un programa". O en una terminal: `chmod +x JamtabaZap-x86_64.AppImage`
  2. Abrilo con doble clic, o con `./JamtabaZap-x86_64.AppImage`
  
  Si dice que falta FUSE, abrilo con `./JamtabaZap-x86_64.AppImage --appimage-extract-and-run`, o instalá el paquete `fuse3`.
- **Plugin VST (Windows):** descomprimí `JamtabaZap-VST-windows-x64.zip` y copiá `JamtabaZapVST.dll` a la carpeta de VST de tu DAW.

La [guía de uso de JamTaba](https://github.com/elieserdejesus/JamTaba/wiki/Jamtaba's-user-guide) (en inglés) explica el resto de la app.

## Límites conocidos

- Se pueden usar plugins VST2 dentro de JamtabaZap. JamtabaZap también existe como plugin VST2 para Windows de 64 bits; todavía no hay versión de plugin para Mac.
- HD usa mucha CPU en computadoras viejas. Cuando el codificador no da abasto se saltean cuadros: elegí VGA o SD si tu computadora no llega.

## Créditos y licencia

Todo el crédito de JamTaba es de su autor, Elieser de Jesus, y de sus colaboradores, y de defterai por la versión sobre la que se construye este fork. Los detalles de licencia están en el [README en inglés](README.md#credits-and-license).

Los problemas se reportan desde Help > Report bugs or suggest improvements, o en [Issues](https://github.com/jacinside/JamtabaZap/issues).
