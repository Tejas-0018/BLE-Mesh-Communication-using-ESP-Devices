# BLE Mesh Communication using ESP Devices


Project Overview

This project implements a highly efficient, two-way smart network bridging a local Bluetooth Low Energy (BLE) Mesh with the Anedya Cloud using ESP32 microcontrollers. Traditional smart home or industrial setups require every device to connect directly to a Wi-Fi router, which consumes excessive power and degrades network performance. To overcome this bottleneck, the system utilizes a decentralized multi-hop Bluetooth Mesh architecture where devices pass messages to neighboring nodes until they reach their destination. In this setup, only a single Gateway node maintains an active internet connection, significantly reducing the Wi-Fi bandwidth load and lowering the overall power consumption of the individual endpoints.

System Architecture and Hardware

The network consists of three distinct ESP32 microcontrollers acting as specialized nodes. The Sensor Node functions as the data collector, utilizing a hardware timer to wake up every 10 seconds to read real-time temperature and humidity from a physical DHT22 sensor. It compresses this information into a lightweight text string and broadcasts it across the mesh to the Gateway. The Gateway Node acts as the central bridge, running continuous background tasks to catch the local sensor data and securely upload it to the Anedya Cloud via HTTP POST requests. Simultaneously, it polls the cloud every 5 seconds for incoming user commands. Finally, the Relay Node operates as an event-driven actuator that listens for verified commands from the Gateway to safely toggle a physical high-voltage power relay.


Firmware and Network Configuration

The firmware relies on FreeRTOS to manage multiple concurrent tasks, allowing the Gateway to seamlessly handle uploading sensor data and downloading cloud commands without freezing. The network was provisioned securely using the nRF Mesh mobile application, where specific Unicast Addresses were assigned to the Gateway, Sensor, and Relay nodes to ensure precise point-to-point routing. To ensure robust security against unauthorized access, all wireless payloads are encrypted using a shared Application Key bound to the specific device models. Furthermore, the Relay Node's code features automatic logic inversion to accommodate Active-Low hardware, ensuring that an "ON" cloud command correctly outputs a 0-volt signal to engage the physical switch.


Applications and Conclusion

Because of its scalable multi-hop routing capabilities, this system is highly adaptable for various real-world scenarios, including smart home automation, precision agriculture, smart commercial buildings, and Industrial IoT environments where thick concrete walls or heavy machinery often block standard Wi-Fi signals. By successfully integrating a local Bluetooth Mesh with cloud telemetry, the project demonstrates a reliable and fault-tolerant solution that allows users to monitor environmental data globally and control physical hardware instantly, all while relying on just a single Wi-Fi connection for the entire network.
