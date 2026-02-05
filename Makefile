# =========================
# Makefile Lavagna/Utente
# =========================

CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -pthread

# =========================
# Directory
# =========================
SRC_DIR = src
INC_DIR = include

# =========================
# File sorgenti
# =========================
LAVAGNA_SRC = $(SRC_DIR)/lavagna.c
UTENTE_SRC  = $(SRC_DIR)/utente.c

# =========================
# File header
# =========================
LAVAGNA_H = $(INC_DIR)/lavagna.h
UTENTE_H  = $(INC_DIR)/utente.h
SHARED_H  = $(INC_DIR)/shared.h

# =========================
# Eseguibili
# =========================
LAVAGNA_BIN = lavagna
UTENTE_BIN  = utente

# =========================
# Regole
# =========================

all: $(LAVAGNA_BIN) $(UTENTE_BIN)

# Lavagna dipende da lavagna.c, lavagna.h E shared.h
$(LAVAGNA_BIN): $(LAVAGNA_SRC) $(LAVAGNA_H) $(SHARED_H)
	$(CC) $(CFLAGS) $(LAVAGNA_SRC) -o $@

# Utente dipende da utente.c, utente.h E shared.h
$(UTENTE_BIN): $(UTENTE_SRC) $(UTENTE_H) $(SHARED_H)
	$(CC) $(CFLAGS) $(UTENTE_SRC) -o $@

clean:
	rm -f $(LAVAGNA_BIN) $(UTENTE_BIN) *.o

.PHONY: all clean
