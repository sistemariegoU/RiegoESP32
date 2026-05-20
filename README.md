# RiegoESP32
Sistema de riego inteligente con ESP32-S3 que monitorea temperatura, humedad del suelo y ambiente en tiempo real. Integra sensores, servomotores, pantalla OLED y TensorFlow Lite Micro para automatizar decisiones de riego y optimizar el uso del agua en cultivos.


# 🌱 Sistema de Riego Inteligente con ESP32-S3

Proyecto de automatización agrícola basado en **ESP32-S3**, diseñado para monitorear variables ambientales y automatizar el riego de cultivos mediante sensores, servomotores e inteligencia artificial embebida con **TensorFlow Lite Micro**.

---

# 📌 Descripción

Este sistema permite medir variables importantes del cultivo como:

* 🌡️ Temperatura
* 💧 Humedad del suelo
* ☁️ Humedad ambiental

Con estos datos, el ESP32-S3 procesa la información y ejecuta acciones automáticas de riego utilizando servomotores y una bomba de agua.

El proyecto también incorpora:

* 📟 Pantalla OLED para visualización en tiempo real
* 🤖 Modelo de IA con TensorFlow Lite Micro
* 🔘 Modo manual mediante pulsador
* 🚿 Automatización del recorrido de riego
* 📡 Posibilidad de conexión IoT y monitoreo remoto

---

# ⚙️ Tecnologías Utilizadas

* ESP32-S3
* Arduino Framework
* PlatformIO
* TensorFlow Lite Micro
* C++
* Sensores de humedad y temperatura
* Pantalla OLED SSD1306 / SH110X

---

# 🧩 Componentes Principales

| Componente                  | Función                         |
| --------------------------- | ------------------------------- |
| ESP32-S3                    | Control principal del sistema   |
| Sensor de humedad del suelo | Medición de humedad             |
| Sensor DHT11/DHT22          | Temperatura y humedad ambiental |
| Pantalla OLED               | Visualización de datos          |
| Servomotores                | Apertura/cierre de válvulas     |
| Bomba de agua               | Sistema de riego                |
| Pulsador                    | Activación manual               |

---

# 🚀 Funcionalidades

* Monitoreo en tiempo real
* Riego automático
* Riego manual
* Visualización en OLED
* Integración de IA embebida
* Simulación de estados del cultivo
* Control de servomotores

---

# 🧠 Inteligencia Artificial

El sistema utiliza un modelo básico de Machine Learning entrenado con variables ambientales para apoyar la toma de decisiones sobre el riego.

El modelo fue implementado usando:

* TensorFlow
* TensorFlow Lite Micro
* ESP32-S3

---

# 📂 Estructura del Proyecto

```bash
📦 smart-irrigation-system
 ┣ 📂 src
 ┣ 📂 include
 ┣ 📂 lib
 ┣ 📂 data
 ┣ 📂 model
 ┣ 📜 platformio.ini
 ┗ 📜 README.md
```

---

# 🔌 Próximas Mejoras

* ☁️ Integración con IoT
* 📱 Aplicación móvil
* 🌦️ Integración con APIs climáticas
* 📊 Dashboard web
* 🔋 Sistema alimentado por energía solar

---

# 👨‍💻 Autor

Desarrollado por Diego Pedraza
Proyecto académico y experimental enfocado en agricultura inteligente, sistemas embebidos e inteligencia artificial aplicada.

---

# 📜 Licencia

Este proyecto es de uso educativo y experimental.
