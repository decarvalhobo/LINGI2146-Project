#!/usr/bin/env python
import socket, time
import os

# socket connection
client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
client_socket.connect(('localhost', 60001))
BROKER_ADDR = 'test.mosquitto.org'

# will hold a single message at a time
message=""
while 1:
    data = client_socket.recv(1024)
    if ( data == 'q' or data == 'Q'):
        client_socket.close()
        break;
    else:
        # data are formatted to end with an EOL
        if (	data != "\n"):
		message+=data
	# when we have everything we process it
	else :
		topic_value = message.split(";")
		myCmd = 'mosquitto_pub -h ' + BROKER_ADDR + ' -t ' + topic_value[0] + ' -m ' + topic_value[1]
		# execute the command that publishes the data received to the specified topic
		os.system(myCmd)
		message=""
		print "Message sent for topic : "+ topic_value[0] + ' with value: ' + topic_value[1]
		
