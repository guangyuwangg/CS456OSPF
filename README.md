CS456ASST3
==========



  ___  ___   __   ___   _       __    ___  ___  ____  ___
 / __)/ __) /. | | __) / )     /__\  / __)/ __)(_  _)(__ )
( (__ \__ \(_  _)|__ \/ _ \   /(__)\ \__ \\__ \  )(   (_ \
 \___)(___/  (_) (___/\___/  (__)(__)(___/(___/ (__) (___/

 Guangyu Wang, 20329227

 Include (4) files:
 ----------------------
        1 header file: router.h
        1 source code: router.c
        1 makefile:    Makefile
        1 readme:      README.txt

How To Compile:
-----------------------
        % make

How to Run:
-----------------------
        1> % ./nse-linux386 <router_host> <nse_port>
        2> % ./router <router_id> <nse_host> <nse_port> <router_port>
        Notice that for this program all router should be running on the same
        machine and in an order of from 1 to 5.


Build and test enviroment:
-----------------------
        machines:                [linux016, linux024, linux32].student.cs.uwaterloo.ca
        version of compiler:     GNU Make 3.81

        This program implements a shortest path algorithm and output a log file
        containing the send/receive messages along with the topology graph and
        routing table of the network.
