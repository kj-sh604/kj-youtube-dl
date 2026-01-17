# installation paths
PREFIX = /usr/local

# compiler and flags
CC = cc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -O2
LDFLAGS =

# GTK3 configuration
INCS = `pkg-config --cflags gtk+-3.0`
LIBS = `pkg-config --libs gtk+-3.0`

# debug build
#CFLAGS = -std=c99 -pedantic -Wall -Wextra -g -O0 -DDEBUG