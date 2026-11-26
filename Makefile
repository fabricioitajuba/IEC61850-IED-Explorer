# Compilador e Flags
CC = gcc
CFLAGS = -Wall -Wextra -O2

# Caminho da libiec61850 dentro do WSL Ubuntu
IEC_DIR = /home/desenvolvimento/libiec61850-1.6.2.1
IEC_BUILD_DIR = $(IEC_DIR)/build

# Localiza automaticamente onde o CMake gerou a libiec61850.a (build/src ou build)
IEC_LIB = $(firstword $(wildcard $(IEC_BUILD_DIR)/src/libiec61850.a $(IEC_BUILD_DIR)/libiec61850.a))

# Se ainda não existir, define o caminho padrão para ser compilado
ifeq ($(IEC_LIB),)
	IEC_LIB = $(IEC_BUILD_DIR)/src/libiec61850.a
endif

# Diretórios de cabeçalhos (Includes) da libiec61850
IEC_INCLUDES = \
	-I$(IEC_DIR)/src/iec61850/inc \
	-I$(IEC_DIR)/src/mms/inc \
	-I$(IEC_DIR)/src/common/inc \
	-I$(IEC_DIR)/src/logging \
	-I$(IEC_DIR)/hal/inc

# Flags do GTK4 via pkg-config
GTK_CFLAGS = $(shell pkg-config --cflags gtk4)
GTK_LIBS = $(shell pkg-config --libs gtk4)

# Diretórios de saída e arquivos do projeto
BIN_DIR = bin
OBJ_DIR = obj
TARGET = $(BIN_DIR)/IED_Explorer

SRCS = main.c
OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS))

# Regra principal: gera o binário final
all: $(TARGET)

# Compila a libiec61850 automaticamente caso ela ainda não tenha sido compilada
$(IEC_LIB):
	@echo "Compilando a libiec61850 pela primeira vez..."
	mkdir -p $(IEC_BUILD_DIR)
	cd $(IEC_BUILD_DIR) && cmake .. && make -j$$(nproc)

# Linka os objetos e gera o executável binário
$(TARGET): $(OBJS) $(IEC_LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(OBJS) -o $(TARGET) $(IEC_LIB) $(GTK_LIBS) -lpthread -lm
	@echo "Binário gerado com sucesso em: $(TARGET)"

# Compila os arquivos .c em objetos .o
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(IEC_INCLUDES) -c $< -o $@

# Limpa apenas os arquivos do seu projeto
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

# Compila e executa o binário
run: all
	./$(TARGET)

.PHONY: all clean run
