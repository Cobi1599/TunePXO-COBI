# TunePXO COBI

Plugin VST3 de afinación vocal para Windows 10/11 de 64 bits.

## Objetivo
- Compatible con Ableton Live 12 y FL Studio.
- Interfaz oscura moderna con acentos púrpura.
- Controles iniciales de Retune Speed, Humanize, Correction Mix, Key y Scale.
- Compilación automática para Windows mediante GitHub Actions.

## Estado actual: prototipo v0.1
Esta primera versión establece el proyecto JUCE/CMake, la interfaz y una ruta de compilación automática. El audio pasa sin alteraciones; la detección de frecuencia mostrada es una vista previa experimental, no un afinador preciso. **Todavía no aplica corrección de tono ni pitch shifting.** Esa será la siguiente fase de desarrollo y pruebas.

## Tecnología
- C++17
- JUCE 8.0.6
- CMake
- VST3 / Standalone
