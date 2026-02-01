CC = gcc
CFLAGS = -Wall -g -std=c99 -fsanitize=address,undefined

server: server.c
	$(CC) $(CFLAGS) server.c -o nimd

specTest: spec_tester.c
	$(CC) -std=c99 spec_tester.c -o spec_tester