http_server:http_server.c
	gcc  -g $^ -o $@ -lpthread
clean:
	rm -rf http_server
