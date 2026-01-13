Compile: gcc main.c -o main $(pkg-config --cflags --libs gtk+-3.0)

Start: gdb ./main

run