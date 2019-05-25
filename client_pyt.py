#!/usr/bin/env python
import subprocess
from threading import Thread, RLock
import socket, time
import os

# Settings
COOJA_ADDR = 'localhost'
COOJA_PORT = 60001
BROKER_ADDR = 'localhost'
BROKER_PORT = "1888"
RCV_WDWS = 1024
RFS_DELAY = 5

# Global variables
topic_set ={}
notified =set()
stop_thr= False

# socket connection to Cooja root mote
client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
client_socket.connect((COOJA_ADDR, COOJA_PORT))

# function to stop transmission of data from the topic topic_name 
# it sends an formatted message to the root that will handle the distribution	
def send_stop(client_socket,topic_name):
	client_socket.send("0:"+topic_name+"\n")

# function to restart transmission of data from the topic topic_name 	
# it sends an formatted message to the root that will handle the distribution	
def send_restart(client_socket,topic_name):
	client_socket.send("1:"+topic_name+"\n")

# class to instantiate the process that will notice the root from topics 
# that no longer needs to be published.
class SubClnr(Thread):
		def __init__(self):
			Thread.__init__(self)
			self.first=1

		def run(self):
			while (not stop_thr): # never stop, unless gateway program dies
    				for topic in topic_set:
					if (len(topic_set[topic])==0 and topic not in notified): # no subscriber and not yet notified to the root
						send_stop(client_socket,topic)
						notified.add(topic)
						print (" STOP "+topic)
					if (len(topic_set[topic])>0 and topic in notified): # has subscriber and still in the notified list
						send_restart(client_socket,topic)
						notified.remove(topic)
						print (" RESTART "+topic)
				if (self.first == 1):
					time.sleep(10)
					self.first=0
				else:	
    					time.sleep(RFS_DELAY)

# class to lauch the MQTT broker, format message received from the root 
# and forward it to the broker
class SubscriptionOpti(Thread):
	
    	def __init__(self):
		Thread.__init__(self)
		self.pr = subprocess.Popen("mosquitto -v -p "+BROKER_PORT, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
		
	def run(self):
	    stdout = []
	    nxtIsTopic=False
	    idSub=""
	    while (not stop_thr):
		line = self.pr.stdout.readline()
		stdout.append(line)
		tabline = line.split(' ')

		# in previous iteration we saw that it was a subscribe so now we will get the topic
		if (nxtIsTopic):
			nxtIsTopic = False
			topic = tabline[1].split('\t')[1]
			if (topic not in topic_set):
				print ("Tried to subscribe to unknown topic: "+topic)
				continue
			else:
				if (len(topic_set[topic])==0): 
					send_restart(client_socket,topic)
					if (topic in notified):
						notified.remove(topic)
					print ("Unmuted " + topic)
				if (idSub not in topic_set[topic]):				
					topic_set[topic].append(idSub)
		# check for subscription and get the id  
		if (tabline[1]=="Received" and tabline[2]=="SUBSCRIBE"):
			nxtIsTopic = True
			#print tabline
			idSub = tabline[4].split('-')[0]
		# check if disconnection to withdraw id from subscriber set
		if (tabline[1]=="Socket" and tabline[2]=="error"):
			idSub = tabline[5].split('-')[0]
			for top in topic_set:
				if (idSub in topic_set[top]): topic_set[top].remove(idSub) # remove idSub from the publisher list for topic top
				if (len(topic_set[top])==0 and top not in notified): 
					notified.add(topic)		# case this topic has no sub and is not notified,
					send_stop(client_socket,top)	# notify the root of it	
					print "Muted " + top
		if (line == '' and self.pr.poll() != None):
		    break


def main(client_socket):
	# will hold a single message at a time
	message=""
	
	while (not stop_thr):			
	    data = client_socket.recv(RCV_WDWS)
	    if ( data != ""):
		# data are formatted to end with an EOL
		if (data != "\n"):
			message+=data
		# when we have everything we process it
		else :
			tmp = message.split("\n") # prevent bug
			for part in tmp:
				topic_value = part.split(";")
				if (len(topic_value) == 4): # ;IP_ADDR;TOPIC;VALUE
					topic = topic_value[2]
					if (topic not in topic_set):
						topic_set[topic]=[]
					#print "Set of topics "
					#print topic_set
					myCmd = 'mosquitto_pub -h ' + BROKER_ADDR + ' -t ' + topic_value[2] + ' -p '+ BROKER_PORT+' -m ' + topic_value[3] 
					# execute the command that publishes the data received to the specified topic
					# the mosquitto clients must be installed : apt-get install mosquitto-clients 
					os.system(myCmd)
					print "Message sent from "+topic_value[1]+ " for topic : "+ topic_value[2] + ' with value: ' + topic_value[3]

				topic_value = []
			tmp=""
			message=""


try:		
	thr1 = SubscriptionOpti()	# thread to keep track of subscription/unsubscription per topic
	thr2 = SubClnr()		# thread to clean after RFS_DELAY the topics that has no subscriber	
	thr1.start()
	thr2.start()
	main(client_socket)		# main thread that listen for the data transmitted by the root 
					# and format in MQTT and send to a broker (settings in global at the begining)
except:
	print "Exiting the client... "	
	stop_thr = True			# tell the threads to stop
	thr1.join()
	thr2.join()
	print "done."	
