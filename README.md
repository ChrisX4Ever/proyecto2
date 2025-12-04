# PASOS A SEGUIR:

1) Ingresar a la página web [https://git-scm.com/install/](https://git-scm.com/install/) y seleccionar el sistema operativo que se esta utilizando en este momento
2) Descargar e instalar el software Git para el dispositivo actual
3) Ingresar a la página web [https://code.visualstudio.com/Download](https://code.visualstudio.com/Download) y seleccionar el sistema operativo que se esta utilizando en este momento
4) Descargar e instalar el software Visual Studio Code para el dispositivo actual
5) Elegir un directorio donde se quiera instalar el proyecto
6) Abrir terminal que apunte a dicho directorio (ejecutar:  cd /RUTA/DE/SU/DIRECTORIO)
7) Ejecutar el comando:

    `git clone https://github.com/ChrisX4Ever/proyecto2/`

8) En la raíz de la carpeta creada, borrar la carpeta "_build_", si existe (**PRECAUCIÓN: NO BORRAR "main/build"**)
9) Dirigirse al dispositivo Raspberry Pi
10) Dirigirse al Computador
11) Abrir el archivo "_main/blink_example_main.c_" utilizando Visual Studio Code
12) Dirigirse a las lineas 26 y 27 y editar los campos "_WIFI_SSID_" y "_WIFI_PASS_" para conectarse a una red local
13) Dirigirse a la linea 28 y editar el campo "_SERVER_IP_" que corresponde a la Dirección IP de la Raspberry Pi (colocar el mouse de la Raspberry Pi en el icono de Wi-Fi para visualizar la Dirección IP)
14) Conectar el dispositivo ESP32-S3 via USB
15) Seleccionar en la esquina inferior izquierda de la interfaz de Visual Studio Code, el botón "_Método de Flasheo_" (el ícono de "estrella") y seleccione _USB-JTAG_
16) Seleccionar en la esquina inferior izquierda de la interfaz de Visual Studio Code, el botón "_Seleccione el Chip a utilizar_" (el ícono de "microchip") y seleccione "_esp32-s3_"
17) Seleccionar en la esquina inferior izquierda de la interfaz de Visual Studio Code, el botón "_Seleccione el puerto a utilizar_" (el ícono de "enchufe") y seleccione la opción que corresponde al USB del dispositivo ESP32-S3
18) Seleccionar en la esquina inferior izquierda de la interfaz de Visual Studio Code, el botón "_Compilar, Flashear y Monitorear_" (el ícono de "fuego") para configurar el dispositivo ESP32-S3 con el código actual
19) Dirigirse al dispositivo Raspberry Pi
20) Configurar la Dirección IP de la ESP32 en el archivo _interfaz.py_ (dicha dirección IP se puede visualizar en la terminal de la ESP32 en Visual Studio Code)
21) Abrir la carpeta _PROYECTO_2_ y seleccionar "_Open Current Folder in Terminal_"
22) En la terminal, escribir "_source venv/bin/activate_" seguido de "_python3 interfaz.py_", así se abrirá la interfaz
23) En la interfaz, seleccionar "_Iniciar Servidor (ESP32)_", eso conectará la ESP32 con la Raspberry Pi mediante el protocolo TCP
24) Seleccionar TCP o UDP para el modo de transmisión de datos, Acelerómetro o Temperatura para utilizar el acelerómetro del sensor BMI270 o el sensor de temperatura del sensor BME688

    NOTA: Si al seleccionar el modo UDP no se visualizan cambios en el gráfico, abrir una terminal en la Raspberry Pi y escribir "__" seguido de "__". Eso logrará que el modo UDP grafique los datos
