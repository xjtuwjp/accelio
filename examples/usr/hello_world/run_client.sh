#!/bin/bash

export LD_LIBRARY_PATH=../../../src/usr/

server_ip=192.168.20.126
port=1234

taskset -c 1 ./xio_client ${server_ip} ${port}

