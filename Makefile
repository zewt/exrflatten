CXXFLAGS=-std=c++1y -Wall   -Wno-sign-compare -O2 -g
LDFLAGS=-lIlmImf -g
CC=g++

all: exrflatten
exrflatten: exrflatten.o helpers.o exrsamples.o

