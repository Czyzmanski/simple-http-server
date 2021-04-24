.PHONY: all clean

all:
	g++ -Wall -Wextra -O2 -std=c++17 main.cpp -o server

clean:
	rm -f *.o server
