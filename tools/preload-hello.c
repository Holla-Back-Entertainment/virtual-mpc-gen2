// preload-hello: the smallest LD_PRELOAD library, to check `lab.py preload` end to end.
// Its constructor logs one line to stderr (the app's journal) and appends it to
// /run/mpclab-preload/hello.log. It hooks nothing.
//   build: lab.py build tools   (-> share/bin/libpreload-hello.so)
//   use:   lab.py preload share/bin/libpreload-hello.so ; lab.py preload --off
#include <stdio.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void hello(void) {
    char comm[32] = "?";
    FILE *f = fopen("/proc/self/comm", "r");
    if (f) {
        if (fgets(comm, sizeof comm, f))
            comm[strcspn(comm, "\n")] = 0;
        fclose(f);
    }
    fprintf(stderr, "mpclab-preload-hello: loaded in pid %d (%s)\n", (int)getpid(), comm);
    f = fopen("/run/mpclab-preload/hello.log", "a");
    if (f) {
        fprintf(f, "loaded in pid %d (%s)\n", (int)getpid(), comm);
        fclose(f);
    }
}
