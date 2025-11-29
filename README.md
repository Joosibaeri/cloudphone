This project contains the server component of CloudPhone. The server is written in C and runs on Linux, handling all processing and user account management while communicating with a small ARM client via SSH.

User accounts are stored under `/userdata/accounts`, and the server automatically creates required directories on first launch. The main source file is `main.c`. Virtual machine-related files are stored in the `vm/` folder and include base root filesystems useful for testing.

Build & run
--------------

The preferred way to build the server is via the provided Makefile which produces an executable named `main`:

```sh
make         # builds `main` from main.c
./main       # run the server
```

If you want the compatibility name used previously, `make cloudphone_server` will produce an executable named `cloudphone_server`.

CloudPhone is designed for controlled environments, keeping the client minimal while the server handles most of the logic and data management.