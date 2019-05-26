# Group Project

## Introduction
This project simulates an IoT network using Rime where the sensor data is published through an MQTT-Rime gateway to normal MQTT subscribers. The motes generate data about temperature and humidity. See the pdf report for more details.

## How run it ?
- Add motes in Cooja
- Run the simulation
- Start the serial socket server :
  - Right-click on the root mote (with address 1.0 by default)
  - Click "Mote tools for Z1 1" > "Serial Socket (SERVER)..."
- Run the python script "client"
