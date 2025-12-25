Compile: gcc -g -Wall -Wextra [main.c](http://_vscodecontentref_/1) -o main $(pkg-config --cflags --libs gtk+-3.0)

Start: gdb ./main

run