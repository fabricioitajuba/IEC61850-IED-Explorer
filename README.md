# IED Explorer

Escrito em linguagem C com interface gráfica GTK4

## Dependências:

- Para instalar as dependências:

```
$ sudo apt update
$ sudo apt install -y build-essential cmake pkg-config libgtk-4-dev adwaita-icon-theme
```

### O que cada pacote faz:

- build-essential: Instala o compilador gcc, o make e as bibliotecas padrão da linguagem C.
- cmake: Necessário caso você precise recompilar a libiec61850.a do zero.
- pkg-config: Localiza automaticamente os caminhos do GTK4 para o Makefile.
- libgtk-4-dev: Instala o GTK4, a GLib e todos os arquivos .h necessários para compilar.
- adwaita-icon-theme: Garante que os ícones vetoriais da árvore e dos botões apareçam corretamente.


### Para Máquina de Produção / Usuário Final (Apenas Executar)

Se você já gerou o arquivo bin/mini_ied_explorer e quer copiá-lo para outro computador com 
Ubuntu/Debian apenas para rodar o programa (sem código-fonte), a instalação é bem mais leve:

```
$ sudo apt update
$ sudo apt install -y libgtk-4-1 adwaita-icon-theme
```

### Para saber todas as dependências do binário:
```
$ ldd ./bin/mini_ied_explorer
```

## Antes de compilar:
Edite o arquivo "Makefile" colocando na variável "IEC_DIR" o caminho onde foi descompactada
a biblioteca "libiec61850.

Ex:
IEC_DIR = /home/desenvolvimento/libiec61850-1.6.2.1

## Para compilar:

```
~/IED Explorer$ make
```

## Para "limpar" a compilação:

```
~/IED Explorer$ make clean
```

O binário executável, será criado na pasta: /bin

## Para executar:

```
~/IED Explorer/bin$ sudo ./mini_ied_explorer
```
