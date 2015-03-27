test:tcp_server.c echo.c
	gcc -g -Wall $^ -o $@
