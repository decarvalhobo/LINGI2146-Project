# Group Project

## Introduction
This project simulates an IoT network using Rime where the sensor data is published through an MQTT-Rime gateway to normal MQTT subscribers. The motes generate data about temperature and humidity. See the pdf report for more details.

## Prerequisites for the gateway
- Having Mosquitto installed (with `apt-add-repository ppa:mosquitto-dev/mosquitto-ppa && apt-get install mosquitto`)
- Having Mosquitto client installed (with `apt-get install mosquitto-clients`)
## How run it ?
- Add motes in Cooja (using the file `mote.c`)
- Start the serial socket server :
  - Right-click on the root mote (with address 1.0 by default)
  - Click "Mote tools for Z1 1" > "Serial Socket (SERVER)..."
- Run the simulation
- Run the python script `./gateway` (the MQTT broker will run on port 1888 unless changed in the global variable)
