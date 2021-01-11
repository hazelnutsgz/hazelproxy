proxy:
	g++ -lpthread -ggdb -o myproxy proxy.cc
clean:
	rm -rf myproxy