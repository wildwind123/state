#ifndef STAT_SERVER_H
#define STAT_SERVER_H

/* Runs the HTTP API + web UI on addr ("HOST:PORT", "[::1]:PORT" or "PORT"). Never returns on success. */
int server_run(const char *addr);

#endif
