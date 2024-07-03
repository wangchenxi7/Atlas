all: swapout_thrpt

swapout_thrpt: swapout_thrpt.cpp
	g++ -o $@.bin $^ -std=c++11 -lpthread

clean:
	rm -rf *.bin