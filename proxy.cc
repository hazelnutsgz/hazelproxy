// std
#include <stdio.h>
#include <map>
#include <vector>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <string>
// network
#include <arpa/inet.h>
#include <sys/socket.h>

// system
#include <signal.h>
#include <pthread.h>
#include <sys/epoll.h>

// common
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <memory>

#include <sched.h> 

const int MAX_EPOLL_SIZE = 10000;
const int PACKET_BUFFER_SIZE = 1000000;

class BufferPool {
public:
    char* buffer;
    int capacity;
    char* current;
    char* start;
    char* end;
    char* buffer_end;
    BufferPool(int);
    BufferPool();
    ~BufferPool();
};


class Connection {
public:
    struct sockaddr_in listen_address;
	struct sockaddr_in target_address;
    int listen_fd;
    int accepted_fd;
    BufferPool buffer_pool;

    int listen_port;
    char* upstream_ip;
    int upstream_port;
    int upstream_fd;
    Connection() {};
    Connection(int listen_fd, int accepted_fd, \
            char* upstream_ip, int upstream_port, int upstream_fd): 
                listen_fd(listen_fd), accepted_fd(accepted_fd), \
                    upstream_ip(upstream_ip), upstream_port(upstream_port), upstream_fd(upstream_fd) {}
};

using ConnectionPtr = std::shared_ptr<Connection>;


class Upstream {
public:
    int upstream_port;
    char* upstream_ip;
    Upstream(int upstream_port, char* upstream_ip): upstream_ip(upstream_ip), upstream_port(upstream_port) {};
    Upstream() {};
    ~Upstream() {};
};

using UpstreamPtr = std::shared_ptr<Upstream>;


class Config {
public:
    int listen_port;
    char* upstream_ip;
    int upstream_port;
    Config(int listen_port, char* upstream_ip, int upstream_port):listen_port(listen_port), upstream_ip(upstream_ip), upstream_port(upstream_port) {};
};

class Worker {
private:
    int epoll_fd;
    std::unordered_map<int, ConnectionPtr> connection_mapping; //fd: connection
    std::unordered_map<int, int> fd_mapping;
    std::unordered_map<int, UpstreamPtr> listen_mapping; //listen fd : upstream
    std::vector<Config>& configs;
public:
    int cpu;
    pthread_t pid;
    Worker(std::vector<Config>& configs, int cpu): configs(configs), cpu(cpu) {};
    ~Worker() {};
    int Serve();
    int buildListener(Config&);
    int onConnection(std::unordered_map<int, UpstreamPtr>::iterator& iter);
    int onDataIn(int fd);
    int onDataOut(int fd);
};

class Handler {
private:
    std::vector<Config> configs;
    std::vector<Worker> workers;
    static int number;
public:
    void Handle();
    int addProxy(int listen_port, char* upstream_ip, int upstream_port);
    int startWorkers(int worker_number);
    static void* threadProcess(void * arg);
};



int Worker::onConnection(std::unordered_map<int, UpstreamPtr>::iterator& iter) {
    UpstreamPtr upstream = iter->second;
    int listen_fd = iter->first;
    
    //accept
    struct sockaddr_in _addr;
	int socklen = sizeof(sockaddr_in);
	int accept_fd = accept(listen_fd, (struct sockaddr *)&_addr, (socklen_t*) & socklen);


    //Connection Upstream
    struct sockaddr_in target_address;
    target_address.sin_family = AF_INET;
    target_address.sin_port = htons(upstream->upstream_port);
    if (inet_pton(AF_INET, upstream->upstream_ip, &target_address.sin_addr) <= 0) { return -1; }

    const int flag = 1;
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(setsockopt(client_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) { return -1; }
    if(connect(client_fd,(struct sockaddr*) &target_address, sizeof(struct sockaddr_in)) < 0) { return -1; }


    //New Connection;
    auto connection = std::make_shared<Connection>(listen_fd, accept_fd, upstream->upstream_ip, upstream->upstream_port, client_fd);
    connection_mapping[accept_fd] = connection;
	connection_mapping[client_fd] = connection;
    fd_mapping[accept_fd] = client_fd;
    fd_mapping[client_fd] = accept_fd;
	int flags;
	flags = fcntl(accept_fd, F_GETFL, 0);    
	fcntl(accept_fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(client_fd, F_GETFL, 0);    
	fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

	// event
	struct epoll_event ev;
	ev.data.fd = accept_fd;
	ev.events = EPOLLIN;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, accept_fd, &ev);
	ev.data.fd = client_fd;
	ev.events = EPOLLIN;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
	
	printf("OPEN: %d <--> %d\n", accept_fd, client_fd);
	return 0;
}

int Worker::buildListener(Config& config) {

    struct sockaddr_in listen_address;
    listen_address.sin_family = AF_INET;
    listen_address.sin_port = htons(config.listen_port);

    listen_address.sin_addr.s_addr = htonl(INADDR_ANY);

	// if (src_host == NULL) {
	// 	src_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	// } else {
	// 	if (inet_pton(AF_INET, src_host, &src_addr.sin_addr) <= 0) { return false; }
	// }

	// listen
	const int flag = 1;
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag)) < 0) { return -1; }
    if (bind(listen_fd,(const struct sockaddr*)&(listen_address), sizeof(struct sockaddr_in)) < 0) { return -1; }
    if (listen(listen_fd, SOMAXCONN) < 0) { return -1; }
    
    listen_mapping[listen_fd] = std::make_shared<Upstream>(config.upstream_port, config.upstream_ip);;
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd  = listen_fd;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    return 0;
}

int Worker::onDataIn(int in_fd) {
    ConnectionPtr connection = connection_mapping[in_fd];
	
    if (connection->buffer_pool.current != connection->buffer_pool.start) {
        //buffer is in used.
        return 0;
    }
	
	// recv
	
	int ret = recv(in_fd, connection->buffer_pool.start, PACKET_BUFFER_SIZE, 0);
    // std::cout << "read size " << ret << std::endl;
	if (ret <= 0) {
		if (errno == EAGAIN) {
			return 0;
		} else {
            std::cout << "Errno: " << errno << std::endl;
            close(in_fd);
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, in_fd, 0);
			return -1;
        }
	}
	
	connection->buffer_pool.end = connection->buffer_pool.start + ret;
	
    int out_fd = fd_mapping[in_fd];
    bool finished = true;
	while(connection->buffer_pool.current < connection->buffer_pool.end) {
		ret = send(out_fd, connection->buffer_pool.current, connection->buffer_pool.end-connection->buffer_pool.current, 0);
        // std::cout << "onDataIn: sent " << ret << std::endl;
		if (ret <= 0) {
			if (errno == EAGAIN) {
                finished = false;
				break;
			} else {
                close(out_fd);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, out_fd, 0);
				return -1;
			}
		} else {
            // Normal
			connection->buffer_pool.current += ret;
		}
	}
    // Drain out
    if (finished) {
        connection->buffer_pool.current = connection->buffer_pool.start;
	    connection->buffer_pool.end = connection->buffer_pool.start;
    } else {
        // start watching EPOLLOUT, because we want to know when kernel writing buffer is ready....
        struct epoll_event ev;
        ev.data.fd = out_fd;
        ev.events = EPOLLIN|EPOLLOUT;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, out_fd, &ev);
    }
}

int Worker::onDataOut(int out_fd) {

    bool finished = true;
    int ret;
    ConnectionPtr connection = connection_mapping[out_fd];
	while(connection->buffer_pool.current < connection->buffer_pool.end) {
		ret = send(out_fd, connection->buffer_pool.current, connection->buffer_pool.end-connection->buffer_pool.current, 0);
        // std::cout << "onDataOut: sent " << ret << std::endl;
		if (ret < 0) {
			if (errno == EAGAIN) {
                finished = false;
				break;
			} else {
				return -1;
			}
		} else {
            // Normal
			connection->buffer_pool.current += ret;
		}
	}

    if (!finished) {
		// keep watch EPOLLOUT
	} else {
		struct epoll_event ev;
		ev.data.fd = out_fd;
		ev.events = EPOLLIN;  //Remove EPOLLOUT
		epoll_ctl(epoll_fd, EPOLL_CTL_MOD, out_fd, &ev);
		
		if (ret < 0) {
			// FIXME: error, should close
		}
        connection->buffer_pool.current = connection->buffer_pool.start;
	    connection->buffer_pool.end = connection->buffer_pool.start;
	}
    return 0;
}

int Worker::Serve() {

    epoll_fd = epoll_create(MAX_EPOLL_SIZE); // epoll_create(int size); size is no longer used

    for (auto& config: configs) {
        buildListener(config);
    }

    struct epoll_event events[MAX_EPOLL_SIZE];
	int count = 0;
	
    //serving
	while(true) {
		count = epoll_wait(epoll_fd, events, MAX_EPOLL_SIZE, -1);
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				printf("epoll error\n");
				return -1;
			}
		}
		for (int i = 0; i < count; i ++) {
            // std::cout << events[i].data.fd << std::endl;
			auto it = listen_mapping.find(events[i].data.fd);
		    if (it != listen_mapping.end()) {
				onConnection(it);
			} else {
				if (events[i].events & EPOLLOUT) {
					onDataOut(events[i].data.fd);
				}
				if (events[i].events & EPOLLIN) {
					onDataIn(events[i].data.fd);
				}
			}
		}
	}
}

BufferPool::BufferPool() {
    capacity = PACKET_BUFFER_SIZE;
    start = new char[capacity];
    current = start;
    end = start;
    buffer_end = start + capacity;
}

BufferPool::BufferPool(int capacity) : capacity(capacity) {
    start = new char[capacity];
    current = start;
    end = start;
    buffer_end = start + capacity;
}

BufferPool::~BufferPool() {
    delete []buffer;
    buffer = nullptr;
}

void Handler::Handle() {
    while(true) {
        
    }
}

int Handler::addProxy(int listen_port, char* upstream_ip, int upstream_port) {
    Config config(listen_port, upstream_ip, upstream_port);
    configs.push_back(config);
    return 0;
}

void* Handler::threadProcess(void * arg) {

    Worker* worker = (Worker *) arg;

    cpu_set_t cpuset; 

    //the CPU we whant to use
    int cpu = worker->cpu;

    CPU_ZERO(&cpuset);       //clears the cpuset
    CPU_SET( cpu , &cpuset); //set CPU 2 on cpuset


    /*
    * cpu affinity for the calling thread 
    * first parameter is the pid, 0 = calling thread
    * second parameter is the size of your cpuset
    * third param is the cpuset in which your thread will be
    * placed. Each bit represents a CPU
    */
    sched_setaffinity(0, sizeof(cpuset), &cpuset);

    worker->Serve();
    return NULL;
}
int Handler::startWorkers(int worker_number) {
    for (int i = 0; i < worker_number; ++i) {
        Worker* worker = new Worker(configs, i);
        pthread_create(&(worker->pid), NULL, &threadProcess, worker); // remember to pthread_join
        // workers.push_back(worker);
    }
}

int main(int argc, char const *argv[])
{
    Handler handler;
	handler.addProxy(8080, "127.0.0.1", 80);
	handler.addProxy(8000, "127.0.0.1", 81);
    handler.startWorkers(20);
	handler.Handle();
    return 0;
}
