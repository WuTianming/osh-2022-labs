all: 1 2

2: 2.cpp
	g++ 2.cpp -o 2 -lpthread

1: 1.c
	gcc 1.c -o 1 -lpthread

EXE=1 2 3 4 5

clean:
	rm -rf $(EXE)
