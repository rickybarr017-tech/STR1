#pragma once

#include <windows.h>
#include <time.h>

typedef unsigned char uInt8;

// -----------------------------------------------------
// ESTRUTURAS DO ARMAZÉM
// -----------------------------------------------------
typedef struct {
    int ocupada;               // 0 = vazia, 1 = ocupada
    int id;                    // ID do componente (1 a 9)
    int familia;               // família do componente (1 a 3)
    ULONGLONG tick_arrumacao;  // GetTickCount64() no fim da arrumação -> para calcular durações
    time_t data_arrumacao;     // data/hora real do fim da arrumação -> para mostrar (FR3, FR11)
    unsigned long ordem;       // nº de ordem de arrumação -> critério FIFO (desempata tempos iguais)
} Celula;

// Estatísticas gerais
typedef struct {
    int total_dispensados;
    int dispensados_familia[4];   // índices 1, 2, 3 = famílias 1, 2, 3
} EstatisticasArmazem;

// Matriz 4x4 para indexar diretamente por [x][z] (1 a 3); índice 0 não usado
extern Celula armazem[4][4];
extern EstatisticasArmazem estatisticas;

// -----------------------------------------------------
// FUNÇÕES BASE DE BITS
// -----------------------------------------------------
int  lerBit(uInt8 valor, uInt8 n_bit);
void escreverBit(uInt8* variavel, int n_bit, int novo_valor);

// -----------------------------------------------------
// EIXO X  (1 = esquerda ... 3 = direita)
// -----------------------------------------------------
void moverXEsquerda();
void moverXDireita();
void pararX();
int  obterPosX();
void irParaX(int x);

// -----------------------------------------------------
// EIXO Y  (1 = fora da cage, 2 = centro, 3 = dentro da célula)
// -----------------------------------------------------
void moverYDentro();
void moverYFora();
void pararY();
int  obterPosY();
void irParaY(int y);

// -----------------------------------------------------
// EIXO Z  (1 = em baixo ... 3 = em cima)
// -----------------------------------------------------
void moverZCima();
void moverZBaixo();
void pararZ();
int  obterPosZ();        // nível 1..3 pelos sensores "de baixo" (-1 se nenhum)
int  obterPosZCima();    // nível 1..3 pelos sensores "de cima"  (-1 se nenhum)
void irParaZ(int z);
int  subirNoNivel();     // do sensor de baixo para o de cima do mesmo nível (1 = chegou)
int  descerNoNivel();    // do sensor de cima para o de baixo do mesmo nível (1 = chegou)

// -----------------------------------------------------
// MOVIMENTO MANUAL POR PASSOS (FR1.2, calibração)
// dir = +1 ou -1. Anda até ao sensor seguinte; se a posição for
// desconhecida, anda até encontrar um sensor. Nunca passa os limites.
// X: +1 direita | Y: +1 para dentro | Z: +1 para cima
// -----------------------------------------------------
void passoX(int dir);
void passoY(int dir);
void passoZ(int dir);

// -----------------------------------------------------
// CALIBRAÇÃO SEMIAUTOMÁTICA (FR2)
// Leva a cage, de qualquer posição, até à referência (X=1, Z=1, Y=2).
// Devolve 1 se conseguiu, 0 se falhou ou foi cancelada.
// -----------------------------------------------------
int calibrar();

// -----------------------------------------------------
// OPERAÇÕES NA CÉLULA (FR1.4, FR1.5)
// Atualizam o indicador "peça na cage" no momento exato em que
// a peça é pousada / levantada.
// -----------------------------------------------------
void porPecaNaCelula();
void tirarPecaDaCelula();
int  pecaNaCage();
void definirPecaNaCage(int existe);

// -----------------------------------------------------
// EMERGÊNCIA (FR5, FR6, FR7)
// -----------------------------------------------------
void ativarEmergencia();       // STOP: pára os motores e guarda os que estavam ligados
void retomarDeEmergencia();    // RESUME: volta a ligar os motores guardados
void cancelarOperacao();       // RESET: os movimentos em curso desistem e os motores ficam parados
void limparCancelamento();     // chamada pela gestora antes de executar o reset
int  emEmergencia();
int  operacaoCancelada();

// -----------------------------------------------------
// BOTÕES E LUZES (FR1.7, FR1.8)
// -----------------------------------------------------
void lerRepousoBotoes();      // guarda o estado dos botões em repouso (chamar no arranque)
int  botaoCimaPremido();
int  botaoBaixoPremido();
void definirLuzCima(int acesa);
void definirLuzBaixo(int acesa);
