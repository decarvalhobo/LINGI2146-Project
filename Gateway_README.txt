1. Launch Cooja with multiple nodes of my_mote.c 
2. right-click on mote 1 -> Mote tools -> Serial Socket (SERVER)

3. Open a terminal in the VM and run the client_pyt.py with 

	./client_pyt.py
	
This will convert all the data from cooja received by the gateway into MQTT publish

4. Verify that the data is transmitted to the broker with 
	
	mosquitto_sub -h test.mosquitto.org -t Hello

(The topic can be switch, depending of witch we want to listen)
