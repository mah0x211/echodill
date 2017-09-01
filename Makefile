# CXXFLAGS=-O2 -g -Wall -pthread -lrt
# CFLAGS = -O2 -g -Wall -pthread -lrt --std=gnu99 -I/usr/local/include
# CXXFLAGS=-O2 -g -Wall -pthread -I/usr/local/include -L/usr/local/lib
# CFLAGS = -O2 -g -Wall -pthread --std=gnu99 -I/usr/local/include -L/usr/local/lib

# all: bench

# bench: bench.c
# 	$(CXX) $(CXXFLAGS) -o $@ $< -ldill
TARGET=bench
SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)
CFLAGS=-O2 -g
WARNINGS=-Wall


all: preprocess $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) $(WARNINGS) $(CPPFLAGS) -Ideps/libdill -o $@ -c $<

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ deps/libdill/.libs/libdill.a

preprocess:
	cd ./deps/libdill; \
	./autogen.sh; \
	./configure; \
	make; \
	make check
